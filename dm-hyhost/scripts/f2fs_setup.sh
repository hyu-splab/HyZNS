#!/bin/bash
#
# Bring up F2FS on top of a dm-hyhost target sized to its R-region.
# This exercises the dm-hyhost path with a real filesystem; no ZenFS,
# no zoned namespace, no S-region access.
#
# Usage:
#   sudo ./scripts/f2fs_setup.sh [BACKING] [R_END_LBA] [MNT]
#
# Defaults: BACKING=/dev/nvme0n1  R_END_LBA=16777216 (8 GiB)  MNT=/mnt/hyhost
#
# What it does:
#   1. (re)load dm_hyhost
#   2. create dm target with size == R_END_LBA so only the R-region is
#      exposed to F2FS (S-region is not touched by this target)
#   3. sync the device's r_end to match (single dmsetup message)
#   4. mkfs.f2fs + mount on MNT
#
# Tear down with scripts/f2fs_teardown.sh.
set -euo pipefail

BACKING="${1:-/dev/nvme0n1}"
R_END="${2:-16777216}"
MNT="${3:-/mnt/hyhost}"
NAME=hyhost0

if [[ ! -b "${BACKING}" ]]; then
    echo "ERROR: ${BACKING} is not a block device" >&2
    exit 1
fi
if (( R_END % 8 != 0 )); then
    echo "ERROR: R_END_LBA must be page-aligned (multiple of 8)" >&2
    exit 1
fi

cd "$(dirname "$0")/.."

if ! lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    ./scripts/load.sh
fi

echo "==> create dm target ${NAME} (size=${R_END}, r_end=${R_END})"
if dmsetup info "${NAME}" >/dev/null 2>&1; then
    dmsetup remove "${NAME}"
fi
echo "0 ${R_END} hyhost ${BACKING} ${R_END}" | dmsetup create "${NAME}"

echo "==> sync device r_end to ${R_END}"
dmsetup message "${NAME}" 0 set_r_end "${R_END}"

echo "==> mkfs.f2fs"
mkfs.f2fs -f "/dev/mapper/${NAME}" >/dev/null

echo "==> mount on ${MNT}"
mkdir -p "${MNT}"
mount -t f2fs "/dev/mapper/${NAME}" "${MNT}"

echo "==> ready"
df -h "${MNT}"
dmsetup status "${NAME}"
