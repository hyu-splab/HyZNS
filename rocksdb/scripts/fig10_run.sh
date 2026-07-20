#!/bin/bash
#
# fig10_run.sh — Fig10 (§eval-rocks-wal): "Placing WAL files in the CNS area",
#   DUAL RocksDB instance, HyZNS only. Repeats the Fig1 motivation (two tenants,
#   WAL on ZNS vs CNS) with the device replaced by HyZNS.
#
#   Two db_bench instances share one HyZNS device; only the WAL path differs:
#     WAL_ZNS (db_bench_zns)   : WAL on each instance's own ZNS S-range (default)
#     WAL_CNS (db_bench_posix) : WAL -> shared aux F2FS on the CNS R-region (O_SYNC)
#
#         inst1 ZenFS [R..MID]     inst2 ZenFS [MID+1..last]     (S-region, split)
#                  \                        /
#                   \   WAL_CNS: both WALs -> F2FS on R (CNS)
#                    \______  ______/  (aux i1, i2)
#                           \/
#                    /dev/mapper/hyzns0  (R = CNS = R_ZONES GiB, S = ZNS = rest)
#                           |
#                    /dev/nvme0n1 (FEMU HyZNS)
#
#   Each instance runs `fillrandom,overwrite` (populate FR NUM KV, then overwrite
#   OW); we SUM the two instances = aggregate device throughput. Sweeping the
#   background-thread count (1 memtable-flush + compaction) {2,4,8}: at >2
#   threads, ZNS active-zone lock contention makes WAL_CNS win (paper: +37%/+49%
#   FR/OW at 4 threads) by excluding the 2 WAL client threads from the ZNS
#   competing set.
#
#   Prereq: db_bench_zns + db_bench_posix built:
#       sudo ./scripts/build_wal_variants.sh
#
#   Usage:
#     sudo GEOM=8x4 ./scripts/fig10_run.sh                  # jobs 2 4 8, NUM 20M
#     sudo JOBS="4" NUM=10000000 ./scripts/fig10_run.sh     # quick single cell
#     sudo CFGS="CNS" ./scripts/fig10_run.sh                # one WAL variant
set -uo pipefail
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPTDIR/.."                                     # rocksdb/
DMHYZNS=../dm-hyzns
source "$SCRIPTDIR/_fig_lib.sh"                        # aba_ok
source "$DMHYZNS/scripts/_lib.sh"                     # HYZNS_ZONE_SECTORS, reset_device_full_r

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- config (env-overridable) ------------------------------------------
GEOM=${GEOM:-8x4}
NUM=${NUM:-10000000}                 # KV-pairs per instance (paper: 20M = 10M*2instances)
# PRE_NUM>0 = tree-depth study, two invocations per instance:
#   ① fillrandom PRE_NUM (MEASURED — the FR column: cost of building the deep
#     DB from empty; full instrumentation incl. ===REPORT + LOG_pre capture)
#   ② overwrite NUM writes over the same PRE_NUM keyspace (use_existing_db;
#     a same-invocation fillrandom would be skipped, and --writes would cap it)
# Levels deepen with PRE_NUM (~420B/key on flash w/ snappy): 10M=3lvl, 40M=4lvl.
PRE_NUM=${PRE_NUM:-0}
KEY=${KEY:-20}
VAL=${VAL:-800}
# snappy = paper config (same as fig7/8). Uncompressed doubles the device write
# volume and dilutes the WAL_CNS gain.
COMP=${COMP:-snappy}
JOBS=${JOBS:-"2 4 8"}                # background-thread sweep (memtable flush + compaction)
CFGS=${CFGS:-"ZNS CNS"}              # WAL placements to run
R_ZONES=${R_ZONES:-4}               # CNS (R-region) size in zones (= GiB); paper: 4
AOZ=${AOZ:-7}                        # active zones PER INSTANCE (dual budget 7+7=14)
STATS_SEC=${STATS_SEC:-1}
DMNAME=${DMNAME:-hyzns0}
BACKING=${BACKING:-/dev/nvme0n1}
DEV=/dev/mapper/$DMNAME
AUXMNT=${AUXMNT:-/mnt/aux_cns}       # F2FS on the CNS R-region mounts here (aux i1,i2)
ZENFS=${ZENFS:-./plugin/zenfs/util/zenfs}
BIN_ZNS=${BIN_ZNS:-./db_bench_zns}
BIN_CNS=${BIN_CNS:-./db_bench_posix}
RESULTS=${RESULTS:-results/fig10}
DRYRUN=${DRYRUN:-0}

