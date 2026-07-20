#!/bin/bash
# Phase D — dual-instance (2-tenant) WAL/L0 placement on HyZNS. Mirrors Phase B's
# 4-way but with TWO concurrent db_bench sharing one device (real ZN540 2-tenant):
#   R = [0,R_ZONES)  shared aux F2FS  (both instances' LOG/LOCK, WAL if walcns, L0 if l0cns)
#   S = [R_ZONES,MID] -> inst1   [MID+1,LAST] -> inst2   (ZNS, split in half)
# Variants (binary = compile-time placement):
#   baseline db_bench_zns  | walcns db_bench_posix | l0cns db_bench_l0cns | walcns_l0cns db_bench_wal_l0_cns
# Aggregate throughput = i1 + i2. NOTE: both instances' CNS traffic shares the one R
# region (per-instance R is infeasible — mkfs.f2fs rejects a dm-linear slice), so dual
# CNS variants pay shared-R contention. Pick R_ZONES big enough for the combined L0
# (l0cns needs roughly 2x the single-instance threshold).
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYZNS=../dm-hyzns; source "$DMHYZNS/scripts/_lib.sh"
BACKING=${BACKING:-/dev/nvme0n1}; DMNAME=${DMNAME:-hyzns0}; DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}; ZS=$HYZNS_ZONE_SECTORS
R_ZONES=${R_ZONES:-24}; NUM=${NUM:-20000000}   # NUM per instance
READS=${READS:-2000000}                         # cap readrandom (else it reads NUM keys = too slow)
KEY=20; VAL=800; AOZ=${AOZ:-7}; JOBS=8; GC=${GC:-true}
BENCH=${BENCH:-fillrandom}; RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS ))
S_START=$R_ZONES; S_LAST=$((TOTAL-1)); MID=$(( (S_START+S_LAST)/2 )); I2_START=$((MID+1))
TS=$(date +%Y%m%d_%H%M%S); TAG="${TS}_phaseDdual_${BENCH//,/+}_n${NUM}_r${R_ZONES}g_ao${AOZ}"
declare -A BIN=( [baseline]=./db_bench_zns [walcns]=./db_bench_posix [l0cns]=./db_bench_l0cns [walcns_l0cns]=./db_bench_wal_l0_cns )
ops_of() { grep -E "^${BENCH%%,*}" "$1" 2>/dev/null | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }

setup() {
    umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
    lsmod | awk '{print $1}' | grep -qx dm_hyzns || "$DMHYZNS/scripts/load.sh" >/dev/null
    reset_device_full_r "$BACKING" || true
    local sz; sz=$(blockdev --getsz "$BACKING")
    echo "0 $sz hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS))
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
    mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"; mkdir -p "$AUX/i1" "$AUX/i2"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i1" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$S_START" --end_zone="$MID" --ao_zones="$AOZ" --enable_gc=$GC --force >/dev/null 2>&1
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i2" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$I2_START" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=$GC --force >/dev/null 2>&1
}
run_variant() {
    local label=$1 bin=${BIN[$1]}
    [[ -x $bin ]] || { echo "missing $bin"; return; }
    echo "========== [$label] dual S=[$S_START..$MID]+[$I2_START..$S_LAST] ao=$AOZ each =========="
    setup
    local l1="$RESULTS/${TAG}_${label}_i1.log" l2="$RESULTS/${TAG}_${label}_i2.log"
    "$bin" --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" --benchmarks="${BENCH},stats" \
        --use_direct_io_for_flush_and_compaction --num="$NUM" --reads="$READS" --key_size="$KEY" --value_size="$VAL" \
        --compression_type=none --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 &> "$l1" &
    local p1=$!
    "$bin" --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" --benchmarks="${BENCH},stats" \
        --use_direct_io_for_flush_and_compaction --num="$NUM" --reads="$READS" --key_size="$KEY" --value_size="$VAL" \
        --compression_type=none --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 &> "$l2" &
    local p2=$!
    wait "$p1"; local r1=$?; wait "$p2"; local r2=$?
    dmsetup status "$DMNAME" > "$RESULTS/${TAG}_${label}_dmstatus.txt" 2>/dev/null || true
    local o1 o2; o1=$(ops_of "$l1"); o2=$(ops_of "$l2")
    local err=$(grep -lE 'put error|write failed|allocation failure|No space' "$l1" "$l2" 2>/dev/null|wc -l)
    echo "[$label] i1=${o1:-FAIL}(rc=$r1) i2=${o2:-FAIL}(rc=$r2) TOTAL=$(( ${o1:-0}+${o2:-0} )) ops/s  err_logs=$err"
    umount "$AUX" 2>/dev/null || true
}
mkdir -p "$RESULTS"
echo "Phase D dual: NUM=$NUM/inst R=${R_ZONES}G ao=$AOZ bench=$BENCH variants=${VARIANTS:-baseline l0cns walcns walcns_l0cns}"
for v in ${VARIANTS:-baseline l0cns walcns walcns_l0cns}; do run_variant "$v"; done
dmsetup remove "$DMNAME" 2>/dev/null || true
echo "TAG=$TAG"
