#!/bin/bash
# fig9_dual_unified.sh — unified Fig9 DUAL: three CNS-separation configs, ALL
# WITH WAL ON, two db_bench sharing one HyZNS device (S split i1/i2). VANILLA
# is the shared baseline for both WAL_CNS and L0_CNS.
#
#   VANILLA      db_bench_zns    L0_ON_AUX=0  WAL->ZNS  L0->ZNS   (= old WAL_ZNS, baseline)
#   WAL_CNS      db_bench_posix  L0_ON_AUX=0  WAL->CNS  L0->ZNS   (WAL only on R, O_SYNC aux)
#   L0_CNS_nomod db_bench_l0cns  L0_ON_AUX=1  WAL->ZNS  L0->CNS   (L0 only on R, static R -> may die)
#   L0_CNS_mod   db_bench_l0cns  L0_ON_AUX=1  WAL->ZNS  L0->CNS   (+ arbiter grows R online)
#
# One `fillrandom,stats,overwrite,stats` run per instance -> FR & OW ops + per-phase levels.
# 3-layer EPOCH-ALIGNED time series for overlay (parse with fig9_parse_ts.py):
#   <pfx>_i{1,2}.log  db_bench per-sec ops (RocksDB wall-clock ts)
#   <pfx>.dmstatus.log  dm-hyzns R-FTL series, prefixed with `date +%s.%N` (epoch.ns)
#   <pfx>.iostat.log    device I/O, S_TIME_FORMAT=ISO (ISO8601 ts, tz-unambiguous)
#   <pfx>.t0            single epoch.ns written right before db_bench launch (common origin)
#
# env: CONFIGS="VANILLA WAL_CNS L0_CNS_mod"  NUMS=25000000  JOBSET=8  R_ZONES=4  AOZ=7
#      arbiter (L0_CNS_mod): ARB_INT/ARB_THR/ARB_MAX/ARB_CD
set -uo pipefail
cd "$(dirname "$0")/../.."                                   # rocksdb/
DMHYZNS=../dm-hyzns; source "$DMHYZNS/scripts/_lib.sh"
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS; ZENFS=./plugin/zenfs/util/zenfs
R_ZONES=${R_ZONES:-4}; R_MAX=${R_MAX:-64}; AOZ=${AOZ:-7}; KEY=20; VAL=800   # R_MAX=growth reservation; S starts at R_MAX
JOBSET=${JOBSET:-8}; NUMS=${NUMS:-25000000}
CONFIGS=${CONFIGS:-"VANILLA WAL_CNS L0_CNS_mod"}
TMO=${TMO:-1800}    # per-instance db_bench timeout (SIGTERM) — backstop vs deadlock (e.g. nomod)
# arbiter (L0_CNS_mod only): early/aggressive grow on dm free_lines. Dual runs
# TWO arbiters on the shared R -> tune ARB_MAX up and watch for over-grow.
ARB_INT=${ARB_INT:-1000}; ARB_THR=${ARB_THR:-50}; ARB_MAX=${ARB_MAX:-32}; ARB_CD=${ARB_CD:-2000}
ARB_FLOOR=${ARB_FLOOR:-2}; ARB_STEP=${ARB_STEP:-1}   # absolute free-line trigger + incremental grow (no over-shoot)
OUT=results/fig9_dual_unified; mkdir -p "$OUT"
SZ=$(blockdev --getsz "$BACKING"); TOTAL=$((SZ/ZS)); S_LAST=$((TOTAL-1))
S_START=$R_MAX; MID=$(( (S_START + S_LAST)/2 )); I2_START=$((MID+1))   # S above R-growth reservation [0..R_MAX) -> grow never collides with ZenFS S
RES="$OUT/unified_$(date +%Y%m%d_%H%M%S).csv"; DRES="${RES%.csv}_device.csv"
echo "config,num,jobs,rep,inst,fillrandom_ops,overwrite_ops,rc" > "$RES"
echo "config,num,jobs,rep,gc_runs,gc_mig_pages,erases,recycles,dev_write_B,R_pre_z,R_post_z,nospc,ingestGB_i1,ingestGB_i2" > "$DRES"
echo "UNIFIED DUAL -> $RES (+ $DRES)  S i1[$S_START..$MID] i2[$I2_START..$S_LAST]  ao=$AOZ R=$R_ZONES  configs=[$CONFIGS]"

