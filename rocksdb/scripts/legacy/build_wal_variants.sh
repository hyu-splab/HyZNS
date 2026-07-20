#!/bin/bash
#
# Build the two db_bench WAL variants used by FigB (and Fig1) by toggling the
# compile-time WAL placement in plugin/zenfs/fs/fs_zenfs.cc, then restore the
# source to its default (all toggles commented = WAL on ZNS).
#
#   db_bench_zns   : WAL stays on ZNS  (wal_type=0, tree default)
#   db_bench_posix : WAL -> aux F2FS POSIX file (wal_type=2, O_DIRECT|O_SYNC),
#                    i.e. on the R-region / CNS in the HyZNS stack
#   db_bench_cns   : WAL -> raw CNS    (wal_type=1)   [only with --cns]
#
# Usage: ./scripts/legacy/build_wal_variants.sh [--cns]
set -uo pipefail
cd "$(dirname "$0")/../.."                 # rocksdb/

SRC=plugin/zenfs/fs/fs_zenfs.cc
BUILD='DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j32 db_bench'

# Force the zenfs plugin (incl. io_zenfs.cc roll-off) to recompile so every variant
# links the current source, not a stale .o from an earlier build.
touch "$SRC" plugin/zenfs/fs/io_zenfs.cc plugin/zenfs/fs/io_zenfs.h 2>/dev/null || true

build_variant() {            # <define-or-empty> <out-binary>
    local def=$1 out=$2
    # reset ALL placement toggles to commented (WAL_POSIX / WAL_CNS / L0_POSIX)
    sed -i 's|^#define WAL_POSIX|//#define WAL_POSIX|; s|^#define WAL_CNS|//#define WAL_CNS|; s|^#define L0_POSIX|//#define L0_POSIX|' "$SRC"
    [[ -n $def ]] && sed -i "s|^//#define $def|#define $def|" "$SRC"
    echo "=== building $out ($( [[ -n $def ]] && echo $def || echo VANILLA_WAL_ZNS )) ==="
    grep -nE 'define WAL_POSIX|define WAL_CNS|define L0_POSIX' "$SRC" | sed 's/^/    /'
    eval "$BUILD" || { echo "BUILD FAILED for $out"; return 1; }
    cp db_bench "$out"
    echo "    -> $out  ($(stat -c%s "$out") bytes)"
}

build_variant ""         db_bench_zns      # VANILLA : WAL->ZNS, L0->ZNS
build_variant WAL_POSIX  db_bench_posix    # WAL_CNS : WAL->aux/CNS (O_SYNC), L0->ZNS
build_variant L0_POSIX   db_bench_l0cns    # L0_CNS  : WAL->ZNS, L0->aux/CNS
[[ "${1:-}" == "--cns" ]] && build_variant WAL_CNS db_bench_cns

# restore source to default (all toggles off = WAL+L0 on ZNS)
sed -i 's|^#define WAL_POSIX|//#define WAL_POSIX|; s|^#define WAL_CNS|//#define WAL_CNS|; s|^#define L0_POSIX|//#define L0_POSIX|' "$SRC"
echo "=== source restored (default VANILLA WAL_ZNS) ==="
ls -la db_bench_zns db_bench_posix db_bench_l0cns ${1:+db_bench_cns} 2>/dev/null
