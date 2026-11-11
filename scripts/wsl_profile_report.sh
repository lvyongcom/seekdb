#!/usr/bin/env bash
# Print the flat profile from the last profiling run.
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_profile_report.sh
set -uo pipefail
cd "$HOME/work/seekdb-round2/minivectordb" || exit 1

if [ ! -f gmon.out ]; then
  echo "no gmon.out; run wsl_profile.sh first"
  exit 1
fi

echo "=== elapsed ==="
tail -2 /tmp/mvdb-prof-time.txt 2>/dev/null

echo
echo "=== flat profile: top 30 by self time ==="
gprof -b -p ./build/minivectordb gmon.out 2>/dev/null | sed -n '1,36p'

echo
echo "=== where Select's time goes (children) ==="
gprof -b -q ./build/minivectordb gmon.out 2>/dev/null \
  | awk '/^\[[0-9]+\].*Engine::Select\(/{flag=1} flag{print} flag&&/^-----/{exit}' \
  | cut -c1-160 | head -16

echo
echo "=== where Scan's time goes (children) ==="
gprof -b -q ./build/minivectordb gmon.out 2>/dev/null \
  | awk '/^\[[0-9]+\].*TableStore::Scan\(/{flag=1} flag{print} flag&&/^-----/{exit}' \
  | cut -c1-160 | head -16
