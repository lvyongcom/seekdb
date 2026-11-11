#!/usr/bin/env bash
# AddressSanitizer + UndefinedBehaviorSanitizer run of every test we have.
#
# The point is the intermittent `database exited before response` seen once
# during the M5 switch: a sanitizer turns memory bugs into deterministic
# reports instead of rare crashes.
#
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_asan.sh [runs]
set -uo pipefail

RUNS=${1:-2}
SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"

mkdir -p "$DST/minivectordb" || exit 1
cp -a "$SRC/minivectordb/." "$DST/minivectordb/" || exit 1
rm -rf "$DST/minivectordb/build"
cd "$DST/minivectordb" || exit 1

export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0

echo "=== sanitizer build ==="
make CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer" \
     LDFLAGS="-fsanitize=address,undefined" > /tmp/mvdb-asan-build.log 2>&1
if [ $? -ne 0 ]; then
  tail -25 /tmp/mvdb-asan-build.log
  exit 1
fi
echo "ok"

failures=0
for run in $(seq 1 "$RUNS"); do
  echo
  echo "=== run $run/$RUNS: public cases ==="
  python3 tests/mvdb_harness.py --exe build/minivectordb 2>&1 | tail -3
  if [ -n "$(ls -t protocol-results | head -1)" ]; then
    log_dir=$(ls -td protocol-results/*/ | head -1)
    if grep -l -E "AddressSanitizer|runtime error|SUMMARY" "$log_dir"*.log >/dev/null 2>&1; then
      echo "SANITIZER REPORT FOUND:"
      grep -h -A6 -E "AddressSanitizer|runtime error|SUMMARY" "$log_dir"*.log | head -40
      failures=$((failures + 1))
    fi
  fi

  echo "=== run $run/$RUNS: crash recovery ==="
  python3 tests/crash_test.py --exe build/minivectordb 2>&1 | tail -2
  if grep -r -l -E "AddressSanitizer|runtime error" protocol-results/crash/*.log >/dev/null 2>&1; then
    echo "SANITIZER REPORT FOUND (crash test):"
    grep -h -A6 -E "AddressSanitizer|runtime error" protocol-results/crash/*.log | head -40
    failures=$((failures + 1))
  fi
done

echo
if [ "$failures" -eq 0 ]; then
  echo "sanitizers: CLEAN"
else
  echo "sanitizers: $failures reports above"
  exit 1
fi
