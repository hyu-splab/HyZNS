#!/bin/bash
#
# fig8_run.sh — Fig8 (device validation, SINGLE-device setting):
#   Unlike Fig7 (aux on a separate CNS device), here the RocksDB aux path lives
#   on the device's OWN CNS area. Two modes:
#
#     MODE=hyzns  (default) FEMU HYHOSTSSD: aux F2FS on the R-region of the
#                 dm-hyhost stack, ZenFS on the S-region. This is the
#                 fixed-size-CNS single-device case the paper compares directly
#                 against a real ZN540.
#     MODE=real   (alias zn540) real 2-namespace SSD (e.g. ZN540): aux F2FS on
#                 the conventional namespace (CNS_DEV, default /dev/nvme3n1),
#                 ZenFS on the zoned namespace (ZNS_DEV, default /dev/nvme3n2)
#                 of the SAME physical SSD. No dm-hyhost. Logged as mode=zn540
#                 (the real-device column of fig_plot.py).
#
#       RocksDB / db_bench
#             |                         aux (LOG/LOCK/…)  ZenFS (SSTs, WAL)
#             |                              |                  |
#             |                         F2FS on R-region   ZenFS on S-region
#             |                              \______  ________/
#             |                                     \/
#             |                          /dev/mapper/hyhost0
#             |                       (R = CNS = R_ZONES GiB, S = ZNS = rest)
#             |                                     |
#             |                             /dev/nvme0n1 (FEMU HYHOSTSSD)
#
#   Same workloads / thread policy as Fig7. Each workload runs on its OWN fresh
#   DB (no cross-workload reuse), so any subset can be run standalone:
#     write  FS  fillseq      1 thread                     — fresh DB, self only
#            FR  fillrandom   1 thread                     — fresh DB, self only
#            OW  overwrite    1 thread                     — fresh DB, self only
#     read   RS  readseq      1 thread, all keys           ┐ prep a fresh DB (not
#            RR  readrandom   16 threads, NUM reads total   ├ logged): fillseq for
#            RWW readwhilewriting 64 readers + 1 writer     ┘ RS/RR, fillrandom for RWW
#   RWW logs RWW-R (read) / RWW-W (write) / RWW-T (read+write). Same shapes as
#   fig7_run.sh (paper fig1/fig7 at half scale): RR reads the keyspace once over;
#   RWW does 1M reads total on a RANDOM-filled DB (overlapping-LSM read path).
#
#   Usage:
#     sudo GEOM=8x4 ./scripts/fig8_run.sh
#     sudo GEOM=8x4 NUM=10000000 ./scripts/fig8_run.sh        # quicker
#     sudo R_ZONES=4 ./scripts/fig8_run.sh                    # CNS size (GiB, hyzns only)
#     sudo WORKLOADS="RR RWW" ./scripts/fig8_run.sh           # subset (each self-contained)
#     sudo MODE=real ./scripts/fig8_run.sh                    # real SSD (nvme3n1=CNS, nvme3n2=ZNS)
#     sudo DRYRUN=1 ./scripts/fig8_run.sh                     # print, don't run
#
#   Results accumulate in results/fig8/fig8_log.csv (mode=hyzns). Compare vs a
#   real ZN540 with scripts/fig_plot.py (--modes zn540,hyzns).
set -uo pipefail
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPTDIR/.."                                     # rocksdb/
DMHYHOST=../dm-hyhost
source "$SCRIPTDIR/_fig_lib.sh"                        # aba_ok, run_bench, parse_log, ...

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- config (env-overridable) ------------------------------------------
MODE=${MODE:-hyzns}                   # hyzns (FEMU dm-hyhost) | real/zn540 (real SSD)
REAL=0
if [[ $MODE == real || $MODE == zn540 ]]; then
    REAL=1; MODE=zn540
    CNS_DEV=${CNS_DEV:-/dev/nvme3n1}  # conventional namespace — aux F2FS
    ZNS_DEV=${ZNS_DEV:-/dev/nvme3n2}  # zoned namespace — ZenFS
    ZNS_CAP_GIB=${ZNS_CAP_GIB:-252}   # cap ZenFS to 252 GiB (126 x 2GiB zones)
    GEOM=${GEOM:-real}                # FEMU geometry tag is meaningless here
elif [[ $MODE != hyzns ]]; then
    die "MODE=$MODE not supported (hyzns | real)"
