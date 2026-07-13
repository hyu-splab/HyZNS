#!/bin/bash
# Paper Fig.7 — device validation, MULTI-device setting (RocksDB aux path on a
# separate conventional device; ZNS area only under test).
# Runs the two FEMU modes (zns, hyzns) and renders the comparison plot.
# The real-SSD column (zn540) requires a 2-namespace drive; if you have one:
#   sudo MODE=real ./rocksdb/scripts/fig7_run.sh
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/rocksdb"; need_bin ./db_bench_zns

GEOM=8x4 MODE=zns   ./scripts/fig7_run.sh
GEOM=8x4 MODE=hyzns ./scripts/fig7_run.sh

python3 scripts/fig_plot.py --template --log results/fig7/fig7_log.csv results/fig7/fig7_values.csv
python3 scripts/fig_plot.py results/fig7/fig7_values.csv --base zns -o results/fig7/fig7.png
echo "done -> rocksdb/results/fig7/{fig7_log.csv,fig7_values.csv,fig7.png}"
