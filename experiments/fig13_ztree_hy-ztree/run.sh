#!/bin/bash
# Paper Fig.13 — ZTree vs Hy-ZTree insert throughput on the FEMU HyZNS
# device, thread scaling 4/8/16/32/64/128/256:
#   ZTree     ZNS-only (whole device sequential, r_end=0)
#   Hy-ZTree  internal nodes on F2FS-over-dm-hyzns CNS, leaves spill to ZNS,
#             hyznsd grows the CNS on demand (coord=ctree 2-phase handshake)
# Runs one sweep per tree, then merges into a single plot-ready CSV.
#
#   sudo -E ./run.sh                 # 10M keys, threads 4..256
#   KEYS=20000000 THREADS="8 64 256" sudo -E ./run.sh
set -euo pipefail
source "$(dirname "$0")/../common.sh"
need_root; need_femu

ZDIR="$ROOT/Ztree"
need_bin "$ZDIR/build/ztree"
need_bin "$ZDIR/build/hy-ztree"
need_bin "$ROOT/hyznsd/hyznsd"

KEYS=${KEYS:-10000000}
THREADS=${THREADS:-"4 8 16 32 64 128 256"}

OUT="$ZDIR/results/fig13/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"
echo "Fig.13 ZTree vs Hy-ZTree: keys=$KEYS threads=[$THREADS] -> $OUT"

OUTDIR="$OUT/ztree"   bash "$ZDIR/scripts/sweep_ztree.sh"   "$KEYS" $THREADS
OUTDIR="$OUT/hyztree" bash "$ZDIR/scripts/sweep_hyztree.sh" "$KEYS" $THREADS

# merge on thread count -> fig13.csv (threads, ztree_ops_s, hyztree_ops_s)
python3 - "$OUT" <<'PY'
import csv, os, sys
out = sys.argv[1]
def load(p):
    d = {}
    if os.path.exists(p):
        with open(p) as f:
            for row in csv.DictReader(f):
                d[int(row["threads"])] = row["tput_ops_s"]
    return d
z = load(os.path.join(out, "ztree",   "summary.csv"))
h = load(os.path.join(out, "hyztree", "summary.csv"))
with open(os.path.join(out, "fig13.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["threads", "ztree_ops_s", "hyztree_ops_s"])
    for t in sorted(set(z) | set(h)):
        w.writerow([t, z.get(t, "NA"), h.get(t, "NA")])
print("merged -> " + os.path.join(out, "fig13.csv"))
PY

echo "done -> $OUT (fig13.csv = ZTree vs Hy-ZTree; per-tree summary.csv + logs alongside)"
