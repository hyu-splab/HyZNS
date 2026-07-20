#!/bin/bash
# fig9_cell.sh — one Fig9 cell: WAL or L0 placement in HyZNS single (CNS=R-region).
# Same dm-hyzns stack everywhere; only the data placement differs.
#
# env (required): TYPE=wal|l0  CFG=<...>  JOBS=<2|4|8>  NUM=<n>  REP=<i>
#   TYPE=wal  CFG: zns (WAL->ZNS S) | posix (WAL->CNS R, aux F2FS O_SYNC)
#   TYPE=l0   CFG: l0zns (L0->ZNS) | l0cns_nomod (L0->CNS R=4 static, expected to die)
#                | l0cns_mod (L0->CNS + in-process arbiter grows R online)
# env (opt): WL=FR|OW|FS (wal only; l0 forced fillrandom,overwrite), R_ZONES=4, AOZ=7, TMO=
set -uo pipefail
cd "$(dirname "$0")/../.."                              # rocksdb/
DMHYZNS=../dm-hyzns; source "$DMHYZNS/scripts/_lib.sh"
: "${TYPE:?} ${CFG:?} ${JOBS:?} ${NUM:?} ${REP:?}"
WL=${WL:-FR}; R_ZONES=${R_ZONES:-4}; AOZ=${AOZ:-7}; KEY=20; VAL=800
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZENFS=./plugin/zenfs/util/zenfs; ZS=$HYZNS_ZONE_SECTORS
OUT=results/fig9; mkdir -p "$OUT"
SZ=$(blockdev --getsz "$BACKING"); TOTAL=$((SZ/ZS)); S_LAST=$((TOTAL-1))

# workload (wal); l0 overrides below
case "$WL" in
  FR) BENCH=fillrandom;            EKEY=fillrandom;;
  OW) BENCH=fillrandom,overwrite;  EKEY=overwrite;;
  FS) BENCH=fillseq;               EKEY=fillseq;;
  *) echo "bad WL=$WL"; exit 2;;
esac

BIN=./db_bench_zns; ARB=""; MAXPROV=0; EXTRA=""
case "$TYPE:$CFG" in
  wal:zns)        BIN=./db_bench_zns;   LABEL=WAL_ZNS;;
  wal:posix)      BIN=./db_bench_posix; LABEL=WAL_CNS;;
  l0:l0zns)       BIN=./db_bench_zns;   LABEL=L0_ZNS;       EXTRA="--disable_wal=1"; BENCH=fillrandom,overwrite; EKEY=overwrite;;
  l0:l0cns_nomod) BIN=./db_bench_l0cns; LABEL=L0_CNS_nomod; EXTRA="--disable_wal=1"; BENCH=fillrandom,overwrite; EKEY=overwrite;;
  l0:l0cns_mod)   BIN=./db_bench_l0cns; LABEL=L0_CNS_mod;   EXTRA="--disable_wal=1"; MAXPROV=1; BENCH=fillrandom,overwrite; EKEY=overwrite;
                  # arbiter tuned to grow EARLY under low pressure: a late grow
                  # hits nospc or races in-flight aux writes (intermittent EIO).
                  ARB="ZENFS_ZONE_ARBITER=1 ZENFS_ARBITER_INTERVAL_MS=${ARB_INT:-1000} ZENFS_ARBITER_THRESHOLD=${ARB_THR:-50} ZENFS_ARBITER_MAXZONES=${ARB_MAX:-8} ZENFS_ARBITER_COOLDOWN_MS=${ARB_CD:-2000}";;
  *) echo "bad TYPE:CFG=$TYPE:$CFG"; exit 2;;
esac
[[ -x $BIN ]] || { echo "missing $BIN"; exit 1; }
TS=$(date +%Y%m%d_%H%M%S); TAG="${TS}_fig9${TYPE}_${LABEL}_${WL}_j${JOBS}_n${NUM}_rep${REP}"; PFX="$OUT/$TAG"
FS_URI="zenfs://dev:dm-0/${R_ZONES}/${S_LAST}/${AOZ}/0/4294967296"
echo "=== $TAG : TYPE=$TYPE CFG=$CFG bin=$BIN bench=$BENCH jobs=$JOBS num=$NUM maxprov=$MAXPROV arb=${ARB:-0} ==="

