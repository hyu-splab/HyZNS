#!/bin/bash
#
# Shared helpers for the dm-hyzns scripts. Source it, don't run it:
#   source "$(dirname "$0")/_lib.sh"
#
# Conventions assumed by callers:
#   - BACKING defaults to /dev/nvme0n1 (the FEMU HyZNS namespace).
#   - 1 GiB zones (= one dm-hyzns line), 512B sectors.

HYZNS_ZONE_SECTORS=$(( 1024 * 1024 * 1024 / 512 ))   # 1 GiB / 512B

# extract <status-line> <key> - pull "key=<int>" out of a dmsetup status line.
extract() {
    echo "$1" | grep -oE "$2=[0-9]+" | head -1 | cut -d= -f2
}

# report_rzones <backing> - current R-zone count via the device's own
# BLKREPORTABA (vendor ZRA 0x21). Works on the raw namespace and on a
# /dev/mapper/hyzns* target (dm-hyzns answers it locally from its r_end).
report_rzones() {
    python3 - "$1" <<'PY'
import fcntl, struct, os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
buf = bytearray(4)
# BLKREPORTABA = _IOR(0x12, 138, struct{__u32})
fcntl.ioctl(fd, (2 << 30) | (4 << 16) | (0x12 << 8) | 138, buf)
print(struct.unpack("<I", bytes(buf))[0])
os.close(fd)
PY
}

# reset_device_full_r <backing> - return the device boundary to full-R
# (every zone an R-zone), the clean starting point before a setup shrinks
# it to a chosen r_end. ModifyZone refuses to grow into non-EMPTY S-zones,
# so the current S-region is reset first. Verifies the result; returns
# non-zero (and warns) if the boundary did not reach full.
reset_device_full_r() {
    local backing="${1:-/dev/nvme0n1}"
    local size_sectors total_zones nr_rz s_start

    size_sectors="$(blockdev --getsz "$backing")"
    total_zones=$(( size_sectors / HYZNS_ZONE_SECTORS ))
    nr_rz="$(report_rzones "$backing")"

    if (( nr_rz < total_zones )); then
        s_start=$(( nr_rz * HYZNS_ZONE_SECTORS ))
        blkzone reset -o "$s_start" "$backing" 2>/dev/null || true
    fi

    # ModifyZone (ZSA 0x20) with the new ABA in SLBA (cdw10/11) = full size.
    nvme io-passthru "$backing" --opcode=0x79 --namespace-id=1 \
        --cdw10=$(( size_sectors & 0xFFFFFFFF )) \
        --cdw11=$(( size_sectors >> 32 )) \
        --cdw13=0x20 -r >/dev/null 2>&1 || true

    nr_rz="$(report_rzones "$backing")"
    if (( nr_rz != total_zones )); then
        echo "WARN: device boundary reset incomplete (nr_rzones=${nr_rz}/${total_zones});" >&2
        echo "      non-EMPTY S-zones may remain. 'blkzone reset ${backing}' to force." >&2
        return 1
    fi
    return 0
}
