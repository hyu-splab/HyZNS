#!/bin/bash
# fig9_overnight.sh — unattended Fig9 sweep on HyZNS single.
# Ordered by value so an early stop still leaves 20M complete:
#   1) WAL 20M N=3   2) L0 20M N=2   3) WAL 40M N=3   4) L0 40M N=2
# Each cell is isolated (fig9_cell.sh); a single cell failure is logged and skipped.
# db_bench is SIGTERM'd on timeout (never SIGKILL) to protect FEMU.
set -uo pipefail
cd "$(dirname "$0")/../.."                                   # rocksdb/
CELL=scripts/legacy/fig9_cell.sh
LOG=results/fig9/overnight.out; mkdir -p results/fig9
WAL_REPS=${WAL_REPS:-3}; L0_REPS=${L0_REPS:-2}
NUMS=${NUMS:-"20000000 40000000"}                         # smoke: NUMS=2000000 WAL_REPS=1 L0_REPS=1
JOBSET=${JOBSET:-"2 4 8"}
WAL_TMO=${WAL_TMO:-1800}; L0_TMO=${L0_TMO:-2400}
say(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

cell(){ # all-env passthrough; never abort the sweep on one failure
  local desc=$1; shift
  say "RUN $desc"
  if env "$@" bash "$CELL" >>"$LOG" 2>&1; then say "OK  $desc"; else say "FAIL($?) $desc (continuing)"; fi
}

femu_alive(){ timeout 6 nvme list >/dev/null 2>&1; }

: > "$LOG"
say "FIG9 OVERNIGHT START  nums=[$NUMS] wal_reps=$WAL_REPS l0_reps=$L0_REPS jobs=[$JOBSET]"
lsmod|awk '{print $1}'|grep -qx dm_hyzns || ../dm-hyzns/scripts/load.sh >/dev/null 2>&1
femu_alive && say "FEMU alive at start" || say "WARN: FEMU not responding at start"

run_wal(){ local NUM=$1 reps=$2
  for r in $(seq 1 "$reps"); do for wl in FR OW; do for j in $JOBSET; do for cfg in zns posix; do
    femu_alive || { say "ABORT phase: FEMU down"; return 1; }
    cell "WAL n=$NUM $wl j=$j $cfg rep$r" TYPE=wal CFG=$cfg WL=$wl JOBS=$j NUM=$NUM REP=$r TMO=$WAL_TMO
  done; done; done; done
}
run_l0(){ local NUM=$1 reps=$2
  for r in $(seq 1 "$reps"); do for j in $JOBSET; do for cfg in l0zns l0cns_nomod l0cns_mod; do
    femu_alive || { say "ABORT phase: FEMU down"; return 1; }
    cell "L0 n=$NUM j=$j $cfg rep$r" TYPE=l0 CFG=$cfg JOBS=$j NUM=$NUM REP=$r TMO=$L0_TMO
  done; done; done
}

set -- $NUMS; N20=${1:-20000000}; N40=${2:-}
say "=== PHASE 1: WAL $N20 N=$WAL_REPS ===";  run_wal "$N20" "$WAL_REPS"; say "PHASE1_DONE"
say "=== PHASE 2: L0  $N20 N=$L0_REPS ===";   run_l0  "$N20" "$L0_REPS"; say "PHASE2_DONE"
if [[ -n "$N40" ]]; then
  say "=== PHASE 3: WAL $N40 N=$WAL_REPS ==="; run_wal "$N40" "$WAL_REPS"; say "PHASE3_DONE"
  say "=== PHASE 4: L0  $N40 N=$L0_REPS ===";  run_l0  "$N40" "$L0_REPS"; say "PHASE4_DONE"
fi
say "FIG9_OVERNIGHT_DONE"
