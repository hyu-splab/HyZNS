#!/bin/bash
# Paper Fig.13c — L0 placement under readwhilewriting: 10M-key fillrandom
# prefill, then 16 readers + 1 writer for 150 s, dual instances, j=8.
# Same three configurations as Fig.13a/b; the fixed-CNS run dies during the
# prefill (expected), giving the truncated curve.
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu
cd "$ROOT/hyhostd"; need_bin "$ROOT/rocksdb/db_bench_zns"; need_bin "$ROOT/rocksdb/db_bench_l0cns"; need_bin ./hyhostd

DIRS=()
for cfg in VANILLA L0_CNS_daemon L0_CNS_static; do
  env CONFIGS="$cfg" ROCKSDB_NO_INTRA_L0=1 SPLIT_NUM=1 SPLIT_DEN=2 \
    bash scripts/fig13_rww_dur.sh 10000000 8
  DIRS+=("$(latest_dir results/fig13_rww_dur)")
done

OUT=results/fig13c/$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUT"
# read-side analysis (throughput, exact per-op read CDF) needs the ZNS and CNS
# logs side by side in one directory
cp "${DIRS[0]}"/L0_ZNS_* "${DIRS[1]}"/L0_CNS_* "$OUT"/ 2>/dev/null || true
python3 "$ROOT/rocksdb/scripts/fig12_post.py" "$OUT"
# write-side running-average + R timeline per phase (FR vs RWW)
python3 scripts/make_ravg_csv.py "$OUT/ravg" "${DIRS[@]}"
echo "done -> hyhostd/$OUT (analysis/ = read CDF+throughput, ravg/ = Fig.13c curves)"
