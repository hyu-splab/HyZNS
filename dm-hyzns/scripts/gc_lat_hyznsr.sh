#!/bin/bash
# gc_lat_hyznsr.sh - measure dm-hyzns host-FTL line-GC relocation latency.
# Create garbage in R (fill + random-overwrite -> partial-valid lines), force a
# GC sweep, read per-line "dm-hyzns: GC line=.. migrated_pages=.. gc_wall_ns=.."
# (pr_info -> dmesg). throughput = migrated*4KiB / gc_wall_ns. Same FEMU R 450µs
# media as BBSSD -> compares host-FTL GC vs BBSSD device-FTL GC.
set -uo pipefail
cd "$(dirname "$0")/.."
source scripts/_lib.sh; ZS=$HYZNS_ZONE_SECTORS
BACK=/dev/nvme0n1; NAME=hyzns0; DEV=/dev/mapper/$NAME
RZ=${RZ:-4}; FILL=${FILL:-3G}; OW=${OW:-2G}
OUT="$(cd "$(dirname "$0")/../.." && pwd)/hyznsd/results/gc_dev_compare"; mkdir -p "$OUT"; STAMP=$(date +%Y%m%d_%H%M%S); RES=$OUT/hyznsr_gc_$STAMP.txt
dm_f(){ dmsetup status "$NAME" 2>/dev/null|grep -oE "$1=[0-9]+"|head -1|cut -d= -f2; }
cleanup(){ dmsetup remove "$NAME" 2>/dev/null||true; }
trap cleanup EXIT

dmsetup remove "$NAME" 2>/dev/null||true
lsmod|awk '{print $1}'|grep -qx dm_hyzns || ./scripts/load.sh >/dev/null 2>&1
reset_device_full_r "$BACK" >/dev/null 2>&1||true
echo "0 $((RZ*ZS)) hyzns $BACK $((RZ*ZS))" | dmsetup create "$NAME" || { echo "dm create fail"|tee $RES; exit 1; }
dmsetup message "$NAME" 0 set_r_end $((RZ*ZS))
echo "hyznsr_gc $STAMP: R=$RZ zones, fill=$FILL ow=$OW (FEMU R 450us media)" | tee $RES

# clear dmesg BEFORE the workload so the background GC's per-line logs (which fire
# DURING the overwrite as free_lines drops) are captured - not wiped after the fact.
dmesg -C 2>/dev/null||true
# garbage: seq fill (parallel for speed) then random 4k overwrite -> partial-valid lines
fio --name=fill --rw=write --bs=256k --size=$FILL --filename=$DEV --direct=1 --iodepth=32 --ioengine=libaio >/dev/null 2>&1
fio --name=ow --rw=randwrite --bs=4k --size=$FILL --io_size=$OW --filename=$DEV --direct=1 --iodepth=32 --ioengine=libaio --randseed=11 >/dev/null 2>&1
echo "  pre-forceGC: free_lines=$(dm_f free_lines) valid_pages=$(dm_f valid_pages) gc_mig=$(dm_f gc_mig)" | tee -a $RES

# also force a sweep (covers any remaining victims); background GC may have already run
t0=$(date +%s.%N)
dmsetup message "$NAME" 0 gc
t1=$(date +%s.%N)
echo "  message gc wall=$(awk "BEGIN{printf \"%.1f\",$t1-$t0}")s  post free_lines=$(dm_f free_lines) gc_mig=$(dm_f gc_mig)" | tee -a $RES
echo "=== per-line GC log (dm-hyzns) ===" | tee -a $RES
dmesg | grep "dm-hyzns: GC line" | tee -a $RES
echo "=== throughput per line (migrated*4KiB / gc_wall_ns) ===" | tee -a $RES
dmesg | grep "dm-hyzns: GC line" | awk '{
  for(i=1;i<=NF;i++){ if($i ~ /migrated_pages=/){split($i,a,"=");m=a[2]} if($i ~ /gc_wall_ns=/){split($i,b,"=");ns=b[2]} }
  if(ns>0){ mib=m*4096/1048576; printf "  line: %d pages, %.2f ms, %.1f MiB/s\n", m, ns/1e6, mib/(ns/1e9) }
}' | tee -a $RES
echo "file: $RES"
