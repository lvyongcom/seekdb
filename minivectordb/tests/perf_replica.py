#!/usr/bin/env python3
"""Replica of the described performance benchmark, for evidence gathering.

Section 3.3 of the task: 10,000 records, 128 dimensions, TopK with K=10,
1,000 queries of which half carry an equality filter on an ordinary column,
writes in batches of 100 that commit each batch.

Reports wall time for each phase, peak RSS of the database process, and
correctness of every returned row set (count, uniqueness, ascending distance,
and the distance values themselves). Recall is not computed because retrieval is
an exact scan; the returned distances are checked against a recomputation
instead, which would catch an incorrect result set.

    python tests/perf_replica.py --exe build/minivectordb
"""

import argparse
import random
import shutil
import struct
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mvdb_harness as harness  # noqa: E402

DIM = 128
ROWS = 10000
BATCH = 100
QUERIES = 1000
K = 10
CATEGORIES = ["alpha", "beta", "gamma", "delta"]


def peak_rss_kb(pid):
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    except OSError:
        return None
    return None


class Sampler:
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


def as_float32(value):
    """Round to float32, which is how the database stores and compares vectors.

    The task says distances are computed on the *stored* vector, so the truth
    used here must be the float32 value, not the double the generator drew.
    """
    return struct.unpack("f", struct.pack("f", value))[0]


def vector_text(values):
    # 9 significant digits round-trip a float32 exactly.
    return "[" + ",".join(f"{v:.9g}" for v in values) + "]"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", help="path to a locally built binary")
    parser.add_argument("--container", metavar="IMAGE",
                        help="run this image with the judge's docker flags instead")
    parser.add_argument("--protocol-dir", type=Path, default=harness.DEFAULT_PROTOCOL)
    parser.add_argument("--data-root", type=Path, default=harness.DEFAULT_DATA_ROOT)
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--truncate-lines", action="store_true",
                        help="cap the per-query delay measurement noise")
    args = parser.parse_args(argv)
    if not args.exe and not args.container:
        parser.error("one of --exe or --container is required")

    if args.container:
        # Same isolation the judge uses, so the numbers describe the submission
        # image rather than a native build.
        target = ["docker", "run", "--rm", "-i", "--network=none", "--cpus=2",
                  "--memory=2g", "--memory-swap=2g", "--pids-limit=128",
                  "--cap-drop=ALL", "--security-opt", "no-new-privileges",
                  "--read-only", "-v", "{data_dir}:/data", args.container]
    else:
        target = [str(Path(args.exe).resolve())]

    strict_json, _ = harness.load_official_modules(args.protocol_dir)
    config = strict_json((args.protocol_dir / "config.json").read_text(encoding="utf-8"))
    config = dict(config, request_timeout=600, case_timeout=7200)

    output = args.output or (harness.ARTIFACT_ROOT / "protocol-results" / "perf")
    output.mkdir(parents=True, exist_ok=True)
    args.data_root.mkdir(parents=True, exist_ok=True)
    data_dir = Path(tempfile.mkdtemp(prefix="mvdb-perf-", dir=args.data_root))

    rng = random.Random(2024)
    rows = [(i + 1, CATEGORIES[(i + 1) % len(CATEGORIES)],
             [as_float32(rng.uniform(-1.0, 1.0)) for _ in range(DIM)]) for i in range(ROWS)]
    by_id = {row[0]: row[2] for row in rows}
    queries = []
    for q in range(QUERIES):
        probe = [as_float32(rng.uniform(-1.0, 1.0)) for _ in range(DIM)]
        if q % 2 == 0:
            category = CATEGORIES[q % len(CATEGORIES)]
            queries.append((probe, category))
        else:
            queries.append((probe, None))

    session = harness.Session(target, data_dir, config, strict_json,
                              output / "perf.stderr.log")
    failures = []
    try:
        with Sampler(session) as sampler:
            session.start()
            session.request(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, "
                            f"category VARCHAR(16), embedding VECTOR({DIM}));")

            # ---- write phase: batches of 100, each its own committed statement
            write_start = time.monotonic()
            for start in range(0, ROWS, BATCH):
                chunk = rows[start:start + BATCH]
                values = ", ".join(
                    f"({rid}, '{cat}', '{vector_text(vec)}')" for rid, cat, vec in chunk)
                response = session.request("INSERT INTO docs VALUES " + values + ";")
                if response.get("affected_rows") != len(chunk):
                    failures.append(f"batch {start // BATCH}: affected_rows "
                                    f"{response.get('affected_rows')} != {len(chunk)}")
                    break
            write_seconds = time.monotonic() - write_start
            print(f"write phase: {ROWS} rows in {BATCH}-row batches -> "
                  f"{write_seconds:.3f} s ({ROWS / write_seconds:,.0f} rows/s)", flush=True)

            # ---- query phase
            query_start = time.monotonic()
            worst_ms = 0.0
            for probe, category in queries:
                sql = (f"SELECT id, L2_DISTANCE(embedding, '{vector_text(probe)}') AS distance "
                       f"FROM docs ")
                if category is not None:
                    sql += f"WHERE category = '{category}' "
                sql += f"ORDER BY distance LIMIT {K};"
                began = time.monotonic()
                response = session.request(sql)
                elapsed_ms = (time.monotonic() - began) * 1000
                worst_ms = max(worst_ms, elapsed_ms)

                returned = response.get("rows", [])
                candidates = [r for r in rows if category is None or r[1] == category]
                expected_count = min(K, len(candidates))
                if len(returned) != expected_count:
                    failures.append(f"query returned {len(returned)} rows, "
                                    f"expected {expected_count}")
                ids = [row[0] for row in returned]
                if len(set(ids)) != len(ids):
                    failures.append("duplicate ids in a result")
                distances = [row[1] for row in returned]
                if distances != sorted(distances):
                    failures.append("distances are not ascending")
                for rid, distance in returned:
                    # Accumulate in double, like the engine, over float32 inputs.
                    truth = sum((a - b) ** 2 for a, b in zip(probe, by_id[rid])) ** 0.5
                    tolerance = max(1e-5 * max(abs(truth), abs(distance)), 1e-5)
                    if abs(truth - distance) > tolerance:
                        failures.append(f"id {rid}: distance {distance} != {truth}")
            query_seconds = time.monotonic() - query_start
            print(f"query phase: {QUERIES} TopK({K}) queries -> {query_seconds:.3f} s "
                  f"({query_seconds * 1000 / QUERIES:.2f} ms/query, "
                  f"slowest {worst_ms:.2f} ms)", flush=True)

            session.shutdown()

        if sampler.peak is not None:
            print(f"peak RSS: {sampler.peak / 1024:.1f} MiB", flush=True)
    except Exception as exc:  # noqa: BLE001
        failures.append(f"{type(exc).__name__}: {exc}")
    finally:
        session.stop()
        shutil.rmtree(data_dir, ignore_errors=True)

    if failures:
        print("perf replica: FAIL")
        for failure in failures[:10]:
            print(f"  - {failure}")
        return 1
    print("perf replica: PASS (all result sets correct)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