fi
GEOM=${GEOM:-8x4}
NUM=${NUM:-20000000}
KEY=${KEY:-20}
VAL=${VAL:-800}
COMP=${COMP:-snappy}
AOZ=${AOZ:-14}
JOBS=${JOBS:-8}
RR_THREADS=${RR_THREADS:-16}
READS=${READS:-$(( NUM / RR_THREADS ))}   # RR reads PER THREAD; default totals NUM (see fig7_run.sh)
RWW_THREADS=${RWW_THREADS:-64}        # RWW reader threads (paper fig1: 64; db_bench adds the writer)
RWW_READS=${RWW_READS:-$(( 1000000 / RWW_THREADS ))}  # per reader; default totals the paper's 1M
STATS_SEC=${STATS_SEC:-1}
WORKLOADS=${WORKLOADS:-"FS FR OW RS RR RWW"}
R_ZONES=${R_ZONES:-4}                 # CNS (R-region) size in zones (= GiB); paper: 4
DMNAME=${DMNAME:-hyhost0}
BACKING=${BACKING:-/dev/nvme0n1}      # FEMU HYHOSTSSD namespace
AUXMNT=${AUXMNT:-/mnt/aux_cns}        # F2FS on the mapper's R-region mounts here
AUXDIR=$AUXMNT/zenfs_aux
DBBENCH=${DBBENCH:-./db_bench}
ZENFS=${ZENFS:-./plugin/zenfs/util/zenfs}
RESULTS=${RESULTS:-results/fig8}
DRYRUN=${DRYRUN:-0}

# ---- device check --------------------------------------------------------
if (( REAL )); then
    [[ -b $CNS_DEV ]] || die "CNS namespace $CNS_DEV not found (set CNS_DEV=...)"
    [[ -b $ZNS_DEV ]] || die "ZNS namespace $ZNS_DEV not found (set ZNS_DEV=...)"
    grep -q host-managed "/sys/block/$(basename "$ZNS_DEV")/queue/zoned" 2>/dev/null \
        || die "$ZNS_DEV is not a host-managed zoned device"
    [[ $CNS_DEV == "$ZNS_DEV" ]] && die "CNS_DEV == ZNS_DEV ($CNS_DEV)"
else
    [[ -b $BACKING ]] || die "backing device $BACKING not found (set BACKING=...)"
    aba_ok "$BACKING" || die "$BACKING does not answer the ABA report — is the HYHOSTSSD (femu_mode=5) FEMU running?"
fi
[[ -x $DBBENCH ]] || die "$DBBENCH not built (see scripts/README.md: build)"
[[ -x $ZENFS ]]   || die "$ZENFS not built (see scripts/README.md: build)"

TS=$(date +%Y%m%d_%H%M%S)
RUNDIR=$RESULTS/${TS}_${MODE}_${GEOM}
CSV=$RESULTS/fig8_log.csv
mkdir -p "$RUNDIR"
[[ -f $CSV ]] || echo "utc,run_tag,mode,geom,workload,threads,num,micros_op,ops_sec,mb_s,log" > "$CSV"

{
    echo "fig8 run $TS  MODE=$MODE GEOM=$GEOM (single-device, aux on CNS)"
    echo "NUM=$NUM KEY=$KEY VAL=$VAL COMP=$COMP AOZ=$AOZ JOBS=$JOBS RR_THREADS=$RR_THREADS READS=$READS/thread RWW=${RWW_THREADS}rd x $RWW_READS (+1 writer)"
    if (( REAL )); then echo "WORKLOADS=$WORKLOADS  cns=$CNS_DEV zns=$ZNS_DEV cap_gib=${ZNS_CAP_GIB:-whole-device} aux=$AUXDIR"
    else echo "WORKLOADS=$WORKLOADS  backing=$BACKING r_zones=$R_ZONES (CNS) dm=$DMNAME aux=$AUXDIR"; fi
    echo "HYSSD git: $(git rev-parse --short HEAD 2>/dev/null)"; uname -a
    lsblk -o NAME,SIZE,TYPE,MODEL 2>/dev/null
} > "$RUNDIR/runinfo.txt"

