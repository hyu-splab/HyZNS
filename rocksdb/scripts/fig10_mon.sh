#!/bin/bash
#
# fig10_mon.sh — external all-layer sampler for fig10 runs (no harness change).
# Attaches to any running db_bench_* processes and samples every INTERVAL s:
#
#   sys   : CPU busy% (/proc/stat), runqueue (/proc/loadavg r), ctxt/s
#   proc  : per-db_bench utime+stime jiffy delta, thread count
#   thread: fg(=non-rocksdb:*) threads' schedstat run_delay delta  <- CPU starvation
#           bg(=rocksdb:low/high) threads' cpu jiffy delta
#   dm    : dmsetup status hyzns0 (raw line: w/gc_mig/free_lines/...)
#   disk  : /proc/diskstats nvme0n1 + dm-0 (wr sectors, in-flight, io_ticks)
#
# Output: one CSV (wide, raw counters — analyze with deltas) + f2fs status
# snapshots every 10 samples.
#
#   sudo ./scripts/fig10_mon.sh <outdir> [interval_s]     # Ctrl-C / kill to stop
set -u
OUT=${1:?usage: fig10_mon.sh <outdir> [interval]}; IV=${2:-2}
mkdir -p "$OUT"
CSV="$OUT/mon.csv"
DM=${DMNAME:-hyzns0}
BACK=nvme0n1

echo "ts,cpu_busy_j,cpu_total_j,runq,ctxt,pids,nthr,fg_cpu_j,fg_delay_ns,bg_cpu_j,dm_status,d_wsec,d_inflight,d_ioticks,dm_wsec,dm_inflight" > "$CSV"
n=0
while :; do
    ts=$(date +%s.%N)
    # ---- system CPU: busy jiffies / total jiffies, runq, ctxt
    read -r _ u ni sy id io irq sirq st _ < /proc/stat
    busy=$((u+ni+sy+irq+sirq+st)); total=$((busy+id+io))
    runq=$(awk '{split($4,a,"/"); print a[1]}' /proc/loadavg)
    ctxt=$(awk '/^ctxt/{print $2}' /proc/stat)
    # ---- db_bench processes
    pids=$(pgrep -d' ' -f 'db_bench_(zns|posix|l0cns)' || true)
    fgcpu=0; fgdel=0; bgcpu=0; nthr=0
    for p in $pids; do
        [ -d /proc/$p ] || continue
        for t in /proc/$p/task/*; do
            [ -r "$t/stat" ] || continue
            nthr=$((nthr+1))
            read -r cpu_u cpu_s <<<"$(awk '{print $14, $15}' "$t/stat" 2>/dev/null)" || continue
            case "$(cat "$t/comm" 2>/dev/null)" in
                rocksdb:*) bgcpu=$((bgcpu+cpu_u+cpu_s));;
                *) fgcpu=$((fgcpu+cpu_u+cpu_s))
                   d=$(awk '{print $2}' "$t/schedstat" 2>/dev/null||echo 0); fgdel=$((fgdel+d));;
            esac
        done
    done
    # ---- dm + disk raw counters
    dmst=$(dmsetup status "$DM" 2>/dev/null | tr ',' ';' || echo NA)
    dstat=$(awk -v d=$BACK '$3==d{print $10","$12","$13}' /proc/diskstats)   # wsec,inflight,ioticks
    mstat=$(awk '$3=="dm-0"{print $10","$12}' /proc/diskstats)               # wsec,inflight
    echo "$ts,$busy,$total,$runq,$ctxt,$(echo $pids|tr ' ' '+'),$nthr,$fgcpu,$fgdel,$bgcpu,\"$dmst\",$dstat,$mstat" >> "$CSV"
    # ---- f2fs snapshot every 10 samples
    if (( n % 10 == 0 )) && [ -r /sys/kernel/debug/f2fs/status ]; then
        { echo "=== $ts ==="; cat /sys/kernel/debug/f2fs/status; } >> "$OUT/f2fs_status.log" 2>/dev/null
    fi
    n=$((n+1)); sleep "$IV"
done
