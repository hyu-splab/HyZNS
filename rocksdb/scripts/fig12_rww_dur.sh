#!/bin/bash
# fig12_rww_dur.sh — WAL placement (WAL_ZNS vs WAL_CNS) under a TIME-BOUNDED
#   readwhilewriting: fillrandom NUM preload -> readwhilewriting for RWW_DUR
#   seconds with RWW_THREADS readers (+1 writer), DUAL instance, background-jobs
#   sweep. The WAL twin of hyznsd/scripts/fig13_rww_dur.sh (which does L0
#   placement). Thin wrapper over fig12_run.sh in RWW_DUR mode — kept SEPARATE
#   from fig12_run.sh (the count-based paper fig11(b)(c)).
#
#   Defaults: NUM=60M FR prefill, 16 readers, 150 s RWW, jobs 2/4/8.
#   Usage:  sudo ./scripts/fig12_rww_dur.sh
#           sudo JOBS="4" ./scripts/fig12_rww_dur.sh          # one bg-jobs cell
#           sudo NUM=30000000 RWW_DUR=120 ./scripts/fig12_rww_dur.sh
#   Post:   python3 scripts/fig12_post.py results/fig12_rww_dur/<run>
set -uo pipefail
cd "$(dirname "$0")/.."                                   # rocksdb/
exec env \
  NUM="${NUM:-60000000}" \
  RWW_THREADS="${RWW_THREADS:-16}" \
  RWW_DUR="${RWW_DUR:-150}" \
  JOBS="${JOBS:-2 4 8}" \
  CFGS="${CFGS:-ZNS CNS}" \
  RESULTS="${RESULTS:-results/fig12_rww_dur}" \
  bash scripts/fig12_run.sh
