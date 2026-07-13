#!/bin/bash
#
# fig7_run.sh — Fig7 (device validation, multi-device setting):
#   db_bench throughput on the ZNS area only, with the RocksDB aux path on a
#   SEPARATE conventional device (CNS), for three modes:
#
#     MODE=zns    FEMU pure-ZNS SSD    : ZenFS directly on the zoned nvme
#     MODE=hyzns  FEMU HYHOSTSSD       : ZenFS on the S-region of dm-hyhost
#                                        (R-region idle — aux is NOT on R)
#     MODE=real   real 2-namespace SSD : ZenFS on the ZNS namespace (ZNS_DEV,
#     (alias zn540)                      default /dev/nvme3n2), aux on the CNS
#                                        namespace (AUX_DEV, default
#                                        /dev/nvme3n1); logged as mode=zn540
#                                        (the real-device column of fig_plot.py)
#
#   The FEMU mode is auto-detected from the attached device (a host-managed
#   zoned nvme present -> zns, otherwise hyzns), so the same command works
#   while you swap FEMU builds. Real mode is NEVER auto-detected — pass
#   MODE=real explicitly:
#
#       sudo GEOM=8x4 ./scripts/fig7_run.sh
#       sudo GEOM=8x4 NUM=10000000 ./scripts/fig7_run.sh      # quicker
#       sudo MODE=hyzns WORKLOADS="RR RWW" ./scripts/fig7_run.sh
#       sudo MODE=real ./scripts/fig7_run.sh                  # real SSD (nvme3n2=ZNS, nvme3n1=CNS)
#       sudo DRYRUN=1 ./scripts/fig7_run.sh                   # print, don't run
#
#   Each workload runs on its OWN fresh DB (no cross-workload reuse):
#     write  FS  fillseq      1 thread                 — fresh DB, self only
#            FR  fillrandom   1 thread                 — fresh DB, self only
#            OW  overwrite    1 thread                 — fresh DB, self only
#     read   RS  readseq      1 thread, all keys          ┐ prep a fresh DB (not
#            RR  readrandom   16 threads, NUM reads total  ├ logged): fillseq for
#            RWW readwhilewriting 64 readers + 1 writer    ┘ RS/RR, fillrandom for RWW
#   RWW logs RWW-R (read) / RWW-W (write) / RWW-T (read+write). Shapes follow the
#   paper fig1/fig7 at half scale (NUM 40M -> 20M): RR reads the keyspace once
#   over; RWW does 1M reads total (absolute count, not scaled) on a RANDOM-filled
#   DB (overlapping levels — the read path the paper measures).
#
#   Every summary line is appended to results/fig7/fig7_log.csv so runs
#   accumulate across FEMU swaps; the script ends with the latest
#   zns-vs-hyzns comparison table (the number that matters is the ratio ~1.0).
#
#   Prereqs: ./db_bench (make: see scripts/README.md), plugin/zenfs/util/zenfs,
#            and for hyzns a buildable dm-hyhost (../dm-hyhost).
set -uo pipefail
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPTDIR/.."                                     # rocksdb/
DMHYHOST=../dm-hyhost
source "$SCRIPTDIR/_fig_lib.sh"                        # aba_ok, run_bench, parse_log, ...

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- config (env-overridable) ------------------------------------------
# MODE=real (alias zn540) is resolved FIRST so the device/geometry defaults
# below can follow it. It reuses the zns code path (raw ZBD, no dm-hyhost) but
# never probes FEMU, and the CSV records mode=zn540.
REAL=0
AUX_PREMOUNTED=${AUX_PREMOUNTED:-0}   # 1 = aux is a dir on an ALREADY-mounted FS
                                      # (no mkfs/mount/umount — never formatted)
if [[ ${MODE:-} == real || ${MODE:-} == zn540 ]]; then
    REAL=1; MODE=zn540
    ZNS_DEV=${ZNS_DEV:-/dev/nvme3n2}  # zoned namespace — ZenFS
    # nvme3n1 (the CNS namespace) is the SAME physical SSD as ZNS_DEV, so it is
    # NOT a separate device. fig7's multi-device topology therefore puts the aux
    # path on a genuinely separate disk: nvme1n1, already mounted at /data2.
    # /data2 is a live ext4 with real data — only $AUXDIR is ever touched here.
    AUX_PREMOUNTED=1
    AUXDIR=${AUXDIR:-/data2/aux}
    # fig7 aux is OFF-device (/data2), so the ZNS keeps the full 256 GiB budget
    # (fig8, single-device, gives 4 GiB to CNS and caps ZNS at 252).
    ZNS_CAP_GIB=${ZNS_CAP_GIB:-256}   # cap ZenFS to 256 GiB (128 x 2GiB zones)
    GEOM=${GEOM:-real}                # FEMU geometry tag is meaningless here
