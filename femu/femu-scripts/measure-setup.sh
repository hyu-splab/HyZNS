#!/bin/bash
#
# measure-setup.sh {on|off|status}
#
# Toggle the host into (or out of) a clean state for FEMU latency-emulation
# measurements. Affects the WHOLE host (sudo). Runtime-only, no reboot:
#
#   on   — disable SMT (hyperthreading) + set CPU governor to performance
#          + stop noisy host services (cron/atd; NOISY_SERVICES to override).
#          nproc drops to the physical-core count; FEMU's polling latency
#          model no longer shares execution units with an SMT sibling.
#   off  — re-enable SMT + restore governor (schedutil/ondemand) + services.
#   status — print current SMT + governor + nproc.
#
# Guest-side counterpart: scripts/measure-setup-guest.sh (run inside the VM).
#
# Pair with the run-*.sh scripts, which auto-pin FEMU to all-but-one online
# physical core via _femu_tune.sh (they adapt to whatever SMT state is set
# here — turn this on first, then launch FEMU).
#
# Run as:  sudo ./measure-setup.sh on    (or via '! sudo ./measure-setup.sh on')

set -u

SMT_CTRL=/sys/devices/system/cpu/smt/control
RESTORE_GOV=${RESTORE_GOV:-schedutil}   # governor to restore on 'off'

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "must run as root (use: sudo $0 $*)" >&2
        exit 1
    fi
}

set_governor() {
    local g="$1"
    if command -v cpupower >/dev/null 2>&1; then
        cpupower frequency-set -g "$g" >/dev/null 2>&1 && return 0
    fi
    # Fallback: write each policy directly. rc=0 (success) unless a write fails.
    local rc=0 f
    for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        { [ -w "$f" ] && echo "$g" > "$f" 2>/dev/null; } || rc=1
    done
    return $rc
}

# Background host services that can steal cycles from FEMU's polling latency
# model mid-run (official FEMU tuning.sh stops similar ones). Space-separated;
# override via env. Missing units are silently skipped.
NOISY_SERVICES=${NOISY_SERVICES:-"cron atd"}

stop_services() {
    local s
    for s in $NOISY_SERVICES; do
        systemctl stop "$s" 2>/dev/null && echo "  service stopped: $s"
    done
}

start_services() {
    local s
    for s in $NOISY_SERVICES; do
        systemctl start "$s" 2>/dev/null && echo "  service restarted: $s"
    done
}

print_status() {
    local smt gov
    smt=$(cat "$SMT_CTRL" 2>/dev/null || echo "n/a")
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "n/a")
    echo "SMT control : ${smt}   (active=$(cat /sys/devices/system/cpu/smt/active 2>/dev/null))"
    echo "governor    : ${gov}"
    echo "nproc       : $(nproc)"
}

case "${1:-}" in
    on)
        need_root "$@"
        echo "==> measurement mode ON"
        if [ -w "$SMT_CTRL" ]; then
            echo off > "$SMT_CTRL" && echo "  SMT disabled"
        else
            echo "  SMT control not available ($SMT_CTRL) — skipping"
        fi
        if set_governor performance; then
            echo "  governor -> performance"
        else
            echo "  governor change failed (cpufreq not available?)"
        fi
        stop_services
        echo "---"
        print_status
        echo "Now launch FEMU (run-*.sh will auto-pin to the physical cores),"
        echo "then run ./pin-threads.sh to separate vCPU / poller threads."
        ;;
    off)
        need_root "$@"
        echo "==> measurement mode OFF (restore)"
        if [ -w "$SMT_CTRL" ]; then
            echo on > "$SMT_CTRL" && echo "  SMT re-enabled"
        fi
        if set_governor "$RESTORE_GOV"; then
            echo "  governor -> ${RESTORE_GOV}"
        else
            echo "  governor restore failed"
        fi
        start_services
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
