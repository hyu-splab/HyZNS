#!/bin/bash
# Online in-process modify test: grow the CNS area WHILE db_bench is running,
# via the ZoneArbiter control file (the "interface" a util/admin would poke).
# Success = db_bench keeps running (no deadlock) AND the device ABA moves live.
# Usage: sudo NUM=30000000 ./scripts/legacy/modify_online_test.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyzns/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS
R0=${R0:-8}; GROW=${GROW:-4}; NUM=${NUM:-30000000}; AOZ=${AOZ:-7}
ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING")/ZS )); S_LAST=$((TOTAL-1)); SZ=$(blockdev --getsz "$BACKING")
rendz(){ echo $(( $(dmsetup status "$DMNAME"|grep -oE 'r_end=[0-9]+'|grep -oE '[0-9]+')/ZS )); }

umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
lsmod|awk '{print $1}'|grep -qx dm_hyzns||../dm-hyzns/scripts/load.sh>/dev/null
reset_device_full_r "$BACKING"||true
echo "0 $SZ hyzns $BACKING $((R0*ZS)) $SZ"|dmsetup create "$DMNAME"
dmsetup message "$DMNAME" 0 set_r_end $((R0*ZS))
mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R0" \
  --start_zone="$R0" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1

echo "=== launch db_bench fillrandom (NUM=$NUM) with ZoneArbiter ON, R=$R0 ==="
ZENFS_ZONE_ARBITER=1 ./db_bench --fs_uri="zenfs://dev:dm-0/$R0/$S_LAST/$AOZ/0/4294967296" \
  --benchmarks=fillrandom,stats --use_direct_io_for_flush_and_compaction \
  --num="$NUM" --key_size=20 --value_size=800 --compression_type=none \
  --max_background_jobs=8 --statistics --stats_interval_seconds=10 ${EXTRA:-} &> /tmp/online_db.log &
DBPID=$!
sleep 60
echo "  [t=60s] r_end pre-trigger = $(rendz) zones, db_bench alive=$(kill -0 $DBPID 2>/dev/null && echo Y||echo N)"
echo "=== TRIGGER online grow +$GROW while db_bench runs (write $AUX/.zenfs_grow) ==="
echo "$GROW" > "$AUX/.zenfs_grow"
sleep 30
echo "  [t=90s] r_end post-trigger = $(rendz) zones (expect $((R0+GROW))), db_bench alive=$(kill -0 $DBPID 2>/dev/null && echo Y||echo N)"
wait "$DBPID"; RC=$?
echo "=== db_bench rc=$RC  (0 = completed, NO deadlock) ==="
echo "--- arbiter / grow log ---"
grep -E 'ZoneArbiter|online R-zone|manual grow|expansion|AcquireHold|ResetHeld|resize_cns|FAILED|error' /tmp/online_db.log | tail -20
echo "--- fillrandom ---"; grep -E '^fillrandom' /tmp/online_db.log | tail -1
echo "--- final r_end = $(rendz) zones ---"
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "ONLINE_TEST_DONE"