# ---- topology ------------------------------------------------------------
dm_prepare_once() {                    # build (if stale) + reload the .ko
    (( DRYRUN )) && { echo "  (dryrun) dm-hyhost build+reload"; return 0; }
    umount "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    if [[ ! -f $DMHYHOST/dm-hyhost.ko ]] \
       || [[ -n $(find "$DMHYHOST" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer "$DMHYHOST/dm-hyhost.ko" 2>/dev/null) ]]; then
        echo "==> rebuilding dm-hyhost.ko"
        "$DMHYHOST/scripts/build.sh" || die "dm-hyhost build failed"
    fi
    "$DMHYHOST/scripts/load.sh" >/dev/null || die "dm-hyhost load failed"
}

FS_URI=""
setup_dev() {                          # fresh single-device stack for a write
    local phase=$1
    if (( REAL )); then                # real SSD: F2FS on the CNS namespace,
        local name zs total last send  # ZenFS on the ZNS namespace (capped)
        name=$(basename "$ZNS_DEV")
        zs=$(cat "/sys/block/$name/queue/chunk_sectors")
        total=$(( $(blockdev --getsz "$ZNS_DEV") / zs )); last=$(( total - 1 )); send=$last
        if [[ -n ${ZNS_CAP_GIB:-} ]]; then                  # force a capacity cap
            local ncap=$(( ZNS_CAP_GIB * 1073741824 / (zs * 512) ))
            (( ncap >= 1 )) || die "ZNS_CAP_GIB=$ZNS_CAP_GIB smaller than one zone"
            (( ncap - 1 < send )) && send=$(( ncap - 1 ))   # zones 0..send inclusive
        fi
        FS_URI="zenfs://dev:$name/0/$send/$AOZ/0/4294967296"
        (( DRYRUN )) && { echo "  (dryrun) mkfs.f2fs $CNS_DEV (aux) + blkzone reset + zenfs mkfs on $name (zones 0..$send of $total)"; return 0; }
        umount "$AUXMNT" 2>/dev/null || true
        echo mq-deadline > "/sys/block/$name/queue/scheduler" 2>/dev/null \
            || echo "WARN: could not set mq-deadline on $name"
        blkzone reset "$ZNS_DEV" || die "blkzone reset $ZNS_DEV failed"
        mkfs.f2fs -f "$CNS_DEV" &> "$RUNDIR/mkfs_aux_${phase}.log" \
            || die "mkfs.f2fs $CNS_DEV failed (see $RUNDIR/mkfs_aux_${phase}.log)"
        mkdir -p "$AUXMNT"
        mount -t f2fs "$CNS_DEV" "$AUXMNT" || die "mount $CNS_DEV -> $AUXMNT failed"
        rm -rf "$AUXDIR"
        "$ZENFS" mkfs --zbd="$name" --aux_path="$AUXDIR" \
            --start_zone=0 --end_zone="$send" --ao_zones="$AOZ" \
            --enable_gc=true --force &> "$RUNDIR/zenfs_mkfs_${phase}.log" \
            || die "zenfs mkfs failed (see $RUNDIR/zenfs_mkfs_${phase}.log)"
        return 0
    fi
    source "$DMHYHOST/scripts/_lib.sh"
    local zs=$HYHOST_ZONE_SECTORS sz total last dmdev
    sz=$(blockdev --getsz "$BACKING")
    total=$(( sz / zs )); last=$(( total - 1 ))
    (( DRYRUN )) && { FS_URI="zenfs://dev:dm-0/$R_ZONES/$last/$AOZ/0/4294967296"
                      echo "  (dryrun) dm R=${R_ZONES}GiB CNS + F2FS on R-region + zenfs mkfs (zones $R_ZONES..$last)"; return 0; }
    umount "$AUXMNT" 2>/dev/null || true
    dmsetup remove "$DMNAME" 2>/dev/null || true
    reset_device_full_r "$BACKING" || true
    echo "0 $sz hyhost $BACKING $((R_ZONES*zs))" | dmsetup create "$DMNAME" || die "dmsetup create failed"
    dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*zs)) || die "set_r_end failed"
    # geometry contract: line_pblocks = nchs*ways*512 (page 16KiB / block 2MiB fixed)
    local _gc _gw _lpb
    IFS=x read -r _gc _gw <<< "$GEOM"; _lpb=$(( _gc * _gw * 512 ))
    local st; st=$(dmsetup status "$DMNAME")
    echo "$st" > "$RUNDIR/dmstatus_${phase}_setup.txt"
    if [[ ${SKIP_D19_CHECK:-0} != 1 ]] && ! grep -q "line_pblocks=$_lpb" <<< "$st"; then
        die "geometry check failed — expected line_pblocks=$_lpb (GEOM=$GEOM) in: $st (SKIP_D19_CHECK=1 to override)"
    fi
    dmdev=$(basename "$(readlink -f "/dev/mapper/$DMNAME")")
    # single-device: aux F2FS lives on the R-region (CNS) of the SAME mapper,
    # ZenFS on the S-region. Mirrors the proven figB/legacy topology.
    mkfs.f2fs -f -m -H -A "/dev/mapper/$DMNAME" &> "$RUNDIR/mkfs_aux_${phase}.log" \
        || die "mkfs.f2fs on CNS (R-region) failed (see $RUNDIR/mkfs_aux_${phase}.log)"
    mkdir -p "$AUXMNT"
    mount -t f2fs "/dev/mapper/$DMNAME" "$AUXMNT" || die "mount CNS -> $AUXMNT failed"
    rm -rf "$AUXDIR"
    FS_URI="zenfs://dev:$dmdev/$R_ZONES/$last/$AOZ/0/4294967296"
    "$ZENFS" mkfs --zbd="$dmdev" --aux_path="$AUXDIR" --hyssd --aux_size="$R_ZONES" \
        --start_zone="$R_ZONES" --end_zone="$last" --ao_zones="$AOZ" \
        --enable_gc=true --force &> "$RUNDIR/zenfs_mkfs_${phase}.log" \
        || die "zenfs mkfs failed (see $RUNDIR/zenfs_mkfs_${phase}.log)"
}

