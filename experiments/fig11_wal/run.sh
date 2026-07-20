#!/bin/bash
# Paper Fig.11 — WAL placement (dual RocksDB instances): WAL on each tenant's
# own ZNS range vs both WALs on the shared CNS area, fillrandom+overwrite,
# background-jobs sweep j=2/4/8, 10M keys per instance.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/rocksdb"
need_bin ./db_bench_zns; need_bin ./db_bench_posix   # sudo ./scripts/build_wal_variants.sh

GEOM=8x4 NUM=10000000 JOBS="2 4 8" ./scripts/fig10_run.sh

RUN=$(latest_dir results/fig10)
python3 scripts/fig10_post.py "$RUN"
echo "done -> rocksdb/results/fig10/fig10_log.csv (bar data) and $RUN (evidence pack)"
