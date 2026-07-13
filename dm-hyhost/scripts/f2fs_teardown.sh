#!/bin/bash
#
# Tear down what scripts/f2fs_setup.sh brought up.
#
# Usage:
#   sudo ./scripts/f2fs_teardown.sh [MNT]
#
# Default MNT=/mnt/hyhost.
set -euo pipefail

MNT="${1:-/mnt/hyhost}"
NAME=hyhost0

if mountpoint -q "${MNT}"; then
    umount "${MNT}"
fi
if dmsetup info "${NAME}" >/dev/null 2>&1; then
    dmsetup remove "${NAME}"
fi
if lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    rmmod dm_hyhost
fi
echo "torn down"
