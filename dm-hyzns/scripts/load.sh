#!/bin/bash
#
# Reload dm-hyzns.ko (rmmod if loaded, then insmod the freshly built one).
# Usage:
#   sudo ./scripts/load.sh
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -f dm-hyzns.ko ]]; then
    echo "ERROR: dm-hyzns.ko not built yet. Run scripts/build.sh first."
    exit 1
fi

if lsmod | awk '{print $1}' | grep -qx dm_hyzns; then
    echo "==> rmmod dm_hyzns"
    rmmod dm_hyzns
fi

echo "==> insmod dm-hyzns.ko"
insmod dm-hyzns.ko

echo "==> dmsetup targets | grep hyzns"
dmsetup targets | grep hyzns || true
