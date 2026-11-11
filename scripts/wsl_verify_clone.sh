#!/usr/bin/env bash
# Build and grade the *pushed* commit, using a clone that was taken from GitHub.
#
# The clone is made on Windows because that is where the local git proxy lives;
# WSL only builds from the mounted path. Cloning the fork inside WSL fails: the
# 302 proxy listens on the Windows loopback, which WSL's NAT network cannot see.
set -uo pipefail

CLONE=${1:-/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/verify-clone}
TAG=minivectordb:verify
cd "$CLONE" || exit 1

echo "=== commit under test ==="
git log -1 --format='%H %s'

echo
echo "=== docker build from the clone ==="
if ! docker build -t "$TAG" . > /tmp/mvdb-verify-build.log 2>&1; then
  echo "BUILD FAILED -- last 30 lines:"
  tail -30 /tmp/mvdb-verify-build.log
  exit 1
fi
tail -3 /tmp/mvdb-verify-build.log

STAMP=$(date +%Y%m%d-%H%M%S)
echo
echo "=== official driver: handshake ==="
python3 protocol_check/run.py --image "$TAG" --handshake-only \
  --output "verify-results/handshake-$STAMP" 2>&1 | tail -3

echo
echo "=== official driver: all public cases ==="
python3 protocol_check/run.py --image "$TAG" \
  --output "verify-results/public-$STAMP" 2>&1 | tail -3

echo
python3 - "$CLONE/verify-results/public-$STAMP/result.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
cases = data.get("cases", [])
passed = [c for c in cases if c.get("status") == "PASS"]
print(f"clone verification: passed={data.get('passed')} pass={len(passed)}/{len(cases)}")
sys.exit(0 if data.get("passed") else 1)
PY
