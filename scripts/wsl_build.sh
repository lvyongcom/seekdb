#!/usr/bin/env bash
# Full Linux verification: native build, local driver, crash recovery, then the
# submission image with the official judge driver.
#
# Run from Windows with:
#   wsl -d Ubuntu -- bash /mnt/e/.../seekdb-round2/scripts/wsl_build.sh
#
# Sources are staged into the Linux filesystem first: building from /mnt/e
# (9p) is several times slower, and this project is I/O sensitive.
set -uo pipefail

SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"

echo "=== staging sources into the Linux filesystem ==="
rm -rf "$DST"
mkdir -p "$DST" || exit 1
cp -a "$SRC/minivectordb" "$SRC/mysqltest" "$SRC/protocol_check" "$SRC/scripts" "$DST/" || exit 1
cp -a "$SRC/Dockerfile" "$SRC/.dockerignore" "$SRC/STDIO_PROTOCOL.md" "$DST/" || exit 1
# Host build artifacts must never reach the container build.
rm -rf "$DST/minivectordb/build" "$DST/minivectordb/protocol-results" \
       "$DST/minivectordb/.tmpdata"
cd "$DST/minivectordb" || exit 1

echo
echo "=== native build (Linux) ==="
if ! make -j"$(nproc)" > /tmp/mvdb-make.log 2>&1; then
  echo "NATIVE BUILD FAILED -- last 30 lines:"
  tail -30 /tmp/mvdb-make.log
  exit 1
fi
echo "ok ($(ls -lh build/minivectordb | awk '{print $5}') binary)"

echo
echo "=== crash handler self-test ==="
cat > /tmp/mvdb-crash-selftest.cpp <<'EOF'
#include <csignal>
#include "platform/crash.h"

int main() {
  mvdb::InstallCrashHandlers();
  mvdb::SetStage("self test stage");
  std::raise(SIGSEGV);
  return 0;
}
EOF
g++ -std=c++17 -O1 -g -Isrc -o /tmp/mvdb-crash-selftest \
  /tmp/mvdb-crash-selftest.cpp src/platform/crash.cpp 2>/tmp/mvdb-selftest-build.log
if [ $? -ne 0 ]; then
  echo "SELFTEST BUILD FAILED"
  tail -10 /tmp/mvdb-selftest-build.log
else
  /tmp/mvdb-crash-selftest 2>&1 | tail -2
fi

echo
echo "=== public cases ==="
python3 tests/mvdb_harness.py --exe build/minivectordb 2>&1 | tail -3

echo
echo "=== crash recovery (Linux) ==="
python3 tests/crash_test.py --exe build/minivectordb 2>&1 | tail -4

echo
echo "=== response size guard ==="
python3 tests/limit_test.py --exe build/minivectordb 2>&1 | tail -4

cd "$DST" || exit 1

echo
echo "=== docker build ==="
if ! docker build -t minivectordb:dev . > /tmp/mvdb-build.log 2>&1; then
  echo "BUILD FAILED -- last 40 lines:"
  tail -40 /tmp/mvdb-build.log
  exit 1
fi
tail -4 /tmp/mvdb-build.log

STAMP=$(date +%Y%m%d-%H%M%S)

echo
echo "=== official driver: handshake ==="
python3 protocol_check/run.py --image minivectordb:dev --handshake-only \
  --output "protocol-results/handshake-$STAMP" 2>&1 | tail -4

echo
echo "=== official driver: all public cases ==="
python3 protocol_check/run.py --image minivectordb:dev \
  --output "protocol-results/public-$STAMP" 2>&1 | tail -4

echo
python3 scripts/summarize_results.py "protocol-results/public-$STAMP/result.json" 2>&1 | head -3

echo
echo "=== image ==="
docker images minivectordb:dev --format '{{.Repository}}:{{.Tag}}  {{.Size}}'