cfgmap(){  # $1 config -> sets globals BIN L0AUX MAXPROV ARB
  BIN=; L0AUX=0; MAXPROV=0; ARB=""
  case "$1" in
    VANILLA)      BIN=./db_bench_zns;   L0AUX=0; MAXPROV=0;;
    WAL_CNS)      BIN=./db_bench_posix; L0AUX=0; MAXPROV=0;;
    L0_CNS_nomod) BIN=./db_bench_l0cns; L0AUX=1; MAXPROV=0;;
    L0_CNS_mod)   BIN=./db_bench_l0cns; L0AUX=1; MAXPROV=1;
                  ARB="ZENFS_ZONE_ARBITER=1 ZENFS_ARBITER_INTERVAL_MS=$ARB_INT ZENFS_ARBITER_THRESHOLD=$ARB_THR ZENFS_ARBITER_MAXZONES=$ARB_MAX ZENFS_ARBITER_COOLDOWN_MS=$ARB_CD ZENFS_ARBITER_FREE_FLOOR=$ARB_FLOOR ZENFS_ARBITER_STEP=$ARB_STEP ZENFS_ARBITER_MAX_RZONE=$R_MAX";;
    *) echo "  bad CONFIG=$1"; return 1;;
  esac
  [[ -x $BIN ]] || { echo "  missing $BIN (build it first)"; return 1; }
}

setup(){  # $1 maxprov(0/1); fresh dm + aux F2FS + 2 ZenFS instances. nonzero on any failure.
  umount "$AUX/i1" 2>/dev/null||true; umount "$AUX/i2" 2>/dev/null||true; umount "$AUX" 2>/dev/null||true
  dmsetup remove "$DMNAME" 2>/dev/null||true
  lsmod|awk '{print $1}'|grep -qx dm_hyzns || "$DMHYZNS/scripts/load.sh">/dev/null 2>&1
  reset_device_full_r "$BACKING" >/dev/null 2>&1||true
  if [[ $1 == 1 ]]; then
    echo "0 $SZ hyzns $BACKING $((R_ZONES*ZS)) $((R_MAX*ZS))" | dmsetup create "$DMNAME" || return 1   # 5th arg=max_r_end=R_MAX (dm hard cap on grow)
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) || return 1
    mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1 || return 1
  else
    echo "0 $SZ hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME" || return 1
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) || return 1
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1 || return 1
  fi
  mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX" || return 1; mkdir -p "$AUX/i1" "$AUX/i2"
  "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i1" --hyssd --aux_size="$R_ZONES" \
    --start_zone="$S_START" --end_zone="$MID" --ao_zones="$AOZ" --enable_gc=true --force >"$OUT/.zmkfs_i1.log" 2>&1 || return 1
  "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i2" --hyssd --aux_size="$R_ZONES" \
    --start_zone="$I2_START" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >"$OUT/.zmkfs_i2.log" 2>&1 || return 1
  return 0
}

ext(){ grep -E "^$1 " "$2" 2>/dev/null|tail -1|grep -oE '[0-9]+ ops/sec'|grep -oE '[0-9]+'; }
dpk(){ local v=$(grep -oE "(^| )$2=[0-9]+" "$1" 2>/dev/null|head -1|grep -oE '[0-9]+'); echo "${v:-0}"; }
BDEV=$(basename "$BACKING")