ZS=$HYZNS_ZONE_SECTORS
SZ=$(blockdev --getsz "$BACKING")
TOTAL=$(( SZ / ZS )); S_LAST=$(( TOTAL - 1 ))
S_START=$R_ZONES; MID=$(( (S_START + S_LAST) / 2 )); I2_START=$(( MID + 1 ))

# ---- prereqs -----------------------------------------------------------
[[ -b $BACKING ]] || die "backing device $BACKING not found"
aba_ok "$BACKING" || die "$BACKING does not answer the ABA report — is the HyZNS (femu_mode=5) FEMU running?"
[[ -x $ZENFS ]]   || die "$ZENFS not built (plugin/zenfs/util: make)"
if (( ! DRYRUN )); then
    [[ -x $BIN_ZNS ]] || die "$BIN_ZNS missing — run ./scripts/build_wal_variants.sh"
    [[ -x $BIN_CNS ]] || die "$BIN_CNS missing — run ./scripts/build_wal_variants.sh"
fi

TS=$(date +%Y%m%d_%H%M%S)
RUNDIR=$RESULTS/${TS}_${GEOM}
CSV=$RESULTS/fig10_log.csv
mkdir -p "$RUNDIR"
[[ -f $CSV ]] || echo "utc,run_tag,geom,cfg,num,jobs,inst,fillrandom_ops,overwrite_ops,gc_mig,erases,dev_write_GB" > "$CSV"

{
    echo "fig10 run $TS  GEOM=$GEOM (dual-instance WAL placement)"
    echo "NUM=$NUM/inst KEY=$KEY VAL=$VAL COMP=$COMP JOBS='$JOBS' CFGS='$CFGS' R_ZONES=$R_ZONES(CNS) AOZ=$AOZ/inst PRE_NUM=$PRE_NUM$([ "$PRE_NUM" -gt 0 ] && echo ' (depth study: FR column = MEASURED fillrandom PRE_NUM build, then overwrite NUM writes over that keyspace w/ use_existing_db)')"
    echo "S split: i1[$S_START..$MID] i2[$I2_START..$S_LAST]  aux=$AUXMNT/{i1,i2}"
    echo "bins: WAL_ZNS=$BIN_ZNS WAL_CNS=$BIN_CNS"
    echo "HYSSD git: $(git rev-parse --short HEAD 2>/dev/null)"; uname -a
} > "$RUNDIR/runinfo.txt"

# ---- topology ----------------------------------------------------------
dm_prepare_once() {
    (( DRYRUN )) && { echo "  (dryrun) dm-hyzns build+reload"; return 0; }
    umount "$AUXMNT/i1" "$AUXMNT/i2" "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    if [[ ! -f $DMHYZNS/dm-hyzns.ko ]] \
       || [[ -n $(find "$DMHYZNS" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer "$DMHYZNS/dm-hyzns.ko" 2>/dev/null) ]]; then
        echo "==> rebuilding dm-hyzns.ko"; "$DMHYZNS/scripts/build.sh" || die "dm-hyzns build failed"
    fi
    "$DMHYZNS/scripts/load.sh" >/dev/null || die "dm-hyzns load failed"
}

