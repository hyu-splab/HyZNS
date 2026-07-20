#!/bin/bash
# fig10_dual_grow.sh — Fig10 (L0->CNS) DUAL, daemon-driven, UNBOUNDED grow (no
# artificial R_MAX). 256GB device split 2:1 by *device* fraction:
#
#   [ ───── inst1 territory = 2/3 (≈170 z) ───── ][ inst2 territory = 1/3 (≈86 z) ]
#   [ R (shared CNS/L0, ↑grow) │ inst1 ZenFS-S(←) ]  [ inst2 ZenFS-S (fixed) ]
#     0 ............... r_end     r_end ..... MID       MID ............ S_LAST
#
# R (shared F2FS-aux holding BOTH instances' L0) grows from R_INIT up into inst1's
# generous 2/3 territory (inst1-S is pushed up via the arbiter as ABA moves). inst2
# sits in its private 1/3 and R never reaches it. The only ceiling on R is the
# inst1|inst2 boundary (MID) — not an artificial cap, but the line that protects the
# 2nd tenant. Daemon drives GROW only (no shrink) at step=1.
#
#   sudo -E ./scripts/fig10_dual_grow.sh [NUM_PER_INST] [JOBS]   default 20000000 8
#   env: SPLIT_NUM/SPLIT_DEN (default 2/3 inst1), BENCH (default fillrandom,overwrite),
#        FSTRIM=1 (default; drains discard-lag so R_C reflects true-live), TMO
set -uo pipefail
cd "$(dirname "$0")/.."                                  # hyznsd/
RDB=../rocksdb; ZENFS=$RDB/plugin/zenfs/util/zenfs
source ../dm-hyzns/scripts/_lib.sh
BACKING=/dev/nvme0n1; NAME=hyzns0; DEV=/dev/mapper/$NAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS; F2FS_IO=${F2FS_IO:-../f2fs-tools-1.16.0/tools/f2fs_io/f2fs_io}
CONFIG=${1:-L0_CNS_daemon}; NUM=${2:-20000000}; JOBS=${3:-8}; AOZ=7; KEY=20; VAL=800; TMO=${TMO:-3600}
MON_INT=${MON_INT:-2}       # mon sampler interval (s); fractional ok, e.g. 0.5.
# The aux-du (ls|wc -c) + LOG parse make each sample heavy — below ~0.3s the
# sampler starts perturbing the workload. 0.5 is a good dense default.
MON_DU_EVERY=${MON_DU_EVERY:-1}   # do the heavy aux .sst du/count only every Nth
                                  # sample (light dm/L0 fields sampled every time)
# OW_NUM>0: run overwrite for OW_NUM ops (not NUM) via db_bench's per-benchmark
# [N<n>] override -> deep FR fill (NUM) + shorter OW, ONE process, no reopen.
OW_NUM=${OW_NUM:-0}
if [ "$OW_NUM" -gt 0 ]; then BENCH=${BENCH:-fillrandom,overwrite[N${OW_NUM}]}; fi
BENCH=${BENCH:-fillrandom,overwrite}
R_INIT=${R_INIT:-4}
SZ=$(blockdev --getsz "$BACKING"); TOTAL=$((SZ/ZS)); S_LAST=$((TOTAL-1))
# 2:1 device split. inst1 territory = [0, MID) (holds R + inst1-S); inst2 = [MID, S_LAST].
SPLIT_NUM=${SPLIT_NUM:-2}; SPLIT_DEN=${SPLIT_DEN:-3}
# RWW=1: measured phase is readwhilewriting (RWW_THREADS readers x RWW_READS
# + 1 writer, use_existing_db) after a fillrandom NUM prefill; per-op read
# latencies traced to tmpfs (ROCKSDB_OPLAT_DIR) -> ${pfx}_oplat.tar.gz
RWW=${RWW:-0}
RWW_THREADS=${RWW_THREADS:-1}          # RocksDB default reader count (+1 writer)
RWW_READS=${RWW_READS:-$(( 10000000 / RWW_THREADS ))}
MID=$(( TOTAL * SPLIT_NUM / SPLIT_DEN ))                  # inst1|inst2 boundary (= R grow ceiling)
I1_START=$R_INIT; I1_END=$((MID-1)); I2_START=$MID; I2_END=$S_LAST
R_MAX=${R_MAX:-$MID}                                      # R grow ceiling (default: inst2 boundary).
# NOTE: past 16 zones F2FS leaves SMALL_VOLUME auto-tuning (IPU off -> slow
# discard+GC); cap R_MAX<=16 to keep L0_CNS in the in-place regime.
[ "$R_MAX" -gt "$MID" ] && R_MAX=$MID                      # never exceed the inst2 boundary
# config matrix: BIN / L0AUX (L0->R?) / MAXPROV (R can grow?) / DAEMON / ARB1(inst1 arbiter env)
case "$CONFIG" in
  VANILLA)       BIN=$RDB/db_bench_zns;   L0AUX=0; MAXPROV=0; DAEMON=0; ARB1="";;
  L0_CNS_static) BIN=$RDB/db_bench_l0cns; L0AUX=1; MAXPROV=0; DAEMON=0; ARB1="";;   # R fixed -> nospc dies
  L0_CNS_daemon) BIN=$RDB/db_bench_l0cns; L0AUX=1; MAXPROV=1; DAEMON=1;
                 ARB1="ZENFS_ZONE_ARBITER=1 ZENFS_ARBITER_EXTERNAL=1 ZENFS_ARBITER_INTERVAL_MS=200 ZENFS_ARBITER_MAX_RZONE=$R_MAX";;
  *) echo "bad CONFIG=$CONFIG (VANILLA|L0_CNS_static|L0_CNS_daemon)"; exit 1;;
