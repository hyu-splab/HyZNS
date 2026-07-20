#!/bin/bash
# Verify the paper's ResizeCNS via the zenfs CLI (offline, between phases):
#   fill -> `zenfs resize-cns` GROW -> reopen+read (data intact) -> SHRINK -> reopen+read.
# Demonstrates online-capable area resize without reformat or data loss.
# Usage: sudo NUM=3000000 ./scripts/legacy/modify_cli_verify.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyzns/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS
R0=${R0:-8}; RGROW=${RGROW:-10}; NUM=${NUM:-3000000}; AOZ=${AOZ:-7}
READS=${READS:-500000}
ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1))
DBB() { local sz=$1 bench=$2 log=$3 extra=${4:-}; \
  ./db_bench --fs_uri="zenfs://dev:dm-0/$sz/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks="$bench" --use_direct_io_for_flush_and_compaction \
    --num="$NUM" --key_size=20 --value_size=800 --compression_type=none \
    --max_background_jobs=8 $extra &> "$log"; }
rend() { dmsetup status "$DMNAME" | grep -oE 'r_end=[0-9]+' | grep -oE '[0-9]+'; }
found() { grep -oE '\([0-9]+ of [0-9]+ found\)' "$1" | tail -1; }

umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
lsmod|awk '{print $1}'|grep -qx dm_hyzns||../dm-hyzns/scripts/load.sh>/dev/null
reset_device_full_r "$BACKING"||true
SZ=$(blockdev --getsz "$BACKING")
# max-provision: dm 5th arg = max r_end (whole dev); mkfs -P lays F2FS metadata
# for the max size so the active region can grow online.
echo "0 $SZ hyzns $BACKING $((R0*ZS)) $SZ"|dmsetup create "$DMNAME"
dmsetup message "$DMNAME" 0 set_r_end $((R0*ZS))
mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R0" \
  --start_zone="$R0" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1

echo "### PHASE 1: fill (R=$R0, r_end=$(rend), expect $((R0*ZS)))  bench=${BENCH:-fillrandom}"
DBB "$R0" "${BENCH:-fillrandom}" /tmp/cli_fill.log "${FILL_EXTRA:-}"
echo "  fill: $(grep -oE 'fillrandom[^;]+ops/sec' /tmp/cli_fill.log|tail -1)"

echo "### GROW via CLI: $R0 -> $RGROW"
"$ZENFS" resize-cns --zbd=dm-0 --aux_path="$AUX" --new_rzone="$RGROW" &> /tmp/cli_grow.log
grep -E 'Migrating|migrat|resize_cns completed|error|failed|ResizeCNS completed' /tmp/cli_grow.log | head
echo "  r_end after grow=$(rend) (expect $((RGROW*ZS)))"

echo "### VERIFY data intact after grow (read R=$RGROW)"
DBB "$RGROW" "readrandom" /tmp/cli_read1.log "--use_existing_db=1 --reads=$READS --threads=1 --use_direct_reads=true"
echo "  read-after-grow: $(found /tmp/cli_read1.log)  (expect $READS of $READS)"

echo "### SHRINK via CLI: $RGROW -> $R0"
"$ZENFS" resize-cns --zbd=dm-0 --aux_path="$AUX" --new_rzone="$R0" &> /tmp/cli_shrink.log
grep -E 'gc_force|resize_cns completed|error|failed|ResizeCNS completed' /tmp/cli_shrink.log | head
echo "  r_end after shrink=$(rend) (expect $((R0*ZS)))"

echo "### VERIFY data intact after shrink (read R=$R0)"
DBB "$R0" "readrandom" /tmp/cli_read2.log "--use_existing_db=1 --reads=$READS --threads=1 --use_direct_reads=true"
echo "  read-after-shrink: $(found /tmp/cli_read2.log)  (expect $READS of $READS)"

umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "CLI_VERIFY_DONE"
