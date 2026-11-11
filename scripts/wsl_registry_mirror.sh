#!/usr/bin/env bash
# Pick a reachable Docker Hub registry mirror and configure the daemon.
#
# Run as root from Windows:
#   wsl -d Ubuntu -u root -- bash /mnt/e/.../scripts/wsl_registry_mirror.sh
#
# Docker Hub is not directly reachable from mainland China, and the public
# mirrors come and go, so this probes each candidate instead of hardcoding one.
set -uo pipefail

CANDIDATES=(
  "https://docker.m.daocloud.io"
  "https://docker.1ms.run"
  "https://docker.xuanyuan.me"
  "https://hub.rat.dev"
  "https://docker.1panel.live"
  "https://dockerproxy.net"
  "https://dockerhub.icu"
  "https://docker.rainbond.cc"
)

echo "=== probing mirrors (expect 200 or 401 for a live registry) ==="
WORKING=()
for mirror in "${CANDIDATES[@]}"; do
  code=$(curl -s -o /dev/null -m 8 -w '%{http_code}' "$mirror/v2/" 2>/dev/null || echo "000")
  printf '  %-36s %s\n' "$mirror" "$code"
  if [ "$code" = "200" ] || [ "$code" = "401" ] || [ "$code" = "404" ]; then
    WORKING+=("$mirror")
  fi
done

if [ ${#WORKING[@]} -eq 0 ]; then
  echo
  echo "No reachable mirror. All candidates timed out or were rejected."
  exit 1
fi

echo
echo "=== writing /etc/docker/daemon.json ==="
mkdir -p /etc/docker

{
  echo '{'
  echo '  "registry-mirrors": ['
  count=${#WORKING[@]}
  index=0
  for mirror in "${WORKING[@]}"; do
    index=$((index + 1))
    if [ "$index" -lt "$count" ]; then
      echo "    \"$mirror\","
    else
      echo "    \"$mirror\""
    fi
  done
  echo '  ]'
  echo '}'
} > /etc/docker/daemon.json
cat /etc/docker/daemon.json

echo
echo "=== restarting docker ==="
if command -v systemctl >/dev/null 2>&1 && systemctl is-system-running >/dev/null 2>&1; then
  systemctl restart docker
else
  service docker restart
fi
sleep 3

echo
echo "=== effective mirrors ==="
docker info 2>/dev/null | grep -A8 'Registry Mirrors' || echo "(none reported)"

echo
echo "=== pulling ubuntu:24.04 ==="
if docker pull ubuntu:24.04; then
  echo "PULL OK"
else
  echo "PULL FAILED"
  exit 1
fi
