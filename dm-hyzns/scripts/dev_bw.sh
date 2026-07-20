#!/bin/bash
#
# dev_bw.sh - raw bandwidth matrix for the FEMU HyZNS (geometry via GEOM env).
# Measures the device ceilings that bound mixed workloads and compares them with
# the FEMU latency model (hyzns.c).
#
#   Model: pg_wr=450us  pg_rd=80us  ch_xfer=25us  erase=2ms
#          NAND page=16KiB, lanes = nchs x ways (GEOM), page-idx round-robin striping
#   Theoretical ceilings (all verified against measurement, error <0.3%):
#     write peak  32 x 16KiB/450us               = 1165 MB/s  (NAND-bound; reached at bs<=508K)
#     read  peak  8ch x 16KiB/25us (channel-bound)= 5243 MB/s  (binds before the 6553 NAND bound)
#     4k-write    32 x  4KiB/450us               =  291 MB/s  (unmerged 4KiB pays the full page)
#     QD1 4k      450+25us + dm/nvme overhead    ~  ~2k IOPS  (measured 517us)
#   Caveat: kernel max_segments=127 -> requests >508KiB split at 508KiB (=31.75
#   NAND pages, unaligned!) -> straddle pages double-charged + lane phase drift ->
#   effective ceiling for 1MiB streams (e.g. ZenFS buffer flush) is ~583 MB/s
#   (= half the peak).
#
# Tests (fio, direct=1):
#   T1  S-region seq-write scaling (zonemode=zbd, bs=1M, jobs 1/2/4/8/14)
#   T2  R-region 4k randwrite QD1 / QD64
#   T3  R-region seq write bs=1M (r_coalesce path)
#   T4  S-region seq read bs=1M jobs 1/8
#   T5  mixed: S write (1M x4) + S randread (16k QD16) concurrently
#
# Usage:  sudo ./scripts/dev_bw.sh [backing-dev]     (default /dev/nvme0n1)
set -uo pipefail
cd "$(dirname "$0")/.."                                   # dm-hyzns/
source scripts/_lib.sh

DEV=${1:-/dev/nvme0n1}
DM=hyzns0
R_ZONES=${R_ZONES:-8}                                     # R 8GiB: headroom so T2/T3 run GC-free
GEOM=${GEOM:-8x8}                                         # nchs x ways (must match the FEMU launch)
IFS=x read -r _GC _GW <<< "$GEOM"; LANES=$(( _GC * _GW ))
TH_W=$(( LANES * 16384 * 1000 / 450 / 1000 ))             # write peak: lanes*16KiB/450us (MB/s)
TH_R=$(( _GC * 16384 * 1000 / 25 / 1000 ))                # read peak: ch-bound nchs*16KiB/25us
TH_4K=$(( LANES * 4096 * 1000 / 450 / 1000 ))             # unmerged 4k ceiling
ZS=$HYZNS_ZONE_SECTORS                                   # zone sectors (1GiB)
RT=${RT:-10}                                              # seconds per test
OUT=${OUT:-/tmp/dev_bw_$$}; mkdir -p "$OUT"

die(){ echo "FATAL: $*" >&2; cleanup; exit 1; }
cleanup(){ dmsetup remove -f "$DM" 2>/dev/null; }

# ---- guard: abort if another experiment holds the device ----------------
pgrep -x fio >/dev/null && die "fio already running"
pgrep -f 'db_bench' >/dev/null && die "db_bench running"

# ---- stack: full-R reset -> dm -> r_end ---------------------------------
# 496KiB (=31 NAND pages) aligned splitting: avoids the straddle double-charge
# of the unaligned 508KiB (nvme 127-seg) split. Must be set before creating
# the dm target so dm-0 inherits it.
echo 496 > "/sys/block/$(basename "$DEV")/queue/max_sectors_kb"
echo "== setup: $DEV R_ZONES=$R_ZONES GEOM=$GEOM (${LANES} lanes) max_sectors_kb=496"
umount /mnt/aux_cns/i1 /mnt/aux_cns/i2 /mnt/aux_cns 2>/dev/null
dmsetup remove -f "$DM" 2>/dev/null
reset_device_full_r "$DEV" >/dev/null 2>&1 || die "reset_device_full_r"
sz=$(blockdev --getsz "$DEV")
echo "0 $sz hyzns $DEV $((R_ZONES*ZS)) $sz" | dmsetup create "$DM" || die "dmsetup create"
dmsetup message "$DM" 0 set_r_end $((R_ZONES*ZS)) || die "set_r_end"
MAP=/dev/mapper/$DM
S_OFF_G=$R_ZONES                                          # S start (GiB = zone#)

reset_s(){ blkzone reset -o $((R_ZONES*ZS)) "$MAP" 2>/dev/null; }