fi
GEOM=${GEOM:-8x4}                    # FEMU geometry tag recorded in the CSV
NUM=${NUM:-20000000}                 # keys per fill benchmark (paper: 40M)
KEY=${KEY:-20}
VAL=${VAL:-800}                      # snappy default ratio 0.5 -> ~400B on flash
COMP=${COMP:-snappy}
AOZ=${AOZ:-14}                       # active zones — single instance budget = 14
JOBS=${JOBS:-8}                      # paper fig7: 8 background threads
RR_THREADS=${RR_THREADS:-16}         # readrandom client threads (others run 1)
READS=${READS:-$(( NUM / RR_THREADS ))}  # RR reads PER THREAD; default totals NUM
                                     # (paper: 40M reads over 40M keys, 16 threads)
RWW_THREADS=${RWW_THREADS:-64}       # RWW reader threads (paper fig1: 64;
                                     # db_bench adds the 1 writer thread itself)
RWW_READS=${RWW_READS:-$(( 1000000 / RWW_THREADS ))}  # per reader; default totals
                                     # the paper's 1M RWW reads (64 x 15625)
STATS_SEC=${STATS_SEC:-1}
WORKLOADS=${WORKLOADS:-"FS FR OW RS RR RWW"}
R_ZONES=${R_ZONES:-0}                # hyzns: idle R-region size in zones (GiB)
DMNAME=${DMNAME:-hyhost0}
AUX_DEV=${AUX_DEV:-/dev/nvme1n1}     # separate CNS (FEMU BBSSD) for the aux path
AUXMNT=${AUXMNT:-/mnt/aux_cns}
AUXDIR=${AUXDIR:-$AUXMNT/zenfs_aux}  # real mode overrides this to /data2/aux
ZNS_CAP_GIB=${ZNS_CAP_GIB:-}         # cap ZenFS to N GiB (empty = whole device)
DBBENCH=${DBBENCH:-./db_bench}
ZENFS=${ZENFS:-./plugin/zenfs/util/zenfs}
RESULTS=${RESULTS:-results/fig7}
DRYRUN=${DRYRUN:-0}

# ---- mode / device detection -------------------------------------------
# A HYHOSTSSD namespace AND a pure-ZNS device BOTH report
# queue/zoned=host-managed, so aba_ok() (vendor ABA report, in _fig_lib.sh) is
# what tells them apart: only HYHOSTSSD answers it.
if [[ -z ${MODE:-} ]]; then            # auto: probe ABA on each zoned nvme
    for _d in $(list_zoned_nvme); do
        if aba_ok "$_d"; then MODE=hyzns; BACKING=${BACKING:-$_d}; break; fi
        ZNS_DEV=${ZNS_DEV:-$_d}        # first non-HYHOSTSSD zoned dev = pure ZNS
    done
    [[ -z ${MODE:-} && -n ${ZNS_DEV:-} ]] && MODE=zns
    [[ -z ${MODE:-} ]] && die "no host-managed zoned nvme found (is FEMU running?)"
fi
BACKING=${BACKING:-/dev/nvme0n1}       # hyzns: FEMU HYHOSTSSD namespace
ZNS_DEV=${ZNS_DEV:-$(list_zoned_nvme | head -1)}

if (( AUX_PREMOUNTED )); then          # aux is a dir on a live, mounted FS
    mountpoint -q "$(dirname "$AUXDIR")" \
        || die "$(dirname "$AUXDIR") is not a mountpoint — mount the aux disk first (this script never formats it)"
else
    [[ -b $AUX_DEV ]] || die "aux CNS device $AUX_DEV not found (set AUX_DEV=...)"
fi
if (( REAL )); then
    [[ -b $ZNS_DEV ]] || die "ZNS namespace $ZNS_DEV not found (set ZNS_DEV=...)"
    grep -q host-managed "/sys/block/$(basename "$ZNS_DEV")/queue/zoned" 2>/dev/null \
        || die "$ZNS_DEV is not a host-managed zoned device"
    (( AUX_PREMOUNTED )) || [[ $AUX_DEV != "$ZNS_DEV" ]] || die "AUX_DEV == ZNS_DEV ($AUX_DEV)"
