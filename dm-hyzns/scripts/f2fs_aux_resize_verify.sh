#!/bin/bash
#
# Verify F2FS aux online ABA-resize (Path 2: max-provision + MAIN_SECS gating).
# RUN AFTER: kernel rebuilt+rebooted (f2fs aux maxprov support) AND
#            f2fs-tools rebuilt+installed (mkfs.f2fs -P).
#
# Exercises: mkfs -A -P (max-provision) -> mount (gate-down to current ABA)
#            -> write+md5 -> grow R -> shrink R -> remount, checking data
#            integrity (md5) and usable-size tracking at every step.
#
# This is a best-effort harness for an as-yet-untested feature: data-integrity
# (md5) and "no error" are hard FAILs; usable-size tracking is reported and
# checked leniently (device/conversion behavior is what we're validating).
#
# Usage:
#   sudo ./scripts/f2fs_aux_resize_verify.sh [R_INIT_GiB] [R_GROW_GiB] [R_SHRINK_GiB]
# Defaults: 8 -> 16 -> 12 (on a 128 GiB device).
set -uo pipefail
cd "$(dirname "$0")/.."
source "$(dirname "$0")/_lib.sh"

BACKING=/dev/nvme0n1
NAME=hyzns0
MNT=/mnt/aux
DEV=/dev/mapper/$NAME
R_INIT=${1:-8}
R_GROW=${2:-16}
R_SHRINK=${3:-12}
ZS=$HYZNS_ZONE_SECTORS           # 1 GiB zone in 512B sectors
F2FS_IO=${F2FS_IO:-$(command -v f2fs_io || echo ../f2fs-tools-1.16.0/tools/f2fs_io/f2fs_io)}
DATA=$MNT/data
PASS=0; FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
note() { echo "  [....] $*"; }

# f2fs_statfs reports f_blocks (df "size") from raw_super->block_count, which for
# a max-provisioned aux volume is ALWAYS the whole device - the gate only moves
# available space (f_bavail = user_block_count - valid - reserved). So track
# "avail", not "size", to observe the gate / grow / shrink.
usable_gib() { df -B1G --output=avail "$MNT" 2>/dev/null | tail -1 | tr -dc 0-9; }
md5_data()   { find "$DATA" -type f | sort | xargs md5sum 2>/dev/null | md5sum | cut -d' ' -f1; }
dmesg_tail() { dmesg | tail -40 | grep -iE "aux|gate|resize|F2FS-fs.*(error|BUG)|blk_update_request" || true; }

echo "=== prereqs ==="
[[ -b $BACKING ]] || { echo "no $BACKING"; exit 1; }
mkfs.f2fs -V 2>/dev/null | head -1
if ! mkfs.f2fs 2>&1 | grep -q "\-P\|aux.*max" && ! strings "$(command -v mkfs.f2fs)" 2>/dev/null | grep -q max-provision; then
    note "cannot confirm mkfs.f2fs -P support; make sure the patched build is installed"
fi
[[ -x $F2FS_IO ]] || { echo "f2fs_io not found ($F2FS_IO); set F2FS_IO="; exit 1; }

echo "=== setup: dm-hyzns target, device boundary -> ${R_INIT} GiB R ==="
umount "$MNT" 2>/dev/null || true
dmsetup remove "$NAME" 2>/dev/null || true
lsmod | awk '{print $1}' | grep -qx dm_hyzns || ./scripts/load.sh >/dev/null
reset_device_full_r "$BACKING" || true
SZ=$(blockdev --getsz "$BACKING")
# 3rd arg = max_r_end_lba: max-provision the dm L2P/rings for the WHOLE device
# so an online grow can re-expose tail lines (dm analogue of mkfs -A -P). Without
# it the L2P is fixed at the initial r_end and grow is rejected with EIO.
echo "0 $SZ hyzns $BACKING $((R_INIT*ZS)) $SZ" | dmsetup create "$NAME"
dmsetup message "$NAME" 0 set_r_end $((R_INIT*ZS))
echo "device nr_rzones=$(report_rzones "$DEV") (want $R_INIT)"