# fio helper: extract write/read MB/s from the json output
bw(){ python3 -c "
import json,sys
j=json.load(open('$1'))
w=sum(x['write']['bw_bytes'] for x in j['jobs'])/1e6
r=sum(x['read']['bw_bytes'] for x in j['jobs'])/1e6
iops=sum(x['write']['iops']+x['read']['iops'] for x in j['jobs'])
print(f'{w:.0f} {r:.0f} {iops:.0f}')"; }

declare -A RES

# ---- T0: pure peak (bs=256k, below the max_segments=127 (508KiB) split) ----
# The kernel splits >508KiB requests at 127 segments (=508KiB), which is not
# NAND-page (16KiB) aligned (31.75 pages), so straddle pages get double-charged
# and lane phase drifts -> 1MiB streams degrade to ~583MB/s. The model peak
# itself shows at 256k.
for m in write read; do
    fio --name=peak --filename="$MAP" --direct=1 --rw=$m --bs=256k \
        --ioengine=io_uring --iodepth=8 --offset=1G --size=3G \
        --time_based --runtime=$RT --group_reporting \
        --output-format=json --output="$OUT/t0_$m.json" >/dev/null 2>&1 || die "T0 $m"
    read -r w r _ <<<"$(bw "$OUT/t0_$m.json")"
    [[ $m == write ]] && RES[t0w]=$w || RES[t0r]=$r
    echo "  T0 peak-$m   256k QD8: $([[ $m == write ]] && echo "$w" || echo "$r") MB/s"
done

# ---- T1: S seq-write scaling --------------------------------------------
for J in 1 2 4 8 14; do
    reset_s
    fio --name=swrite --filename="$MAP" --zonemode=zbd --direct=1 --rw=write \
        --bs=1M --ioengine=psync --numjobs=$J --offset=${S_OFF_G}G \
        --offset_increment=16G --size=16G --time_based --runtime=$RT \
        --group_reporting --output-format=json --output="$OUT/t1_$J.json" >/dev/null 2>&1 \
        || die "T1 j$J"
    read -r w _ _ <<<"$(bw "$OUT/t1_$J.json")"; RES[t1_$J]=$w
    echo "  T1 S-seq-write  jobs=$J : ${w} MB/s"
done

# ---- T2: R 4k randwrite QD1 / QD64 --------------------------------------
for QD in 1 64; do
    fio --name=r4k --filename="$MAP" --direct=1 --rw=randwrite --bs=4k \
        --ioengine=io_uring --iodepth=$QD --offset=0 --size=2G \
        --time_based --runtime=$RT --norandommap --group_reporting \
        --output-format=json --output="$OUT/t2_$QD.json" >/dev/null 2>&1 || die "T2 qd$QD"
    read -r w _ i <<<"$(bw "$OUT/t2_$QD.json")"; RES[t2_$QD]="$w"; RES[t2i_$QD]="$i"
    echo "  T2 R-4k-randwr  QD=$QD : ${w} MB/s (${i} IOPS)"
done

# ---- T3: R seq write 1M (coalesce path) ----------------------------------
fio --name=rseq --filename="$MAP" --direct=1 --rw=write --bs=1M \
    --ioengine=io_uring --iodepth=4 --offset=4G --size=3G \
    --time_based --runtime=$RT --group_reporting \
    --output-format=json --output="$OUT/t3.json" >/dev/null 2>&1 || die "T3"
read -r w _ _ <<<"$(bw "$OUT/t3.json")"; RES[t3]=$w
echo "  T3 R-seq-write  1M    : ${w} MB/s"

# ---- T4: S seq read (unwritten LBAs time the same; FEMU charges per LBA) --
for J in 1 8; do
    fio --name=sread --filename="$MAP" --direct=1 --rw=read --bs=1M \
        --ioengine=psync --numjobs=$J --offset=${S_OFF_G}G --offset_increment=16G \
        --size=16G --time_based --runtime=$RT --group_reporting \
        --output-format=json --output="$OUT/t4_$J.json" >/dev/null 2>&1 || die "T4 j$J"
    read -r _ r _ <<<"$(bw "$OUT/t4_$J.json")"; RES[t4_$J]=$r
    echo "  T4 S-seq-read   jobs=$J : ${r} MB/s"
done

# ---- T5: mixed - S write (1Mx4) + S randread (16k QD16) concurrently -----
reset_s
fio --output-format=json --output="$OUT/t5.json" >/dev/null 2>&1 \
    --name=mixw --filename="$MAP" --zonemode=zbd --direct=1 --rw=write --bs=1M \
      --ioengine=psync --numjobs=4 --offset=${S_OFF_G}G --offset_increment=16G \
      --size=16G --time_based --runtime=$RT \
    --name=mixr --filename="$MAP" --direct=1 --rw=randread --bs=16k \
      --ioengine=io_uring --iodepth=16 --offset=$((S_OFF_G+96))G --size=32G \
      --time_based --runtime=$RT --norandommap \
    || die "T5"
read -r w r _ <<<"$(bw "$OUT/t5.json")"; RES[t5w]=$w; RES[t5r]=$r
echo "  T5 mixed        write=${w} MB/s + read=${r} MB/s"

cleanup

# ---- summary: measured vs model ------------------------------------------
cat <<EOF

================ dev_bw results (raw: $OUT) ================
                                     measured(MB/s)   model(MB/s, GEOM=$GEOM)
PEAK write    256k QD8               ${RES[t0w]}       $TH_W (${LANES}lane x 16K/450us, NAND-bound)
PEAK read     256k QD8               ${RES[t0r]}       $TH_R (${_GC}ch x 16K/25us, channel-bound)
S seq write   1M jobs=1              ${RES[t1_1]}       ~$TH_W (with 496K-aligned splitting)
S seq write   1M jobs=14             ${RES[t1_14]}       ~$TH_W
R 4k randwr   QD1                    ${RES[t2_1]} (${RES[t2i_1]} IOPS)   ~2k IOPS (1 lane, 450+25us+oh)
R 4k randwr   QD64                   ${RES[t2_64]}        $TH_4K(unmerged)~(higher if dm merges)
R seq write   1M coalesce            ${RES[t3]}       ~$TH_W
S seq read    1M jobs=8              ${RES[t4_8]}       (below peak depending on splitting/poller)
mixed  W+R                           ${RES[t5w]} + ${RES[t5r]}   (mixed-workload reference)
Model constants (verified at 8x4): pg_wr=450.2us pg_rd=80.05us ch_xfer=25.06us
   (re-verify the stride fingerprint after a geometry change: same-lane period = lanes*16KiB stride)
==========================================================
EOF
