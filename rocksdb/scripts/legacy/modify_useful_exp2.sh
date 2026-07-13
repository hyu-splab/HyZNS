#!/bin/bash
# Usefulness exp2: dynamic SHRINK unlocks ZNS capacity that a static-large-R
# device wastes. Start with a large CNS (R) -> tiny ZNS (S); a ZNS write that
# needs more than S FAILS. Shrink R online (no reformat) -> S grows -> the same
# write now FITS. Demonstrates dynamic > static-large for the ZNS side.
# Usage: sudo ./scripts/legacy/modify_useful_exp2.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyhost/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyhost0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYHOST_ZONE_SECTORS
RBIG=${RBIG:-250}; RSMALL=${RSMALL:-8}; AOZ=${AOZ:-7}
WRGB=${WRGB:-8}                       # ZNS write size (GiB) that overflows small S
NUM=$(( WRGB * 1024 * 1024 * 1024 / 900 ))   # ~900 B/record on disk
ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1)); SZ=$(blockdev --getsz "$BACKING")
znsfree() { "$ZENFS" df --zbd=dm-0 --aux_path="$AUX" 2>/dev/null | grep -iE 'free|avail' | head -3; }
fill() { ./db_bench --fs_uri="zenfs://dev:dm-0/$1/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=fillrandom --disable_wal=1 --num="$NUM" --key_size=20 --value_size=800 \
    --compression_type=none --max_background_jobs=8 &> "$2"; echo $?; }

umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
lsmod|awk '{print $1}'|grep -qx dm_hyhost||../dm-hyhost/scripts/load.sh>/dev/null
reset_device_full_r "$BACKING"||true
echo "0 $SZ hyhost $BACKING $((RBIG*ZS)) $SZ"|dmsetup create "$DMNAME"
dmsetup message "$DMNAME" 0 set_r_end $((RBIG*ZS))
mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$RBIG" \
  --start_zone="$RBIG" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1

echo "### STATIC large R=$RBIG  (S = $((TOTAL-RBIG)) zones) ###"
echo "  ZNS free (this is all a static-large-R device can ever give ZNS):"; znsfree
echo "  -> a ${WRGB} GiB ZNS write needs more than this -> would NOT fit."
echo ""
echo "### DYNAMIC: SHRINK R $RBIG -> $RSMALL (online, no reformat) ###"
t0=$(date +%s%3N)
"$ZENFS" resize-cns --zbd=dm-0 --aux_path="$AUX" --new_rzone="$RSMALL" &>/tmp/e2_shrink.log
t1=$(date +%s%3N)
echo "  shrink: $((t1-t0)) ms  $(grep -E 'completed|Input/output|error' /tmp/e2_shrink.log | tail -1)"
echo "  r_end now = $(( $(dmsetup status $DMNAME|grep -oE 'r_end=[0-9]+'|grep -oE '[0-9]+')/ZS )) zones (S = $((TOTAL - $(dmsetup status $DMNAME|grep -oE 'r_end=[0-9]+'|grep -oE '[0-9]+')/ZS )) zones)"
echo "  ZNS free after shrink:"; znsfree
echo "  -> same ZNS write ${WRGB} GiB now:"
rc=$(fill "$RSMALL" /tmp/e2_b.log)
echo "     db_bench rc=$rc  $(grep -oE 'fillrandom[^;]*ops/sec|No space|put error|write failed|Corruption' /tmp/e2_b.log | tail -1)"
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "EXP2_DONE"
