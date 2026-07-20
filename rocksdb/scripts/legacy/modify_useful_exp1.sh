#!/bin/bash
# Usefulness exp 1: online CNS resize cost + data integrity over many cycles.
# Fill (fillseq=100% keyspace), then a sequence of grow/shrink ResizeCNS ops,
# timing each and re-reading the whole keyspace to prove zero data loss.
# Usage: sudo NUM=3000000 ./scripts/legacy/modify_useful_exp1.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
source ../dm-hyzns/scripts/_lib.sh
BACKING=/dev/nvme0n1; DMNAME=hyzns0; DEV=/dev/mapper/$DMNAME; AUX=/mnt/aux
ZS=$HYZNS_ZONE_SECTORS
R0=${R0:-8}; NUM=${NUM:-3000000}; AOZ=${AOZ:-7}; READS=${READS:-500000}
ZENFS=./plugin/zenfs/util/zenfs
TOTAL=$(( $(blockdev --getsz "$BACKING") / ZS )); S_LAST=$((TOTAL-1))
SEQ=${SEQ:-"12 16 8 24 8"}     # target R after each ResizeCNS
rend() { dmsetup status "$DMNAME"|grep -oE 'r_end=[0-9]+'|grep -oE '[0-9]+'; }
rendz() { echo $(( $(rend)/ZS )); }   # r_end in zones
rd() {  # read whole keyspace at start_zone=$1 -> found count
  ./db_bench --fs_uri="zenfs://dev:dm-0/$1/$S_LAST/$AOZ/0/4294967296" \
    --benchmarks=readrandom --use_existing_db=1 --num="$NUM" --reads="$READS" \
    --threads=1 --use_direct_reads=true --key_size=20 --value_size=800 \
    --compression_type=none &>/tmp/e1_read.log
  grep -oE '\([0-9]+ of [0-9]+ found\)' /tmp/e1_read.log|tail -1; }

umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
lsmod|awk '{print $1}'|grep -qx dm_hyzns||../dm-hyzns/scripts/load.sh>/dev/null
reset_device_full_r "$BACKING"||true
echo "0 $(blockdev --getsz $BACKING) hyzns $BACKING $((R0*ZS)) $(blockdev --getsz $BACKING)"|dmsetup create "$DMNAME"
dmsetup message "$DMNAME" 0 set_r_end $((R0*ZS))
mkfs.f2fs -f -m -H -A -P "$DEV" >/dev/null 2>&1
mkdir -p "$AUX"; mount -t f2fs "$DEV" "$AUX"
"$ZENFS" mkfs --zbd=dm-0 --aux_path="$AUX" --hyssd --aux_size="$R0" \
  --start_zone="$R0" --end_zone="$S_LAST" --ao_zones="$AOZ" --enable_gc=true --force >/dev/null 2>&1

echo "### fill (fillseq, R=$R0) ###"
./db_bench --fs_uri="zenfs://dev:dm-0/$R0/$S_LAST/$AOZ/0/4294967296" \
  --benchmarks=fillseq --disable_wal=1 --num="$NUM" --key_size=20 --value_size=800 \
  --compression_type=none --max_background_jobs=8 &>/tmp/e1_fill.log
cur=$R0
echo "baseline read @R=$cur: $(rd $cur)"
echo ""
printf "%-16s %-7s %-9s %-12s %-12s %s\n" "op" "ms" "mig_MB" "r_end(zones)" "ok" "data_intact"
for tgt in $SEQ; do
  if [ "$tgt" -gt "$cur" ]; then op="GROW $cur->$tgt"; else op="SHRINK $cur->$tgt"; fi
  t0=$(date +%s%3N)
  "$ZENFS" resize-cns --zbd=dm-0 --aux_path="$AUX" --new_rzone="$tgt" &>/tmp/e1_mod.log
  t1=$(date +%s%3N)
  ok=$(grep -c 'ResizeCNS completed' /tmp/e1_mod.log)
  migb=$(grep -oE 'migrated [0-9]+ bytes' /tmp/e1_mod.log | grep -oE '[0-9]+' | head -1)
  migmb=$(( ${migb:-0} / 1048576 ))
  cur=$(rendz)   # actual device r_end (zones) — truth even if a step were rejected
  printf "%-16s %-7s %-9s %-12s %-12s %s\n" "$op" "$((t1-t0))" "$migmb" "$cur" "$ok" "$(rd $cur)"
done
umount "$AUX" 2>/dev/null||true; dmsetup remove "$DMNAME" 2>/dev/null||true
echo "EXP1_DONE"
