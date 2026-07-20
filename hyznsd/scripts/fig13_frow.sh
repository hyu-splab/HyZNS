#!/bin/bash
# fig13_frow.sh — paper Fig13 FALLBACK (L0 placement THROUGHPUT, no resize story):
#   L0_ZNS (VANILLA) vs L0_res_CNS (L0_CNS_daemon), DUAL instance, single client
#   thread/inst, workload = fillrandom,overwrite. Shows the write-throughput
#   gain of putting L0 on the (auto-growing) CNS area.
#
#   FR and OW use the SAME count (=NUM): db_bench runs both in ONE process
#   (--num shared; per-benchmark op counts unsupported), and one process is
#   REQUIRED — the l0cns fork cannot reopen an existing DB (aux-L0 fd/level not
#   restored -> footer short-read -> "Sst file size mismatch"), so the classic
#   fill-then-overwrite two-invocation split is impossible for L0_CNS.
#
#   Usage:  sudo -E bash scripts/fig13_frow.sh [NUM] [JOBS...]
#           sudo -E bash scripts/fig13_frow.sh 10000000 8
#   Env:    RCNS_INIT(4) RZNS_INIT(4) SPLIT_NUM/DEN + fig10_dual_grow knobs
set -uo pipefail
cd "$(dirname "$0")/.."            # hyznsd/
NUM=${1:-10000000}; shift 2>/dev/null || true
JOBS_LIST=("${@:-}"); [ -z "${JOBS_LIST[0]:-}" ] && JOBS_LIST=(8)

TS=$(date +%Y%m%d_%H%M%S)
OUT=results/fig13_frow/${TS}
mkdir -p "$OUT"
echo "fig13 FR/OW run $TS  NUM=$NUM/inst (FR=OW)  jobs=${JOBS_LIST[*]}" | tee "$OUT/runinfo.txt"
CSV=$OUT/fig13_frow.csv
echo "cfg,jobs,inst,fillrandom_ops,overwrite_ops" > "$CSV"

# CONFIGS: which placements to run (default both). e.g. CONFIGS="L0_CNS_daemon"
CONFIGS=${CONFIGS:-"VANILLA L0_CNS_daemon"}
for jobs in "${JOBS_LIST[@]}"; do
  for cfg in $CONFIGS; do
    name=$([ "$cfg" = VANILLA ] && echo L0_ZNS || echo L0_CNS)
    rinit=$([ "$cfg" = VANILLA ] && echo "${RZNS_INIT:-4}" || echo "${RCNS_INIT:-4}")
    echo; echo "#### [$name j$jobs  R_INIT=$rinit] $(date +%T) ####"
    # RWW unset -> fig10_dual_grow's default BENCH=fillrandom,overwrite (one process)
    R_INIT=$rinit bash scripts/fig10_dual_grow.sh "$cfg" "$NUM" "$jobs" \
      2>&1 | tee "$OUT/${name}_n${NUM}_j${jobs}.console"
    pfx=$(ls -t /dev/shm/hyexp/fig10_live/${cfg}_*_i1.log 2>/dev/null | head -1); pfx=${pfx%_i1.log}
    if [ -z "$pfx" ]; then echo "!! no artifacts for $cfg j$jobs"; continue; fi
    for i in 1 2; do cp "${pfx}_i${i}.log" "$OUT/${name}_n${NUM}_j${jobs}_i${i}.log" 2>/dev/null; done
    cp "${pfx}.daemon.log" "$OUT/${name}_n${NUM}_j${jobs}.daemon.log" 2>/dev/null
    cp "${pfx}.csv"        "$OUT/${name}_n${NUM}_j${jobs}.mon.csv"    2>/dev/null
    cp "${pfx}.runinfo"    "$OUT/${name}_n${NUM}_j${jobs}.runinfo"    2>/dev/null
    g(){ tr '\r' '\n' <"$OUT/${name}_n${NUM}_j${jobs}_i${2}.log" 2>/dev/null \
         | grep -E "^$1 +:" | tail -1 | grep -oE '[0-9]+ ops/sec' | grep -oE '[0-9]+'; }
    fr1=$(g fillrandom 1); fr2=$(g fillrandom 2); ow1=$(g overwrite 1); ow2=$(g overwrite 2)
    echo "$cfg,$jobs,i1,${fr1:-0},${ow1:-0}" >> "$CSV"
    echo "$cfg,$jobs,i2,${fr2:-0},${ow2:-0}" >> "$CSV"
    # convention: report as the MEAN of the two instances (not the sum)
    echo "$cfg,$jobs,avg,$(( (${fr1:-0}+${fr2:-0})/2 )),$(( (${ow1:-0}+${ow2:-0})/2 ))" >> "$CSV"
    echo ">> [$name j$jobs] FR avg=$(( (${fr1:-0}+${fr2:-0})/2 ))  OW avg=$(( (${ow1:-0}+${ow2:-0})/2 )) ops/s"
  done
done
echo; echo "collected -> $OUT   ($CSV)"
column -t -s, "$CSV" 2>/dev/null || cat "$CSV"
