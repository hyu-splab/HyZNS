#!/bin/bash
# Shared helpers for the per-figure wrappers. Source this from each run.sh.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die()       { echo "ERROR: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "run with: sudo -E $0 $*"; }
need_femu() { timeout 8 nvme list >/dev/null 2>&1 \
              || die "NVMe device not visible — boot the FEMU VM first (femu/femu-scripts/)"; }
need_bin()  { [ -x "$1" ] || die "missing $1 — see experiments/README.md (build)"; }
# newest subdirectory of $1 (harnesses create one timestamped dir per run)
latest_dir(){ ls -td "$1"/*/ 2>/dev/null | head -1; }
