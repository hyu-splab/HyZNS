#!/bin/bash
# figC_modify_live.sh — grow R online (ABA-modify) UNDER a live db_bench_l0cns
# workload that would otherwise EIO-abort at small R: start R small, grow it on
# the fly when L0 pressure rises, and check that the run completes.
#
#   max-prov dm-hyhost (3rd ctr arg = whole-dev max_r_end) + aux mkfs -A -P, gated to R0.
#   start db_bench_l0cns (L0 SST -> aux F2FS on R).  poll dmsetup status; when GC is
#   losing (free_lines<=1 & gc_runs>=N) OR a fallback timer fires, issue
#   f2fs_io resize_cns R1 <target> to grow R0->R1.  Check db_bench finishes vs aborts.
#
# Usage: sudo ./scripts/legacy/figC_modify_live.sh [R0_GiB] [R1_GiB] [NUM]   (defaults 8 24 20000000)
set -uo pipefail
cd "$(dirname "$0")/../.."
DMHYHOST=../dm-hyhost; source "$DMHYHOST/scripts/_lib.sh"
BACKING=/dev/nvme0n1; DMNAME=hyhost0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYHOST_ZONE_SECTORS
R0=${1:-8}; R1=${2:-24}; NUM=${3:-20000000}
KEY=20; VAL=800; AOZ=7; JOBS=8
F2FS_IO=${F2FS_IO:-$(command -v f2fs_io || echo ../f2fs-tools-1.16.0/tools/f2fs_io/f2fs_io)}
RESULTS=results; ZENFS=./plugin/zenfs/util/zenfs; BIN=./db_bench_l0cns
TS=$(date +%Y%m%d_%H%M%S); LOG="$RESULTS/${TS}_figC4_modlive_r${R0}to${R1}_n${NUM}.log"
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1))
[[ -x $BIN ]] || { echo "missing $BIN"; exit 1; }
[[ -x $F2FS_IO ]] || { echo "no f2fs_io ($F2FS_IO)"; exit 1; }

echo "=== setup: max-prov dm, R0=${R0} GiB (max_r_end=whole dev) ==="
umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
lsmod | awk '{print $1}' | grep -qx dm_hyhost || "$DMHYHOST/scripts/load.sh" >/dev/null
reset_device_full_r "$BACKING" || true
SZ=$(blockdev --getsz "$BACKING")
echo "0 $SZ hyhost $BACKING $((R0*ZS)) $SZ" | dmsetup create "$DMNAME"     # 3rd arg = max-prov
dmsetup message "$DMNAME" 0 set_r_end $((R0*ZS))
mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1 || { echo "mkfs -A -P failed"; exit 1; }
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
touch "$AUX/.modtarget"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R0" \
    --start_zone="$R0" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1
echo "  R0 nr_rzones=$(report_rzones "$DEV"), aux avail=$(df -B1G --output=avail "$AUX"|tail -1|tr -dc 0-9) GiB"

echo "=== launch db_bench_l0cns (R0=${R0}, NUM=${NUM}) ==="
"$BIN" --fs_uri="zenfs://dev:dm-0/${R0}/${S_LAST}/${AOZ}/0/4294967296" \
    --benchmarks="fillrandom,stats" --use_direct_io_for_flush_and_compaction \
    --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
    --max_background_jobs="$JOBS" --statistics --stats_interval_seconds=1 &> "$LOG" &
DBPID=$!
echo "  db_bench pid=$DBPID -> $LOG"

echo "=== monitor + grow trigger (R0->R1) ==="
grown=0
for t in $(seq 1 180); do
    kill -0 $DBPID 2>/dev/null || { echo "  [t=${t}s] db_bench EXITED (rc pending)"; break; }
    st=$(dmsetup status "$DMNAME" 2>/dev/null)
    fl=$(echo "$st"|grep -oE 'free_lines=[0-9]+'|grep -oE '[0-9]+')
    gr=$(echo "$st"|grep -oE 'gc_runs=[0-9]+'|grep -oE '[0-9]+')
    ns=$(echo "$st"|grep -oE 'nospc=[0-9]+'|grep -oE '[0-9]+')
    if [[ $grown -eq 0 ]]; then
        # trigger: GC losing (free_lines<=1 AND gc_runs>=3) OR fallback at t>=70s
        if { [[ ${fl:-9} -le 1 && ${gr:-0} -ge 3 ]]; } || [[ $t -ge 70 ]]; then
            echo "  [t=${t}s] TRIGGER grow: free_lines=$fl gc_runs=$gr nospc=$ns -> resize_cns $R1"
            if "$F2FS_IO" resize_cns "$R1" "$AUX/.modtarget" > /tmp/c4_grow.log 2>&1; then
                echo "  [t=${t}s] resize_cns grow returned 0; nr_rzones=$(report_rzones "$DEV") avail=$(df -B1G --output=avail "$AUX"|tail -1|tr -dc 0-9) GiB"
            else
                echo "  [t=${t}s] resize_cns FAILED:"; cat /tmp/c4_grow.log
            fi
            grown=1
        fi
    fi
    [[ $((t % 10)) -eq 0 ]] && echo "  [t=${t}s] free_lines=$fl gc_runs=$gr nospc=$ns grown=$grown ops=$(grep -oE '[0-9]+ ops/sec' "$LOG"|tail -1)"
    sleep 1
done

# bounded wait — if db_bench hangs (e.g. grow deadlock), don't block forever
RC=0
if kill -0 $DBPID 2>/dev/null; then
    for w in $(seq 1 300); do kill -0 $DBPID 2>/dev/null || break; sleep 2; done
    if kill -0 $DBPID 2>/dev/null; then
        echo "  WEDGE: db_bench still alive after wait window -> SIGTERM (single, gentle)"
        kill -TERM $DBPID 2>/dev/null; sleep 5; kill -0 $DBPID 2>/dev/null && kill -KILL $DBPID 2>/dev/null
        RC=124
    fi
fi
wait $DBPID 2>/dev/null || RC=${RC:-$?}
echo "=== RESULT (rc=$RC) ==="
echo "  result: $(grep -E '^fillrandom ' "$LOG"|tail -1|sed 's/  */ /g')"
echo "  errors: $(grep -cE 'write failed|put error' "$LOG") (put-error => aborted)"
echo "  final dmstatus: $(dmsetup status "$DMNAME"|grep -oE 'free_lines=[0-9]+|gc_runs=[0-9]+|gc_mig=[0-9]+|nospc=[0-9]+'|tr '\n' ' ')"
dmsetup status "$DMNAME" > "${LOG%.log}_dmstatus.txt" 2>/dev/null || true
umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
echo "C4_DONE rc=$RC log=$LOG"
