#!/usr/bin/env bash
# Startup-environment matrix: prove the image starts in the shape the judge uses
# and that it fails loudly (not silently) when /data is not writable.
#
#   wsl -d Ubuntu -- bash /mnt/e/.../scripts/wsl_startup_check.sh
set -uo pipefail

SRC=/mnt/e/Users/LvYongkang/Documents/deepseek-harness/default-workspace/seekdb-round2
DST="$HOME/work/seekdb-round2"
TAG=minivectordb:startup
WORK=$(mktemp -d)

mkdir -p "$DST/minivectordb" || exit 1
cp -a "$SRC/minivectordb/." "$DST/minivectordb/" || exit 1
cp -a "$SRC/mysqltest" "$SRC/protocol_check" "$DST/" 2>/dev/null || true
cp -a "$SRC/Dockerfile" "$SRC/.dockerignore" "$DST/" || exit 1
rm -rf "$DST/minivectordb/build" "$DST/minivectordb/protocol-results" "$DST/minivectordb/.tmpdata"
cd "$DST" || exit 1

echo "=== build image ==="
if ! docker build -t "$TAG" . > /tmp/mvdb-startup-build.log 2>&1; then
  echo "BUILD FAILED"; tail -25 /tmp/mvdb-startup-build.log; exit 1
fi
echo "ok"

REQUEST='{"id":1,"op":"shutdown"}'

echo
echo "=== A: judge shape (volume at /data, read-only root, 2 CPU / 2 GiB) ==="
mkdir -p "$WORK/data-a" && chmod 777 "$WORK/data-a"
printf '%s\n' "$REQUEST" | docker run --rm -i --network=none --cpus=2 --memory=2g \
  --memory-swap=2g --pids-limit=128 --cap-drop=ALL \
  --security-opt no-new-privileges --read-only \
  -v "$WORK/data-a:/data" "$TAG" 2>"$WORK/a.err"
echo "  exit=$?"
echo "  stderr: $(tail -2 "$WORK/a.err" | tr '\n' ' | ')"

echo
echo "=== B: read-only root, NO volume (worst case) ==="
printf '%s\n' "$REQUEST" | docker run --rm -i --read-only "$TAG" \
  > "$WORK/b.out" 2>"$WORK/b.err"
echo "  exit=$?"
echo "  stdout: $(cat "$WORK/b.out" | tr '\n' ' ')"
echo "  stderr: $(tail -3 "$WORK/b.err" | tr '\n' ' | ')"

echo
echo "=== C: no explicit args (relies on CMD) ==="
mkdir -p "$WORK/data-c" && chmod 777 "$WORK/data-c"
printf '%s\n' "$REQUEST" | docker run --rm -i -v "$WORK/data-c:/data" "$TAG" \
  > "$WORK/c.out" 2>"$WORK/c.err"
echo "  exit=$?"
echo "  stdout: $(cat "$WORK/c.out" | tr '\n' ' ')"

echo
echo "=== D: read-only root + writable /tmp only (catches /tmp assumptions) ==="
mkdir -p "$WORK/data-d" && chmod 777 "$WORK/data-d"
printf '%s\n' "$REQUEST" | docker run --rm -i --read-only \
  --tmpfs /tmp -v "$WORK/data-d:/data" "$TAG" > "$WORK/d.out" 2>"$WORK/d.err"
echo "  exit=$?"
echo "  stdout: $(cat "$WORK/d.out" | tr '\n' ' ')"

rm -rf "$WORK"
echo
echo "done"
