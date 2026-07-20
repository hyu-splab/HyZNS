#!/bin/bash
# WAL placement under readwhilewriting (dual instances) — extra experiment,
# not referenced by the current paper:
# 60M-key fillrandom prefill (deep tree), then readwhilewriting with 64 readers
# + 1 writer per instance (10M reads total/inst), background-jobs sweep j=2/4/8,
# WAL on own ZNS range vs both WALs on the shared CNS area.
# Panels: (a) read/write throughput per jobs, (c) per-op read-latency CDF.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/rocksdb"
need_bin ./db_bench_zns; need_bin ./db_bench_posix   # sudo ./scripts/build_wal_variants.sh

GEOM=8x4 NUM=60000000 ./scripts/fig12_run.sh

RUN=$(latest_dir results/fig12)
python3 scripts/fig12_post.py "$RUN"
echo "done -> $RUN/analysis (throughput bars, read CDF, REPORT.md)"
