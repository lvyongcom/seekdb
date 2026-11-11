#!/usr/bin/env python3
"""Large-data scaling check for the over-memory requirement.

The task description states that formal cases include a data set larger than the
memory budget, so the database must not depend on holding every row in RAM.
This script loads a configurable number of rows and reports:

  * wall-clock time to load and query
  * size of the data directory
  * peak resident memory of the database process (Linux only, via VmHWM)

Run against a locally built binary:

    python tests/large_test.py --exe build/minivectordb.exe --rows 200000
"""

import argparse
import json
import os
import random
import shutil
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mvdb_harness as harness  # noqa: E402

CATEGORIES = ["alpha", "beta", "gamma", "delta"]


def peak_rss_kb(pid):
    """VmHWM from /proc; None where unsupported (Windows)."""
    path = Path(f"/proc/{pid}/status")
    try:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    except OSError:
        return None
    return None


class RssSampler:
    """Samples the child's high-water mark while the test runs."""

    def __init__(self, session):
        self.session = session
        self.peak = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self.stop.is_set():
            proc = self.session.proc
            if proc is not None:
                value = peak_rss_kb(proc.pid)
                if value is not None and (self.peak is None or value > self.peak):
                    self.peak = value
            self.stop.wait(0.1)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(timeout=2)


def build_vector(rng, dimension):
    return "[" + ", ".join(f"{rng.uniform(-1.0, 1.0):.3f}" for _ in range(dimension)) + "]"


def load_rows(session, total, batch, dimension, rng):
    loaded = 0
    started = time.monotonic()
    while loaded < total:
        count = min(batch, total - loaded)
        values = []
        for k in range(count):
            identifier = loaded + k + 1
            group = identifier % len(CATEGORIES)
            values.append(f"({identifier}, {group}, '{build_vector(rng, dimension)}')")
        session.request("INSERT INTO docs VALUES " + ", ".join(values) + ";")
        loaded += count
        if loaded % (batch * 20) == 0:
            elapsed = time.monotonic() - started
            rate = loaded / elapsed if elapsed > 0 else 0
            print(f"  loaded {loaded}/{total}  ({rate:,.0f} rows/s)", flush=True)
    return time.monotonic() - started


def directory_size(path):
    return sum(item.stat().st_size for item in Path(path).rglob("*") if item.is_file())


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True)
    parser.add_argument("--rows", type=int, default=100000)
    parser.add_argument("--dim", type=int, default=32)
    parser.add_argument("--batch", type=int, default=500)
    parser.add_argument("--protocol-dir", type=Path, default=harness.DEFAULT_PROTOCOL)
    parser.add_argument("--data-root", type=Path, default=harness.DEFAULT_DATA_ROOT)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument("--seed", type=int, default=7)
    args = parser.parse_args(argv)

    strict_json, _ = harness.load_official_modules(args.protocol_dir)
    config = strict_json((args.protocol_dir / "config.json").read_text(encoding="utf-8"))
    # Loading this much data needs a longer per-request budget than a unit case.
    config = dict(config, request_timeout=600, case_timeout=3600)

    output = args.output or (harness.ARTIFACT_ROOT / "protocol-results" / "large")
    output.mkdir(parents=True, exist_ok=True)
    args.data_root.mkdir(parents=True, exist_ok=True)
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-large-", dir=args.data_root))

    rng = random.Random(args.seed)
    argv_target = [str(Path(args.exe).resolve())]
    session = harness.Session(argv_target, data_dir, config, strict_json,
                              output / "large.stderr.log")
    failures = []
    try:
        with RssSampler(session) as sampler:
            session.start()
            session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, g BIGINT, "
                            f"embedding VECTOR({args.dim}));")

            print(f"loading {args.rows:,} rows x {args.dim} dims ...", flush=True)
            load_seconds = load_rows(session, args.rows, args.batch, args.dim, rng)
            print(f"  load: {load_seconds:.1f}s", flush=True)

            probe = build_vector(rng, args.dim)
            query = (f"SELECT id, L2_DISTANCE(embedding, '{probe}') AS distance FROM docs "
                     f"WHERE g = 2 ORDER BY distance LIMIT 10;")
            started = time.monotonic()
            response = session.request(query)
            query_ms = (time.monotonic() - started) * 1000
            print(f"  filtered topk: {query_ms:.1f} ms", flush=True)

            rows = response.get("rows", [])
            if len(rows) != 10:
                failures.append(f"expected 10 rows, got {len(rows)}")
            distances = [row[1] for row in rows]
            if distances != sorted(distances):
                failures.append(f"distances are not ascending: {distances}")
            if len({row[0] for row in rows}) != len(rows):
                failures.append("duplicate ids in topk result")

            # Point lookup for a row that must exist, and one that must not.
            last = args.rows
            present = session.request(f"SELECT id FROM docs WHERE id = {last};")
            if present.get("rows") != [[last]]:
                failures.append(f"row {last} missing: {present.get('rows')!r}")
            absent = session.request(f"SELECT id FROM docs WHERE id = {last + 1};")
            if absent.get("rows") != []:
                failures.append(f"phantom row {last + 1}: {absent.get('rows')!r}")

            size = directory_size(data_dir)
            print(f"  data dir: {size / 1024 / 1024:.1f} MiB", flush=True)

            # Restart on the same data directory: recovery must be complete
            # before `ready`, and the data must be identical afterwards.
            session.shutdown()
            started = time.monotonic()
            session.start()
            startup_ms = (time.monotonic() - started) * 1000
            print(f"  restart+recovery: {startup_ms:.1f} ms", flush=True)

            after = session.request(f"SELECT id FROM docs WHERE id = {last};")
            if after.get("rows") != [[last]]:
                failures.append("row missing after restart")
            again = session.request(query)
            if [row[0] for row in again.get("rows", [])] != [row[0] for row in rows]:
                failures.append("topk result changed after restart")

            session.shutdown()

        if sampler.peak is not None:
            print(f"  peak rss: {sampler.peak / 1024:.1f} MiB", flush=True)
        else:
            print("  peak rss: n/a (not Linux)", flush=True)

    except Exception as exc:  # noqa: BLE001
        failures.append(f"{type(exc).__name__}: {exc}")
    finally:
        session.stop()
        if args.keep_data:
            print(f"data kept at {data_dir}")
        else:
            shutil.rmtree(data_dir, ignore_errors=True)

    if failures:
        print("large-data: FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"large-data: PASS ({args.rows:,} rows, {args.dim} dims)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