# ---- setup dm-hyzns R-region + aux F2FS + ZenFS ----
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
lsmod|awk '{print $1}'|grep -qx dm_hyzns || "$DMHYZNS/scripts/load.sh" >/dev/null 2>&1
reset_device_full_r "$BACKING" >/dev/null 2>&1 || true
if [[ $MAXPROV == 1 ]]; then
  echo "0 $SZ hyzns $BACKING $((R_ZONES*ZS)) $SZ" | dmsetup create "$DMNAME"   # 5th arg = max_r_end (whole dev) for online grow
  dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS))
  mkfs.f2fs -f -m -H -A -P "$DEV" >"$PFX.mkfs.log" 2>&1
else
  echo "0 $SZ hyzns $BACKING $((R_ZONES*ZS))" | dmsetup create "$DMNAME"
  dmsetup message "$DMNAME" 0 set_r_end $((R_ZONES*ZS))
  mkfs.f2fs -f -m -H -A "$DEV" >"$PFX.mkfs.log" 2>&1
fi
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R_ZONES" \
  --start_zone="$R_ZONES" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >"$PFX.zenfs_mkfs.log" 2>&1
sync

# ---- samplers (R-FTL time series + device write series) ----
# 1s cadence to align GC (gc_runs↑) / grow (r_end↑) events with the per-second
# db_bench throughput series for the Fig9(b) time-series overlay.
( while :; do printf '%s ' "$(date +%s.%N)"; dmsetup status "$DMNAME" 2>/dev/null; echo; sleep 1; done ) > "$PFX.dmstatus.log" 2>&1 & DSPID=$!
iostat -x -t 1 /dev/nvme0n1 > "$PFX.iostat.log" 2>&1 & IOPID=$!
stop(){ kill $DSPID $IOPID 2>/dev/null; wait $DSPID $IOPID 2>/dev/null; }
trap stop EXIT
dmsetup status "$DMNAME" > "$PFX.dmstatus_pre.txt" 2>/dev/null || true
awk '{print $7}' /sys/block/$(basename "$BACKING")/stat > "$PFX.wsec_pre.txt" 2>/dev/null || echo 0 > "$PFX.wsec_pre.txt"

# ---- db_bench (time series via stats_interval_seconds=1). SIGTERM on timeout (never SIGKILL -> FEMU safe). ----
T0=$(date +%s.%N)
timeout -s TERM "${TMO:-2400}" env $ARB ZENFS_L0_ON_AUX=$([[ $TYPE == l0 && $CFG != l0zns ]] && echo 1 || echo 0) \
  "$BIN" --fs_uri="$FS_URI" --benchmarks="${BENCH},stats" $EXTRA \
  --use_direct_io_for_flush_and_compaction \
  --num="$NUM" --key_size="$KEY" --value_size="$VAL" --compression_type=none \
  --max_background_jobs="$JOBS" --histogram --statistics --stats_interval_seconds=1 > "$PFX.i1.log" 2>&1
R1=$?
T1=$(date +%s.%N)
SS=$(date +%s.%N); sync; SE=$(date +%s.%N)
dmsetup status "$DMNAME" > "$PFX.dmstatus_post.txt" 2>/dev/null || true
awk '{print $7}' /sys/block/$(basename "$BACKING")/stat > "$PFX.wsec_post.txt" 2>/dev/null || echo 0 > "$PFX.wsec_post.txt"

