#!/usr/bin/env python3
"""Check the 16 MiB single-response guard.

The judge caps one response line at 16 MiB. Emitting more is a protocol
violation, so the engine refuses the result with `response_too_large` instead of
producing a line the judge must reject.

    python tests/limit_test.py --exe build/minivectordb.exe
"""

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mvdb_harness as harness  # noqa: E402

ROW_WIDTH = 2000          # characters per row
ROWS = 9000               # 9000 * ~2 KB > 16 MiB
BATCH = 200


def payload(index):
    return ("x%07d" % index).ljust(ROW_WIDTH, "y")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--protocol-dir", type=Path, default=harness.DEFAULT_PROTOCOL)
    parser.add_argument("--data-root", type=Path, default=harness.DEFAULT_DATA_ROOT)
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args(argv)

    strict_json, _ = harness.load_official_modules(args.protocol_dir)
    config = strict_json((args.protocol_dir / "config.json").read_text(encoding="utf-8"))
    config = dict(config, request_timeout=600, case_timeout=3600,
                  max_response_bytes=16 * 1024 * 1024)

    output = args.output or (harness.ARTIFACT_ROOT / "protocol-results" / "limit")
    output.mkdir(parents=True, exist_ok=True)
    args.data_root.mkdir(parents=True, exist_ok=True)
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-limit-", dir=args.data_root))

    session = harness.Session([str(Path(args.exe).resolve())], data_dir, config,
                              strict_json, output / "limit.stderr.log")
    failures = []
    try:
        session.start()
        session.request(f"CREATE TABLE big (id BIGINT PRIMARY KEY, "
                        f"content VARCHAR({ROW_WIDTH}));")
        loaded = 0
        while loaded < ROWS:
            count = min(BATCH, ROWS - loaded)
            values = ", ".join(
                f"({loaded + k + 1}, '{payload(loaded + k + 1)}')" for k in range(count))
            session.request("INSERT INTO big VALUES " + values + ";")
            loaded += count
        print(f"loaded {loaded} rows of {ROW_WIDTH} chars", flush=True)

        # No LIMIT: the engine has to materialise everything.
        response = session.request("SELECT * FROM big;")
        if response.get("ok") is not False:
            failures.append("expected the oversized result to be refused")
        else:
            code = response.get("error", {}).get("code")
            if code != "response_too_large":
                failures.append(f"unexpected error code {code!r}")
            else:
                print(f"refused cleanly: {code}", flush=True)

        # The process must survive and keep serving, with a small response.
        small = session.request("SELECT id FROM big ORDER BY id LIMIT 3;")
        rows = small.get("rows", [])
        if rows != [[1], [2], [3]]:
            failures.append(f"small query after refusal is wrong: {rows!r}")
        else:
            print("process still healthy after refusal", flush=True)

        session.shutdown()
    except Exception as exc:  # noqa: BLE001
        failures.append(f"{type(exc).__name__}: {exc}")
    finally:
        session.stop()
        shutil.rmtree(data_dir, ignore_errors=True)

    if failures:
        print("response limit: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("response limit: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