elif [[ $MODE == zns ]]; then
    [[ -n $ZNS_DEV ]] || die "MODE=zns but no host-managed zoned nvme found"
    [[ $AUX_DEV == "$ZNS_DEV" ]] && die "AUX_DEV == ZNS_DEV ($AUX_DEV)"
    ! aba_ok "$ZNS_DEV" || echo "WARN: $ZNS_DEV answers the ABA report — looks like HYHOSTSSD, not pure ZNS (MODE=hyzns?)"
else
    [[ $AUX_DEV == "$BACKING" ]] && die "AUX_DEV == BACKING ($AUX_DEV)"
    [[ -b $BACKING ]] || die "backing device $BACKING not found (set BACKING=...)"
    aba_ok "$BACKING" || die "MODE=hyzns but $BACKING does not answer the ABA report — is the HYHOSTSSD (femu_mode=5) FEMU running?"
fi
[[ -x $DBBENCH ]] || die "$DBBENCH not built (see scripts/README.md: build)"
[[ -x $ZENFS ]]   || die "$ZENFS not built (see scripts/README.md: build)"

TS=$(date +%Y%m%d_%H%M%S)
RUNDIR=$RESULTS/${TS}_${MODE}_${GEOM}
CSV=$RESULTS/fig7_log.csv
mkdir -p "$RUNDIR"
[[ -f $CSV ]] || echo "utc,run_tag,mode,geom,workload,threads,num,micros_op,ops_sec,mb_s,log" > "$CSV"

# ---- run metadata --------------------------------------------------------
{
    echo "fig7 run $TS  MODE=$MODE GEOM=$GEOM"
    echo "NUM=$NUM KEY=$KEY VAL=$VAL COMP=$COMP AOZ=$AOZ JOBS=$JOBS RR_THREADS=$RR_THREADS READS=$READS/thread RWW=${RWW_THREADS}rd x $RWW_READS (+1 writer)"
    if (( AUX_PREMOUNTED )); then echo "WORKLOADS=$WORKLOADS  aux(premounted, not formatted)=$AUXDIR"
    else echo "WORKLOADS=$WORKLOADS  aux=$AUX_DEV -> $AUXDIR"; fi
    if [[ $MODE == hyzns ]]; then echo "backing=$BACKING r_zones=$R_ZONES dm=$DMNAME"
    else echo "zns_dev=$ZNS_DEV cap_gib=${ZNS_CAP_GIB:-whole-device}"; fi
    echo "HYSSD git: $(git rev-parse --short HEAD 2>/dev/null)"; uname -a
    lsblk -o NAME,SIZE,TYPE,MODEL 2>/dev/null
} > "$RUNDIR/runinfo.txt"

# ---- topology setup ------------------------------------------------------
setup_aux() {
    if (( AUX_PREMOUNTED )); then      # aux dir on a live, already-mounted FS
        case "$AUXDIR" in ""|/|/data2|/data2/) die "refusing to clear unsafe AUXDIR='$AUXDIR'";; esac
        (( DRYRUN )) && { echo "  (dryrun) rm -rf + mkdir $AUXDIR (premounted, NOT formatted)"; return 0; }
        mountpoint -q "$(dirname "$AUXDIR")" || die "$(dirname "$AUXDIR") not mounted — refusing to write aux"
        rm -rf "$AUXDIR"; mkdir -p "$AUXDIR"
        return 0
    fi
    (( DRYRUN )) && { echo "  (dryrun) mkfs.f2fs $AUX_DEV + mount $AUXMNT"; return 0; }
    umount "$AUXMNT" 2>/dev/null || true
    mkfs.f2fs -f "$AUX_DEV" &> "$RUNDIR/mkfs_aux.log" || die "mkfs.f2fs $AUX_DEV failed (see $RUNDIR/mkfs_aux.log)"
    mkdir -p "$AUXMNT"
    mount -t f2fs "$AUX_DEV" "$AUXMNT" || die "mount $AUX_DEV -> $AUXMNT failed"
    rm -rf "$AUXDIR"
}

dm_prepare_once() {                    # build (if stale) + reload the .ko
    (( DRYRUN )) && { echo "  (dryrun) dm-hyhost build+reload"; return 0; }
    dmsetup remove "$DMNAME" 2>/dev/null || true
    if [[ ! -f $DMHYHOST/dm-hyhost.ko ]] \
       || [[ -n $(find "$DMHYHOST" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer "$DMHYHOST/dm-hyhost.ko" 2>/dev/null) ]]; then
        echo "==> rebuilding dm-hyhost.ko"
        "$DMHYHOST/scripts/build.sh" || die "dm-hyhost build failed"
    fi
    "$DMHYHOST/scripts/load.sh" >/dev/null || die "dm-hyhost load failed"
}

