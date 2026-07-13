#!/bin/bash
#
# build_wal_variants.sh — build the db_bench variants used by the WAL/L0
# placement experiments (Fig10 WAL, Fig11 L0) by toggling the compile-time
# placement in plugin/zenfs/fs/fs_zenfs.cc, then restoring the source.
#
#   db_bench_zns    VANILLA : WAL->ZNS,  L0->ZNS   (all toggles off; the default)
#   db_bench_posix  WAL_CNS : WAL-> aux F2FS on the CNS R-region (O_SYNC, wal_type=2)
#   db_bench_l0cns  L0_CNS  : L0 -> aux F2FS on the CNS R-region
#   db_bench_cns    WAL_CNS : WAL-> raw CNS (wal_type=1)   [only with --cns]
#
# Usage:  sudo ./scripts/build_wal_variants.sh [--cns]
set -uo pipefail
cd "$(dirname "$0")/.."                         # rocksdb/

SRC=plugin/zenfs/fs/fs_zenfs.cc
JOBS=${BUILD_JOBS:-$(nproc)}
BUILD="DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j${JOBS} db_bench"

[[ -f $SRC ]] || { echo "ERROR: $SRC not found (run from rocksdb/)"; exit 1; }

# Force the zenfs plugin (incl. io_zenfs.cc roll-off) to recompile so every
# variant links the current source, not a stale .o.
touch "$SRC" plugin/zenfs/fs/io_zenfs.cc plugin/zenfs/fs/io_zenfs.h 2>/dev/null || true

reset_toggles() {
    sed -i 's|^#define WAL_POSIX|//#define WAL_POSIX|; s|^#define WAL_CNS|//#define WAL_CNS|; s|^#define L0_POSIX|//#define L0_POSIX|' "$SRC"
}

build_variant() {            # <define-or-empty> <out-binary>
    local def=$1 out=$2
    reset_toggles
    [[ -n $def ]] && sed -i "s|^//#define $def|#define $def|" "$SRC"
    echo "=== building $out ($( [[ -n $def ]] && echo "$def" || echo VANILLA_WAL_ZNS )) ==="
    grep -nE 'define (WAL_POSIX|WAL_CNS|L0_POSIX)' "$SRC" | sed 's/^/    /'
    eval "$BUILD" || { echo "BUILD FAILED for $out"; reset_toggles; return 1; }
    cp db_bench "$out"
    echo "    -> $out  ($(stat -c%s "$out") bytes)"
}

build_variant ""         db_bench_zns      || exit 1   # VANILLA: WAL->ZNS, L0->ZNS
build_variant WAL_POSIX  db_bench_posix    || exit 1   # WAL_CNS : WAL->aux/CNS (O_SYNC)
build_variant L0_POSIX   db_bench_l0cns    || exit 1   # L0_CNS  : L0->aux/CNS
[[ "${1:-}" == "--cns" ]] && build_variant WAL_CNS db_bench_cns

reset_toggles                                          # restore default (WAL+L0 on ZNS)
echo "=== source restored (default VANILLA WAL_ZNS) ==="
ls -la db_bench_zns db_bench_posix db_bench_l0cns ${1:+db_bench_cns} 2>/dev/null
