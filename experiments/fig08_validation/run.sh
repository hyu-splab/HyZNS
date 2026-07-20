#!/bin/bash
# Paper Fig.8 — device validation, SINGLE-device setting (RocksDB aux path on
# the device's OWN fixed-size CNS area).
# Runs the FEMU hyzns mode. The paper normalizes against a real ZN540; without
# one you still get the hyzns column (sudo MODE=real ... adds the zn540 rows).
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/rocksdb"; need_bin ./db_bench_zns

GEOM=8x4 MODE=hyzns ./scripts/fig8_run.sh

python3 scripts/fig_plot.py --template --log results/fig8/fig8_log.csv results/fig8/fig8_values.csv
python3 scripts/fig_plot.py results/fig8/fig8_values.csv --base hyzns -o results/fig8/fig8.png
echo "done -> rocksdb/results/fig8/{fig8_log.csv,fig8_values.csv,fig8.png}"
