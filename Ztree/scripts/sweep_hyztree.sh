#!/bin/bash
# sweep_hyztree.sh — Hy-ZTree thread sweep over the FEMU HYHOSTSSD device.
# Hy-ZTree keeps internal nodes on a CNS area (F2FS over the dm-hyhost R-region)
# and spills leaves to the ZNS S-region; hyhostd grows the CNS on demand via the
# ctree 2-phase handshake (coord=ctree). Per thread count we (re)build the stack:
#   dm-hyhost(R=INIT_R zones, cap MAX_R) -> mkfs.f2fs -H -A -P -> mount ->
#   hyhostd(coord=ctree) -> run hy-ztree against /dev/mapper/hyhost0.
#
#   sudo -E bash sweep_hyztree.sh [keys] [threads...]
#   sudo -E bash sweep_hyztree.sh                # 10M keys, threads 4 8 16 32 64 128 256
#   sudo -E bash sweep_hyztree.sh 10000000 64    # single point
#   BIN=... HYD=... F2FS_IO=... OUTDIR=... DEV=... override supported
#
# hyhostd uses its built-in FTR policy defaults (grow/shrink thresholds); we pass
# only coord=ctree plus the paths and the R clamp. Do NOT pass ftr/grow_guard/step.
set -u

KEYS=${1:-10000000}
if [ $# -ge 2 ]; then shift 1; THREADS="$*"; else THREADS="4 8 16 32 64 128 256"; fi

HERE=$(cd "$(dirname "$0")" && pwd)
DEV=${DEV:-/dev/nvme0n1}
DM=${DM:-hyhost0}
MNT=${MNT:-/mnt/hyhost}
ZSEC=2097152                     # 1 GiB zone / 512B sectors
INIT_R=${INIT_R:-4}
MAX_R=${MAX_R:-64}
GC_MS=${GC_MS:-5000}
BIN=${BIN:-$HERE/../build/hy-ztree}
HYD=${HYD:-$HERE/../../hyhostd/hyhostd}
F2FS_IO=${F2FS_IO:-$(ls "$HERE"/../../f2fs-tools-*/tools/f2fs_io/f2fs_io 2>/dev/null | head -1)}

[ -x "$BIN" ]  || { echo "missing hy-ztree binary: $BIN (run 'make hy-ztree' in Ztree/)"; exit 1; }
[ -x "$HYD" ]  || { echo "missing hyhostd: $HYD (cd hyhostd && make)"; exit 1; }
[ -n "${F2FS_IO:-}" ] && [ -x "$F2FS_IO" ] || { echo "missing f2fs_io (resize_cns build)"; exit 1; }

TS=$(date +%Y%m%d_%H%M%S)
OUTDIR=${OUTDIR:-results/hyztree_$TS}
mkdir -p "$OUTDIR"
CSV=$OUTDIR/summary.csv
echo "hy-ztree sweep: keys=$KEYS threads=[$THREADS] R=[$INIT_R,$MAX_R] coord=ctree dm=/dev/mapper/$DM -> $OUTDIR"
echo "threads,tput_ops_s,elapsed_s,r_final_zones,grows" > "$CSV"

for NT in $THREADS; do
  echo "===== hy-ztree NT=$NT keys=$KEYS ====="
  sudo pkill -f '[h]yhostd' 2>/dev/null
  sudo pkill -f "$(basename "$BIN") [0-9]" 2>/dev/null
  sleep 1
  sudo umount "$MNT" 2>/dev/null
  sudo dmsetup remove "$DM" 2>/dev/null

  LOG=$OUTDIR/t${NT}.log
  : > "$LOG"
  sudo nvme zns reset-zone -a "$DEV" >>"$LOG" 2>&1
  SIZE=$(sudo blockdev --getsz "$DEV")
  echo "0 $SIZE hyhost $DEV $((INIT_R*ZSEC)) $((MAX_R*ZSEC))" | sudo dmsetup create "$DM"
  sudo dmsetup message "$DM" 0 set_r_end $((INIT_R*ZSEC))
  sudo mkfs.f2fs -f -m -H -A -P "/dev/mapper/$DM" >>"$LOG" 2>&1
  sudo mkdir -p "$MNT"
  sudo mount -t f2fs -o discard,mode=adaptive "/dev/mapper/$DM" "$MNT"
  sudo rm -f "$MNT/.hyzns_prepare" "$MNT/.hyzns_prepare.ack" "$MNT/.hyzns_commit"

  # hyhostd: default FTR policy; only coord=ctree + paths + R clamp are passed.
  sudo "$HYD" -v --set fs=f2fs --set coord=ctree --set dm="$DM" --set backing="$DEV" \
     --set mnt="$MNT" --set aux="$MNT" --set f2fs_io="$F2FS_IO" --set rz_source=report \
     --set r_min="$INIT_R" --set r_max="$MAX_R" \
     > "$OUTDIR/t${NT}_hyhostd.log" 2>&1 &
  sleep 1
  echo "  setup OK: dm(R=$INIT_R,cap$MAX_R) + F2FS($MNT) + hyhostd(coord=ctree)"

  sudo env CNS_ODIRECT=1 CTREE_CNS_DIR="$MNT" CTREE_CNS_DM_NAME="$DM" \
     CTREE_DAEMON_GROW=1 CTREE_CNS_MAX_RZONES="$MAX_R" \
     CTREE_DYNAMIC_ZNS_GC=1 CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS="$GC_MS" \
     CTREE_TPUT_PATH="$OUTDIR/t${NT}_tput.csv" \
     "$BIN" "$KEYS" "$NT" "/dev/mapper/$DM" 2>>"$LOG" | tee -a "$LOG"
  rc=${PIPESTATUS[0]}
  [ "$rc" -ne 0 ] && echo "  !! run failed rc=$rc (see $LOG)"

  sudo pkill -f '[h]yhostd' 2>/dev/null

  tput=$(awk '/Average throughput/{print $3}' "$LOG")
  el=$(awk '/^Elapsed time/{print $3}' "$LOG")
  DMST=$(sudo dmsetup status "$DM" 2>/dev/null)
  echo "$DMST" > "$OUTDIR/t${NT}_dmstatus.txt"
  rend=$(echo "$DMST" | grep -oE 'r_end=[0-9]+' | cut -d= -f2)
  grows=$(grep -c "grow COMMIT" "$LOG")
  echo "$NT,${tput:-NA},${el:-NA},$(( ${rend:-0} / ZSEC )),${grows}" >> "$CSV"
  echo "  -> ${tput:-?} ops/s  R_final=$(( ${rend:-0} / ZSEC ))z  grows=$grows"
done
echo "===== HY-ZTREE SWEEP DONE -> $CSV ====="
cat "$CSV"
