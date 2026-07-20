#!/bin/bash
# fig9_wal_dual.sh — Fig9(a) WAL placement, DUAL-instance (matches ZN540 2-tenant).
# Two db_bench share one HyZNS device; only the WAL path differs:
#   db_bench_zns   = WAL_ZNS  (WAL on each instance's ZNS S-range)
#   db_bench_posix = WAL_CNS  (WAL on the shared aux F2FS on R, O_SYNC)
# One run of `fillrandom,overwrite` yields BOTH FR and OW per instance; we sum
# the two instances (aggregate device throughput).
# Sweep: num {20M,40M} x jobs {2,4,8} x cfg {zns,posix}, N=1.
set -uo pipefail
cd "$(dirname "$0")/../.."                                   # rocksdb/
DMHYZNS=../dm-hyzns; source "$DMHYZNS/scripts/_lib.sh"
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS; ZENFS=./plugin/zenfs/util/zenfs
R_ZONES=${R_ZONES:-4}; AOZ=${AOZ:-7}; KEY=20; VAL=800; JOBSET=${JOBSET:-"2 4 8"}
CFGS=${CFGS:-"ZNS CNS"}   # which WAL placements to run: "ZNS CNS" | "CNS" | "ZNS"
NUMS=${NUMS:-"20000000 40000000"}
OUT=results/fig9_dual; mkdir -p "$OUT"
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1))
S_START=$R_ZONES; MID=$(( (S_START + S_LAST) / 2 )); I2_START=$((MID+1))
SZ=$(blockdev --getsz "$BACKING")
RES="$OUT/wal_dual_$(date +%Y%m%d_%H%M%S).csv"
DRES="${RES%.csv}_device.csv"
echo "label,cfg,num,jobs,inst,fillrandom_ops,overwrite_ops,rc" > "$RES"
# device/dm-hyzns layer (shared by both instances, per dual-run): R-region GC churn
# (WAL_CNS WAL garbage) + actual device write (wsec) for the 3-layer copy/WAF analysis.
echo "label,num,jobs,gc_runs,gc_mig_pages,erases,recycles,dev_write_B,R_pre_z,R_post_z,ingestGB_i1,ingestGB_i2" > "$DRES"
echo "WAL DUAL sweep -> $RES (+ device $DRES)  (S split i1[$S_START..$MID] i2[$I2_START..$S_LAST], ao=$AOZ each)"

setup() {   # fresh dm + aux F2FS + 2 ZenFS instances; returns non-zero on any failure.
  umount "$AUX/i1" 2>/dev/null||true; umount "$AUX" 2>/dev/null||true
  dmsetup remove "$DMNAME" 2>/dev/null||true
  lsmod|awk '{print $1}'|grep -qx dm_hyzns || "$DMHYZNS/scripts/load.sh">/dev/null 2>&1
  reset_device_full_r "$BACKING" >/dev/null 2>&1||true
  echo "0 $SZ hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME" || return 1
  dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) || return 1
  mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1 || return 1
  mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX" || return 1; mkdir -p "$AUX/i1" "$AUX/i2"
  # NOTE: zenfs mkfs does NOT populate the aux dir (LOG/LOCK appear only when the DB
  # is opened at runtime) — so verify by exit code, not by aux files.
  "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i1" --hyssd --aux_size="$R_ZONES" \
    --start_zone="$S_START" --end_zone="$MID" --ao_zones="$AOZ" --enable_gc=true --force >"$OUT/.zmkfs_i1.log" 2>&1 || return 1
  "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i2" --hyssd --aux_size="$R_ZONES" \
    --start_zone="$I2_START" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >"$OUT/.zmkfs_i2.log" 2>&1 || return 1
  return 0
}

ext(){ grep -E "^$1 " "$2" 2>/dev/null|tail -1|grep -oE '[0-9]+ ops/sec'|grep -oE '[0-9]+'; }
dpk(){ local v=$(grep -oE "(^| )$2=[0-9]+" "$1" 2>/dev/null|head -1|grep -oE '[0-9]+'); echo "${v:-0}"; }   # $1=dmstatus file $2=key
BDEV=$(basename "$BACKING")