# ---- extract ----
ops(){ grep -E "^${EKEY} " "$PFX.i1.log" 2>/dev/null|tail -1|grep -oE '[0-9]+ ops/sec'|grep -oE '[0-9]+'; }
p99(){ awk -v k="$EKEY" 'index($0,k" ")==1{f=1} f&&/Percentiles:/{a="?";b="?";for(i=1;i<=NF;i++){if($i=="P99:")a=$(i+1);if($i=="P99.99:")b=$(i+1)} print a"/"b; exit}' "$PFX.i1.log" 2>/dev/null; }
rb(){ grep "rocksdb.$1 COUNT" "$PFX.i1.log" 2>/dev/null|grep -oE '[0-9]+$'|tail -1; }
dst(){ grep -oE "(^| )$2=[0-9]+" "$PFX.dmstatus_$1.txt" 2>/dev/null|head -1|grep -oE '[0-9]+'; }
dd(){ echo $(( ${2:-0}-${1:-0} )); }
o1=$(ops); l1=$(p99)
bw=$(rb bytes.written); flw=$(rb flush.write.bytes); cmw=$(rb compact.write.bytes); walb=$(rb wal.bytes)
wamp=$(grep -E '^ Sum ' "$PFX.i1.log" 2>/dev/null|tail -1|awk '{print $12}')
cum=$(grep -i 'Cumulative compaction:' "$PFX.i1.log" 2>/dev/null|tail -1|grep -oE '[0-9.]+ GB write'|grep -oE '[0-9.]+')
gcm_d=$(dd "$(dst pre gc_mig)" "$(dst post gc_mig)"); gcr_d=$(dd "$(dst pre gc_runs)" "$(dst post gc_runs)")
er_d=$(dd "$(dst pre erases)" "$(dst post erases)"); nsp=$(dst post nospc)
r_end_pre=$(dst pre r_end); r_end_post=$(dst post r_end)
rzpre=$(( ${r_end_pre:-0}/ZS )); rzpost=$(( ${r_end_post:-0}/ZS ))   # R size in zones (grow proof for mod)
grows=$(grep -cE 'Expanding R-zone|Expansion complete|grow .* -> R=|ResizeCNS|GrowCNS' "$PFX.i1.log" 2>/dev/null)
err=$(grep -oE 'No space|put error|write failed|Corruption|abnormal|IO error|Operation not permitted' "$PFX.i1.log" 2>/dev/null|sort|uniq -c|tr '\n' ' ')
wsp=$(cat "$PFX.wsec_pre.txt" 2>/dev/null); wsq=$(cat "$PFX.wsec_post.txt" 2>/dev/null)
dev_bytes=$(( (${wsq:-0}-${wsp:-0})*512 ))
dev_waf=$(awk "BEGIN{if(${bw:-0}>0)printf \"%.3f\",$dev_bytes/${bw:-1};else print \"?\"}")
done_ok=$([[ -n "$o1" ]] && echo yes || echo NO)
{
  echo "TAG=$TAG type=$TYPE cfg=$CFG label=$LABEL wl=$WL jobs=$JOBS num=$NUM rep=$REP rc=$R1 completed=$done_ok"
  echo "ops=${o1:-NA}  ekey=$EKEY  lat_p99/p99.99_us=${l1:-?}"
  echo "wall_s=$(awk "BEGIN{printf \"%.1f\",$T1-$T0}")  (rc124=timeout/livelock, rc!=0&no ops=EIO/space die)"
  echo "user_bytes.written=${bw:-?} flush.write.bytes=${flw:-?} compact.write.bytes=${cmw:-?} wal.bytes=${walb:-?}"
  echo "Wamp=${wamp:-?} cum_comp_GBwrite=${cum:-?} device_write_B=${dev_bytes} device_WAF=$dev_waf"
  echo "R_zones pre=$rzpre post=$rzpost (grow=$((rzpost-rzpre)))  grow_events=$grows  gc_runs+=$gcr_d gc_mig+=$gcm_d erases+=$er_d nospc=${nsp:-?}"
  echo "errors: ${err:-none}"
  echo "# sources: i1.log(ts+ops+copy/WAF) dmstatus.log(R-FTL series) dmstatus_{pre,post}.txt wsec_{pre,post}.txt iostat.log"
} > "$PFX.summary.txt" 2>&1
stop; trap - EXIT
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "=== done $TAG rc=$R1 completed=$done_ok ==="; cat "$PFX.summary.txt"
