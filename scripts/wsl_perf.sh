#!/usr/bin/env bash
# Reproduce the performance benchmark described in section 3.3 and report our
# own numbers, so a platform-side failure can be argued with evidence.
#
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_perf.sh
set -uo pipefail

SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"

mkdir -p "$DST/minivectordb" || exit 1
cp -a "$SRC/minivectordb/." "$DST/minivectordb/" || exit 1
cp -a "$SRC/scripts/." "$DST/scripts/" 2>/dev/null || true
cp -a "$SRC/mysqltest" "$SRC/protocol_check" "$DST/" 2>/dev/null || true
rm -rf "$DST/minivectordb/build" "$DST/minivectordb/protocol-results" \
       "$DST/minivectordb/.tmpdata"
cd "$DST/minivectordb" || exit 1

echo "=== build ==="
make -j"$(nproc)" > /tmp/mvdb-perf-build.log 2>&1 || { tail -20 /tmp/mvdb-perf-build.log; exit 1; }
echo "ok"

echo
echo "=== performance benchmark replica, native (10k x 128, K=10, 1000 queries) ==="
/usr/bin/time -f "  harness wall clock %e s" \
  python3 tests/perf_replica.py --exe build/minivectordb 2>&1 | tail -10

echo
echo "=== same workload inside the submission image (2 CPU / 2 GiB / read-only) ==="
docker build -t minivectordb:perf . > /tmp/mvdb-perf-docker.log 2>&1
if [ $? -ne 0 ]; then
  echo "image build failed"; tail -15 /tmp/mvdb-perf-docker.log; exit 1
fi
/usr/bin/time -f "  harness wall clock %e s" \
  python3 tests/perf_replica.py --container minivectordb:perf 2>&1 | tail -10
