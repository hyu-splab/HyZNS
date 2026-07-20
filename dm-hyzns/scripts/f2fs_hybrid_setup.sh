#!/bin/bash
#
# Single-mount F2FS on dm-hyzns (R+S hybrid, like HYSSD).
#
# Whole-device dm target + patched mkfs.f2fs (-H -B <num_rzone>) + mount.
# Requires the HYSSD-patched kernel and f2fs-tools: the kernel's F2FS
# patches query num_rzone via vendor opcode 0x21 (REPORT_RZONE) and split
# the device into a multi-device mount internally:
#     Device[0] = R-region treated as BLK_ZONED_NONE (conventional)
#     Device[1] = S-region as host-managed zoned
#
# Compare with f2fs_setup.sh, which mounts only the R-region as a flat
# conventional device.
#
# Usage:
#   sudo ./scripts/f2fs_hybrid_setup.sh [R_END_LBA] [MNT]
#
# Defaults:
#   R_END_LBA = half of backing device   (134217728 = 64 GiB on a 128 GiB device)
#   MNT       = /mnt/hyzns
#
# Tear down with scripts/f2fs_hybrid_teardown.sh.
set -euo pipefail

BACKING=/dev/nvme0n1
NAME=hyzns0

source "$(dirname "$0")/_lib.sh"

if [[ ! -b "${BACKING}" ]]; then
    echo "ERROR: ${BACKING} is not a block device" >&2; exit 1
fi

SIZE_SECTORS="$(blockdev --getsz "${BACKING}")"
R_END="${1:-$((SIZE_SECTORS / 2))}"
MNT="${2:-/mnt/hyzns}"

if (( R_END % HYZNS_ZONE_SECTORS != 0 )); then
    echo "ERROR: R_END_LBA must be zone-aligned (multiple of ${HYZNS_ZONE_SECTORS} sectors = 1 GiB)" >&2
    exit 1
fi
if (( R_END > SIZE_SECTORS )); then
    echo "ERROR: R_END_LBA (${R_END}) exceeds device size (${SIZE_SECTORS} sectors)." >&2
    echo "       R_END_LBA is in 512B SECTORS, not bytes (8 GiB = 16777216)." >&2
    exit 1
fi
NUM_RZONE=$((R_END / HYZNS_ZONE_SECTORS))

cd "$(dirname "$0")/.."

if ! lsmod | awk '{print $1}' | grep -qx dm_hyzns; then
    ./scripts/load.sh >/dev/null
fi

if dmsetup info "${NAME}" >/dev/null 2>&1; then
    dmsetup remove "${NAME}"
fi

# Boundary must start at full-R so setup can shrink it to the requested
# r_end; resets any non-EMPTY S-zones left by a previous run.
reset_device_full_r "${BACKING}" || true

echo "==> dm target ${NAME}: whole device (${SIZE_SECTORS} sectors), r_end=${R_END} (= ${NUM_RZONE} R-zones)"
echo "0 ${SIZE_SECTORS} hyzns ${BACKING} ${R_END}" | dmsetup create "${NAME}"

echo "==> sync device r_end to ${R_END}"
dmsetup message "${NAME}" 0 set_r_end "${R_END}"

echo "==> mkfs.f2fs -f -m -H -B ${NUM_RZONE} /dev/mapper/${NAME}"
mkfs.f2fs -f -m -H -B "${NUM_RZONE}" "/dev/mapper/${NAME}" >/dev/null

mkdir -p "${MNT}"
echo "==> mount on ${MNT}"
mount -t f2fs "/dev/mapper/${NAME}" "${MNT}"

echo "==> ready"
df -h "${MNT}"
dmsetup status "${NAME}" | tr ' ' '\n' | grep -E '^(r_end|free_lines|w=|r_bios|s_bios|gc_)' | paste -sd ' '
