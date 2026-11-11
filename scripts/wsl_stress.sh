#!/usr/bin/env bash
# Long stress loop over the full public suite, hunting the intermittent death.
#
# Every occurrence so far predates the crash handlers, so there has never been a
# failure *with* diagnostics attached. The point of this loop is to produce one.
#
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_stress.sh [runs]
set -uo pipefail

RUNS=${1:-40}
SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"

mkdir -p "$DST/minivectordb" || exit 1
cp -a "$SRC/minivectordb/." "$DST/minivectordb/" || exit 1
rm -rf "$DST/minivectordb/build"
cd "$DST/minivectordb" || exit 1

echo "=== build (-O2, crash handlers on) ==="
make -j"$(nproc)" > /tmp/mvdb-stress-build.log 2>&1 || { tail -20 /tmp/mvdb-stress-build.log; exit 1; }
echo "ok"

for run in $(seq 1 "$RUNS"); do
  out=$(python3 tests/mvdb_harness.py --exe build/minivectordb 2>&1)
  if ! echo "$out" | grep -q "20/20 passed"; then
    echo
    echo "!!! FAILURE on run $run"
    echo "$out" | tail -6
    latest=$(ls -td protocol-results/*/ | head -1)
    echo "--- result ---"
    python3 "$DST/scripts/summarize_results.py" "${latest}result.json" 2>/dev/null | head -6
    echo "--- logs mentioning FATAL ---"
    grep -l "FATAL" "$latest"*.log 2>/dev/null || echo "(no fatal line)"
    for f in "$latest"*.log; do
      if grep -q "FATAL" "$f" 2>/dev/null; then
        echo "== $f"
        grep -A2 "FATAL" "$f"
      fi
    done
    exit 1
  fi
  if [ $((run % 5)) -eq 0 ]; then echo "  $run/$RUNS clean"; fi
done

echo
echo "no failure in $RUNS runs"
