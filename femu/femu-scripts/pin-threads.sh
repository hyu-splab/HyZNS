#!/bin/bash
#
# pin-threads.sh [qemu-pid]
#
# Per-thread CPU affinity for a running FEMU VM. run-*.sh already taskset the
# whole qemu process to FEMU_CPUS (via _femu_tune.sh), but a process-wide mask
# lets the scheduler co-locate/migrate the busy-polling femu-poller
# thread(s) with vCPUs — visible as tail-latency noise in the emulation.
# This is the single-socket analogue of official FEMU's pin.sh / hiops
# 03-pin.sh: split threads by comm name onto disjoint cores WITHIN the
# process's existing affinity mask:
#
#   femu-poller*  -> one dedicated core each, taken from the TOP of the
#                         mask (matches _femu_tune.sh, which shrinks -smp by
#                         FEMU_POLLER_CORES so these cores have no vCPU).
#   CPU <n>/KVM vCPUs  -> the remaining cores, round-robin.
#   everything else    -> the vCPU core set (main loop, AIO/worker threads
#                         are not latency-critical; they share).
#
# Run ONCE after the VM is up (pollers are created at device realize, so
# right after launch is fine):
#   sudo ./pin-threads.sh          # newest qemu-system-x86_64
#   sudo ./pin-threads.sh <pid>    # explicit (e.g. dual-VM setups: run per VM)
#
# Pair with: measure-setup.sh on  ->  run-*.sh  ->  pin-threads.sh

set -u

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root (use: sudo $0 $*)" >&2
    exit 1
fi

# pidof matches the exact executable (pgrep -f would also match the parent
# sudo/tee pipeline whose cmdline contains the qemu path). First PID = newest.
QP=${1:-$(pidof qemu-system-x86_64 2>/dev/null | awk '{print $1}')}
if [ -z "${QP}" ] || [ ! -d "/proc/$QP" ]; then
    echo "no live qemu-system-x86_64 found (pass a PID explicitly)" >&2
    exit 1
fi

# Expand the process's current affinity list ("0-14" / "0-3,8-11") into an
# array. This is the FEMU_CPUS mask run-*.sh applied at launch.
mask=$(taskset -pc "$QP" | awk -F': ' '{print $2}')
cpus=()
for part in ${mask//,/ }; do
    case "$part" in
        *-*) for ((c = ${part%-*}; c <= ${part#*-}; c++)); do cpus+=("$c"); done ;;
        *)   cpus+=("$part") ;;
    esac
done
n=${#cpus[@]}

# Collect thread IDs by comm. vCPU threads are "CPU <n>/KVM"; poller comm is
# "femu-poller" (TASK_COMM_LEN safe: <=15 chars).
vcpu_tids=() poller_tids=() other_tids=()
for t in $(ls "/proc/$QP/task"); do
    comm=$(cat "/proc/$QP/task/$t/comm" 2>/dev/null) || continue
    case "$comm" in
        CPU*KVM*)         vcpu_tids+=("$t") ;;
        femu-poller*)  poller_tids+=("$t") ;;
        *)                other_tids+=("$t") ;;
    esac
done
np=${#poller_tids[@]}

if [ "$np" -eq 0 ]; then
    echo "WARNING: no femu-poller threads found. Nothing pinned." >&2
    if ! grep -qx "qemu-system-x86" "/proc/$QP/comm" || \
       [ "$(cat "/proc/$QP/task/"*/comm 2>/dev/null | sort -u | wc -l)" -le 1 ]; then
        echo "  All threads share one comm — QEMU only names threads when" \
             "launched with '-name <vm>,debug-threads=on' (run-*.sh have it" \
             "now; restart the VM with the updated script)." >&2
    else
        echo "  Is this a FEMU VM, and is the femu device realized?" >&2
    fi
    exit 1
fi
if [ "$np" -ge "$n" ]; then
    echo "WARNING: $np pollers >= $n cores in mask '$mask' — cannot give" \
         "pollers exclusive cores. Nothing pinned." >&2
    exit 1
fi

# vCPU set = bottom (n - np) cores; poller i = top core n-1-i (exclusive).
vcpu_set=$(IFS=,; echo "${cpus[*]:0:n-np}")

i=0
for t in "${poller_tids[@]}"; do
    taskset -pc "${cpus[$((n - 1 - i))]}" "$t" > /dev/null && i=$((i + 1))
done
j=0
for t in "${vcpu_tids[@]}"; do
    taskset -pc "${cpus[$((j % (n - np)))]}" "$t" > /dev/null && j=$((j + 1))
done
k=0
for t in "${other_tids[@]}"; do
    taskset -pc "$vcpu_set" "$t" > /dev/null && k=$((k + 1))
done

echo "qemu pid $QP, affinity mask '$mask' ($n cores):"
echo "  $i poller(s) -> exclusive top core(s): ${cpus[*]:n-np}"
echo "  $j vCPU(s)   -> round-robin over: $vcpu_set"
echo "  $k other(s)  -> $vcpu_set (shared)"
echo "--- verify (poller threads) ---"
for t in "${poller_tids[@]}"; do
    printf "  tid %-8s %-16s %s\n" "$t" \
        "$(cat "/proc/$QP/task/$t/comm" 2>/dev/null)" \
        "$(taskset -pc "$t" 2>/dev/null | awk -F': ' '{print "cpus "$2}')"
done
