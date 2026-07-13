#!/bin/bash
# Usefulness exp3 (paper §6.3): dynamic modify lets ONE device capture the
# L0-CNS write benefit (which needs R>=~12) on demand, then reclaim ZNS capacity.
#   - static-small (R=4): cannot host L0-CNS -> stuck at L0-ZNS baseline tput.
#   - static-large (R=16): L0-CNS tput, but R=16 committed (ZNS capacity lost).
#   - dynamic: R=4 -> GROW 16 -> L0-CNS tput -> SHRINK 4 (ZNS capacity back).
# Usage: sudo NUM=8000000 ./scripts/legacy/modify_useful_exp3.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyhost/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyhost0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYHOST_ZONE_SECTORS
RSMALL=${RSMALL:-4}; RBIG=${RBIG:-16}; NUM=${NUM:-8000000}; AOZ=${AOZ:-7}
ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1)); SZ=$(blockdev --getsz "$BACKING")
rendz() { echo $(( $(dmsetup status "$DMNAME"|grep -oE 'r_end=[0-9]+'|grep -oE '[0-9]+')/ZS )); }
znsfreeMB() { "$ZENFS" df --zbd=dm-0 --aux_path="$AUX" 2>/dev/null | grep -oE 'Free *: *[0-9]+' | grep -oE '[0-9]+' | head -1; }

setup() {  # $1 = initial R (zones)
  umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
  lsmod|awk '{print $1}'|grep -qx dm_hyhost||../dm-hyhost/scripts/load.sh>/dev/null
  reset_device_full_r "$BACKING"||true
  echo "0 $SZ hyhost $BACKING $(( $1*ZS )) $SZ"|dmsetup create "$DMNAME"
  dmsetup message "$DMNAME" 0 set_r_end $(( $1*ZS ))
  mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1
  mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
  "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$1" \
    --start_zone="$1" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
}
filltput() {  # $1=binary $2=startzone $3=log -> ops/sec
  ZENFS_L0_ON_AUX=0 ./$1 --fs_uri="zenfs://dev:dm-0/$2/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,stats --use_direct_io_for_flush_and_compaction \
    --num="$NUM" --key_size=20 --value_size=800 --compression_type=none \
    --max_background_jobs="${BG:-8}" --statistics &> "$3"
  grep -oE 'fillrandom[^;]*ops/sec' "$3" | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+' | head -1
  grep -lE 'put error|No space|write failed|Corruption' "$3" >/dev/null 2>&1 && echo " (ERRORS)"
}

echo "### CONFIG 1 — static-small R=$RSMALL : L0 must stay on ZNS (baseline) ###"
setup "$RSMALL"
T_ZNS=$(filltput db_bench_zns "$RSMALL" /tmp/e3_zns.log)
echo "  L0-ZNS fillrandom = ${T_ZNS} ops/sec  (free ZNS=$(znsfreeMB) MB)"

echo ""
echo "### CONFIG 2 — static-large R=$RBIG : L0-CNS, but R committed ###"
setup "$RBIG"
T_CNS_S=$(filltput db_bench_l0cns "$RBIG" /tmp/e3_cns_static.log)
echo "  L0-CNS fillrandom = ${T_CNS_S} ops/sec  (free ZNS=$(znsfreeMB) MB)  [R=$RBIG committed]"

echo ""
echo "### CONFIG 3 — DYNAMIC: R=$RSMALL -> GROW $RBIG -> L0-CNS -> SHRINK $RSMALL ###"
setup "$RSMALL"
echo "  start R=$(rendz), ZNS free=$(znsfreeMB) MB"
t0=$(date +%s%3N); "$ZENFS" resize-cns --zbd=dm-0 --aux_path="$AUX" --new_rzone="$RBIG" &>/tmp/e3_grow.log; t1=$(date +%s%3N)
echo "  GROW -> R=$(rendz) in $((t1-t0)) ms"
T_CNS_D=$(filltput db_bench_l0cns "$RBIG" /tmp/e3_cns_dyn.log)
echo "  L0-CNS fillrandom = ${T_CNS_D} ops/sec"
t0=$(date +%s%3N); "$ZENFS" resize-cns --zbd=dm-0 --aux_path="$AUX" --new_rzone="$RSMALL" &>/tmp/e3_shrink.log; t1=$(date +%s%3N)
echo "  SHRINK -> R=$(rendz) in $((t1-t0)) ms, ZNS free=$(znsfreeMB) MB (reclaimed)"

echo ""
echo "### SUMMARY ###"
echo "  static-small (R=$RSMALL): L0-ZNS  ${T_ZNS} ops/s   (no L0-CNS benefit)"
echo "  static-large (R=$RBIG): L0-CNS  ${T_CNS_S} ops/s   (R=$RBIG wasted)"
echo "  dynamic:               L0-CNS  ${T_CNS_D} ops/s   (R grown on demand, ZNS reclaimed)"
awk -v z="$T_ZNS" -v c="$T_CNS_D" 'BEGIN{ if(z>0) printf "  -> dynamic L0-CNS / static-small L0-ZNS = %.2fx\n", c/z }'
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "EXP3_DONE"
