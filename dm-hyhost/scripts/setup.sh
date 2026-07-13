#!/bin/bash
#
# Create a hyhost dm target on top of the FEMU HYHOSTSSD device.
# Usage:
#   sudo ./scripts/setup.sh [BACKING] [R_END_LBA] [NAME]
#
# Defaults:
#   BACKING    = /dev/nvme0n1
#   R_END_LBA  = entire backing device, in 512B sectors. This is the
#                L2P capacity cap; the R/S boundary is runtime-movable
#                anywhere in [0, R_END_LBA] via `dmsetup message ...
#                set_r_end <lba>` or F2FS ioctl #28.
#   NAME       = hyhost0
#
# After this you'll have /dev/mapper/${NAME} ready for fio / mkfs / RocksDB.
set -euo pipefail

BACKING="${1:-/dev/nvme0n1}"
NAME="${3:-hyhost0}"

if [[ ! -b "${BACKING}" ]]; then
    echo "ERROR: ${BACKING} is not a block device"
    exit 1
fi

if ! lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    echo "ERROR: dm_hyhost not loaded. Run scripts/load.sh first."
    exit 1
fi

# Backing size in 512B sectors.
SIZE="$(blockdev --getsz "${BACKING}")"
R_END="${2:-${SIZE}}"

echo "==> backing=${BACKING} size_sectors=${SIZE} r_end_lba=${R_END} name=${NAME}"

if dmsetup info "${NAME}" >/dev/null 2>&1; then
    echo "==> existing target ${NAME} found, removing"
    dmsetup remove "${NAME}"
fi

echo "0 ${SIZE} hyhost ${BACKING} ${R_END}" | dmsetup create "${NAME}"

echo "==> dmsetup table ${NAME}"
dmsetup table "${NAME}"
echo "==> dmsetup status ${NAME}"
dmsetup status "${NAME}"
echo "==> /dev/mapper/${NAME} ready"
ls -l "/dev/mapper/${NAME}"
