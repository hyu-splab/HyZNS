#!/bin/bash
#
# fig12_run.sh — paper Fig11(b)(c) (§eval-rocks-wal): readwhilewriting under WAL
#   placement (WAL_ZNS vs WAL_CNS), DUAL instance, background-thread sweep. The
#   RWW companion of fig10_run.sh (paper fig11(a) FR/OW throughput): same dual
#   HyZNS topology, but the measured phase is RWW_THREADS readers + 1 writer per
#   instance on a DEEP fillrandom-populated DB. Deliverables:
#     fig11(b) RWW throughput per jobs — total-only AND read/write versions
#     fig11(c) read-latency CDF — exact, from the per-op oplat trace
#
#   Per instance and cell:
#     1) fillrandom NUM (1 client thread)          — preload (deep tree), not the metric
#     2) readwhilewriting --threads=RWW_THREADS    — measured (reads/thread=RWW_READS;
#        + db_bench's own writer thread)             default total reads = 10M/inst.
#        ROCKSDB_OPLAT_DIR (tmpfs) captures every read's latency -> <tag>_oplat.tar.gz
#
#   Prereq: db_bench_zns + db_bench_posix (sudo ./scripts/build_wal_variants.sh)
#   Usage (paper fig11(b)(c): 60M deep prefill, 10M RWW reads):
#     sudo NUM=60000000 GEOM=8x4 ./scripts/fig12_run.sh      # jobs 2 4 8
#     sudo NUM=60000000 JOBS="4" ./scripts/fig12_run.sh      # single cell
#     sudo CFGS="CNS" DRYRUN=1 ./scripts/fig12_run.sh
#   Post: python3 scripts/fig12_post.py results/fig12/<run>
set -uo pipefail
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPTDIR/.."                                     # rocksdb/
DMHYHOST=../dm-hyhost
source "$SCRIPTDIR/_fig_lib.sh"                        # aba_ok
source "$DMHYHOST/scripts/_lib.sh"                     # HYHOST_ZONE_SECTORS, reset_device_full_r

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- config (env-overridable) ------------------------------------------
GEOM=${GEOM:-8x4}
NUM=${NUM:-10000000}                 # preload KV-pairs per instance (paper: 20M = 10M*2)
KEY=${KEY:-20}
VAL=${VAL:-800}
COMP=${COMP:-snappy}                 # policy: all experiments run snappy
JOBS=${JOBS:-"2 4 8"}                # background-thread sweep (flush + compaction)
CFGS=${CFGS:-"ZNS CNS"}              # WAL placements to run
RWW_THREADS=${RWW_THREADS:-64}       # RWW reader threads/inst (paper fig1: 64;
                                     # db_bench adds the 1 writer itself)
RWW_READS=${RWW_READS:-$(( 10000000 / RWW_THREADS ))}  # per reader -> 10M total/inst
RWW_DUR=${RWW_DUR:-0}                 # >0: RWW is TIME-bounded (--duration s) not read-count
if [ "$RWW_DUR" -gt 0 ]; then RWW_BOUND="--duration=$RWW_DUR"; else RWW_BOUND="--reads=$RWW_READS"; fi
R_ZONES=${R_ZONES:-4}                # CNS (R-region) size in zones (= GiB); paper: 4
AOZ=${AOZ:-7}                        # active zones PER INSTANCE (dual budget 7+7=14)
STATS_SEC=${STATS_SEC:-1}
DMNAME=${DMNAME:-hyhost0}
BACKING=${BACKING:-/dev/nvme0n1}
DEV=/dev/mapper/$DMNAME
AUXMNT=${AUXMNT:-/mnt/aux_cns}
ZENFS=${ZENFS:-./plugin/zenfs/util/zenfs}
BIN_ZNS=${BIN_ZNS:-./db_bench_zns}
BIN_CNS=${BIN_CNS:-./db_bench_posix}
RESULTS=${RESULTS:-results/fig12}
DRYRUN=${DRYRUN:-0}

ZS=$HYHOST_ZONE_SECTORS
SZ=$(blockdev --getsz "$BACKING")
TOTAL=$(( SZ / ZS )); S_LAST=$(( TOTAL - 1 ))
S_START=$R_ZONES; MID=$(( (S_START + S_LAST) / 2 )); I2_START=$(( MID + 1 ))

# ---- prereqs (device probes skipped in DRYRUN so it runs on any host) ---
if (( ! DRYRUN )); then
    [[ -b $BACKING ]] || die "backing device $BACKING not found"
    aba_ok "$BACKING" || die "$BACKING does not answer the ABA report — is the HYHOSTSSD (femu_mode=5) FEMU running?"
fi
[[ -x $ZENFS ]]   || die "$ZENFS not built (plugin/zenfs/util: make)"
if (( ! DRYRUN )); then
    [[ -x $BIN_ZNS ]] || die "$BIN_ZNS missing — run ./scripts/build_wal_variants.sh"
    [[ -x $BIN_CNS ]] || die "$BIN_CNS missing — run ./scripts/build_wal_variants.sh"