esac
OUT=results/fig10_dual_grow; mkdir -p "$OUT"; STAMP=$(date +%Y%m%d_%H%M%S)
# ALL live logs (mon csv, daemon.log, db i1/i2, zmkfs) go to tmpfs during the run
# (no disk IO alongside the workload); cleanup() copies them into $OUT at exit.
RUN=/dev/shm/hyexp/fig10_live; mkdir -p "$RUN"
pfx=$RUN/${CONFIG}_${STAMP}
MON=$pfx.csv; DPID=""; MPID=""; FTPID=""; P1=""; P2=""; GUPID=""; WPID=""
# run metadata for the wrapper SUMMARY (copied with the other ${pfx}.* logs)
{ echo "config=$CONFIG  num=$NUM/inst  jobs=$JOBS  bench=$BENCH"
  echo "device=$TOTAL zones ($TOTAL GiB)  backing=$BACKING"
  echo "split=$SPLIT_NUM/$SPLIT_DEN -> i1=[0,$MID) ($MID z, R+i1-S) : i2=[$MID,$TOTAL) ($((TOTAL-MID)) z)"
  echo "R_INIT=$R_INIT  R_MAX=$R_MAX  watchdog=${WATCHDOG_S:-300}s  tmo=${TMO}s  fstrim=${FSTRIM:-1} gcurgent_loop=${GCURGENT:-0}"
} > "$pfx.runinfo"
for b in "$ZENFS" "$BIN" ./hyznsd; do [ -x "$b" ]||{ echo "missing $b"; exit 1; }; done
dm_f(){ dmsetup status "$NAME" 2>/dev/null|grep -oE "$1=[0-9]+"|head -1|cut -d= -f2; }
cur_R(){ echo $(( $(dm_f r_end)/( $(dm_f zone_pblocks)*8) )); }
# count EMPTY zones whose start-zone index is in [lo,hi). blkzone line has "start: 0x..".
zfree(){ blkzone report "$DEV" 2>/dev/null | awk -v lo=$1 -v hi=$2 -v zs=$ZS '
  /em/ && match($0,/start: (0x[0-9a-f]+)/,m){ s=strtonum(m[1])/zs; if(s>=lo && s<hi) c++ }
  END{print c+0}'; }
ext(){ tr '\r' '\n' < "$2" 2>/dev/null | grep -E "^$1 +:" | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }
cleanup(){ for p in "$WPID" "$GUPID" "$FTPID" "$MPID" "$DPID" "$P1" "$P2"; do [ -n "$p" ]&&kill "$p" 2>/dev/null; done
  echo 0 > /sys/fs/f2fs/dm-0/gc_urgent 2>/dev/null||true
  wait 2>/dev/null
  # capture the RocksDB info LOGs (L0 counts / compaction / stalls) + F2FS
  # segment_info before unmount
  cp "$AUX/i1/rocksdbtest/dbbench/LOG" "${pfx}_rocksdb_LOG_i1" 2>/dev/null || true
  cp "$AUX/i2/rocksdbtest/dbbench/LOG" "${pfx}_rocksdb_LOG_i2" 2>/dev/null || true
  cat /sys/kernel/debug/f2fs/status > "${pfx}_f2fs_status" 2>/dev/null || true
  { echo "== live aux-L0 SST files at teardown =="
    ls -l "$AUX/i1/rocksdbtest/dbbench/"*.sst "$AUX/i2/rocksdbtest/dbbench/"*.sst 2>/dev/null | awk '{n++; b+=$5} END{print n" files, "b/1048576" MiB"}'
  } > "${pfx}_aux_live" 2>/dev/null || true
  umount "$AUX" 2>/dev/null||true; dmsetup remove "$NAME" 2>/dev/null||true
  cp ${pfx}.* ${pfx}_* "$OUT"/ 2>/dev/null   # tmpfs -> persistent results
  echo "[cleanup] done (logs -> $OUT/)"; }
trap cleanup EXIT
# ^C / kill must tear the WHOLE run down: children started with '&' inherit
# SIGINT ignored (POSIX non-interactive shell), so the ^C alone doesn't reach
# db_bench/monitors — exiting here makes the EXIT trap kill them all by PID.
trap 'echo "[abort] signal received"; exit 130' INT TERM

echo "=== fig10 DUAL [$CONFIG] : $TOTAL z, split $SPLIT_NUM/$SPLIT_DEN ==="
echo "    inst1 territory [0,$MID): R[0..r_end)+ZenFS-S[$I1_START..$I1_END]"
echo "    inst2 territory [$MID,$TOTAL): ZenFS-S[$I2_START..$I2_END]  (R-safe, fixed)"
echo "    config: dual, ao=$AOZ+$AOZ(=14), L0AUX=$L0AUX maxprov=$MAXPROV daemon=$DAEMON step=1 R_MAX=$R_MAX num=$NUM/inst jobs=$JOBS bench=$BENCH"
timeout 8 nvme list >/dev/null 2>&1 || { echo "FEMU down"; exit 2; }
umount "$AUX" 2>/dev/null||true; dmsetup remove "$NAME" 2>/dev/null||true
../dm-hyzns/scripts/load.sh>/dev/null   # ALWAYS reload (rmmod+insmod) -> run uses the freshly-built .ko
reset_device_full_r "$BACKING">/dev/null 2>&1||true
if [[ $MAXPROV == 1 ]]; then
  echo "0 $SZ hyzns $BACKING $((R_INIT*ZS)) $((R_MAX*ZS))"|dmsetup create "$NAME"||exit 1   # 6th=max -> R can grow
  dmsetup message "$NAME" 0 set_r_end $((R_INIT*ZS))||exit 1
  mkfs.f2fs -f -m -H -A -P "$DEV">/dev/null 2>&1||{ echo mkfs fail; exit 1; }
else
  echo "0 $SZ hyzns $BACKING $((R_INIT*ZS))"|dmsetup create "$NAME"||exit 1                  # 5th only -> R fixed at R_INIT
  dmsetup message "$NAME" 0 set_r_end $((R_INIT*ZS))||exit 1
  mkfs.f2fs -f -m -H -A "$DEV">/dev/null 2>&1||{ echo mkfs fail; exit 1; }
fi
mkdir -p "$AUX"
# mount with online discard so F2FS pushes TRIM to dm as soon as segments free
# (default 'nodiscard' = freed blocks sit as prefree and are NOT reusable ->
# aux-L0 ENOSPC while dm still shows free). discard_unit=block trims sub-segment.
mount -t f2fs -o discard,discard_unit=block "$DEV" "$AUX" \
  || mount -t f2fs -o discard "$DEV" "$AUX" \
  || mount -t f2fs "$DEV" "$AUX"
mkdir -p "$AUX/i1" "$AUX/i2"
# make F2FS discard AGGRESSIVE. Only touch knobs that EXIST+are writable on this
# kernel ([ -w ] guard avoids the redirect-permission-denied noise; this F2FS
# exposes: discard_granularity, max_small_discards, discard_idle_interval).
S=/sys/fs/f2fs/dm-0
setk(){ [ -w "$S/$1" ] && echo "$2" > "$S/$1" 2>/dev/null && echo -n "$1=$2 "; }
printf "  [f2fs discard] mounted -o discard; set: "
setk discard_granularity 1        # trim at 4KiB granularity (max eagerness)
setk max_small_discards 128000     # allow many small discards/round (no throttle)
setk discard_idle_interval 0       # don't wait for idle to issue discards
echo
echo "  [dev] dm-0 zoned=$(cat /sys/block/dm-0/queue/zoned 2>/dev/null) nr_zones=$(cat /sys/block/dm-0/queue/nr_zones 2>/dev/null) | F2FS discard_gran=$(cat /sys/fs/f2fs/dm-0/discard_granularity 2>/dev/null) discard-attrs=[$(ls /sys/fs/f2fs/dm-0/ 2>/dev/null | grep -i discard | tr '\n' ' ')]"
dmesg 2>/dev/null | grep -iE "F2FS-fs.*(zone|discard|Mount)" | tail -2 | sed 's/^/  [f2fs-dmesg] /'
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i1" --hyssd --aux_size="$R_INIT" --start_zone="$I1_START" \
  --end_zone="$I1_END" --ao_zones="$AOZ" --force >"$pfx.zmkfs_i1.log" 2>&1||{ echo "zenfs i1 fail"; tail "$pfx.zmkfs_i1.log"; exit 1; }
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX/i2" --hyssd --aux_size="$R_INIT" --start_zone="$I2_START" \
  --end_zone="$I2_END" --ao_zones="$AOZ" --force >"$pfx.zmkfs_i2.log" 2>&1||{ echo "zenfs i2 fail"; tail "$pfx.zmkfs_i2.log"; exit 1; }

if [[ ${FSTRIM:-1} == 1 && $L0AUX == 1 ]]; then
  ( while :; do sync; fstrim "$AUX" 2>/dev/null; sleep 2; done ) & FTPID=$!
  echo "  [fstrim loop on]"
fi
if [[ ${GCURGENT:-0} == 1 ]]; then
  ( while :; do echo 1 > /sys/fs/f2fs/dm-0/gc_urgent 2>/dev/null; sleep 1; done ) & GUPID=$!
  echo "  [gc_urgent loop on]"
fi
# daemon. Policy flags come from hyznsd.conf (single source of truth); an env
# var (FTR=0, FTR_GCU_PARK=1, ...) overrides ONLY when explicitly set.
if [[ $DAEMON == 1 ]]; then
  PSET=""
  for kv in ftr_grow_rc=FTR_GROW_RC ftr_grow_rz=FTR_GROW_RZ \
            ftr_shrink_rc=FTR_SHRINK_RC ftr_shrink_rz=FTR_SHRINK_RZ \
            ftr_gcu_resize=FTR_GCU_RESIZE ftr_gcu_park=FTR_GCU_PARK \
            rz_source=RZ_SOURCE; do
    key=${kv%%=*}; env=${kv#*=}
    [ -n "${!env:-}" ] && PSET="$PSET --set $key=${!env}"
  done
  echo "daemon_overrides=${PSET:-none}" >> "$pfx.runinfo"
  echo "RMAX=$R_MAX"
  ./hyznsd -q -c hyznsd.conf --set dm=$NAME --set backing=$BACKING --set fs=zenfs --set aux="$AUX/i1" \
    --set f2fs_io="$F2FS_IO" --set r_min=$R_INIT --set r_max=$R_MAX \
    --set s_end_zone=$((I1_END+1)) \
    --set snapfile="$pfx.resize_snap.csv" \
    $PSET --set logfile="$pfx.daemon.log" & DPID=$!
  sleep 1
fi
# monitor: dm(R,free,valid/invalid/free MiB) + F2FS(aux df used) + ZenFS(used_zones)
# 3-layer valid so the delayed-discard gap is visible: gap = dm_validMiB - f2fs_usedMiB
#   (F2FS freed dead L0 but dm still maps it until discard).
# 4-layer time-series: RocksDB (L0/L1/L2 file counts) | ZenFS/aux (actual .sst
# files+size ON the fs) | F2FS (true-live valid) | dm (valid/invalid/free). The
# key cross-check: RocksDB-L0-count x 64MB vs aux-du vs f2fs-valid vs dm-valid —
# a divergence localizes the leak; a match proves genuine L0 accumulation.
DBDIR=rocksdbtest/dbbench
( echo "t,R,i1free,i2free,free_lines,validGB,nospc,discards,discard_pgs,ovr,recycles,gc_mig,dm_validMiB,dm_invalidMiB,dm_freeMiB,f2fs_usedMiB,zenfs_usedZ,rdb_L0,rdb_L1,rdb_L2,aux_sst_n,aux_sst_MiB,rdb_stalls"
  while :; do
    S=$(dmsetup status "$NAME" 2>/dev/null)
    g(){ echo "$S"|grep -oE "$1=[0-9]+"|head -1|cut -d= -f2; }
    vp=$(g valid_pages); fp=$(g free_pages); re=$(g r_end)
    CR=$(( ${re:-0} / ZS ))
    iv=$(( ${re:-0}/8 - ${vp:-0} - ${fp:-0} )); [ ${iv:-0} -lt 0 ] && iv=0
    f2vb=$(awk '/partition info\(dm-0/{f=1} f&&/Utilization:/{if(match($0,/\(([0-9]+) valid/,m)){print m[1];exit}}' /sys/kernel/debug/f2fs/status 2>/dev/null)
    f2u=$(( ${f2vb:-0}/256 ))
    zu=$(grep -oE 'used_zones=[0-9]+' "$AUX/i1/.hyzns_status" 2>/dev/null|cut -d= -f2)
    # RocksDB LSM shape (i1 LOG, latest files[L0 L1 L2 ...]) + cumulative stalls
    LOG1="$AUX/i1/$DBDIR/LOG"
    if [ -f "$LOG1" ]; then
      fl=$(tr '\r' '\n' < "$LOG1" 2>/dev/null | grep -oE "files\[[0-9 ]+\]" | tail -1 | grep -oE "[0-9]+")
      l0=$(echo "$fl"|sed -n 1p); l1=$(echo "$fl"|sed -n 2p); l2=$(echo "$fl"|sed -n 3p)
      st=$(tr '\r' '\n' < "$LOG1" 2>/dev/null | grep -ciE "Stalling writes|Stopping writes")
    fi
    # actual aux-L0 SST files ON the fs (both instances). count = # dirents;
    # MiB = SUM of logical sizes via stat (fast, NO content read -> no workload
    # perturbation). aux(on-disk dirents) << f2fs_valid  =>  leaked inodes
    # (RocksDB unlinked the file but F2FS still holds its blocks).
    _k=$(( ${_k:-0} + 1 ))
    if [ $(( _k % MON_DU_EVERY )) -eq 0 ]; then
      set -- "$AUX/i1/$DBDIR/"*.sst "$AUX/i2/$DBDIR/"*.sst
      an=$(stat -c %s "$@" 2>/dev/null | wc -l)
      amb=$(stat -c %s "$@" 2>/dev/null | awk '{s+=$1} END{print int(s/1048576)}')
    fi
    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" "$(date +%s)" "$CR" \
      "$(zfree "$CR" "$((I1_END+1))")" "$(zfree "$I2_START" "$((I2_END+1))")" \
      "$(g free_lines)" "$(( ${vp:-0}*4096/1073741824 ))" "$(g nospc)" "$(g discards)" "$(g discard_pgs)" "$(g ovr)" "$(g recycles)" "$(g gc_mig)" \
      "$(( ${vp:-0}/256 ))" "$(( iv/256 ))" "$(( ${fp:-0}/256 ))" "${f2u:-0}" "${zu:-0}" \
      "${l0:-0}" "${l1:-0}" "${l2:-0}" "${an:-0}" "${amb:-0}" "${st:-0}"
    sleep "$MON_INT"
  done
) > "$MON" 2>&1 & MPID=$!

EXTRA=""; OPL1=""; OPL2=""; OD=""
if [[ $RWW == 1 ]]; then
  # SINGLE process: fillrandom -> readwhilewriting, no close/reopen (avoids the
  # l0cns aux-L0 reopen bug: footer tail-read returns short on a freshly
  # reopened ZenFS -> "Sst file size mismatch"/abort).
  # fillrandom[N NUM] pins the fill to NUM total ops via db_bench's per-benchmark
  # op-count (fillrandom is forced to 1 thread, so --writes=NUM/threads would
  # under-fill when RWW_THREADS>1; the [N] form is thread-count-independent).
  # RWW measured phase: RWW_DUR>0 -> time-bounded (--duration s); else read-
  # count-bounded (--reads/thread). --threads = reader count (+1 writer).
  BENCH="fillrandom[N${NUM}],readwhilewriting,stats"
  if [ "${RWW_DUR:-0}" -gt 0 ]; then
    EXTRA="--threads=$RWW_THREADS --duration=$RWW_DUR --histogram --statistics"
    _rwwdesc="${RWW_THREADS}rd x ${RWW_DUR}s"
  else
    EXTRA="--threads=$RWW_THREADS --reads=$RWW_READS --histogram --statistics"
    _rwwdesc="${RWW_THREADS}rd x $RWW_READS reads"
  fi
  OD="/dev/shm/hyexp/oplat_$$"; rm -rf "$OD"; mkdir -p "$OD/i1" "$OD/i2"
  OPL1="ROCKSDB_OPLAT_DIR=$OD/i1"; OPL2="ROCKSDB_OPLAT_DIR=$OD/i2"
  echo "=== RWW single-proc: fill(${NUM}) -> wait -> $_rwwdesc, oplat->$OD ==="
fi
echo "=== workload start (dual concurrent, bench=$BENCH) ==="
timeout -s TERM "$TMO" env $ARB1 $OPL1 ZENFS_L0_ON_AUX=$L0AUX "$BIN" \
  --fs_uri="zenfs://dev:dm-0/$I1_START/$I1_END/$AOZ/0/4294967296" \
  --benchmarks="$BENCH" $EXTRA --use_direct_io_for_flush_and_compaction \
  --num="$NUM" --key_size=$KEY --value_size=$VAL  \
  --max_background_jobs="$JOBS" --report_interval_seconds=1 &> "${pfx}_i1.log" & P1=$!
timeout -s TERM "$TMO" env $OPL2 ZENFS_L0_ON_AUX=$L0AUX "$BIN" \
  --fs_uri="zenfs://dev:dm-0/$I2_START/$I2_END/$AOZ/0/4294967296" \
  --benchmarks="$BENCH" $EXTRA --use_direct_io_for_flush_and_compaction \
  --num="$NUM" --key_size=$KEY --value_size=$VAL  \
  --max_background_jobs="$JOBS" --report_interval_seconds=1 &> "${pfx}_i2.log" & P2=$!
# stall watchdog: if BOTH instances make zero progress (===REPORT ops sum frozen)
# for WATCHDOG_S seconds, abort the whole run instead of burning TMO. A wedged
# run (grow BUSY loop, device full, dm park deadlock) is never worth waiting out.
WDOG=${WATCHDOG_S:-300}
if [[ $WDOG -gt 0 ]]; then
  ( prev=-1; idle=0
    while :; do sleep 15
      cur=$(cat "${pfx}_i1.log" "${pfx}_i2.log" 2>/dev/null | tr '\r' '\n' | awk '/===REPORT/{o+=$3} END{print o+0}')
      if [[ "$cur" == "$prev" ]]; then idle=$((idle+15)); else idle=0; prev=$cur; fi
      if [[ $idle -ge $WDOG ]]; then
        echo "[watchdog] no combined progress for ${WDOG}s — aborting run"
        echo "abort=watchdog: no combined progress for ${WDOG}s" >> "$pfx.runinfo"
        kill -TERM $$ 2>/dev/null; exit 0
      fi
    done ) & WPID=$!
fi
wait "$P1"; r1=$?; wait "$P2"; r2=$?
kill "$MPID" "$DPID" "$FTPID" "$WPID" 2>/dev/null
if [[ $RWW == 1 && -n $OD ]]; then
  tar czf "${pfx}_oplat.tar.gz" --exclude='*_fillrandom_*' -C "$OD" . 2>/dev/null \
    && echo "oplat -> ${pfx}_oplat.tar.gz ($(find "$OD" -name '*.csv'|wc -l) files)"
  rm -rf "$OD"
  echo "i1: RWW=$(ext readwhilewriting "${pfx}_i1.log") W=$(ext rww-writes "${pfx}_i1.log")"
  echo "i2: RWW=$(ext readwhilewriting "${pfx}_i2.log") W=$(ext rww-writes "${pfx}_i2.log")"
fi
echo "=== done | R:$R_INIT->$(cur_R) (ceiling $R_MAX) nospc=$(dm_f nospc) ==="
echo "i1: FR=$(ext fillrandom "${pfx}_i1.log") OW=$(ext overwrite "${pfx}_i1.log") rc=$r1"
echo "i2: FR=$(ext fillrandom "${pfx}_i2.log") OW=$(ext overwrite "${pfx}_i2.log") rc=$r2"
echo "death: i1=$(tr '\r' '\n' <"${pfx}_i1.log"|grep -ciE 'allocation failure|No space|l0_aux|Input/output|put error') i2=$(tr '\r' '\n' <"${pfx}_i2.log"|grep -ciE 'allocation failure|No space|l0_aux|Input/output|put error')"
echo "MON tail:"; tail -4 "$MON"
echo "files: ${pfx}_{i1,i2}.log $MON $pfx.daemon.log"
