#!/bin/bash
# Phase B — 4-way WAL/L0 placement on HyZNS (single instance):
#   baseline      db_bench_zns         WAL=ZNS  L0=ZNS
#   walcns        db_bench_posix       WAL=CNS  L0=ZNS
#   l0cns         db_bench_l0cns       WAL=ZNS  L0=CNS
#   walcns_l0cns  db_bench_wal_l0_cns  WAL=CNS  L0=CNS
# Identical dm-hyhost stack + workloads; only the binary (macro-baked placement) differs.
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYHOST=../dm-hyhost; source "$DMHYHOST/scripts/_lib.sh"
BACKING=${BACKING:-/dev/nvme0n1}; DMNAME=${DMNAME:-hyhost0}; DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}; ZS=$HYHOST_ZONE_SECTORS
R_ZONES=${R_ZONES:-16}; NUM=${NUM:-20000000}; READS=${READS:-2000000}
KEY=20; VAL=800; AOZ=${AOZ:-7}; JOBS=8
BENCH=${BENCH:-fillrandom,readrandom,overwrite}
RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1))
FS_URI="zenfs://dev:dm-0/${R_ZONES}/${S_LAST}/${AOZ}/0/4294967296"
TS=$(date +%Y%m%d_%H%M%S); TAG="${TS}_phaseB_${BENCH//,/+}_n${NUM}_r${R_ZONES}g_ao${AOZ}"

setup_topology() {
    umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
    lsmod | awk '{print $1}' | grep -qx dm_hyhost || "$DMHYHOST/scripts/load.sh" >/dev/null
    reset_device_full_r "$BACKING" || true
    local sz; sz=$(blockdev --getsz "$BACKING")
    echo "0 $sz hyhost $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) 2>/dev/null || true
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
    mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$R_ZONES" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
}
run_variant() {                                  # <label> <binary>
    local label=$1 bin=$2
    [[ -x $bin ]] || { echo "missing $bin"; return; }
    echo "========== [$label] $bin =========="
    setup_topology
    local log="$RESULTS/${TAG}_${label}.log"
    "$bin" --fs_uri="$FS_URI" --benchmarks="${BENCH},stats" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --reads="$READS" --key_size="$KEY" --value_size="$VAL" \
        --compression_type=none --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 &> "$log"
    dmsetup status "$DMNAME" > "${log%.log}_dmstatus.txt" 2>/dev/null || true
    echo "[$label] $(grep -E "^($(echo $BENCH|sed 's/,/|/g')) " "$log" | grep -oE '^[a-z]+ +: +[0-9.]+ micros.* ops/sec' | sed 's/  */ /g' | paste -sd' | ')"
    umount "$AUX" 2>/dev/null || true
}
mkdir -p "$RESULTS"
echo "Phase B 4-way: NUM=$NUM R=${R_ZONES}G ao=$AOZ bench=$BENCH variants=${VARIANTS:-all}"
declare -A BIN=( [baseline]=./db_bench_zns [walcns]=./db_bench_posix [l0cns]=./db_bench_l0cns [walcns_l0cns]=./db_bench_wal_l0_cns )
for v in ${VARIANTS:-baseline walcns l0cns walcns_l0cns}; do
  run_variant "$v" "${BIN[$v]}"
done
dmsetup remove "$DMNAME" 2>/dev/null || true
echo "TAG=$TAG"