echo "=== mkfs.f2fs -m -H -A -P (max-provision) ==="
if mkfs.f2fs -f -m -H -A -P "$DEV" > /tmp/aux_mkfs.log 2>&1; then
    ok "mkfs -A -P succeeded"
    grep -iE "max-provision|whole device|total sectors" /tmp/aux_mkfs.log | head -3 | sed 's/^/    /'
else
    bad "mkfs -A -P failed"; tail -5 /tmp/aux_mkfs.log; exit 1
fi

echo "=== mount + verify gate-down to current ABA ==="
mkdir -p "$MNT"
dmesg -C >/dev/null 2>&1 || true
if mount -t f2fs "$DEV" "$MNT"; then ok "mount"; else bad "mount"; dmesg_tail; exit 1; fi
U_INIT=$(usable_gib)
note "avail=${U_INIT} GiB (expect 0<avail<=${R_INIT}, i.e. gated to R, NOT whole device $((SZ/ZS)))"
if dmesg | grep -q "aux gate-init"; then ok "gate-init ran: $(dmesg | grep 'aux gate-init' | tail -1 | sed 's/.*aux/aux/')"; else bad "no 'aux gate-init' in dmesg (gate-down didn't run?)"; fi
if (( U_INIT > 0 && U_INIT <= R_INIT )); then ok "avail gated to R-region (<= ${R_INIT} GiB), not whole device"; else bad "avail=${U_INIT} not gated to R - F2FS may allocate into S!"; fi

echo "=== write workload + baseline md5 ==="
# 1 GiB total - comfortably under R even after mkfs overprovision (sized for the
# max-prov device) eats into the gated R. The point is integrity across resize,
# not filling R.
mkdir -p "$DATA"
for i in 1 2 3 4; do dd if=/dev/urandom of="$DATA/f$i" bs=1M count=256 oflag=direct status=none; done
sync; touch "$MNT/.modtarget"
MD5_0=$(md5_data); note "baseline md5=$MD5_0"

echo "=== GROW R ${R_INIT} -> ${R_GROW} GiB (resize_cns) ==="
dmesg -C >/dev/null 2>&1 || true
if "$F2FS_IO" resize_cns "$R_GROW" "$MNT/.modtarget" > /tmp/aux_grow.log 2>&1; then ok "resize_cns grow returned 0"; else bad "resize_cns grow failed"; cat /tmp/aux_grow.log; fi
sync; U_GROW=$(usable_gib)
note "usable after grow=${U_GROW} GiB (expect ~${R_GROW})"
(( U_GROW > U_INIT )) && ok "usable grew" || bad "usable did not grow"
[[ "$(md5_data)" == "$MD5_0" ]] && ok "data intact after grow" || bad "DATA CHANGED after grow"
dmesg_tail | sed 's/^/    /'

echo "=== SHRINK R ${R_GROW} -> ${R_SHRINK} GiB (resize_cns; drains tail) ==="
dmesg -C >/dev/null 2>&1 || true
if "$F2FS_IO" resize_cns "$R_SHRINK" "$MNT/.modtarget" > /tmp/aux_shrink.log 2>&1; then ok "resize_cns shrink returned 0"; else bad "resize_cns shrink failed"; cat /tmp/aux_shrink.log; fi
sync; U_SHRINK=$(usable_gib)
note "usable after shrink=${U_SHRINK} GiB (expect ~${R_SHRINK})"
(( U_SHRINK < U_GROW )) && ok "usable shrank" || bad "usable did not shrink"
[[ "$(md5_data)" == "$MD5_0" ]] && ok "data intact after shrink (drain relocated, no loss)" || bad "DATA LOST after shrink"
dmesg_tail | sed 's/^/    /'

echo "=== remount consistency ==="
umount "$MNT" && mount -t f2fs "$DEV" "$MNT" && ok "remount" || bad "remount"
[[ "$(md5_data)" == "$MD5_0" ]] && ok "data intact after remount" || bad "DATA CHANGED after remount"
note "usable after remount=$(usable_gib) GiB"

echo
echo "=== RESULT: PASS=$PASS FAIL=$FAIL ==="
echo "logs: /tmp/aux_{mkfs,grow,shrink}.log ; teardown: umount $MNT; dmsetup remove $NAME"
exit $(( FAIL > 0 ? 1 : 0 ))
