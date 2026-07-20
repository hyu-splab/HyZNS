#!/bin/bash
# fig9_rww.sh — paper Fig9/Fig10 (fig:res_effect_throughput / fig:resize-cdf):
#   ResizeCNS effect on a LIVE readwhilewriting run. ONE grow and/or ONE shrink
#   fired at chosen times (relative to the first ===REPORT = the plots' x axis),
#   RWW only, NO fio prefill (the shrink drains whatever F2FS actually holds).
#
#   sudo -E env bash fig9_rww.sh                             # baseline (no resize)
#   sudo -E env GROW_AT=60 bash fig9_rww.sh                  # grow once @t=60s
#   sudo -E env SHRINK_AT=60 bash fig9_rww.sh                # shrink once
#   sudo -E env GROW_AT=60 SHRINK_AT=120 bash fig9_rww.sh    # both in one run
#   (comma lists still work: GROW_AT=60,90 — but the paper shape is once each)
#
# knobs (env):
#   GROW_AT/SHRINK_AT  seconds relative to first ===REPORT (empty = none = baseline)
#   NUM(10M)   prefill KV count (uncapped fillrandom, separate run)
#   DUR(180)   RWW wall duration (s) — size it so all triggers fit
#   THREADS(16) RWW reader threads (db_bench adds its writer thread)
#   READS      optional reads/thread cap (default: unlimited, DUR ends the run)
#   WRITE_RATE optional --benchmark_write_rate_limit for the RWW writer (B/s)
#   BG(8)      max_background_jobs (env-overridable; the older ops-timeline
#              runs used the RocksDB default 2)
#   R0(4) RMAX(24) AOZ(14, single-instance budget) KEY(20) VAL(800) COMP(snappy)
#   RPREP_GB(0)/RPREP_PASSES(4)  shrink-migration prep: write an RPREP_GB file
#              on the aux F2FS and overwrite it PASSES times (file KEPT alive),
#              so live blocks spread across R at both the F2FS and dm layers and
#              the shrink actually has data to migrate. Shrink runs: R0=8 + this.
#   STATS      --statistics (default: baseline=1, resize runs=0)
#   ZOOM_PAD(15) WINDOWS("A:B[,A:B]")  post-processing views (re-run via fig9_post)
#
# Artifacts in /dev/shm/hyexp/resize_ops/<tag>_<ts>/ then copied to
# results/resize_ops/<tag>_<ts>/. Auto-post: timeline, per-event phase-shaded
# zooms (Fig9), read-only in/out-of-resize CDFs (Fig10 via fig10_cdf.py).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)              # repo root
source "$ROOT/dm-hyzns/scripts/_lib.sh"
B=/dev/nvme0n1; N=hyzns0; DEV=/dev/mapper/$N; MNT=/mnt/aux; AUXI=$MNT/i1
# use the installed f2fs_io (the source-tree binary may not be built here);
# env-overridable.
F2IO=${F2IO:-/usr/local/sbin/f2fs_io}
ZENFS="$ROOT/rocksdb/plugin/zenfs/util/zenfs"
BIN=${BIN:-"$ROOT/rocksdb/db_bench_zns"}       # standard build: WAL+L0 on S
ZS=$HYZNS_ZONE_SECTORS; SZ=$(blockdev --getsz "$B"); SLAST=$(( SZ/ZS - 1 ))
GROW_AT=${GROW_AT:-}; SHRINK_AT=${SHRINK_AT:-}
NUM=${NUM:-10000000}; KEY=${KEY:-20}; VAL=${VAL:-800}; COMP=${COMP:-snappy}
# PREFILL=fillseq = the grow "bad case": sequential data stays live (compaction
# never invalidates it), so the boundary zone is FULL of live SSTs and the grow
# roll-off is heavy. fillrandom churn kills early zones -> tens-of-MiB roll-offs.
PREFILL=${PREFILL:-fillrandom}
DUR=${DUR:-180}; THREADS=${THREADS:-16}; BG=${BG:-8}
R0=${R0:-4}; RMAX=${RMAX:-24}; AOZ=${AOZ:-14}; CTL=$MNT/ctl.dat
RPREP_GB=${RPREP_GB:-0}; RPREP_PASSES=${RPREP_PASSES:-4}
RES="$ROOT/hyznsd/results/resize_ops"
RCAP=$(( SZ/ZS - 8 ))
ARB="ZENFS_ZONE_ARBITER=1 ZENFS_ARBITER_EXTERNAL=1 ZENFS_ARBITER_INTERVAL_MS=100 ZENFS_ARBITER_MAX_RZONE=$RCAP ZENFS_ARBITER_MIN_RZONE=2 ZENFS_GROW_DRAIN_POLL_MS=20 ZENFS_GROW_DRAIN_BUDGET_MS=120000"
cur_R(){ echo $(( $(dmsetup status $N|grep -oE 'r_end=[0-9]+'|cut -d= -f2) / ($(dmsetup status $N|grep -oE 'zone_pblocks=[0-9]+'|cut -d= -f2)*8) )); }
dm_gcmig(){ dmsetup status $N 2>/dev/null|grep -oE 'gc_mig=[0-9]+'|cut -d= -f2; }

