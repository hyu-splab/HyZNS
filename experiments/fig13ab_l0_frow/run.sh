#!/bin/bash
# Paper Fig.13a (fillrandom) and Fig.13b (overwrite) — L0 placement with the
# growing CNS area, dual instances, j=8, 10M-key FR then 10M-op OW per instance:
#   L0_ZNS       (VANILLA)        L0 stays on ZNS
#   L0_fixed_CNS (L0_CNS_static)  L0 on a fixed 4-zone CNS -> dies out of space
#   L0_res_CNS   (L0_CNS_daemon)  L0 on CNS, hyhostd FTR grows it on demand
# Produces the running-average throughput views with R-grow / death markers.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/hyhostd"; need_bin "$ROOT/rocksdb/db_bench_zns"; need_bin "$ROOT/rocksdb/db_bench_l0cns"; need_bin ./hyhostd

DIRS=()
for cfg in VANILLA L0_CNS_static L0_CNS_daemon; do
  env CONFIGS="$cfg" ROCKSDB_NO_INTRA_L0=1 OW_NUM=10000000 SPLIT_NUM=1 SPLIT_DEN=2 \
    bash scripts/fig13_frow.sh 10000000 8
  DIRS+=("$(latest_dir results/fig13_frow)")
done
# note: the static run dying mid-fillrandom (out of space) is the expected result

OUT=results/fig13ab/$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
python3 scripts/make_ravg_csv.py "$OUT" "${DIRS[@]}"
python3 scripts/make_fig144_plot.py "$OUT"
echo "done -> hyhostd/$OUT (fig144_fr = Fig.13a, fig144_ow = Fig.13b, + plot CSVs)"
