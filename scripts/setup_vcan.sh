#!/usr/bin/env bash
# Create a virtual CAN interface for SocketCAN tests. Linux only.
set -euo pipefail

IFACE="${1:-vcan0}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "setup_vcan.sh: SocketCAN requires Linux; this host is $(uname -s)" >&2
  echo "run the replay-source tests instead: ./build/test_signals" >&2
  exit 1
fi

if ! command -v ip >/dev/null 2>&1; then
  echo "setup_vcan.sh: 'ip' from iproute2 is required" >&2
  exit 1
fi

sudo modprobe vcan
if ip link show "${IFACE}" >/dev/null 2>&1; then
  echo "${IFACE} already exists"
else
  sudo ip link add dev "${IFACE}" type vcan
fi
sudo ip link set up "${IFACE}"
echo "${IFACE} is up"
ip -details link show "${IFACE}"
