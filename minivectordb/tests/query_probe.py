#!/usr/bin/env python3
"""Split the query cost into scan vs distance.

Loads one table, then times query shapes that differ only in how much work each
matching row needs:

  A  scan + filter, order by primary key        (no vector, no projection cost)
  B  scan + filter, order by id, project only id
  C  scan + filter + L2 distance + TopK         (the real workload)

The gap between B and C is the distance work; A to B is projection.
"""

import argparse
import random
import shutil
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mvdb_harness as harness  # noqa: E402


def build_vector(rng, dimension):
    return "[" + ", ".join(f"{rng.uniform(-1.0, 1.0):.3f}" for _ in range(dimension)) + "]"


def timed(session, sql, repeats=20):
    best = None
    for _ in range(repeats):
        started = time.monotonic()
        response = session.request(sql)
        elapsed = (time.monotonic() - started) * 1000
        if response.get("ok") is not True:
            raise RuntimeError(f"query failed: {response!r}")
        best = elapsed if best is None else min(best, elapsed)
    return best


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--rows", type=int, default=10000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--protocol-dir", type=Path, default=harness.DEFAULT_PROTOCOL)
    parser.add_argument("--data-root", type=Path, default=harness.DEFAULT_DATA_ROOT)
    args = parser.parse_args(argv)

    strict_json, _ = harness.load_official_modules(args.protocol_dir)
    config = strict_json((args.protocol_dir / "config.json").read_text(encoding="utf-8"))
    config = dict(config, request_timeout=600)

    args.data_root.mkdir(parents=True, exist_ok=True)
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-probe-", dir=args.data_root))
    rng = random.Random(5)
    session = harness.Session([str(Path(args.exe).resolve())], data_dir, config,
                              strict_json, args.data_root / "probe.stderr.log")
    try:
        session.start()
        session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, g BIGINT, "
                        f"embedding VECTOR({args.dim}));")
        batch = 500
        for start in range(1, args.rows + 1, batch):
            values = []
            for i in range(start, min(start + batch, args.rows + 1)):
                values.append(f"({i}, {i % 4}, '{build_vector(rng, args.dim)}')")
            session.request("INSERT INTO docs VALUES " + ", ".join(values) + ";")

        probe = build_vector(rng, args.dim)
        shapes = {
            "A scan+filter, order by pk":  "SELECT id FROM docs WHERE g = 2 ORDER BY id LIMIT 10;",
            "B + project id (no distance)": "SELECT id, embedding FROM docs WHERE g = 2 ORDER BY id LIMIT 10;",
            "C + L2 distance + TopK":       f"SELECT id, L2_DISTANCE(embedding, '{probe}') AS distance "
                                            f"FROM docs WHERE g = 2 ORDER BY distance LIMIT 10;",
        }
        print(f"rows={args.rows} dim={args.dim}")
        for label, sql in shapes.items():
            print(f"  {label:32s} {timed(session, sql):7.2f} ms")

        session.shutdown()
    finally:
        session.stop()
        shutil.rmtree(data_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
