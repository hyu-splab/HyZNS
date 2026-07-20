#!/bin/bash
# Decisive read-fairness test: dual L0->ZNS vs L0->CNS readrandom with a COLD
# page cache. Prefill (fillseq) and read run as SEPARATE db_bench processes with
# `echo 3 > drop_caches` in between, so neither variant carries a warm cache from
# the in-process prefill. ZNS reads are O_DIRECT (never cached); this removes the
# only way L0-CNS could look faster (buffered/page-cache hits).
# Usage: sudo R_ZONES=48 NUM=10000000 READS=2000000 ./scripts/legacy/l0_readcold_dual.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYZNS=../dm-hyzns; source "$DMHYZNS/scripts/_lib.sh"
BACKING=${BACKING:-/dev/nvme0n1}; DMNAME=${DMNAME:-hyzns0}; DEV=/dev/mapper/$DMNAME
AUX=${AUX:-/mnt/aux}; ZS=$HYZNS_ZONE_SECTORS
R_ZONES=${R_ZONES:-48}; NUM=${NUM:-10000000}; KEY=${KEY:-20}; VAL=${VAL:-800}
AOZ=${AOZ:-7}; JOBS=${JOBS:-8}; GC=${GC:-true}; READS=${READS:-2000000}
RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS ))
S_START=$R_ZONES; S_LAST=$((TOTAL-1)); MID=$(( (S_START+S_LAST)/2 )); I2_START=$((MID+1))
TS=$(date +%Y%m%d_%H%M%S); TAG="${TS}_l0readcold_n${NUM}_r${R_ZONES}g"
declare -A BIN=( [zns]=./db_bench_zns [l0cns]=./db_bench_l0cns )

ops_of() { grep -E "^$2 " "$1" 2>/dev/null | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }

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

# one db_bench instance: $1=bin $2=zoneRange(start/mid) $3=benchmarks $4=logfile $5=extra
launch() {
    ZENFS_L0_ON_AUX=0 "$1" --fs_uri="zenfs://dev:dm-0/$2/$AOZ/0/4294967296" \
        --benchmarks="$3" --use_direct_io_for_flush_and_compaction \
        --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
        --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 $5 &> "$4"
}

run_variant() {
    local v=$1 bin=${BIN[$v]}
    [[ -x $bin ]] || { echo "missing $bin"; return; }
    setup
    # phase 1: prefill (fillseq), processes EXIT
    launch "$bin" "$S_START/$MID"   "fillseq"          "$RESULTS/${TAG}_${v}_fill_i1.log" "" &
    launch "$bin" "$I2_START/$S_LAST" "fillseq"        "$RESULTS/${TAG}_${v}_fill_i2.log" "" &
    wait
    sync; echo 3 > /proc/sys/vm/drop_caches    # COLD cache before reads
    # phase 2: readrandom on existing DB, fresh (cold) processes
    local rx="--use_existing_db=1 --reads=$READS --threads=1 --use_direct_reads=true"
    launch "$bin" "$S_START/$MID"   "readrandom,stats" "$RESULTS/${TAG}_${v}_read_i1.log" "$rx" &
    launch "$bin" "$I2_START/$S_LAST" "readrandom,stats" "$RESULTS/${TAG}_${v}_read_i2.log" "$rx" &
    wait
    dmsetup status "$DMNAME" > "$RESULTS/${TAG}_${v}_dmstatus.txt" 2>/dev/null || true
    local o1 o2; o1=$(ops_of "$RESULTS/${TAG}_${v}_read_i1.log" readrandom); o2=$(ops_of "$RESULTS/${TAG}_${v}_read_i2.log" readrandom)
    local g1; g1=$(grep -oE 'db.get.micros P50 : [0-9.]+' "$RESULTS/${TAG}_${v}_read_i1.log" | head -1)
    echo "RESULT $v read i1=${o1:-FAIL} i2=${o2:-FAIL} TOTAL=$(( ${o1:-0}+${o2:-0} ))  [$g1]"
    umount "$AUX" 2>/dev/null || true
}

mkdir -p "$RESULTS"
echo "L0 read-COLD dual: NUM=$NUM/inst R=${R_ZONES}G reads=$READS (prefill+drop_caches+read separate procs)"
for v in ${VARIANTS:-zns l0cns}; do run_variant "$v"; done
dmsetup remove "$DMNAME" 2>/dev/null || true
echo "TAG=$TAG"; echo "DONE"
