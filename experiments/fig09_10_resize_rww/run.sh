#!/bin/bash
# Paper Fig.9 (resize effect on a live readwhilewriting run, throughput zoom)
# and Fig.10 (read-latency CDF around each resize) — one command per panel:
#
#   sudo -E ./run.sh grow       # Fig.9a / Fig.10a : GrowCNS   x2 @t=73s,98s (R0=4)
#   sudo -E ./run.sh shrink     # Fig.9b / Fig.10b : ShrinkCNS x2 @t=30s,72s (R0=8)
#   sudo -E ./run.sh baseline   # no-resize reference run
#
# Each run: 10M-key fillrandom prefill, then 16 readers + 1 writer for 180 s.
# Post-processing (zoom windows, per-op CDF, matched bundle) runs automatically.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/hyznsd"; need_bin "$ROOT/rocksdb/db_bench_zns"; need_bin ./hyznsd

case "${1:-}" in
  grow)     env R0=4 GROW_AT=73,98   bash scripts/resize_ops/fig9_rww.sh ;;
  shrink)   env R0=8 SHRINK_AT=30,72 bash scripts/resize_ops/fig9_rww.sh ;;
  baseline) env R0=4                 bash scripts/resize_ops/fig9_rww.sh ;;
  *) die "usage: $0 grow|shrink|baseline" ;;
esac

RUN=$(latest_dir results/resize_ops)
bash scripts/resize_ops/fig9_post.sh "$RUN"
echo "done -> $RUN (zoom CSV/PNG, *_fig10_cdf_points.csv, *_matched.zip)"
