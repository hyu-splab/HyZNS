#!/bin/bash
# L0-CNS livelock rescue: a small static R-region can't sustain L0-CNS GC churn
# (overwrite) and livelocks; with the in-process ZoneArbiter ON, R grows online
# under pressure and the run completes. Demonstrates modify's necessity for L0-CNS.
# Usage: sudo R0=8 NUM=15000000 ./scripts/legacy/modify_l0cns_rescue.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyzns/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS
R0=${R0:-8}; NUM=${NUM:-15000000}; AOZ=${AOZ:-7}
ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING")/ZS )); S_LAST=$((TOTAL-1)); SZ=$(blockdev --getsz "$BACKING")
rendz(){ echo $(( $(dmsetup status "$DMNAME"|grep -oE 'r_end=[0-9]+'|grep -oE '[0-9]+')/ZS )); }
dmst(){ dmsetup status "$DMNAME" 2>/dev/null|tr ' ' '\n'|grep -E 'free_lines=|gc_runs=|nospc=|gc_mig='|tr '\n' ' '; }

setup(){
  umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
  lsmod|awk '{print $1}'|grep -qx dm_hyzns||../dm-hyzns/scripts/load.sh>/dev/null
  reset_device_full_r "$BACKING"||true
  echo "0 $SZ hyzns $BACKING $((R0*ZS)) $SZ"|dmsetup create "$DMNAME"
  dmsetup message "$DMNAME" 0 set_r_end $((R0*ZS))
  mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1
  mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
  "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R0" \
    --start_zone="$R0" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
}
run(){  # $1 label  $2 arbiter(0/1)
  setup
  local log=/tmp/rescue_$1.log env=""
  [ "$2" = 1 ] && env="ZENFS_ZONE_ARBITER=1"
  echo "### $1 (R0=$R0, arbiter=$2) ###"
  timeout -s KILL ${TMO:-900} env $env ./db_bench_l0cns --fs_uri="zenfs://dev:dm-0/$R0/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,overwrite,stats --disable_wal=1 \
    --use_direct_io_for_flush_and_compaction \
    --num="$NUM" --key_size=20 --value_size=800 --compression_type=none \
    --max_background_jobs=8 --statistics --stats_interval_seconds=10 &> "$log"
  local rc=$?
  echo "  rc=$rc (124/137=timeout/livelock, non-0=EIO)  final R=$(rendz) zones"
  echo "  fill: $(grep -E '^fillrandom' "$log"|grep -oE '[0-9.]+ ops/sec'|head -1)  overwrite: $(grep -E '^overwrite' "$log"|grep -oE '[0-9.]+ ops/sec'|head -1)"
  echo "  err: $(grep -oE 'No space|put error|write failed|Corruption|abnormal|mismatch' "$log"|sort|uniq -c|tr '\n' ' ')"
  echo "  grows: $(grep -cE 'Expanding R-zone|Expansion complete|grow .* -> R=' "$log") | dmstatus: $(dmst)"
}
run "static" 0
run "dynamic" 1
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "RESCUE_DONE"
