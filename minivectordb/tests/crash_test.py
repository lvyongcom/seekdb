#!/usr/bin/env python3
"""Hard-kill recovery checks that the public package does not cover.

The 20 public cases only restart cleanly (`--restart` sends an orderly
shutdown). The task description also requires surviving a forced kill:

  * data acknowledged before the kill must still be there
  * an uncommitted transaction must leave no trace
  * recovery must be repeatable

Run against a locally built binary:

    python tests/crash_test.py --exe build/minivectordb.exe
"""

import argparse
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mvdb_harness as harness  # noqa: E402


def make_session(argv, data_dir, config, strict_json, log_path):
    session = harness.Session(argv, data_dir, config, strict_json, log_path)
    return session


def rows_of(response):
    return response.get("rows", [])


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(argv, data_dir, config, strict_json, log_path):
    session = make_session(argv, data_dir, config, strict_json, log_path)
    try:
        # -- committed writes survive a forced kill ----------------------------
        session.start()
        session.request("CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));")
        session.request("INSERT INTO docs VALUES (1, '一'), (2, '二'), (3, '三');")
        session.stop()  # hard kill, no shutdown request

        session.start()
        response = session.request("SELECT id FROM docs ORDER BY id;")
        check(rows_of(response) == [[1], [2], [3]],
              f"committed rows lost after kill: {rows_of(response)!r}")

        # -- an uncommitted transaction leaves no trace ------------------------
        session.request("BEGIN;")
        session.request("DELETE FROM docs WHERE id >= 1;")
        session.request("INSERT INTO docs VALUES (9, '九');")
        uncommitted = session.request("SELECT id FROM docs ORDER BY id;")
        check(rows_of(uncommitted) == [[9]],
              f"a transaction must see its own writes: {uncommitted!r}")
        session.stop()  # kill with the transaction still open

        session.start()
        response = session.request("SELECT id FROM docs ORDER BY id;")
        check(rows_of(response) == [[1], [2], [3]],
              f"uncommitted changes survived the kill: {rows_of(response)!r}")

        # -- recovery is repeatable -------------------------------------------
        session.stop()
        session.start()
        response = session.request("SELECT id FROM docs ORDER BY id;")
        check(rows_of(response) == [[1], [2], [3]],
              f"second recovery changed the result: {rows_of(response)!r}")

        # -- committed changes after a rollback survive ------------------------
        session.request("BEGIN;")
        session.request("UPDATE docs SET content = '变更' WHERE id = 2;")
        session.request("ROLLBACK;")
        session.request("UPDATE docs SET content = '提交' WHERE id = 3;")
        session.stop()

        session.start()
        response = session.request("SELECT id, content FROM docs ORDER BY id;")
        expected = [[1, "一"], [2, "二"], [3, "提交"]]
        check(rows_of(response) == expected,
              f"rollback/commit after recovery is wrong: {rows_of(response)!r}")

        session.shutdown()
    finally:
        session.stop()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="path to the built binary")
    parser.add_argument("--protocol-dir", type=Path, default=harness.DEFAULT_PROTOCOL)
    parser.add_argument("--data-root", type=Path, default=harness.DEFAULT_DATA_ROOT)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--keep-data", action="store_true",
                        help="keep the data directory for inspection")
    args = parser.parse_args(argv)

    strict_json, _ = harness.load_official_modules(args.protocol_dir)
    config = strict_json((args.protocol_dir / "config.json").read_text(encoding="utf-8"))

    output = args.output or (harness.ARTIFACT_ROOT / "protocol-results" /
                             "crash")
    output.mkdir(parents=True, exist_ok=True)
    args.data_root.mkdir(parents=True, exist_ok=True)

    import tempfile

    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-crash-", dir=args.data_root))
    try:
        run([str(Path(args.exe).resolve())], data_dir, config, strict_json,
            output / "crash.stderr.log")
    except Exception as exc:  # noqa: BLE001 - report, never crash the run
        print(f"crash recovery: FAIL - {exc}")
        return 1
    finally:
        if args.keep_data:
            print(f"data kept at {data_dir}")
        else:
            shutil.rmtree(data_dir, ignore_errors=True)

    print("crash recovery: PASS (committed kept, uncommitted discarded, repeatable)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
