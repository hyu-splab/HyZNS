#!/bin/bash
# sweep_ztree.sh — ZTree (ZNS-only) thread sweep over the FEMU HYHOSTSSD device.
# ZTree has no CNS/F2FS/hyhostd: we create the dm-hyhost target with r_end=0 so
# the whole device is one sequential (S/ZNS) region and run ZTree on top.
#
#   sudo -E bash sweep_ztree.sh [keys] [threads...]
#   sudo -E bash sweep_ztree.sh                  # 10M keys, threads 4 8 16 32 64 128 256
#   sudo -E bash sweep_ztree.sh 10000000 64      # single point
#   BIN=... OUTDIR=... DEV=... override supported
set -u

KEYS=${1:-10000000}
if [ $# -ge 2 ]; then shift 1; THREADS="$*"; else THREADS="4 8 16 32 64 128 256"; fi

DEV=${DEV:-/dev/nvme0n1}
DM=${DM:-hyhost0}
MNT=${MNT:-/mnt/hyhost}
ZSEC=2097152                     # 1 GiB zone / 512B sectors
MAX_R=${MAX_R:-64}               # dm r cap; r_end=0 means it never grows
GC_MS=${GC_MS:-5000}
BIN=${BIN:-$(dirname "$0")/../build/ztree}

[ -x "$BIN" ] || { echo "missing ztree binary: $BIN (run 'make ztree' in Ztree/)"; exit 1; }

TS=$(date +%Y%m%d_%H%M%S)
OUTDIR=${OUTDIR:-results/ztree_$TS}
mkdir -p "$OUTDIR"
CSV=$OUTDIR/summary.csv
echo "ztree sweep: keys=$KEYS threads=[$THREADS] r_end=0(all-S) dm=/dev/mapper/$DM -> $OUTDIR"
echo "threads,tput_ops_s,elapsed_s" > "$CSV"

for NT in $THREADS; do
  echo "===== ztree NT=$NT keys=$KEYS ====="
  sudo pkill -f "$(basename "$BIN") [0-9]" 2>/dev/null
  sleep 1
  sudo umount "$MNT" 2>/dev/null
  sudo dmsetup remove "$DM" 2>/dev/null

  # dm-hyhost with r_end=0 -> every zone is S (ZNS). No F2FS, no hyhostd.
  # ztree's in-bench nvme reset does not reach the raw device through dm, so
  # reset the backing device here.
  sudo nvme zns reset-zone -a "$DEV"
  SIZE=$(sudo blockdev --getsz "$DEV")
  echo "0 $SIZE hyhost $DEV 0 $((MAX_R*ZSEC))" | sudo dmsetup create "$DM"
  sudo dmsetup message "$DM" 0 set_r_end 0

  LOG=$OUTDIR/t${NT}.log
  sudo env \
     CTREE_DYNAMIC_ZNS_GC=1 CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS="$GC_MS" \
     CTREE_TPUT_PATH="$OUTDIR/t${NT}_tput.csv" \
     "$BIN" "$KEYS" "$NT" "/dev/mapper/$DM" 2>&1 | tee "$LOG"
  rc=${PIPESTATUS[0]}
  [ "$rc" -ne 0 ] && echo "  !! run failed rc=$rc (see $LOG)"

  tput=$(awk '/Average throughput/{print $3}' "$LOG")
  el=$(awk '/^Elapsed time/{print $3}' "$LOG")
  echo "$NT,${tput:-NA},${el:-NA}" >> "$CSV"
  echo "  -> ${tput:-?} ops/s  elapsed=${el:-?}s"
done
echo "===== ZTREE SWEEP DONE -> $CSV ====="
cat "$CSV"
