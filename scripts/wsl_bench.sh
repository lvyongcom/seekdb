#!/usr/bin/env bash
# Linux-side verification + scaling measurement, without rebuilding the image.
# Run from Windows with:
#   wsl -d Ubuntu -- bash /mnt/e/.../seekdb-round2/scripts/wsl_bench.sh
set -uo pipefail

SCRIPT_DIR=$(dirname "$0")
ROWS=${1:-200000}
DIM=${2:-64}

SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"

mkdir -p "$DST/minivectordb" || exit 1
cp -a "$SRC/minivectordb/." "$DST/minivectordb/" || exit 1
cp -a "$SRC/scripts/." "$DST/scripts/" 2>/dev/null || true
cp -a "$SRC/mysqltest" "$SRC/protocol_check" "$DST/" 2>/dev/null || true
rm -rf "$DST/minivectordb/build" "$DST/minivectordb/protocol-results" \
       "$DST/minivectordb/.tmpdata"

cd "$DST/minivectordb" || exit 1
echo "=== native build ==="
make -j"$(nproc)" > /tmp/mvdb-make.log 2>&1 || { tail -25 /tmp/mvdb-make.log; exit 1; }
echo "ok"

echo
echo "=== public cases ==="
python3 tests/mvdb_harness.py --exe build/minivectordb 2>&1 | tail -3

echo
echo "=== crash recovery ==="
python3 tests/crash_test.py --exe build/minivectordb 2>&1 | tail -2

echo
echo "=== scaling: ${ROWS} rows x ${DIM} dims ==="
python3 tests/large_test.py --exe build/minivectordb --rows "$ROWS" --dim "$DIM" 2>&1 | tail -9
