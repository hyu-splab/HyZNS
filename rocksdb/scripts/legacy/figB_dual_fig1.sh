#!/bin/bash
# FigB-dual Fig1 sweep — extends figB_dual.sh to the 5 Fig1 workloads:
#   fillseq, fillrandom, overwrite, readseq, readrandom
# Comparison: WAL->ZNS (db_bench_zns) vs WAL->CNS/R (db_bench_posix), TWO concurrent
# instances sharing one HyZNS device (R = shared aux F2FS, S = ZNS split in half).
# Reads prefill with fillseq then read (--reads/--threads/--use_direct_reads, per run-insh).
# Aggregate = i1+i2. Usage: sudo NUM=25000000 ./scripts/legacy/figB_dual_fig1.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYHOST=../dm-hyhost; source "$DMHYHOST/scripts/_lib.sh"
BACKING=${BACKING:-/dev/nvme0n1}; DMNAME=${DMNAME:-hyhost0}; DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}; ZS=$HYHOST_ZONE_SECTORS
R_ZONES=${R_ZONES:-4}; NUM=${NUM:-25000000}; KEY=${KEY:-20}; VAL=${VAL:-800}
AOZ=${AOZ:-7}; JOBS=${JOBS:-8}; GC=${GC:-true}
THREADS=${THREADS:-1}; READS=${READS:-$NUM}        # run-insh: reads = num/threads
RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS ))
S_START=$R_ZONES; S_LAST=$((TOTAL-1)); MID=$(( (S_START+S_LAST)/2 )); I2_START=$((MID+1))
TS=$(date +%Y%m%d_%H%M%S); TAG="${TS}_figBdualFig1_n${NUM}_r${R_ZONES}g_ao${AOZ}"

# workload -> benchmark chain / result line to extract / read-flags?
declare -A CHAIN=( [fillseq]="fillseq" [fillrandom]="fillrandom" [overwrite]="fillrandom,overwrite" [readseq]="fillseq,readseq" [readrandom]="fillseq,readrandom" )
declare -A EXTRACT=( [fillseq]="fillseq" [fillrandom]="fillrandom" [overwrite]="overwrite" [readseq]="readseq" [readrandom]="readrandom" )
declare -A ISREAD=( [readseq]=1 [readrandom]=1 )
WORKLOADS=${WORKLOADS:-"fillseq fillrandom overwrite readseq readrandom"}
declare -A BIN=( [zns]=./db_bench_zns [posix]=./db_bench_posix [posixbuf]=./db_bench_posix_buf [l0cns]=./db_bench_l0cns )

ops_of() { grep -E "^$2 " "$1" 2>/dev/null | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }

setup() {
    umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
    lsmod | awk '{print $1}' | grep -qx dm_hyhost || "$DMHYHOST/scripts/load.sh" >/dev/null
    reset_device_full_r "$BACKING" || true
    local sz; sz=$(blockdev --getsz "$BACKING")
    echo "0 $sz hyhost $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS))
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
    mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"; mkdir -p "$AUX/i1" "$AUX/i2"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i1" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$S_START" --end_zone="$MID" --ao_zones="$AOZ" --enable_gc=$GC --force >/dev/null 2>&1
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i2" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$I2_START" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=$GC --force >/dev/null 2>&1
}

run_one() {                                  # <workload> <variant>
    local wl=$1 v=$2 bin=${BIN[$2]} bench=${CHAIN[$1]} ekey=${EXTRACT[$1]}
    [[ -x $bin ]] || { echo "missing $bin"; return; }
    local extra=""; [[ -n ${ISREAD[$wl]:-} ]] && extra="--reads=$READS --threads=$THREADS --use_direct_reads=true"
    setup
    local l1="$RESULTS/${TAG}_${wl}_${v}_i1.log" l2="$RESULTS/${TAG}_${wl}_${v}_i2.log"
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" \
        --benchmarks="${bench},stats" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
        --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 $extra &> "$l1" &
    local p1=$!
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" \
        --benchmarks="${bench},stats" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
        --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 $extra &> "$l2" &
    local p2=$!
    wait "$p1"; local r1=$?; wait "$p2"; local r2=$?
    dmsetup status "$DMNAME" > "$RESULTS/${TAG}_${wl}_${v}_dmstatus.txt" 2>/dev/null || true
    local o1 o2 err; o1=$(ops_of "$l1" "$ekey"); o2=$(ops_of "$l2" "$ekey")
    err=$(grep -lE 'put error|write failed|allocation failure|No space' "$l1" "$l2" 2>/dev/null|wc -l)
    echo "RESULT $wl $v i1=${o1:-FAIL} i2=${o2:-FAIL} TOTAL=$(( ${o1:-0}+${o2:-0} )) (rc=$r1/$r2 err_logs=$err)"
    umount "$AUX" 2>/dev/null || true
}

mkdir -p "$RESULTS"
echo "FigB-dual Fig1: WAL->ZNS vs WAL->CNS  NUM=$NUM/inst R=${R_ZONES}G ao=$AOZ threads=$THREADS reads=$READS"
echo "workloads: $WORKLOADS  variants: ${VARIANTS:-zns posix}"
for wl in $WORKLOADS; do
  echo "########## WORKLOAD=$wl (bench=${CHAIN[$wl]}, extract=${EXTRACT[$wl]}) ##########"
  for v in ${VARIANTS:-zns posix}; do run_one "$wl" "$v"; done
done
dmsetup remove "$DMNAME" 2>/dev/null || true
echo "TAG=$TAG"
echo "FIG1_DONE"
