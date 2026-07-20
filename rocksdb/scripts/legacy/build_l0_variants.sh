#!/bin/bash
# Build the L0-placement db_bench variant for FigC by toggling the compile-time
# L0_POSIX macro in plugin/zenfs/fs/fs_zenfs.cc, then restore the source.
# WAL macros are forced OFF so WAL stays on ZNS — FigC isolates L0 placement only.
#
#   db_bench_l0cns : L0 SST -> aux F2FS (CNS/R-region)  [#define L0_POSIX]
#   db_bench_zns   : (already built by build_wal_variants.sh) L0 + WAL on ZNS = baseline
set -uo pipefail
cd "$(dirname "$0")/../.."                 # rocksdb/
SRC=plugin/zenfs/fs/fs_zenfs.cc
BUILD='DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j32 db_bench'

# WAL off (WAL->ZNS), L0_POSIX on (L0->CNS)
sed -i 's|^#define WAL_POSIX|//#define WAL_POSIX|; s|^#define WAL_CNS|//#define WAL_CNS|; s|^//#define L0_POSIX|#define L0_POSIX|' "$SRC"
echo "=== toggles for db_bench_l0cns ==="
grep -nE 'define (WAL_POSIX|WAL_CNS|L0_POSIX)' "$SRC" | sed 's/^/    /'
eval "$BUILD" || { echo "BUILD FAILED"; sed -i 's|^#define L0_POSIX|//#define L0_POSIX|' "$SRC"; exit 1; }
cp db_bench db_bench_l0cns
echo "    -> db_bench_l0cns ($(stat -c%s db_bench_l0cns) bytes)"

# restore source to default (all WAL/L0 toggles commented = WAL+L0 on ZNS)
sed -i 's|^#define L0_POSIX|//#define L0_POSIX|' "$SRC"
echo "=== source restored ==="; grep -nE 'define (WAL_POSIX|WAL_CNS|L0_POSIX)' "$SRC" | sed 's/^/    /'
ls -la db_bench_l0cns db_bench_zns 2>/dev/null