FS_URI=""
setup_dev() {                          # fresh ZenFS for a write invocation
    local phase=$1
    if [[ $MODE != hyzns ]]; then      # zns and real: ZenFS on the raw ZBD
        local name; name=$(basename "$ZNS_DEV")
        local zs total last
        zs=$(cat "/sys/block/$name/queue/chunk_sectors")
        total=$(( $(blockdev --getsz "$ZNS_DEV") / zs ))
        last=$(( total - 1 ))
        local send=$last
        [[ ${SHALF:-0} == 1 ]] && send=$(( total/2 - 1 ))   # SHALF=1: use half the zones
        if [[ -n ${ZNS_CAP_GIB:-} ]]; then                  # force a capacity cap
            local ncap=$(( ZNS_CAP_GIB * 1073741824 / (zs * 512) ))
            (( ncap >= 1 )) || die "ZNS_CAP_GIB=$ZNS_CAP_GIB smaller than one zone"
            (( ncap - 1 < send )) && send=$(( ncap - 1 ))   # zones 0..send inclusive
        fi
        FS_URI="zenfs://dev:$name/0/$send/$AOZ/0/4294967296"
        (( DRYRUN )) && { echo "  (dryrun) blkzone reset + zenfs mkfs on $name (zones 0..$send of $total)"; return 0; }
        echo mq-deadline > "/sys/block/$name/queue/scheduler" 2>/dev/null \
            || echo "WARN: could not set mq-deadline on $name"
        blkzone reset "$ZNS_DEV" || die "blkzone reset $ZNS_DEV failed"
        "$ZENFS" mkfs --zbd="$name" --aux_path="$AUXDIR" \
            --start_zone=0 --end_zone="$send" --ao_zones="$AOZ" \
            --enable_gc=true --force &> "$RUNDIR/zenfs_mkfs_${phase}.log" \
            || die "zenfs mkfs failed (see $RUNDIR/zenfs_mkfs_${phase}.log)"
    else
        source "$DMHYHOST/scripts/_lib.sh"
        local zs=$HYHOST_ZONE_SECTORS sz total last dmdev
        sz=$(blockdev --getsz "$BACKING")
        total=$(( sz / zs )); last=$(( total - 1 ))
        # fig7: ZenFS uses HALF the S capacity (2x over-provisioning; also matches
        # a fig10 per-instance footprint). SHALF=0 to use the whole device.
        local send=$last
        [[ ${SHALF:-0} == 1 ]] && send=$(( R_ZONES + (total - R_ZONES)/2 - 1 ))   # SHALF=1: use half the S zones
        (( DRYRUN )) && { FS_URI="zenfs://dev:dm-0/$R_ZONES/$send/$AOZ/0/4294967296"
                          echo "  (dryrun) dm stack R=${R_ZONES}GiB + zenfs mkfs (zones $R_ZONES..$send of $total)"; return 0; }
        dmsetup remove "$DMNAME" 2>/dev/null || true
        reset_device_full_r "$BACKING" || true
        echo "0 $sz hyhost $BACKING $((R_ZONES*zs))" | dmsetup create "$DMNAME" || die "dmsetup create failed"
        dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*zs)) || die "set_r_end failed"
        # geometry contract: line = nchs*ways*2MiB -> line_pblocks = nchs*ways*512
        # (page 16KiB / block 2MiB fixed; GEOM must match the FEMU launch)
        local _gc _gw _lpb
        IFS=x read -r _gc _gw <<< "$GEOM"; _lpb=$(( _gc * _gw * 512 ))
        local st; st=$(dmsetup status "$DMNAME")
        echo "$st" > "$RUNDIR/dmstatus_${phase}_setup.txt"
        if [[ ${SKIP_D19_CHECK:-0} != 1 ]] && ! grep -q "line_pblocks=$_lpb" <<< "$st"; then
            die "geometry check failed — expected line_pblocks=$_lpb (GEOM=$GEOM) in: $st (SKIP_D19_CHECK=1 to override)"
        fi
        dmdev=$(basename "$(readlink -f "/dev/mapper/$DMNAME")")
        FS_URI="zenfs://dev:$dmdev/$R_ZONES/$send/$AOZ/0/4294967296"
        # R=0: no CNS on this device (aux is on the separate BBSSD) -> plain ZBD
        # mkfs. --hyssd requires reserved (R) zones, which don't exist at R=0.
        if (( R_ZONES > 0 )); then
            "$ZENFS" mkfs --zbd="$dmdev" --aux_path="$AUXDIR" --hyssd --aux_size="$R_ZONES" \
                --start_zone="$R_ZONES" --end_zone="$send" --ao_zones="$AOZ" \
                --enable_gc=true --force &> "$RUNDIR/zenfs_mkfs_${phase}.log" \
                || die "zenfs mkfs failed (see $RUNDIR/zenfs_mkfs_${phase}.log)"
        else
            "$ZENFS" mkfs --zbd="$dmdev" --aux_path="$AUXDIR" \
                --start_zone=0 --end_zone="$send" --ao_zones="$AOZ" \
                --enable_gc=true --force &> "$RUNDIR/zenfs_mkfs_${phase}.log" \
                || die "zenfs mkfs failed (see $RUNDIR/zenfs_mkfs_${phase}.log)"
        fi
    fi
}

