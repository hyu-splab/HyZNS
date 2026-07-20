#!/bin/bash
# Smoke test: online CNS grow via ZoneArbiter during a live db_bench run.
# Starts R small, runs fillrandom on ZenFS(HYSSD)+aux-F2FS with the arbiter on,
# triggers a grow mid-run via the control file, and checks the device ABA moved
# and the run completed. Usage: sudo NUM=5000000 ./scripts/legacy/modify_smoke.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyzns/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS
R0=${R0:-8}; GROW=${GROW:-2}; NUM=${NUM:-5000000}; AOZ=${AOZ:-7}
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_START=$R0; S_LAST=$((TOTAL-1))
ZENFS=./plugin/zenfs/util/zenfs

umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
lsmod | awk '{print $1}' | grep -qx dm_hyzns || ../dm-hyzns/scripts/load.sh >/dev/null
reset_device_full_r "$BACKING" || true
SZ=$(blockdev --getsz "$BACKING")
echo "0 $SZ hyzns $BACKING $((R0*ZS))" | dmsetup create "$DMNAME"
dmsetup message "$DMNAME" 0 set_r_end $((R0*ZS))
mkfs.f2fs -f -m -H -A "$DEV" >/dev/null 2>&1
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R0" \
    --start_zone="$S_START" --end_zone="$S_LAST" --ao_zones="$AOZ" \
    --enable_gc=true --force >/dev/null 2>&1

rbefore=$(dmsetup status "$DMNAME" | grep -oE 'r_end=[0-9]+')
echo "=== R before: $rbefore (expect $((R0*ZS))) ==="

ZENFS_ZONE_ARBITER=1 ./db_bench \
    --fs_uri="zenfs://dev:dm-0/$S_START/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=fillrandom,stats --use_direct_io_for_flush_and_compaction \
    --num="$NUM" --key_size=20 --value_size=800 --compression_type=none \
    --max_background_jobs=8 --statistics --stats_interval_seconds=5 \
    &> /tmp/modify_smoke_db.log &
DBPID=$!

sleep 25
echo "=== triggering grow +$GROW (R $R0 -> $((R0+GROW))) ==="
echo "$GROW" > "$AUX/.zenfs_grow"
sleep 20
rafter=$(dmsetup status "$DMNAME" | grep -oE 'r_end=[0-9]+')
echo "=== R after trigger: $rafter (expect $(((R0+GROW)*ZS))) ==="

wait "$DBPID"; RC=$?
echo "=== db_bench rc=$RC ==="
echo "--- arbiter / errors / result ---"
grep -E 'ZoneArbiter|manual grow|Expansion|GrowReserved|^fillrandom|error|EIO|failed' /tmp/modify_smoke_db.log | tail -25
echo "--- final dmstatus ---"
dmsetup status "$DMNAME" | tr ' ' '\n' | grep -E 'r_end=|pages=|free_lines=|gc_runs=|nospc='
umount "$AUX" 2>/dev/null || true; dmsetup remove "$DMNAME" 2>/dev/null || true
echo "SMOKE_DONE"
