#!/bin/bash
# Diagnostic: is the L0-CNS readrandom advantage just OS page cache?
# Runs in-process (fillseq,readrandom) — the path that works (no reopen) — while a
# background dropper flushes the page cache every 1s, so cached reads are forced to
# the device. ZNS (O_DIRECT) is unaffected; if L0-CNS falls to ~ZNS, the gap was cache.
# Single instance for speed. Usage: sudo R_ZONES=48 NUM=10000000 ./scripts/legacy/l0_readdrop_diag.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYZNS=../dm-hyzns; source "$DMHYZNS/scripts/_lib.sh"
BACKING=${BACKING:-/dev/nvme0n1}; DMNAME=${DMNAME:-hyzns0}; DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}; ZS=$HYZNS_ZONE_SECTORS
R_ZONES=${R_ZONES:-48}; NUM=${NUM:-10000000}; KEY=${KEY:-20}; VAL=${VAL:-800}
AOZ=${AOZ:-7}; JOBS=${JOBS:-8}; READS=${READS:-2000000}
RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS ))
S_START=$R_ZONES; S_LAST=$((TOTAL-1))
TS=$(date +%Y%m%d_%H%M%S); TAG="${TS}_l0readdrop_n${NUM}_r${R_ZONES}g"
declare -A BIN=( [zns]=./db_bench_zns [l0cns]=./db_bench_l0cns )

run_variant() {
    local v="${1:?variant}"; local drop="${2:?drop}"; local bin   # drop=1 => bg page-cache dropper
    case "$v" in zns) bin=./db_bench_zns;; l0cns) bin=./db_bench_l0cns;; *) echo "bad variant $v"; return;; esac
    umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
    lsmod | awk '{print $1}' | grep -qx dm_hyzns || "$DMHYZNS/scripts/load.sh" >/dev/null
    reset_device_full_r "$BACKING" || true
    local sz; sz=$(blockdev --getsz "$BACKING")
    echo "0 $sz hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS))
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
    mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"; mkdir -p "$AUX/i1"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i1" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$S_START" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
    local dpid=""
    if [[ $drop == 1 ]]; then ( while :; do echo 1 > /proc/sys/vm/drop_caches; sleep 1; done ) & dpid=$!; fi
    local log="$RESULTS/${TAG}_${v}_drop${drop}.log"
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="zenfs://dev:dm-0/$S_START/$S_LAST/$AOZ/0/4294967296" \
        --benchmarks="fillseq,readrandom,stats" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
        --reads="$READS" --threads=1 --use_direct_reads=true \
        --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=10 &> "$log"
    [[ -n $dpid ]] && kill "$dpid" 2>/dev/null
    local rr g50 g95; rr=$(grep -E '^readrandom' "$log" | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+')
    g50=$(grep -oE 'db.get.micros P50 : [0-9.]+' "$log" | head -1 | grep -oE '[0-9.]+$')
    g95=$(grep -oE 'P95 : [0-9.]+' "$log" | head -1 | grep -oE '[0-9.]+$')
    echo "RESULT $v drop=$drop readrandom=${rr:-FAIL} ops/sec  get.P50=${g50}us get.P95=${g95}us"
    umount "$AUX" 2>/dev/null || true
}

mkdir -p "$RESULTS"
echo "L0 read-drop diag: single inst NUM=$NUM R=${R_ZONES}G reads=$READS"
run_variant zns   0
run_variant l0cns 0
run_variant l0cns 1     # same but page cache continuously dropped
dmsetup remove "$DMNAME" 2>/dev/null || true
echo "TAG=$TAG"; echo "DONE"
