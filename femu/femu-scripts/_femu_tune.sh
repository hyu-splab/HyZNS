# _femu_tune.sh — shared CPU-affinity / tuning helper for FEMU run scripts.
#
# Sourced (not executed) by run-*.sh. Provides:
#   femu_cpu_plan  — compute FEMU_CPUS / FEMU_SMP / HOST_CPU from the live
#                    online-CPU topology (SMT-aware: auto-adapts whether
#                    hyperthreading is on or off) and export FEMU_LAUNCH,
#                    the pin+nice prefix to put in front of ./qemu-system-x86_64.
#
# Usage in a run script:
#   source ./_femu_tune.sh
#   femu_cpu_plan
#   sudo ${FEMU_LAUNCH} \
#       -name ... -smp ${FEMU_SMP} ... 2>&1 | tee log
# (FEMU_LAUNCH = "taskset -c <cpus> nice -n <n> ./qemu-system-x86_64")
#
# Policy:
#   - This host's only heavy process is the qemu (FEMU) process; db_bench /
#     RocksDB run inside the guest. We reserve ONE physical core for the host
#     side (measurement loggers: iostat/mpstat/vmstat, OS daemons, IRQs) so
#     they don't steal cycles from FEMU's polling latency model. FEMU gets
#     every other online physical core, and -smp matches that count.
#   - For accurate latency emulation, run with SMT off and governor
#     performance (toggle via measure-setup.sh on|off). The plan below still
#     works with SMT on — it just pins to all-but-one *logical* CPU then.
#
# Override knobs (env):
#   FEMU_CPUS  — explicit taskset list (e.g. "0-14"); skips auto-compute.
#   FEMU_SMP   — explicit vCPU count; defaults to FEMU_CPUS width minus
#                FEMU_POLLER_CORES.
#   FEMU_NICE  — niceness (default -10; needs sudo, which run-*.sh already use).
#   FEMU_RESERVE_HOST_CORES — physical cores to hold back for host (default 1).
#   FEMU_POLLER_CORES — cores pin-threads.sh dedicates to femu-nvme-poller
#                threads at the top of FEMU_CPUS (default 1); vCPU count
#                shrinks by the same amount so pollers never share a core.
#
# After the VM is up, run ./pin-threads.sh once to apply the per-thread split
# (vCPUs vs pollers) — taskset on the whole process can't distinguish them.

# Count online logical CPUs (e.g. 32 with SMT on, 16 off).
_femu_online_cpus() { nproc; }

# Physical cores online = online logical CPUs / threads-per-core.
_femu_phys_cores() {
    local tpc
    tpc=$(lscpu 2>/dev/null | awk -F: '/^Thread\(s\) per core/{gsub(/ /,"",$2);print $2}')
    [ -z "$tpc" ] && tpc=1
    echo $(( $(_femu_online_cpus) / tpc ))
}

# Build FEMU_CPUS / FEMU_SMP / HOST_CPU.
femu_cpu_plan() {
    local online reserve last_femu
    online=$(_femu_online_cpus)
    reserve=${FEMU_RESERVE_HOST_CORES:-1}

    if [ -n "$FEMU_CPUS" ]; then
        : # caller pinned explicitly; honor it.
    else
        # Reserve `reserve` logical CPUs at the top for the host; FEMU takes
        # the rest. With SMT off, online == physical, so this is "all but the
        # last physical core". With SMT on it's "all but the last sibling".
        last_femu=$(( online - reserve - 1 ))
        if [ "$last_femu" -lt 0 ]; then last_femu=$(( online - 1 )); fi
        FEMU_CPUS="0-${last_femu}"
        HOST_CPU="$(( last_femu + 1 ))-$(( online - 1 ))"
    fi

    # vCPU count = width of FEMU_CPUS minus the poller cores pin-threads.sh
    # carves out of the top of the mask (default 1 = nr_pollers with
    # multipoller off). Keeps the busy-polling femu-nvme-poller from having
    # to share its core with a vCPU.
    local width poller_cores=${FEMU_POLLER_CORES:-1}
    width=$(echo "$FEMU_CPUS" | tr ',' '\n' | awk -F- '
        { if (NF==2) n+=($2-$1+1); else n+=1 } END{print n}')
    if [ -z "$FEMU_SMP" ]; then
        FEMU_SMP=$(( width - poller_cores ))
        if [ "$FEMU_SMP" -lt 1 ]; then FEMU_SMP=$width; fi
    fi

    local nice_lvl=${FEMU_NICE:--10}
    # FEMU_QEMU_BIN lets a debug wrapper splice gdb in front of qemu, e.g.
    #   FEMU_QEMU_BIN="gdb -batch -x cmds --args ./qemu-system-x86_64"
    local qbin=${FEMU_QEMU_BIN:-./qemu-system-x86_64}
    FEMU_LAUNCH="taskset -c ${FEMU_CPUS} nice -n ${nice_lvl} ${qbin}"

    export FEMU_CPUS FEMU_SMP HOST_CPU FEMU_LAUNCH
    echo "[femu-tune] online_cpus=${online} phys_cores=$(_femu_phys_cores)" \
         "smt=$(cat /sys/devices/system/cpu/smt/active 2>/dev/null)" \
         "-> FEMU pinned to CPUs ${FEMU_CPUS} (smp ${FEMU_SMP}), host reserves ${HOST_CPU:-none}"
    local gov
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
    if [ -n "$gov" ] && [ "$gov" != "performance" ]; then
        echo "[femu-tune] WARNING: governor='${gov}' (run ./measure-setup.sh on for 'performance')"
    fi
    return 0
}
