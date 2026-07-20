#!/bin/bash
#
# debug-dual-gdb.sh [hyzns|zns]   (default: hyzns)
#
# Launch the dual-device FEMU under gdb (batch) to capture a backtrace when
# qemu crashes. The DRAM backends are mlock'd (256G+16G) so a real coredump
# would be hundreds of GB — attaching gdb is the practical way to get the
# crashing hw/femu/ file:line. The qemu binary is built not-stripped with
# debug_info, so frames resolve to source.
#
# Mechanism: _femu_tune.sh honors $FEMU_QEMU_BIN, so we set it to a gdb batch
# invocation. On SIGSEGV/SIGABRT gdb writes a full backtrace (all threads) to
# ./cores/gdb-bt-<mode>-<ts>.txt.
#
#   ./debug-dual-gdb.sh hyzns     # run-hyzns-dual.sh under gdb
#   ./debug-dual-gdb.sh zns        # run-zns-dual.sh under gdb
# Then drive the guest (ssh -p 4440) as usual; on crash read the bt file.
#
# NOTE: run scripts use `sudo ${FEMU_LAUNCH}`, so gdb runs as root too — fine.

set -u
MODE="${1:-hyzns}"
case "$MODE" in
    hyzns) SRC="run-hyzns-dual.sh" ;;
    zns)    SRC="run-zns-dual.sh" ;;
    *) echo "usage: $0 {hyzns|zns}" >&2; exit 1 ;;
esac
[ -e "$SRC" ] || { echo "no such launcher: $SRC" >&2; exit 1; }

mkdir -p ./cores
TS=$(date +%Y%m%d_%H%M%S)
BT="$(pwd)/cores/gdb-bt-${MODE}-${TS}.txt"
GDBCMDS="$(pwd)/cores/.gdbcmds-${TS}"

cat > "$GDBCMDS" <<GDB
set pagination off
set logging file ${BT}
set logging overwrite on
set logging on
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIGSEGV stop print
handle SIGABRT stop print
run
echo \n==== CRASH CAUGHT ====\n
bt full
echo \n==== crash-frame inspection (nvme_process_cq_cpl) ====\n
frame 0
# Always-safe context FIRST (independent of the possibly-bad req), so we keep
# it even if a later req deref aborts the batch.
echo \n-- controller identity --\n
print n->devname
print n->femu_mode
print index_poller
echo \n-- pqueue depth --\n
print pqueue_size(pq)
echo \n-- poller config (why multiple CQ pollers?) --\n
print n->poller_on
print n->nr_pollers
print n->multipoller_enabled
print n->nr_io_queues
echo \n-- req pointer (value only; deref may fault) --\n
print req
echo \n==== all threads ====\n
thread apply all bt
echo \n==== registers ====\n
info registers
# req field derefs LAST: if req is unmapped gdb aborts the batch here, but
# everything above (threads, regs, controller, pq) is already saved.
echo \n==== req fields (deref — may abort batch if dangling) ====\n
print req->cmd_opcode
print req->status
print req->expire_time
print req->slba
print req->sq
print req->sq->sqid
quit
GDB

echo "[debug] ${SRC} under gdb -> backtrace will be written to:"
echo "        ${BT}"
echo "[debug] drive the guest as usual (ssh -p 4440); on crash, read that file."

# Splice gdb in front of qemu via the FEMU_QEMU_BIN knob; the launcher's
# femu_cpu_plan picks it up when building FEMU_LAUNCH.
export FEMU_QEMU_BIN="gdb -batch -x ${GDBCMDS} --args ./qemu-system-x86_64"

exec ./"$SRC"
