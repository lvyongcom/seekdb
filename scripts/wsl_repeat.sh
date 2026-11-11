#!/usr/bin/env bash
# Repeats the public suite to expose intermittent failures.
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_repeat.sh [runs]
set -uo pipefail

RUNS=${1:-3}
cd "$HOME/work/seekdb-round2/minivectordb" || exit 1

for run in $(seq 1 "$RUNS"); do
  echo "--- run $run/$RUNS ---"
  python3 tests/mvdb_harness.py --exe build/minivectordb 2>&1 | tail -2
done
