#!/bin/bash
#
# FigB — WAL placement in HyZNS:  WAL->ZNS  vs  WAL->R(CNS)
#
# Both runs use the IDENTICAL dm-hyhost stack so only the WAL path differs:
#
#     RocksDB / db_bench  (fillrandom)
#               |
#     ZenFS (S-region, ZNS)  +  aux F2FS (R-region, via dm-hyhost L2P)
#               |
#     /dev/mapper/hyhost0   (R = R_ZONES GiB, S = rest;  ABA STATIC — no modify)
#               |
#     /dev/nvme0n1          (FEMU HYHOSTSSD, femu_mode=5)
#
#   db_bench_zns   : WAL stays on ZNS S-region        (wal_type=0)
#   db_bench_posix : WAL -> aux F2FS file on R-region (wal_type=2, O_SYNC)
#
# Build the variants first:  ./scripts/legacy/build_wal_variants.sh
#
# Usage:  sudo ./scripts/legacy/figB.sh           # both variants, default params
#         sudo NUM=20000000 R_ZONES=16 ./scripts/legacy/figB.sh
set -uo pipefail
cd "$(dirname "$0")/../.."                       # rocksdb/
DMHYHOST=../dm-hyhost
source "$DMHYHOST/scripts/_lib.sh"            # reset_device_full_r, HYHOST_ZONE_SECTORS

# ---- config (env-overridable) ----
BACKING=${BACKING:-/dev/nvme0n1}
DMNAME=${DMNAME:-hyhost0}
DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}
ZS=$HYHOST_ZONE_SECTORS                       # 1 GiB zone in 512B sectors
R_ZONES=${R_ZONES:-4}                         # R-region size in zones (= GiB); real nvme3n1 = 4 GiB
NUM=${NUM:-40000000}                          # real-server num
KEY=${KEY:-20}
VAL=${VAL:-800}
AOZ=${AOZ:-7}                                 # active open zones (real used 7/instance)
BENCH=${BENCH:-fillrandom}                    # fillrandom | fillseq | overwrite | fillrandom,overwrite
JOBS=${JOBS:-8}
RESULTS=results
ZENFS=./plugin/zenfs/util/zenfs
# S-region spans [R_ZONES .. last zone]; compute the last index from the
# device size so the script is device-size agnostic.
TOTAL_ZONES=$(( $(blockdev --getsz "$BACKING") / ZS ))
S_LAST=$(( TOTAL_ZONES - 1 ))
# fs_uri fields: dev / start_zone / end_zone(inclusive) / ao_zones / cns_start / cns_len
FS_URI="zenfs://dev:dm-0/${R_ZONES}/${S_LAST}/${AOZ}/0/4294967296"
TS=$(date +%Y%m%d_%H%M%S)
TAG="${TS}_figB_${BENCH//,/+}_n${NUM}_r${R_ZONES}g_ao${AOZ}"

setup_topology() {
    umount "$AUX" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    lsmod | awk '{print $1}' | grep -qx dm_hyhost || "$DMHYHOST/scripts/load.sh" >/dev/null
    reset_device_full_r "$BACKING" || true
    local sz; sz=$(blockdev --getsz "$BACKING")
    echo "0 $sz hyhost $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS))
    # aux F2FS on the R-region (LOG/LOCK always; WAL too under WAL_POSIX)
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
    mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
    # ZenFS on the S-region (reserved aux zones auto-detected = R_ZONES).
    # ao_zones / enable_gc match the real-server scripts.
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$R_ZONES" --end_zone="$S_LAST" \
        --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
}

run_variant() {                               # <label> <binary>
    local label=$1 bin=$2
    local log="$RESULTS/${TAG}_${label}.log"
    [[ -x $bin ]] || { echo "missing $bin — run ./scripts/legacy/build_wal_variants.sh"; exit 1; }
    echo "=========================================================="
    echo "[$label] WAL=$( [[ $label == posix ]] && echo 'R-region (F2FS POSIX, O_SYNC)' || echo 'ZNS S-region')"
    echo "[$label] setup: dm-hyhost R=${R_ZONES}GiB/S=rest, aux F2FS + ZenFS"
    setup_topology
    echo "[$label] $BENCH num=$NUM key=$KEY val=$VAL ao=$AOZ jobs=$JOBS -> $log"
    ZENFS_L0_ON_AUX=0 "$bin" \
        --fs_uri="$FS_URI" \
        --benchmarks="${BENCH},stats" \
        --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" \
        --compression_type=none \
        --max_background_jobs="$JOBS" \
        --histogram --statistics --stats_interval_seconds=1 \
        &> "$log"
    # dm-hyhost R-region counters (proof: WAL hits R only under POSIX)
    dmsetup status "$DMNAME" > "${log%.log}_dmstatus.txt" 2>/dev/null || true
    echo "[$label] $(grep -E "^(${BENCH//,/|})" "$log" | tail -1)"
    umount "$AUX" 2>/dev/null || true
}

mkdir -p "$RESULTS"
echo "FigB: WAL->ZNS vs WAL->R(CNS) on HyZNS  (NUM=$NUM, R=${R_ZONES}GiB, static ABA)"
run_variant zns   ./db_bench_zns
run_variant posix ./db_bench_posix
dmsetup remove "$DMNAME" 2>/dev/null || true

echo
echo "logs:  $RESULTS/${TAG}_{zns,posix}.log"
echo "plot:  python3.9 scripts/legacy/plot_figB.py $RESULTS/${TAG}_zns.log $RESULTS/${TAG}_posix.log"
# auto-plot if matplotlib-capable python is available
if command -v python3.9 >/dev/null && python3.9 -c "import matplotlib" 2>/dev/null; then
    python3.9 scripts/legacy/plot_figB.py "$RESULTS/${TAG}_zns.log" "$RESULTS/${TAG}_posix.log" || true
fi