# run_bench / parse_log are provided by _fig_lib.sh.

# read_workload <inv> <benchmark> <threads> <reads|-1> [prep] — fresh aux+ZenFS,
# prep-fill (1 thread, NOT recorded; fillseq unless given), then the read
# workload on it (recorded). RWW preps with fillrandom: the paper measures the
# read path of an OVERLAPPING LSM, which fillseq (non-overlapping) hides.
read_workload() {
    local inv=$1 bench=$2 threads=$3 reads=$4 prep=${5:-fillseq}
    setup_aux; setup_dev "$inv"
    if run_bench "${inv}_fill" "$prep" 1 -1 0 0; then
        run_bench "$inv" "$bench" "$threads" "$reads" 1 || rc=1
    else rc=1; fi
}

# ---- comparison table ----------------------------------------------------
compare_modes() {
    echo; echo "---- latest per-mode results (geom=$GEOM, $CSV) ----"
    awk -F, -v geom="$GEOM" 'NR>1 && $4==geom { ops[$5","$3]=$9; seen[$5]=1 }
        END {
            n=split("RS RR RWW-R RWW-W RWW-T FS FR OW", order, " ")
            printf "%-8s %12s %12s %12s %12s\n", "workload", "zn540", "zns", "hyzns", "hyzns/zns"
            for (i=1; i<=n; i++) {
                w=order[i]; if (!(w in seen)) continue
                d=ops[w",zn540"]; z=ops[w",zns"]; h=ops[w",hyzns"]
                r=(z>0 && h>0) ? sprintf("%.3f", h/z) : "-"
                printf "%-8s %12s %12s %12s %12s\n", w, (d?d:"-"), (z?z:"-"), (h?h:"-"), r
            }
        }' "$CSV"
}

# ---- main ----------------------------------------------------------------
echo "fig7: MODE=$MODE GEOM=$GEOM NUM=$NUM AOZ=$AOZ JOBS=$JOBS -> $RUNDIR"
have() { [[ " $WORKLOADS " == *" $1 "* ]]; }
[[ $MODE == hyzns ]] && dm_prepare_once
rc=0

# write workloads — each on its own fresh DB, measuring only itself
if have FS; then setup_aux; setup_dev fs; run_bench fs "fillseq"    1 -1 0 || rc=1; fi
if have FR; then setup_aux; setup_dev fr; run_bench fr "fillrandom" 1 -1 0 || rc=1; fi
if have OW; then setup_aux; setup_dev ow; run_bench ow "overwrite"  1 -1 0 || rc=1; fi

# read workloads — each fillseq-populates a fresh DB, then runs the read
if have RS;  then read_workload rs  "readseq"          1              -1                      ; fi
if have RR;  then read_workload rr  "readrandom"       "$RR_THREADS"  "$READS"                ; fi
if have RWW; then read_workload rww "readwhilewriting" "$RWW_THREADS" "$RWW_READS" fillrandom ; fi

(( DRYRUN )) || compare_modes
echo; echo "logs: $RUNDIR   cumulative csv: $CSV"
echo "plot: python3 scripts/fig_plot.py --template --log $CSV $RESULTS/fig7_values.csv"
echo "      # fill the zn540 column, then:"
echo "      python3 scripts/fig_plot.py $RESULTS/fig7_values.csv -o $RESULTS/fig7.png"
exit $rc