# merged trigger list "t:dir", time-sorted, so grow/shrink interleave freely
EVENTS=""
for g in ${GROW_AT//,/ };   do EVENTS="$EVENTS $g:grow";   done
for s in ${SHRINK_AT//,/ }; do EVENTS="$EVENTS $s:shrink"; done
EVENTS=$(echo $EVENTS | tr ' ' '\n' | sort -t: -k1 -n | paste -sd' ' -)
MODE=$([ -n "$EVENTS" ] && echo resize || echo vanilla)
STATS=${STATS:-$([ "$MODE" = vanilla ] && echo 1 || echo 0)}

DBPID=""; RUN=""; COLLECTED=1; PPDONE=1
collect(){
  [ -n "$RUN" ] && [ "$COLLECTED" = 0 ] || return 0
  COLLECTED=1
  local dst=$RES/$(basename "$RUN"); mkdir -p "$dst"
  [ -d "$RUN/oplat" ] && tar czf "$RUN/oplat.tar.gz" -C "$RUN" oplat 2>/dev/null
  find "$RUN" -maxdepth 1 -type f -exec cp {} "$dst"/ \;
  echo "[collect] -> $dst"; ls -1 "$dst" | sed 's/^/    /'
}
postprocess(){
  [ -n "$RUN" ] && [ "$PPDONE" = 0 ] || return 0
  PPDONE=1
  [ -f "$DBLOG" ] || : > "$DBLOG"
  # RocksDB info LOG (on the aux F2FS -> gone after umount): the source of the
  # 3-panel timeline (L0 file count, stall causes, flush/compaction lanes)
  local RLOG
  for RLOG in "$AUXI/rocksdbtest/dbbench/LOG" \
              $(find "$MNT" -maxdepth 4 -iname LOG -type f 2>/dev/null); do
    [ -f "$RLOG" ] && { cp "$RLOG" "$RUN/rocksdb_LOG" 2>/dev/null; break; }
  done
  [ -f "$RUN/rocksdb_LOG" ] || echo "[postprocess] WARN: rocksdb LOG not found — timeline will be throughput-only"
  dmesg > "$RUN/kernel.log" 2>/dev/null
  { echo "ts_epoch,total_ops_per_100ms,read_ops_per_100ms,write_ops_per_100ms"
    grep "===REPORT" "$DBLOG" 2>/dev/null | awk '{print $1","$3","$4","$5}'; } > "$RUN/ops.csv"
  { echo "ts_epoch,bench"
    tr '\r' '\n' < "$DBLOG" 2>/dev/null | awk '/===PHASE_START/{print $1","$3}'; } > "$RUN/phases.csv"
}
cleanup(){ [ -n "$RUN" ] || return 0
  if [ -n "$DBPID" ]; then
    kill "$DBPID" 2>/dev/null
    for i in $(seq 1 20); do kill -0 "$DBPID" 2>/dev/null || break; sleep 0.5; done
    kill -9 "$DBPID" 2>/dev/null; wait "$DBPID" 2>/dev/null; DBPID=""
  fi
  postprocess; collect
  umount "$MNT" 2>/dev/null || { sleep 2; umount "$MNT" 2>/dev/null || true; }
  dmsetup remove "$N" 2>/dev/null || true
  echo "[cleanup]"; }
trap cleanup EXIT
trap 'echo "[abort] signal"; exit 130' INT TERM

TS=$(date +%Y%m%d_%H%M%S); tag=fig9_rww_${MODE}
RUN=/dev/shm/hyexp/resize_ops/${tag}_$TS; mkdir -p "$RUN/oplat" "$RES"
COLLECTED=0; PPDONE=0
DBLOG=$RUN/db.log; EVT=$RUN/events.csv
{
  echo "exp=fig9_rww tag=$tag ts=$TS"
  echo "mode=$MODE events='${EVENTS:-none}' (t rel. to first ===REPORT)"
  echo "workload=readwhilewriting dur=${DUR}s threads=$THREADS(readers)+1writer reads/thread=${READS:-unlimited} write_rate=${WRITE_RATE:-uncapped}"
  echo "prefill=$PREFILL num=$NUM key=$KEY val=$VAL comp=$COMP"
  echo "single-instance ao=$AOZ R0=$R0 RMAX=$RMAX(gate cap) step=1/event max_background_jobs=$BG statistics=$STATS rprep=${RPREP_GB}GBx${RPREP_PASSES}"
  echo "device=$((SZ/2048))MiB zones=$((SZ/ZS)) bin=$BIN"
} > "$RUN/runinfo"
echo "dir,target_s,req_t_epoch,echo_t_epoch,done_t_epoch,R_pre,R_post,roll_mib,k_ms,f2_mib,dm_mib,dm_gcmig_MiB" > "$EVT"
dmesg -C 2>/dev/null || true

echo "=== [$tag] setup: R gate $RMAX->$R0, ZenFS at ABA=$R0 (no gap), SLAST=$SLAST ==="
umount "$MNT" 2>/dev/null||true; dmsetup remove "$N" 2>/dev/null||true
"$ROOT/dm-hyzns/scripts/load.sh" >/dev/null 2>&1
reset_device_full_r "$B" >/dev/null 2>&1||true
echo "0 $SZ hyzns $B $((RMAX*ZS)) $SZ" | dmsetup create "$N"; dmsetup message "$N" 0 set_r_end $((RMAX*ZS))
mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1 || { echo MKFS_FAIL; exit 1; }
mkdir -p "$MNT"; mount -t f2fs "$DEV" "$MNT"
dd if=/dev/zero of="$CTL" bs=4k count=1 oflag=direct >/dev/null 2>&1; sync
# installed f2fs_io ships modify_zone (resize_cns was renamed; same signature)
f2io_set(){ "$F2IO" modify_zone "$1" "$CTL" >/dev/null 2>&1 || "$F2IO" resize_cns "$1" "$CTL" >/dev/null 2>&1; }
f2io_set "$R0"; sync
while [ "$(cur_R)" -gt "$R0" ]; do f2io_set $(( $(cur_R)-1 )) || { echo "gate stuck R=$(cur_R)"; break; }; sync; done
echo 1 > /sys/fs/f2fs/dm-0/gc_urgent 2>/dev/null||true
fl=""
for i in $(seq 1 30); do fstrim "$MNT" 2>/dev/null; sync
  fl=$(dmsetup status $N|grep -oE 'free_lines=[0-9]+'|cut -d= -f2)
  [ "${fl:-0}" -ge $((R0-2)) ] && break; sleep 1; done
echo 0 > /sys/fs/f2fs/dm-0/gc_urgent 2>/dev/null||true; sync
echo "  ABA=R$(cur_R) free_lines=${fl:-?}/$R0 (gate residue discarded)"
if [ "$RPREP_GB" -gt 0 ]; then
  # shrink-migration prep: repeated in-place overwrites of one KEPT file make
  # F2FS log-write its blocks across segments (and dm across lines), so the
  # tail zones removed by a later shrink hold LIVE data -> real migration.
  echo "=== [$tag] R-prep: ${RPREP_GB}GiB x${RPREP_PASSES} overwrites on aux F2FS (file kept) ==="
  for p in $(seq 1 "$RPREP_PASSES"); do
    dd if=/dev/zero of="$MNT/rprep.dat" bs=1M count=$((RPREP_GB*1024)) \
       conv=fsync,notrunc oflag=direct 2>/dev/null \
      || { echo RPREP_FAIL pass=$p; exit 1; }
  done
  sync
  echo "  rprep done: live $(du -m "$MNT/rprep.dat" 2>/dev/null | cut -f1)MiB on R"
fi
mkdir -p "$AUXI"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUXI" --hyssd --aux_size="$R0" --start_zone="$R0" \
  --end_zone="$SLAST" --ao_zones="$AOZ" --enable_gc=true --force >"$RUN/zmkfs.log" 2>&1 \
  || { echo ZMKFS_FAIL; tail "$RUN/zmkfs.log"; exit 1; }

echo "=== [$tag] prefill: uncapped $PREFILL num=$NUM (separate run) ==="
env $ARB "$BIN" --fs_uri="zenfs://dev:dm-0/$R0/$SLAST/$AOZ/0/4294967296" \
  --benchmarks=$PREFILL --use_direct_io_for_flush_and_compaction \
  --compression_type=$COMP --num=$NUM --key_size=$KEY --value_size=$VAL \
  --max_background_jobs=$BG > "$RUN/prefill.log" 2>&1 \
  || { echo PREFILL_FAIL; tail "$RUN/prefill.log"; exit 1; }

echo "=== [$tag] RWW dur=${DUR}s threads=$THREADS bg=$BG stats=$STATS ==="
env ROCKSDB_REPORT_INTERVAL_MS=100 ROCKSDB_OPLAT_DIR="$RUN/oplat" $ARB "$BIN" \
  --fs_uri="zenfs://dev:dm-0/$R0/$SLAST/$AOZ/0/4294967296" \
  --benchmarks=readwhilewriting,stats --use_existing_db=1 \
  --use_direct_io_for_flush_and_compaction --compression_type=$COMP \
  --duration=$DUR --threads=$THREADS \
  $([ -n "${READS:-}" ] && echo --reads=$READS) \
  $([ "${READ_DIRECT:-0}" = 1 ] && echo --use_direct_reads) \
  $([ -n "${WRITE_RATE:-}" ] && echo --benchmark_write_rate_limit=$WRITE_RATE) \
  $([ "${SYNC:-0}" = 1 ] && echo --sync=1) \
  --num=$NUM --key_size=$KEY --value_size=$VAL \
  $([ "$STATS" = 1 ] && echo --statistics) \
  --histogram --max_background_jobs=$BG --report_interval_seconds=1 &> "$DBLOG" & DBPID=$!

for k in $(seq 1 240); do grep -q "===REPORT" "$DBLOG" 2>/dev/null && break
  kill -0 "$DBPID" 2>/dev/null || { echo dbexit; tail "$DBLOG"; exit 1; }; sleep 0.5; done
T0=$(grep -m1 "===REPORT" "$DBLOG" | awk '{print $1}')
echo "  t0=$T0 (GROW_AT/SHRINK_AT are relative to this = the plots' x axis)"

for ev in $EVENTS; do
  tgt=${ev%%:*}; dir=${ev##*:}
  while :; do
    awk -v a="$(date +%s.%N)" -v t0="$T0" -v g="$tgt" 'BEGIN{exit !(a-t0>=g)}' && break
    kill -0 "$DBPID" 2>/dev/null || break; sleep 0.05
  done
  kill -0 "$DBPID" 2>/dev/null || { echo "  db ended before t=$tgt"; break; }
  R=$(cur_R)
  if [ "$dir" = grow ]; then
    [ "$R" -ge "$RMAX" ] && { echo "  grow cap R=$R"; continue; }
    NRtgt=$((R+1)); trig=$AUXI/.zenfs_grow
  else
    [ "$R" -le 2 ] && { echo "  shrink floor R=$R"; continue; }
    NRtgt=$((R-1)); trig=$AUXI/.zenfs_shrink
  fi
  req_t=$(awk -v t0="$T0" -v g="$tgt" 'BEGIN{printf "%.3f", t0+g}')
  gm0=$(dm_gcmig)
  echo "===HYEXP ${dir}_${tgt} BEGIN R=$R===" > /dev/kmsg 2>/dev/null||true
  echo_t=$(date +%s.%N)
  echo 1 > "$trig"
  for j in $(seq 1 1800); do [ "$(cur_R)" = "$NRtgt" ] && break; kill -0 "$DBPID" 2>/dev/null||break; sleep 0.1; done
  done_t=$(date +%s.%N); NR=$(cur_R); sync; sleep 0.2
  echo "===HYEXP ${dir}_${tgt} END===" > /dev/kmsg 2>/dev/null||true
  gm1=$(dm_gcmig); gcmib=$(( (${gm1:-0} - ${gm0:-0}) * 4 / 1024 ))
  roll=""
  if [ "$dir" = grow ]; then
    GL=$(grep -E "\[GrowLAT\]" "$DBLOG"|awk '/begin/{b=$0;c=1;next} c{b=b"\n"$0; if(/done/){last=b;c=0}} END{print last}')
    mb=$(echo "$GL"|awk '/begin/{for(i=1;i<=NF;i++)if($i~/movebytes=/){split($i,m,"=");print m[2]}}'|head -1)
    roll=$(awk -v m=${mb:-0} 'BEGIN{printf "%.0f",m/1048576}')
  fi
  kms=$(dmesg | awk -v s="${dir}_${tgt}" '$0 ~ (s" BEGIN"){f=1} f&&/resize total/{print; f=0}' \
        | grep -oE "[0-9]+ ms" | grep -oE "[0-9]+" | head -1); kms=${kms:-0}
  # shrink: F2FS-side migration from the kernel summary ("valid blocks moved
  # ... (N MiB)") — the ground truth for f2_mib; dm-side is dm_gcmig_MiB.
  f2mib=""
  if [ "$dir" = shrink ]; then
    f2mib=$(dmesg | awk -v s="${dir}_${tgt}" '$0 ~ (s" BEGIN"){f=1} f&&/valid blocks moved/{print; exit}' \
            | grep -oE "\([0-9]+ MiB\)" | grep -oE "[0-9]+"); f2mib=${f2mib:-0}
  fi
  echo "$dir,$tgt,$req_t,$echo_t,$done_t,$R,$NR,${roll},$kms,${f2mib},,$gcmib" >> "$EVT"
  printf "  %s @%ss R%d->%d %s in %.1fs (kernel %sms, dm gc_mig +%sMiB)\n" \
    "$dir" "$tgt" "$R" "$NR" "${roll:+roll=${roll}MiB}" \
    "$(awk -v d=$done_t -v e=$echo_t 'BEGIN{print d-e}')" "$kms" "$gcmib"
done

WDOG=${WATCHDOG_S:-120}
echo "=== [$tag] running to dur=${DUR}s (watchdog: ${WDOG}s of 0 ops aborts) ==="
nz0=0; t_ok=$(date +%s); k=0
while kill -0 "$DBPID" 2>/dev/null; do
  sleep 5; k=$((k+1)); now=$(date +%s)
  nz1=$(tr '\r' '\n' < "$DBLOG" | awk '/===REPORT/ && $3>0' | wc -l)
  [ "$nz1" -gt "$nz0" ] && { nz0=$nz1; t_ok=$now; }
  if [ $((now - t_ok)) -ge "$WDOG" ]; then
    echo "[watchdog] 0 ops for ${WDOG}s -> aborting (kernel.log: free_lines/GC/nospc)"
    echo "abort=watchdog_zero_ops_${WDOG}s" >> "$RUN/runinfo"
    kill "$DBPID" 2>/dev/null; break
  fi
  [ $((k % 6)) -eq 0 ] && echo "  [t=$((now - ${T0%.*}))s] 100ms-bins with ops: $nz1"
done
wait "$DBPID" 2>/dev/null; DBPID=""
postprocess

echo "=== [$tag] auto-plots (re-run later: bash fig9_post.sh <results dir>) ==="
bash "$HERE/fig9_post.sh" "$RUN" || true

{
  echo "== $tag ($TS) =="
  cat "$RUN/runinfo"; echo
  echo "-- resize events --"; column -t -s, "$EVT"
  echo; echo "-- RWW summary --"
  tr '\r' '\n' < "$DBLOG" | grep -E "^(readwhilewriting|rww-writes)\s+:"
} > "$RUN/SUMMARY.txt"
cat "$RUN/SUMMARY.txt"
collect
echo "=== DONE ==="
