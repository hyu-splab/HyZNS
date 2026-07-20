#!/bin/bash
# fig13_rww_dur.sh — L0 placement under a TIME-BOUNDED readwhilewriting:
#   L0_ZNS (VANILLA) vs L0_res_CNS (L0_CNS_daemon), DUAL instance,
#   background-jobs sweep. Per cell: fillrandom NUM (deep-ish tree) ->
#   readwhilewriting for RWW_DUR seconds with RWW_THREADS readers (+1 writer).
#   Per-op read latencies traced (oplat) for the CDF.
#
#   Differs from fig13_rww.sh: RWW is DURATION-bounded (--duration), not read-
#   count-bounded, and RWW_THREADS defaults to 16.
#
#   Usage:  sudo -E bash scripts/fig13_rww_dur.sh [NUM] [JOBS...]
#           sudo -E bash scripts/fig13_rww_dur.sh 10000000 2 4 8
#   Env:    RWW_THREADS(16) RWW_DUR(150) RCNS_INIT(4) RZNS_INIT(4)
#           SPLIT_NUM/DEN, ROCKSDB_NO_INTRA_L0=1 (keep L0_CNS lean), + fig10_dual_grow knobs
#   Post:   python3 ../rocksdb/scripts/fig12_post.py results/fig13_rww_dur/<run>
set -uo pipefail
cd "$(dirname "$0")/.."            # hyznsd/
NUM=${1:-10000000}; shift 2>/dev/null || true
JOBS_LIST=("${@:-}"); [ -z "${JOBS_LIST[0]:-}" ] && JOBS_LIST=(2 4 8)
export RWW_THREADS=${RWW_THREADS:-16}
export RWW_DUR=${RWW_DUR:-150}
CONFIGS=${CONFIGS:-"VANILLA L0_CNS_daemon"}

TS=$(date +%Y%m%d_%H%M%S)
OUT=results/fig13_rww_dur/${TS}
mkdir -p "$OUT"
echo "fig13 RWW(dur) run $TS  NUM=$NUM/inst  jobs=${JOBS_LIST[*]}  ${RWW_THREADS}rd x ${RWW_DUR}s" \
  | tee "$OUT/runinfo.txt"

for jobs in "${JOBS_LIST[@]}"; do
  for cfg in $CONFIGS; do
    name=$([ "$cfg" = VANILLA ] && echo L0_ZNS || echo L0_CNS)
    rinit=$([ "$cfg" = VANILLA ] && echo "${RZNS_INIT:-4}" || echo "${RCNS_INIT:-4}")
    echo; echo "#### [$name j$jobs  ${RWW_THREADS}rd/${RWW_DUR}s  R_INIT=$rinit] $(date +%T) ####"
    RWW=1 R_INIT=$rinit bash scripts/fig10_dual_grow.sh "$cfg" "$NUM" "$jobs" \
      2>&1 | tee "$OUT/${name}_n${NUM}_j${jobs}.console"
    pfx=$(ls -t /dev/shm/hyexp/fig10_live/${cfg}_*_i1.log 2>/dev/null | head -1); pfx=${pfx%_i1.log}
    if [ -z "$pfx" ]; then echo "!! no artifacts for $cfg j$jobs"; continue; fi
    for i in 1 2; do cp "${pfx}_i${i}.log" "$OUT/${name}_n${NUM}_j${jobs}_i${i}.log" 2>/dev/null; done
    cp "${pfx}_oplat.tar.gz" "$OUT/${name}_n${NUM}_j${jobs}_oplat.tar.gz" 2>/dev/null
    cp "${pfx}.daemon.log"   "$OUT/${name}_n${NUM}_j${jobs}.daemon.log"   2>/dev/null
    cp "${pfx}.csv"          "$OUT/${name}_n${NUM}_j${jobs}.mon.csv"      2>/dev/null
    cp "${pfx}.runinfo"      "$OUT/${name}_n${NUM}_j${jobs}.runinfo"      2>/dev/null
    rd1=$(tr '\r' '\n' <"$OUT/${name}_n${NUM}_j${jobs}_i1.log"|grep -E "^readwhilewriting +:"|grep -oE '[0-9]+ ops/sec'|grep -oE '[0-9]+')
    rd2=$(tr '\r' '\n' <"$OUT/${name}_n${NUM}_j${jobs}_i2.log"|grep -E "^readwhilewriting +:"|grep -oE '[0-9]+ ops/sec'|grep -oE '[0-9]+')
    echo ">> [$name j$jobs] RWW read avg=$(( (${rd1:-0}+${rd2:-0})/2 )) ops/s (i1=${rd1:-0} i2=${rd2:-0})"
  done
done
echo; echo "collected -> $OUT"
echo "post: python3 ../rocksdb/scripts/fig12_post.py $OUT"