fi

TS=$(date +%Y%m%d_%H%M%S)
RUNDIR=$RESULTS/${TS}_${GEOM}
CSV=$RESULTS/fig12_log.csv
mkdir -p "$RUNDIR"
[[ -f $CSV ]] || echo "utc,run_tag,geom,cfg,num,jobs,inst,rww_read_ops,rww_write_ops" > "$CSV"

{
    echo "fig12 run $TS  GEOM=$GEOM (dual-instance WAL placement, readwhilewriting)"
    echo "NUM=$NUM/inst KEY=$KEY VAL=$VAL COMP=$COMP JOBS='$JOBS' CFGS='$CFGS'"
    echo "RWW=${RWW_THREADS} readers x $RWW_READS reads (+1 writer) per inst; preload=fillrandom"
    echo "R_ZONES=$R_ZONES(CNS) AOZ=$AOZ/inst  S split: i1[$S_START..$MID] i2[$I2_START..$S_LAST]"
    echo "bins: WAL_ZNS=$BIN_ZNS WAL_CNS=$BIN_CNS"
    echo "HYSSD git: $(git rev-parse --short HEAD 2>/dev/null)"; uname -a
} > "$RUNDIR/runinfo.txt"

# ---- topology (same dual stack as fig10_run.sh) --------------------------
dm_prepare_once() {
    (( DRYRUN )) && { echo "  (dryrun) dm-hyhost build+reload"; return 0; }
    umount "$AUXMNT/i1" "$AUXMNT/i2" "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    if [[ ! -f $DMHYHOST/dm-hyhost.ko ]] \
       || [[ -n $(find "$DMHYHOST" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer "$DMHYHOST/dm-hyhost.ko" 2>/dev/null) ]]; then
        echo "==> rebuilding dm-hyhost.ko"; "$DMHYHOST/scripts/build.sh" || die "dm-hyhost build failed"
    fi
    "$DMHYHOST/scripts/load.sh" >/dev/null || die "dm-hyhost load failed"
}

setup_stack() {                        # fresh dm + F2FS(CNS) + 2 ZenFS(S split)
    local phase=$1
    (( DRYRUN )) && { echo "  (dryrun) dm R=${R_ZONES}GiB CNS + F2FS + 2 ZenFS (i1 $S_START..$MID / i2 $I2_START..$S_LAST)"; return 0; }
    umount "$AUXMNT/i1" "$AUXMNT/i2" "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    reset_device_full_r "$BACKING" >/dev/null 2>&1 || true
    echo "0 $SZ hyhost $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME" || die "dmsetup create failed"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS)) || die "set_r_end failed"
    local st; st=$(dmsetup status "$DMNAME")
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