setup_stack() {                        # fresh dm + F2FS(CNS) + 2 ZenFS(S split)
    local phase=$1
    (( DRYRUN )) && { echo "  (dryrun) dm R=${R_ZONES}GiB CNS + F2FS + 2 ZenFS (i1 $S_START..$MID / i2 $I2_START..$S_LAST)"; return 0; }
    umount "$AUXMNT/i1" "$AUXMNT/i2" "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    reset_device_full_r "$BACKING" >/dev/null 2>&1 || true
    echo "0 $SZ hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME" || die "dmsetup create failed"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) || die "set_r_end failed"
    local st; st=$(dmsetup status "$DMNAME")
    # geometry contract: line_pblocks = nchs*ways*512 (page 16KiB / block 2MiB fixed)
    local _gc _gw _lpb; IFS=x read -r _gc _gw <<< "$GEOM"; _lpb=$(( _gc * _gw * 512 ))
    [[ ${SKIP_D19_CHECK:-0} == 1 ]] || grep -q "line_pblocks=$_lpb" <<< "$st" \
        || die "geometry check failed (expected line_pblocks=$_lpb, GEOM=$GEOM): $st"
    mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1 || die "mkfs.f2fs on CNS failed"
    mkdir -p "$AUXMNT"; mount -t f2fs "$DEV" "$AUXMNT" || die "mount CNS failed"
    mkdir -p "$AUXMNT/i1" "$AUXMNT/i2"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUXMNT/i1" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$S_START" --end_zone="$MID" --ao_zones="$AOZ" --enable_gc=true --force \
        &> "$RUNDIR/zmkfs_${phase}_i1.log" || die "zenfs mkfs i1 failed"
    "$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUXMNT/i2" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$I2_START" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force \
        &> "$RUNDIR/zmkfs_${phase}_i2.log" || die "zenfs mkfs i2 failed"
}

ops(){ grep -E "^$1 " "$2" 2>/dev/null | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }
dmk(){ dmsetup status "$DMNAME" 2>/dev/null | grep -oE "(^| )$1=[0-9]+" | head -1 | grep -oE '[0-9]+'; }