run_cfg(){  # $1 label  $2 bin  $3 num  $4 jobs
  local label=$1 bin=$2 num=$3 jobs=$4
  local tag="${label}_n${num}_j${jobs}"; local l1="$OUT/waldual_${tag}_i1.log" l2="$OUT/waldual_${tag}_i2.log"
  [[ -x $bin ]] || { echo "missing $bin"; return 1; }
  # FEMU-alive guard before each cell
  timeout 8 nvme list >/dev/null 2>&1 || { echo "  [$tag] ABORT: FEMU not responding"; echo "$label,$2,$num,$jobs,-,FEMU_DOWN,FEMU_DOWN,rc=na" >> "$RES"; return 2; }
  # verify setup succeeded by exit code (+ dm/mount sanity). zenfs mkfs success != aux files.
  if ! setup || ! dmsetup status "$DMNAME" >/dev/null 2>&1 || ! mountpoint -q "$AUX"; then
    echo "  [$tag] SETUP FAILED — skipping cell (see $OUT/.zmkfs_i*.log)"; echo "$label,$2,$num,$jobs,-,SETUP_FAIL,SETUP_FAIL,rc=na" >> "$RES"
    umount "$AUX/i1" 2>/dev/null; umount "$AUX" 2>/dev/null; dmsetup remove "$DMNAME" 2>/dev/null||true; return 3
  fi
  echo "  [$tag] launching 2 instances (fillrandom,overwrite num=$num/inst jobs=$jobs)"
  local pfx="$OUT/waldual_${tag}"
  # device/dm samplers (shared device, both instances) + pre snapshots
  ( while :; do printf '%s ' "$(date +%s.%N)"; dmsetup status "$DMNAME" 2>/dev/null; echo; sleep 1; done ) > "$pfx.dmstatus.log" 2>&1 & local dspid=$!
  iostat -x -t 1 "$BACKING" > "$pfx.iostat.log" 2>&1 & local iopid=$!
  dmsetup status "$DMNAME" > "$pfx.dmstatus_pre.txt" 2>/dev/null
  awk '{print $7}' /sys/block/$BDEV/stat > "$pfx.wsec_pre.txt" 2>/dev/null || echo 0 > "$pfx.wsec_pre.txt"
  ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,stats,overwrite,stats --use_direct_io_for_flush_and_compaction \
    --num="$num" --key_size=$KEY --value_size=$VAL --compression_type=none \
    --max_background_jobs="$jobs" --histogram --statistics --stats_interval_seconds=1 &> "$l1" & local p1=$!
  ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,stats,overwrite,stats --use_direct_io_for_flush_and_compaction \
    --num="$num" --key_size=$KEY --value_size=$VAL --compression_type=none \
    --max_background_jobs="$jobs" --histogram --statistics --stats_interval_seconds=1 &> "$l2" & local p2=$!
  wait $p1; local r1=$?; wait $p2; local r2=$?
  # post snapshots + stop samplers + device/dm delta
  awk '{print $7}' /sys/block/$BDEV/stat > "$pfx.wsec_post.txt" 2>/dev/null || echo 0 > "$pfx.wsec_post.txt"
  dmsetup status "$DMNAME" > "$pfx.dmstatus_post.txt" 2>/dev/null
  kill $dspid $iopid 2>/dev/null
  local gr=$(( $(dpk "$pfx.dmstatus_post.txt" gc_runs)-$(dpk "$pfx.dmstatus_pre.txt" gc_runs) ))
  local gm=$(( $(dpk "$pfx.dmstatus_post.txt" gc_mig)-$(dpk "$pfx.dmstatus_pre.txt" gc_mig) ))
  local er=$(( $(dpk "$pfx.dmstatus_post.txt" erases)-$(dpk "$pfx.dmstatus_pre.txt" erases) ))
  local rc2=$(( $(dpk "$pfx.dmstatus_post.txt" recycles)-$(dpk "$pfx.dmstatus_pre.txt" recycles) ))
  local rp=$(( $(dpk "$pfx.dmstatus_pre.txt" r_end)/ZS )) rq=$(( $(dpk "$pfx.dmstatus_post.txt" r_end)/ZS ))
  local ws=$(( ($(cat "$pfx.wsec_post.txt" 2>/dev/null)-$(cat "$pfx.wsec_pre.txt" 2>/dev/null))*512 ))
  local ig1=$(grep -oE 'ingest: [0-9.]+ GB' "$l1" 2>/dev/null|tail -1|grep -oE '[0-9.]+')
  local ig2=$(grep -oE 'ingest: [0-9.]+ GB' "$l2" 2>/dev/null|tail -1|grep -oE '[0-9.]+')
  echo "$label,$num,$jobs,$gr,$gm,$er,$rc2,$ws,$rp,$rq,${ig1:-NA},${ig2:-NA}" >> "$DRES"
  echo "    device: gc_mig=$gm erases=$er recycles=$rc2 dev_write=$((ws/1073741824))GB R=$rp->$rq"
  echo "$label,$2,$num,$jobs,i1,$(ext fillrandom "$l1"),$(ext overwrite "$l1"),rc=$r1" >> "$RES"
  echo "$label,$2,$num,$jobs,i2,$(ext fillrandom "$l2"),$(ext overwrite "$l2"),rc=$r2" >> "$RES"
  echo "    i1 FR=$(ext fillrandom "$l1") OW=$(ext overwrite "$l1") rc=$r1 | i2 FR=$(ext fillrandom "$l2") OW=$(ext overwrite "$l2") rc=$r2"
  umount "$AUX/i1" 2>/dev/null; umount "$AUX" 2>/dev/null; dmsetup remove "$DMNAME" 2>/dev/null||true
}

for num in $NUMS; do for jobs in $JOBSET; do
  [[ " $CFGS " == *" ZNS "* ]] && run_cfg WAL_ZNS  ./db_bench_zns   "$num" "$jobs"
  [[ " $CFGS " == *" CNS "* ]] && run_cfg WAL_CNS  ./db_bench_posix "$num" "$jobs"
done; done
echo "WAL_DUAL_DONE -> $RES"
