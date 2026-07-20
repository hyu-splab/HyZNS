#!/bin/bash
# FigC — L0 SST placement in HyZNS:  L0->ZNS  vs  L0->CNS(R-region)
# WAL stays on ZNS for BOTH (isolate the L0-placement effect). Identical dm-hyzns
# stack; only which binary (L0_POSIX) and thus where L0 SSTs land differs.
#
#   db_bench_zns    : L0 (and WAL) on ZNS S-region          (baseline)
#   db_bench_l0cns  : L0 SST -> aux F2FS on R-region (CNS)  [#define L0_POSIX]; WAL still ZNS
#
# Build first: ./scripts/legacy/build_wal_variants.sh (db_bench_zns) + ./scripts/legacy/build_l0_variants.sh
# Usage: sudo ./scripts/legacy/figC.sh        |  sudo NUM=20000000 R_ZONES=16 ./scripts/legacy/figC.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYZNS=../dm-hyzns
source "$DMHYZNS/scripts/_lib.sh"

BACKING=${BACKING:-/dev/nvme0n1}; DMNAME=${DMNAME:-hyzns0}; DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}; ZS=$HYZNS_ZONE_SECTORS
R_ZONES=${R_ZONES:-16}              # R big enough to hold L0 comfortably (baseline, no growCNS)
NUM=${NUM:-20000000}; READS=${READS:-2000000}
KEY=${KEY:-20}; VAL=${VAL:-800}; AOZ=${AOZ:-7}; JOBS=${JOBS:-8}
# readwhilewriting is not in the default chain (the unthrottled writer starves the
# readers; run it separately with --duration if needed). fill/read/overwrite cover
# the L0 write-flush / read / compaction signals.
BENCH=${BENCH:-fillrandom,readrandom,overwrite}
RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1))
FS_URI="zenfs://dev:dm-0/${R_ZONES}/${S_LAST}/${AOZ}/0/4294967296"
TS=$(date +%Y%m%d_%H%M%S)
TAG="${TS}_figC_${BENCH//,/+}_n${NUM}_r${R_ZONES}g_ao${AOZ}"

setup_topology() {
    umount "$AUX" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    lsmod | awk '{print $1}' | grep -qx dm_hyzns || "$DMHYZNS/scripts/load.sh" >/dev/null
    reset_device_full_r "$BACKING" || true
    local sz; sz=$(blockdev --getsz "$BACKING")
    echo "0 $sz hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) 2>/dev/null || true
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
    mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$R_ZONES" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
}

run_variant() {                                  # <label> <binary>
    local label=$1 bin=$2
    [[ -x $bin ]] || { echo "missing $bin — build it"; exit 1; }
    echo "=========================================================="
    echo "[$label] L0=$( [[ $label == l0cns ]] && echo 'R-region (CNS aux F2FS)' || echo 'ZNS S-region'); WAL=ZNS"
    setup_topology
    local log="$RESULTS/${TAG}_${label}.log"
    echo "[$label] $BENCH num=$NUM reads=$READS ao=$AOZ R=${R_ZONES}GiB -> $log"
    "$bin" --fs_uri="$FS_URI" --benchmarks="${BENCH},stats" \
        --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --reads="$READS" --key_size="$KEY" --value_size="$VAL" \
        --compression_type=none --max_background_jobs="$JOBS" \
        --statistics --stats_interval_seconds=1 &> "$log"
    dmsetup status "$DMNAME" > "${log%.log}_dmstatus.txt" 2>/dev/null || true
    echo "[$label] results:"; grep -E "^($(echo $BENCH|sed 's/,/|/g')) " "$log" | sed 's/^/    /'
    umount "$AUX" 2>/dev/null || true
}

mkdir -p "$RESULTS"
echo "FigC: L0->ZNS vs L0->CNS  (NUM=$NUM, R=${R_ZONES}GiB, WAL=ZNS, ao=$AOZ)"
run_variant l0zns  ./db_bench_zns
run_variant l0cns  ./db_bench_l0cns
dmsetup remove "$DMNAME" 2>/dev/null || true
echo; echo "logs: $RESULTS/${TAG}_{l0zns,l0cns}.log"
