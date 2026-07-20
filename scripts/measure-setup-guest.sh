#!/bin/bash
#
# measure-setup-guest.sh {on|off|status}
#
# GUEST-side counterpart of femu/femu-scripts/measure-setup.sh — run this as
# root INSIDE the FEMU VM before a measurement run. Collects into one script
# what official FEMU spreads across pre.sh / femu-run.sh hints / the README's
# "Guest Optimization" section:
#
#   on  — * I/O scheduler -> mq-deadline on every nvme block device
#           (GUEST_SCHED=none to bypass the scheduler for raw fio latency)
#         * ASLR off            (kernel.randomize_va_space = 0; FEMU pre.sh)
#         * timer_migration = 0 (keep timers on the CPU that armed them)
#         * stop noisy services (cron/atd/unattended-upgrades + apt timers)
#   off — restore defaults (ASLR 2, timer_migration 1, restart services;
#         scheduler is left as set — pass GUEST_SCHED to change it back).
#   status — print all of the above.
#
# Order of a full measurement run:
#   host:  sudo ./measure-setup.sh on && ./run-*.sh && sudo ./pin-threads.sh
#   guest: sudo ~/HYSSD/scripts/measure-setup-guest.sh on
#   guest: <fio / db_bench / fxmark ...>
#
# NOTE: dm-hyzns mapper devices are bio-based (no I/O scheduler); the
# scheduler is set on the underlying /dev/nvme* queues, which is what counts.

set -u

# Space-separated env overrides.
GUEST_SCHED=${GUEST_SCHED:-mq-deadline}
NOISY_SERVICES=${NOISY_SERVICES:-"cron atd unattended-upgrades"}
NOISY_TIMERS=${NOISY_TIMERS:-"apt-daily.timer apt-daily-upgrade.timer man-db.timer"}

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "must run as root (use: sudo $0 $*)" >&2
        exit 1
    fi
}

set_sched() {
    local g="$1" f dev found=0
    for f in /sys/block/nvme*/queue/scheduler; do
        [ -e "$f" ] || continue
        found=1
        dev=$(basename "$(dirname "$(dirname "$f")")")
        if echo "$g" > "$f" 2>/dev/null; then
            echo "  $dev scheduler -> $g"
        else
            echo "  $dev scheduler -> FAILED ('$g' not in: $(cat "$f"))"
        fi
    done
    [ "$found" -eq 0 ] && echo "  no nvme block devices found"
}

print_status() {
    local f dev
    for f in /sys/block/nvme*/queue/scheduler; do
        [ -e "$f" ] || continue
        dev=$(basename "$(dirname "$(dirname "$f")")")
        echo "scheduler ($dev)  : $(cat "$f")"
    done
    echo "ASLR              : $(cat /proc/sys/kernel/randomize_va_space 2>/dev/null)   (0=off, 2=default)"
    echo "timer_migration   : $(cat /proc/sys/kernel/timer_migration 2>/dev/null)   (0=pinned, 1=default)"
    local s
    for s in $NOISY_SERVICES $NOISY_TIMERS; do
        echo "service           : $s = $(systemctl is-active "$s" 2>/dev/null)"
    done
}

case "${1:-}" in
    on)
        need_root "$@"
        echo "==> guest measurement mode ON"
        set_sched "$GUEST_SCHED"
        echo 0 > /proc/sys/kernel/randomize_va_space 2>/dev/null \
            && echo "  ASLR disabled"
        echo 0 > /proc/sys/kernel/timer_migration 2>/dev/null \
            && echo "  timer_migration -> 0"
        for s in $NOISY_SERVICES $NOISY_TIMERS; do
            systemctl stop "$s" 2>/dev/null && echo "  stopped: $s"
        done
        echo "---"
        print_status
        ;;
    off)
        need_root "$@"
        echo "==> guest measurement mode OFF (restore)"
        echo 2 > /proc/sys/kernel/randomize_va_space 2>/dev/null \
            && echo "  ASLR restored (2)"
        echo 1 > /proc/sys/kernel/timer_migration 2>/dev/null \
            && echo "  timer_migration -> 1"
        for s in $NOISY_SERVICES $NOISY_TIMERS; do
            systemctl start "$s" 2>/dev/null && echo "  restarted: $s"
        done
        echo "---"
        print_status
        ;;
    status|"")
        print_status
        ;;
    *)
        echo "usage: $0 {on|off|status}" >&2
        exit 1
        ;;
esac
