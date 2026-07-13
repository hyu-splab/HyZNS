#!/bin/bash
#
# Reload dm-hyhost.ko (rmmod if loaded, then insmod the freshly built one).
# Usage:
#   sudo ./scripts/load.sh
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -f dm-hyhost.ko ]]; then
    echo "ERROR: dm-hyhost.ko not built yet. Run scripts/build.sh first."
    exit 1
fi

if lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    echo "==> rmmod dm_hyhost"
    rmmod dm_hyhost
fi

echo "==> insmod dm-hyhost.ko"
insmod dm-hyhost.ko

echo "==> dmsetup targets | grep hyhost"
dmsetup targets | grep hyhost || true
