#!/usr/bin/env bash
# Hunt the intermittent crash in storage_mutations under ASan.
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_hunt.sh [runs]
set -uo pipefail

RUNS=${1:-40}
SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"

mkdir -p "$DST/minivectordb" || exit 1
cp -a "$SRC/minivectordb/." "$DST/minivectordb/" || exit 1
rm -rf "$DST/minivectordb/build"
cd "$DST/minivectordb" || exit 1

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:print_stacktrace=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0

echo "=== sanitizer build ==="
make CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer" \
     LDFLAGS="-fsanitize=address,undefined" > /tmp/mvdb-hunt-build.log 2>&1
if [ $? -ne 0 ]; then
  tail -25 /tmp/mvdb-hunt-build.log
  exit 1
fi
echo "ok"

for run in $(seq 1 "$RUNS"); do
  out=$(python3 tests/mvdb_harness.py --exe build/minivectordb --case storage_mutations 2>&1)
  if ! echo "$out" | grep -q "1/1 passed"; then
    echo
    echo "!!! FAILED on run $run"
    echo "$out" | tail -5
    latest=$(ls -td protocol-results/*/ | head -1)
    echo "--- stderr ---"
    cat "$latest/storage_mutations.stderr.log" 2>/dev/null | tail -60
    exit 1
  fi
  if [ $((run % 10)) -eq 0 ]; then echo "  $run/$RUNS clean"; fi
done

echo
echo "no failure in $RUNS runs"