# ---- one cell: <cfg> <bin> <jobs> --------------------------------------
run_cell() {
    local cfg=$1 bin=$2 jobs=$3
    local tag="${cfg}_n${NUM}_j${jobs}"
    local common=(--use_direct_io_for_flush_and_compaction --num="$NUM" --key_size="$KEY"
        --value_size="$VAL" --compression_type="$COMP" --max_background_jobs="$jobs"
        --histogram --statistics --stats_interval_seconds="$STATS_SEC")
    local u1="zenfs://dev:dm-0/$S_START/$MID/$AOZ/0/4294967296"
    local u2="zenfs://dev:dm-0/$I2_START/$S_LAST/$AOZ/0/4294967296"
    echo "== [$tag] 2 instances: fillrandom preload -> RWW ${RWW_THREADS}rd x $RWW_READS  jobs=$jobs"
    if (( DRYRUN )); then
        echo "   fill: $bin --fs_uri=$u1 --benchmarks=fillrandom ${common[*]}"
        echo "   rww : $bin --fs_uri=$u1 --benchmarks=readwhilewriting,stats --use_existing_db=1 --threads=$RWW_THREADS $RWW_BOUND ${common[*]}"
        return 0
    fi
    setup_stack "$tag"
    # phase 1: preload both instances (1 client thread each; own logs)
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="$u1" --benchmarks=fillrandom "${common[@]}" \
        &> "$RUNDIR/${tag}_fill_i1.log" & local p1=$!
    ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="$u2" --benchmarks=fillrandom "${common[@]}" \
        &> "$RUNDIR/${tag}_fill_i2.log" & local p2=$!
    wait $p1 || { echo "  !! preload i1 failed"; tail -3 "$RUNDIR/${tag}_fill_i1.log"; return 1; }
    wait $p2 || { echo "  !! preload i2 failed"; tail -3 "$RUNDIR/${tag}_fill_i2.log"; return 1; }
    # phase 2: measured RWW, both instances concurrently.
    # Per-op read-latency trace (ROCKSDB_OPLAT_DIR) -> tmpfs (in-RAM, one CSV per
    # reader thread: oplat_p*_readwhilewriting_t*.csv, cols ts_us,lat_us,op;
    # op 0=read). Set ONLY on the RWW phase, not the preload. Copied to the run
    # dir as <tag>_oplat.tar.gz after the run -> fig11(c) exact read CDF.
    local l1="$RUNDIR/${tag}_i1.log" l2="$RUNDIR/${tag}_i2.log"
    local od="/dev/shm/hyexp/oplat_${TS}_${tag}"
    rm -rf "$od"; mkdir -p "$od/i1" "$od/i2"
    ROCKSDB_OPLAT_DIR="$od/i1" ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="$u1" \
        --benchmarks=readwhilewriting,stats \
        --use_existing_db=1 --threads="$RWW_THREADS" $RWW_BOUND "${common[@]}" \
        &> "$l1" & p1=$!
    ROCKSDB_OPLAT_DIR="$od/i2" ZENFS_L0_ON_AUX=0 "$bin" --fs_uri="$u2" \
        --benchmarks=readwhilewriting,stats \
        --use_existing_db=1 --threads="$RWW_THREADS" $RWW_BOUND "${common[@]}" \
        &> "$l2" & p2=$!
    wait $p1; local r1=$?; wait $p2; local r2=$?
    tar czf "$RUNDIR/${tag}_oplat.tar.gz" -C "$od" . 2>/dev/null \
        && echo "   oplat -> ${tag}_oplat.tar.gz ($(find "$od" -name '*.csv' | wc -l) thread files)"
    rm -rf "$od"
    local rd1 rd2 wr1 wr2
    rd1=$(ops readwhilewriting "$l1"); rd2=$(ops readwhilewriting "$l2")
    wr1=$(ops rww-writes "$l1");       wr2=$(ops rww-writes "$l2")
    local u; u=$(date -u +%FT%TZ)
    echo "$u,$TS,$GEOM,$cfg,$NUM,$jobs,i1,${rd1:-0},${wr1:-0}" >> "$CSV"
    echo "$u,$TS,$GEOM,$cfg,$NUM,$jobs,i2,${rd2:-0},${wr2:-0}" >> "$CSV"
    # convention: report ops as the MEAN of the two instances (legacy/paper
    # tables are per-instance averages; a sum would read 2x inflated)
    echo "$u,$TS,$GEOM,$cfg,$NUM,$jobs,avg,$(( (${rd1:-0}+${rd2:-0})/2 )),$(( (${wr1:-0}+${wr2:-0})/2 ))" >> "$CSV"
    printf "   i1 RWW-R=%s W=%s (rc=%s) | i2 RWW-R=%s W=%s (rc=%s) | AVG R=%s W=%s\n" \
        "${rd1:-0}" "${wr1:-0}" "$r1" "${rd2:-0}" "${wr2:-0}" "$r2" \
        "$(( (${rd1:-0}+${rd2:-0})/2 ))" "$(( (${wr1:-0}+${wr2:-0})/2 ))"
    umount "$AUXMNT/i1" "$AUXMNT/i2" "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
}

report() {
    echo; echo "---- Fig12 RWW throughput, avg of 2 instances (geom=$GEOM, $CSV) ----"
    awk -F, -v geom="$GEOM" 'NR>1 && $3==geom && $7=="avg" {
            rd[$4","$6]=$8; wr[$4","$6]=$9; jset[$6]=1 }
        END{
            n=split("", js); for (j in jset) js[++n]=j
            for (a=1;a<=n;a++) for (b=a+1;b<=n;b++) if (js[a]+0>js[b]+0){t=js[a];js[a]=js[b];js[b]=t}
            printf "%-6s | %12s %12s %7s | %12s %12s\n","jobs","RD WAL_ZNS","RD WAL_CNS","CNS/ZNS","WR WAL_ZNS","WR WAL_CNS"
            for (i=1;i<=n;i++){ j=js[i]
                rz=rd["WAL_ZNS,"j]; rcs=rd["WAL_CNS,"j]
                rr=(rz>0&&rcs>0)?sprintf("%.2fx",rcs/rz):"-"
                printf "%-6s | %12s %12s %7s | %12s %12s\n", j, (rz?rz:"-"),(rcs?rcs:"-"),rr, \
                       (wr["WAL_ZNS,"j]?wr["WAL_ZNS,"j]:"-"),(wr["WAL_CNS,"j]?wr["WAL_CNS,"j]:"-") }
        }' "$CSV"
}

# ---- main --------------------------------------------------------------
echo "fig12: GEOM=$GEOM NUM=$NUM/inst JOBS='$JOBS' CFGS='$CFGS' RWW=${RWW_THREADS}x$RWW_READS AOZ=$AOZ/inst -> $RUNDIR"
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
echo "post: python3 scripts/fig12_post.py $RUNDIR   (throughput bars + read-lat CDF)"
exit $rc
