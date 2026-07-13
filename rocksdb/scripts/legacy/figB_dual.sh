#!/bin/bash
#
# FigB (dual-instance) — matches the real ZN540 2-tenant setup: TWO concurrent
# db_bench processes, each capped at ao_zones=7, sharing one HyZNS device.
#
#   dm-hyhost (R = aux F2FS / CNS, S = ZNS) split:
#     R = [0, R_ZONES)              aux F2FS (both instances' LOG/LOCK[/WAL])
#     S = [R_ZONES, MID)  -> ZenFS instance 1  (aux_path = /mnt/aux/i1)
#         [MID, TOTAL)    -> ZenFS instance 2  (aux_path = /mnt/aux/i2)
#
#   db_bench_zns   : WAL on each instance's ZNS range
#   db_bench_posix : WAL on the shared aux F2FS (R-region) per instance
#
# Aggregate throughput = i1 + i2 (they run at the same time). Compare variants.
# Build variants first: ./scripts/legacy/build_wal_variants.sh
#
# Usage: sudo ./scripts/legacy/figB_dual.sh
#        sudo NUM=40000000 R_ZONES=4 AOZ=7 BENCH=fillrandom ./scripts/legacy/figB_dual.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYHOST=../dm-hyhost
source "$DMHYHOST/scripts/_lib.sh"

BACKING=${BACKING:-/dev/nvme0n1}
DMNAME=${DMNAME:-hyhost0}
DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}
ZS=$HYHOST_ZONE_SECTORS
R_ZONES=${R_ZONES:-4}
NUM=${NUM:-20000000}          # per instance
KEY=${KEY:-20}; VAL=${VAL:-800}
AOZ=${AOZ:-7}; BENCH=${BENCH:-fillrandom}; JOBS=${JOBS:-8}
GC=${GC:-true}            # ZenFS enable_gc; false avoids the GC-worker crash on tight devices
RESULTS=results
ZENFS=./plugin/zenfs/util/zenfs
TOTAL_ZONES=$(( $(blockdev --getsz "$BACKING") / ZS ))
# zenfs --end_zone / fs_uri field-2 are INCLUSIVE zone INDICES (not counts).
# S region = [R_ZONES .. TOTAL_ZONES-1]; split into I1=[S_START..MID], I2=[I2_START..S_LAST].
S_START=$R_ZONES
S_LAST=$(( TOTAL_ZONES - 1 ))
MID=$(( (S_START + S_LAST) / 2 ))
I2_START=$(( MID + 1 ))
TS=$(date +%Y%m%d_%H%M%S)
TAG="${TS}_figBdual_${BENCH//,/+}_n${NUM}_r${R_ZONES}g_ao${AOZ}"

ops_of() { grep -E "^${BENCH%%,*}" "$1" 2>/dev/null | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }

setup() {
    umount "$AUX" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
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

run_variant() {                                # <label> <binary>
    local label=$1 bin=$2
    [[ -x $bin ]] || { echo "missing $bin — run ./scripts/legacy/build_wal_variants.sh"; exit 1; }
    echo "=========================================================="
    echo "[$label] dual-instance: S split [$S_START..$MID] + [$I2_START..$S_LAST], ao=$AOZ each"
    setup
    local l1="$RESULTS/${TAG}_${label}_i1.log" l2="$RESULTS/${TAG}_${label}_i2.log"
    echo "[$label] launching 2 concurrent $BENCH num=$NUM each ..."
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" \
        --benchmarks="${BENCH},stats" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
        --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 ${EXTRA:-} &> "$l1" &
    local p1=$!
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" \
        --benchmarks="${BENCH},stats" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
        --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 ${EXTRA:-} &> "$l2" &
    local p2=$!
    wait "$p1"; local r1=$?; wait "$p2"; local r2=$?
    dmsetup status "$DMNAME" > "$RESULTS/${TAG}_${label}_dmstatus.txt" 2>/dev/null || true
    local o1 o2; o1=$(ops_of "$l1"); o2=$(ops_of "$l2")
    echo "[$label] i1=${o1:-FAIL} ops/s (rc=$r1)  i2=${o2:-FAIL} ops/s (rc=$r2)  TOTAL=$(( ${o1:-0} + ${o2:-0} )) ops/s"
    umount "$AUX" 2>/dev/null || true
}

mkdir -p "$RESULTS"
echo "FigB dual: WAL->ZNS vs WAL->R(CNS), 2 instances ao=$AOZ  (NUM=$NUM/inst, R=${R_ZONES}GiB)"
for V in ${VARIANTS:-zns posix}; do
  case $V in
    zns)   run_variant zns   ./db_bench_zns   ;;
    posix) run_variant posix ./db_bench_posix ;;
  esac
done
dmsetup remove "$DMNAME" 2>/dev/null || true
echo
echo "logs: $RESULTS/${TAG}_{zns,posix}_i{1,2}.log"
