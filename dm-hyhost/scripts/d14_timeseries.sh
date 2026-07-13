#!/bin/bash
#
# D14 time-series - bandwidth + GC activity vs time at a single r_end.
#
# fio writes a per-second bandwidth log (--log_avg_msec=1000); a
# parallel poller snapshots dmsetup status every second for matching
# gc_mig / user-write deltas. The two streams use the same wall clock
# so the plotter can overlay them on a single time axis.
#
# Usage:
#   ./d14_timeseries.sh <r_end_gib> [workload_gib] [outdir]
#
# Defaults: workload=8 GiB (≥ 4× overwrite at r_end=2), outdir=results/
#
set -euo pipefail
cd "$(dirname "$0")/.."

R_END_GIB=${1:?usage: $0 <r_end_gib> [workload_gib] [outdir]}
WORKLOAD_GIB=${2:-8}
OUTDIR=${3:-/data/HYSSD/dm-hyhost/results}
NAME=hyhost0
BACKING=/dev/nvme0n1

mkdir -p "$OUTDIR"
TS=$(date +%Y%m%d_%H%M%S)
TAG="${TS}_r${R_END_GIB}g_w${WORKLOAD_GIB}g"
BW_LOG="$OUTDIR/${TAG}_bw"
DM_LOG="$OUTDIR/${TAG}_dm.log"
META="$OUTDIR/${TAG}.meta"

ZONE_SECTORS=$((1 * 1024 * 1024 * 1024 / 512))
r_end=$((R_END_GIB * ZONE_SECTORS))
workload_bytes=$((WORKLOAD_GIB * 1024 * 1024 * 1024))

if ! lsmod | awk '{print $1}' | grep -qx dm_hyhost; then
    ./scripts/load.sh >/dev/null
fi

dmsetup remove "$NAME" 2>/dev/null || true

# Reset device r_end (whole-device R) so setup can shrink to the target.
nvme io-passthru "$BACKING" --opcode=0x79 --namespace-id=1 \
    --cdw10=268435456 --cdw11=0 --cdw13=0x20 -r >/dev/null 2>&1 || true

./scripts/setup.sh "$BACKING" "$r_end" "$NAME" >/dev/null

# Snapshot config + initial status into meta.
{
    echo "tag=$TAG"
    echo "r_end_gib=$R_END_GIB"
    echo "workload_gib=$WORKLOAD_GIB"
    echo "started=$(date -Iseconds)"
    echo "status_initial=$(dmsetup status "$NAME" | sed 's/^.*hyhost //')"
} > "$META"

# Background poller: 1 Hz dmsetup status snapshot. Each line is
# "elapsed_sec <full status string>"; matches fio's per-second bw log.
(
    start=$(date +%s)
    while true; do
        now=$(date +%s)
        elapsed=$((now - start))
        line=$(dmsetup status "$NAME" 2>/dev/null | sed 's/^.*hyhost //') || break
        echo "$elapsed $line"
        sleep 1
    done
) > "$DM_LOG" &
poll_pid=$!

# Workload: 4 KiB random write across the R-region. log_avg_msec=1000
# emits a single (time, bw, ...) row per second to ${BW_LOG}_bw.1.log.
fio --name=ts --filename="/dev/mapper/$NAME" --rw=randwrite \
    --bs=4k --io_size="$workload_bytes" --offset=0 --size="$((r_end * 512))" \
    --direct=1 --ioengine=psync --numjobs=1 \
    --random_generator=tausworthe --norandommap --randrepeat=0 \
    --log_avg_msec=1000 --write_bw_log="$BW_LOG" \
    --output-format=normal > "${BW_LOG}.fio.txt" 2>&1 || true

kill $poll_pid 2>/dev/null || true
wait 2>/dev/null || true

{
    echo "ended=$(date -Iseconds)"
    echo "status_final=$(dmsetup status "$NAME" | sed 's/^.*hyhost //')"
} >> "$META"

dmsetup remove "$NAME" 2>/dev/null || true
nvme io-passthru "$BACKING" --opcode=0x79 --namespace-id=1 \
    --cdw10=268435456 --cdw11=0 --cdw13=0x20 -r >/dev/null 2>&1 || true

echo "==="
echo "BW log:  ${BW_LOG}_bw.1.log"
echo "DM log:  $DM_LOG"
echo "Meta:    $META"
echo
echo "Final dmsetup status:"
grep status_final "$META" | sed 's/^/  /'
