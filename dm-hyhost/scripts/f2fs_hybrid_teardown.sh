#!/bin/bash
#
# Tear down the F2FS hybrid mount created by f2fs_hybrid_setup.sh.
#
# Usage:
#   sudo ./scripts/f2fs_hybrid_teardown.sh [MNT]
#
# Default: MNT=/mnt/hyhost
set -euo pipefail

BACKING=/dev/nvme0n1
NAME=hyhost0
MNT="${1:-/mnt/hyhost}"

source "$(dirname "$0")/_lib.sh"

if mountpoint -q "${MNT}"; then
    echo "==> umount ${MNT}"
    umount "${MNT}"
fi

if dmsetup info "${NAME}" >/dev/null 2>&1; then
    echo "==> dmsetup remove ${NAME}"
    dmsetup remove "${NAME}"
fi

# Return the device boundary to full-R so the next setup is clean.
echo "==> reset device boundary to full-R"
reset_device_full_r "${BACKING}" || true

echo "==> done"