report_results() {
    echo; echo "---- Fig8 $MODE results (geom=$GEOM, $CSV) ----"
    awk -F, -v mode="$MODE" -v geom="$GEOM" 'NR>1 && $3==mode && $4==geom { ops[$5]=$9; seen[$5]=1 }
        END {
            n=split("RS RR RWW-R RWW-W RWW-T FS FR OW", order, " ")
            printf "%-8s %14s\n", "workload", mode" ops/s"
            for (i=1; i<=n; i++) { w=order[i]; if (w in seen) printf "%-8s %14s\n", w, ops[w] }
        }' "$CSV"
}

# read_workload <inv> <benchmark> <threads> <reads|-1> [prep] — fresh DB,
# prep-fill (1 thread, NOT recorded; fillseq unless given), then the read
# workload on it (recorded). RWW preps with fillrandom (see fig7_run.sh).
read_workload() {
    local inv=$1 bench=$2 threads=$3 reads=$4 prep=${5:-fillseq}
    setup_dev "$inv"
    if run_bench "${inv}_fill" "$prep" 1 -1 0 0; then
        run_bench "$inv" "$bench" "$threads" "$reads" 1 || rc=1
    else rc=1; fi
}

# ---- main ----------------------------------------------------------------
if (( REAL )); then
    echo "fig8: MODE=$MODE cns=$CNS_DEV zns=$ZNS_DEV NUM=$NUM AOZ=$AOZ JOBS=$JOBS -> $RUNDIR"
else
    echo "fig8: MODE=$MODE GEOM=$GEOM NUM=$NUM R_ZONES=${R_ZONES}(CNS) AOZ=$AOZ JOBS=$JOBS -> $RUNDIR"
fi
have() { [[ " $WORKLOADS " == *" $1 "* ]]; }
(( REAL )) || dm_prepare_once
rc=0

# write workloads — each on its own fresh DB, measuring only itself
if have FS; then setup_dev fs; run_bench fs "fillseq"    1 -1 0 || rc=1; fi
if have FR; then setup_dev fr; run_bench fr "fillrandom" 1 -1 0 || rc=1; fi
if have OW; then setup_dev ow; run_bench ow "overwrite"  1 -1 0 || rc=1; fi

# read workloads — each fillseq-populates a fresh DB, then runs the read
if have RS;  then read_workload rs  "readseq"          1              -1                      ; fi
if have RR;  then read_workload rr  "readrandom"       "$RR_THREADS"  "$READS"                ; fi
if have RWW; then read_workload rww "readwhilewriting" "$RWW_THREADS" "$RWW_READS" fillrandom ; fi

(( DRYRUN )) || report_results
echo; echo "logs: $RUNDIR   cumulative csv: $CSV"
echo "plot: python3 scripts/fig_plot.py --template --log $CSV --modes zn540,hyzns $RESULTS/fig8_values.csv"
echo "      # fill the zn540 column, then:"
echo "      python3 scripts/fig_plot.py $RESULTS/fig8_values.csv -o $RESULTS/fig8.png"
exit $rc