# ---- one cell: <cfg> <bin> <jobs> --------------------------------------
run_cell() {
    local cfg=$1 bin=$2 jobs=$3
    local tag="${cfg}_n${NUM}_j${jobs}"
    local l1="$RUNDIR/${tag}_i1.log" l2="$RUNDIR/${tag}_i2.log"
    local common=(--use_direct_io_for_flush_and_compaction --key_size="$KEY"
        --value_size="$VAL" --compression_type="$COMP" --max_background_jobs="$jobs"
        --histogram --statistics --stats_interval_seconds="$STATS_SEC"
        --report_interval_seconds=1)   # + ROCKSDB_REPORT_INTERVAL_MS -> ===REPORT @100ms
    local bench_list="fillrandom,stats,overwrite,stats"
    local meas=(--num="$NUM")
    if (( PRE_NUM > 0 )); then
        # depth mode, two invocations: ① fillrandom PRE_NUM — MEASURED, this IS
        # the FR column (cost of building the deep DB) ② overwrite NUM writes
        # over the same keyspace (use_existing_db; db_bench skips fillrandom
        # there, and --writes would also cap a same-invocation fillrandom)
        bench_list="overwrite,stats"
        meas=(--num="$PRE_NUM" --writes="$NUM" --use_existing_db=1)
    fi
    echo "== [$tag] 2 instances: fillrandom,overwrite num=$NUM/inst jobs=$jobs"
    if (( DRYRUN )); then
        echo "   i1: $bin --fs_uri=zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296 --benchmarks=$bench_list ${meas[*]} ${common[*]}$([ "$PRE_NUM" -gt 0 ] && echo "   (after fillrandom prefill num=$PRE_NUM/inst")"
        echo "   i2: $bin --fs_uri=zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296 ..."
        return 0
    fi
    setup_stack "$tag"
    local gm0 er0 ws0; gm0=$(dmk gc_mig); er0=$(dmk erases)
    ws0=$(awk '{print $7}' "/sys/block/$(basename "$BACKING")/stat" 2>/dev/null || echo 0)
    # all-layer sampler (1s): device write/read sectors + io_ticks (busy) +
    # CPU idle — shows whether the device is at its bandwidth ceiling
    local bkn; bkn=$(basename "$BACKING")
    ( echo "ts,wr_sectors,rd_sectors,io_ticks_ms,cpu_idle_jiffies"
      while :; do
        echo "$(date +%s.%N),$(awk -v d="$bkn" '$3==d{print $10","$6","$13}' /proc/diskstats),$(awk '/^cpu /{print $5; exit}' /proc/stat)"
        sleep 1
      done ) > "$RUNDIR/${tag}_mon.csv" 2>/dev/null & local MONPID=$!
    if (( PRE_NUM > 0 )); then
        echo "   [depth] fillrandom num=$PRE_NUM per instance (MEASURED - FR column)"
        ROCKSDB_REPORT_INTERVAL_MS=100 ZENFS_L0_ON_AUX=0 "$bin" \
            --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" \
            --benchmarks=fillrandom,stats --num="$PRE_NUM" "${common[@]}" \
            &> "$RUNDIR/${tag}_pre_i1.log" & local q1=$!
        ROCKSDB_REPORT_INTERVAL_MS=100 ZENFS_L0_ON_AUX=0 "$bin" \
            --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" \
            --benchmarks=fillrandom,stats --num="$PRE_NUM" "${common[@]}" \
            &> "$RUNDIR/${tag}_pre_i2.log" & local q2=$!
        wait $q1 || { echo "  !! prefill i1 failed"; tail -3 "$RUNDIR/${tag}_pre_i1.log"; return 1; }
        wait $q2 || { echo "  !! prefill i2 failed"; tail -3 "$RUNDIR/${tag}_pre_i2.log"; return 1; }
    fi
    ROCKSDB_REPORT_INTERVAL_MS=100 ZENFS_L0_ON_AUX=0 "$bin" \
        --fs_uri="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296" \
        --benchmarks="$bench_list" "${meas[@]}" "${common[@]}" &> "$l1" & local p1=$!
    ROCKSDB_REPORT_INTERVAL_MS=100 ZENFS_L0_ON_AUX=0 "$bin" \
        --fs_uri="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296" \
        --benchmarks="$bench_list" "${meas[@]}" "${common[@]}" &> "$l2" & local p2=$!
    wait $p1; local r1=$?; wait $p2; local r2=$?
    kill "$MONPID" 2>/dev/null; wait "$MONPID" 2>/dev/null
    # RocksDB info LOGs (ZenFS redirects them to the aux F2FS) BEFORE teardown:
    # the source of the L0-count / stall-cause / compaction-lane panels
    cp "$AUXMNT/i1/rocksdbtest/dbbench/LOG" "$RUNDIR/${tag}_rocksdb_LOG_i1" 2>/dev/null || true
    cp "$AUXMNT/i2/rocksdbtest/dbbench/LOG" "$RUNDIR/${tag}_rocksdb_LOG_i2" 2>/dev/null || true
    if (( PRE_NUM > 0 )); then
        # the FR invocation's LOG rotates to LOG.old.* when the OW run reopens
        local lo
        for n in 1 2; do
            lo=$(ls -t "$AUXMNT/i$n/rocksdbtest/dbbench/"LOG.old.* 2>/dev/null | head -1)
            [[ -n $lo ]] && cp "$lo" "$RUNDIR/${tag}_rocksdb_LOG_pre_i$n"
        done
    fi
    local gm er ws; gm=$(( $(dmk gc_mig) - gm0 )); er=$(( $(dmk erases) - er0 ))
    ws=$(( ( $(awk '{print $7}' "/sys/block/$(basename "$BACKING")/stat" 2>/dev/null||echo 0) - ws0 ) * 512 ))
    local fr1 fr2 ow1 ow2
    if (( PRE_NUM > 0 )); then
        # depth mode: FR column = the measured deep-DB build (pre logs)
        fr1=$(ops fillrandom "$RUNDIR/${tag}_pre_i1.log")
        fr2=$(ops fillrandom "$RUNDIR/${tag}_pre_i2.log")
    else
        fr1=$(ops fillrandom "$l1"); fr2=$(ops fillrandom "$l2")
    fi
    ow1=$(ops overwrite "$l1");  ow2=$(ops overwrite "$l2")
    local u=$(date -u +%FT%TZ) gwb=$(( ws / 1073741824 ))
    echo "$u,$TS,$GEOM,$cfg,$NUM,$jobs,i1,${fr1:-0},${ow1:-0},,," >> "$CSV"
    echo "$u,$TS,$GEOM,$cfg,$NUM,$jobs,i2,${fr2:-0},${ow2:-0},,," >> "$CSV"
    # convention: report ops as the MEAN of the two instances (legacy/paper
    # tables are per-instance averages; a sum reads 2x inflated)
    echo "$u,$TS,$GEOM,$cfg,$NUM,$jobs,avg,$(( (${fr1:-0}+${fr2:-0})/2 )),$(( (${ow1:-0}+${ow2:-0})/2 )),$gm,$er,$gwb" >> "$CSV"
    printf "   i1 FR=%s OW=%s (rc=%s) | i2 FR=%s OW=%s (rc=%s)\n" "${fr1:-0}" "${ow1:-0}" "$r1" "${fr2:-0}" "${ow2:-0}" "$r2"
    printf "   AVG FR=%s OW=%s  | device gc_mig=%s erases=%s write=%sGB\n" \
        "$(( (${fr1:-0}+${fr2:-0})/2 ))" "$(( (${ow1:-0}+${ow2:-0})/2 ))" "$gm" "$er" "$gwb"
    umount "$AUXMNT/i1" "$AUXMNT/i2" "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
}

report() {
    echo; echo "---- Fig10 throughput, avg of 2 instances (geom=$GEOM, $CSV; legacy 'agg' rows = sums) ----"
    awk -F, -v geom="$GEOM" 'NR>1 && $3==geom && $7=="avg" {
            fr[$4","$6]=$8; ow[$4","$6]=$9; jset[$6]=1 }
        END{
            n=split("", js); for (j in jset) js[++n]=j
            # numeric sort of thread counts
            for (a=1;a<=n;a++) for (b=a+1;b<=n;b++) if (js[a]+0>js[b]+0){t=js[a];js[a]=js[b];js[b]=t}
            printf "%-6s | %12s %12s %7s | %12s %12s %7s\n","jobs","FR WAL_ZNS","FR WAL_CNS","CNS/ZNS","OW WAL_ZNS","OW WAL_CNS","CNS/ZNS"
            for (i=1;i<=n;i++){ j=js[i]
                fz=fr["WAL_ZNS,"j]; fc=fr["WAL_CNS,"j]; oz=ow["WAL_ZNS,"j]; oc=ow["WAL_CNS,"j]
                frr=(fz>0&&fc>0)?sprintf("%.2fx",fc/fz):"-"; orr=(oz>0&&oc>0)?sprintf("%.2fx",oc/oz):"-"
                printf "%-6s | %12s %12s %7s | %12s %12s %7s\n", j, (fz?fz:"-"),(fc?fc:"-"),frr,(oz?oz:"-"),(oc?oc:"-"),orr }
        }' "$CSV"
}

# ---- main --------------------------------------------------------------
echo "fig10: GEOM=$GEOM NUM=$NUM/inst JOBS='$JOBS' CFGS='$CFGS' R_ZONES=${R_ZONES}(CNS) AOZ=$AOZ/inst -> $RUNDIR"
(( DRYRUN )) || dm_prepare_once
rc=0
for jobs in $JOBS; do
    for cfg in $CFGS; do
        case $cfg in
            ZNS) run_cell WAL_ZNS "$BIN_ZNS" "$jobs" || rc=1 ;;
            CNS) run_cell WAL_CNS "$BIN_CNS" "$jobs" || rc=1 ;;
            *)   echo "unknown CFG '$cfg' (use ZNS/CNS)"; rc=1 ;;
        esac
    done
done
(( DRYRUN )) || report
echo; echo "logs: $RUNDIR   cumulative csv: $CSV"
echo "plot: python3 scripts/fig10_plot.py $CSV -o $RESULTS/fig10.png"
exit $rc