run_cfg(){  # $1 config  $2 num  $3 jobs  $4 rep
  local cfg=$1 num=$2 jobs=$3 rep=${4:-1}
  cfgmap "$cfg" || { echo "$cfg,$num,$jobs,-,CFGMAP_FAIL,CFGMAP_FAIL,rc=na">>"$RES"; return 1; }
  local tag="${cfg}_n${num}_j${jobs}_r${rep}"; local pfx="$OUT/u_${tag}"
  local l1="${pfx}_i1.log" l2="${pfx}_i2.log"
  timeout 8 nvme list >/dev/null 2>&1 || { echo "  [$tag] FEMU down"; echo "$cfg,$num,$jobs,-,FEMU_DOWN,FEMU_DOWN,rc=na">>"$RES"; return 2; }
  if ! setup "$MAXPROV" || ! dmsetup status "$DMNAME" >/dev/null 2>&1 || ! mountpoint -q "$AUX"; then
    echo "  [$tag] SETUP FAILED (see $OUT/.zmkfs_i*.log)"; echo "$cfg,$num,$jobs,-,SETUP_FAIL,SETUP_FAIL,rc=na">>"$RES"
    umount "$AUX" 2>/dev/null; dmsetup remove "$DMNAME" 2>/dev/null||true; return 3
  fi
  echo "  [$tag] bin=$BIN L0_ON_AUX=$L0AUX maxprov=$MAXPROV arb=${ARB:+on}"
  # samplers: dmstatus(epoch.ns) + iostat(ISO) -- both land on the epoch axis
  ( while :; do printf '%s ' "$(date +%s.%N)"; dmsetup status "$DMNAME" 2>/dev/null; echo; sleep 1; done ) > "$pfx.dmstatus.log" 2>&1 & local dspid=$!
  # iostat keeps ISO8601 (S_TIME_FORMAT=ISO): unambiguous (+0900 tz), parser converts trivially.
  S_TIME_FORMAT=ISO iostat -x -t 1 "$BACKING" > "$pfx.iostat.log" 2>&1 & local iopid=$!
  dmsetup status "$DMNAME" > "$pfx.dmstatus_pre.txt" 2>/dev/null
  awk '{print $7}' /sys/block/$BDEV/stat > "$pfx.wsec_pre.txt" 2>/dev/null || echo 0 > "$pfx.wsec_pre.txt"
  date +%s.%N > "$pfx.t0"                                  # common time origin for the 3 streams
  timeout -s TERM "$TMO" env $ARB ZENFS_L0_ON_AUX=$L0AUX "$BIN" --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,stats,overwrite,stats --use_direct_io_for_flush_and_compaction \
    --num="$num" --key_size=$KEY --value_size=$VAL --compression_type=none \
    --max_background_jobs="$jobs" --histogram --statistics --stats_interval_seconds=1 \
    --report_interval_seconds=1 --report_file="${pfx}_i1.report.csv" &> "$l1" & local p1=$!
  timeout -s TERM "$TMO" env $ARB ZENFS_L0_ON_AUX=$L0AUX "$BIN" --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,stats,overwrite,stats --use_direct_io_for_flush_and_compaction \
    --num="$num" --key_size=$KEY --value_size=$VAL --compression_type=none \
    --max_background_jobs="$jobs" --histogram --statistics --stats_interval_seconds=1 \
    --report_interval_seconds=1 --report_file="${pfx}_i2.report.csv" &> "$l2" & local p2=$!
  wait $p1; local r1=$?; wait $p2; local r2=$?
  awk '{print $7}' /sys/block/$BDEV/stat > "$pfx.wsec_post.txt" 2>/dev/null || echo 0 > "$pfx.wsec_post.txt"
  dmsetup status "$DMNAME" > "$pfx.dmstatus_post.txt" 2>/dev/null
  kill $dspid $iopid 2>/dev/null
  local gr=$(( $(dpk "$pfx.dmstatus_post.txt" gc_runs)-$(dpk "$pfx.dmstatus_pre.txt" gc_runs) ))
  local gm=$(( $(dpk "$pfx.dmstatus_post.txt" gc_mig)-$(dpk "$pfx.dmstatus_pre.txt" gc_mig) ))
  local er=$(( $(dpk "$pfx.dmstatus_post.txt" erases)-$(dpk "$pfx.dmstatus_pre.txt" erases) ))
  local rc2=$(( $(dpk "$pfx.dmstatus_post.txt" recycles)-$(dpk "$pfx.dmstatus_pre.txt" recycles) ))
  local rp=$(( $(dpk "$pfx.dmstatus_pre.txt" r_end)/ZS )) rq=$(( $(dpk "$pfx.dmstatus_post.txt" r_end)/ZS ))
  local nsp=$(dpk "$pfx.dmstatus_post.txt" nospc)
  local ws=$(( ($(cat "$pfx.wsec_post.txt" 2>/dev/null)-$(cat "$pfx.wsec_pre.txt" 2>/dev/null))*512 ))
  local ig1=$(grep -oE 'ingest: [0-9.]+ GB' "$l1" 2>/dev/null|tail -1|grep -oE '[0-9.]+')
  local ig2=$(grep -oE 'ingest: [0-9.]+ GB' "$l2" 2>/dev/null|tail -1|grep -oE '[0-9.]+')
  echo "$cfg,$num,$jobs,$rep,$gr,$gm,$er,$rc2,$ws,$rp,$rq,$nsp,${ig1:-NA},${ig2:-NA}" >> "$DRES"
  echo "$cfg,$num,$jobs,$rep,i1,$(ext fillrandom "$l1"),$(ext overwrite "$l1"),rc=$r1" >> "$RES"
  echo "$cfg,$num,$jobs,$rep,i2,$(ext fillrandom "$l2"),$(ext overwrite "$l2"),rc=$r2" >> "$RES"
  echo "    i1 FR=$(ext fillrandom "$l1") OW=$(ext overwrite "$l1") rc=$r1 | i2 FR=$(ext fillrandom "$l2") OW=$(ext overwrite "$l2") rc=$r2"
  echo "    device: gc_mig=$gm erases=$er R=$rp->$rq nospc=$nsp dev_write=$((ws/1073741824))GB"
  umount "$AUX" 2>/dev/null; dmsetup remove "$DMNAME" 2>/dev/null||true
}

# rep is the OUTER loop: a crash mid-run still leaves complete earlier reps.
for rep in $(seq 1 ${REP:-1}); do for num in $NUMS; do for jobs in $JOBSET; do for cfg in $CONFIGS; do
  run_cfg "$cfg" "$num" "$jobs" "$rep"
done; done; done; done
echo "UNIFIED_DUAL_DONE -> $RES"
