#!/bin/bash
#
# Run FEMU with TWO devices for the CNS-faithfulness check:
#
#   /dev/nvme0n1 = HYZNS (femu_mode=5, 256 GiB)  -- host-managed hybrid FTL
#   /dev/nvme1n1 = BBSSD     (femu_mode=1,   8 GiB)  -- device-managed page FTL
#
# Goal: isolate whether the WAL->R(CNS) throughput gap vs real ZN540 is the
# dm-hyzns host-FTL (out-of-place + GC) overhead or just FEMU's NAND timing.
# To make that comparison valid, the BBSSD's NAND timing AND geometry are set
# IDENTICAL to the HYZNS R-region (same ch x lun, page size, and per-op
# latencies). Any throughput delta is then host-FTL vs device-FTL, not media.
#
# HYZNS R-region geometry/timing baked in hyzns/hyzns.c:
#   nchs=32  luns_per_ch=16  page=16 KiB  pgs_per_blk=128 (2 MiB block)
#   pg_rd_lat=80us  pg_wr_lat=450us  blk_er_lat=2ms  ch_xfer_lat=25us
#
# Build with femu-compile.sh first (never raw make).

# Image directory
# Per-machine paths go in ./femu-local.sh (untracked) — do not hard-code here.
[ -f ./femu-local.sh ] && . ./femu-local.sh
IMGDIR=${IMGDIR:-$HOME/images}
# Virtual machine disk image
EMTYF=${EMTYF:-$IMGDIR/disk.img}
EMTYF2=${EMTYF2:-$IMGDIR/disk2.img}
OSIMGF=${OSIMGF:-$IMGDIR/u20s.qcow2}

if [[ ! -e "$OSIMGF" ]]; then
    echo ""
    echo "VM disk image couldn't be found ..."
    echo "Please prepare a usable VM image and place it as $OSIMGF"
    echo "Once VM disk image is ready, please rerun this script again"
    echo ""
    exit
fi

# ---------------------------------------------------------------------------
# Device 0: HYZNS (femu_mode=5)  ->  /dev/nvme0n1
# ---------------------------------------------------------------------------
ssd_size_hh=262144     # 16G=16384 / 64G=65536 / 128G=131072 / 256G=262144  (FigB dual: 256 GiB = 256 zones)

FEMU_OPTIONS_HH="-device femu"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",devsz_mb=${ssd_size_hh}"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",namespaces=1"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",femu_mode=5"

# ---------------------------------------------------------------------------
# Device 1: BBSSD (femu_mode=1)  ->  /dev/nvme1n1
#   Geometry/timing matched to HYZNS R-region (see header).
#   OVERPROVISIONING: BBSSD physical capacity comes from the geometry below;
#   devsz_mb is the *logical* (host-visible) size. They must NOT be equal —
#   the device needs spare physical blocks for GC. With physical==logical
#   (OP 0%) the FTL has no free line to reclaim: GC can deadlock, and a write
#   to the last lpn indexes maptbl[] out of bounds (ftl_assert is compiled
#   out by default -> silent heap corruption -> delayed qemu crash). This is
#   the suspected cause of the dual hard-crash during mkfs.
#   Physical geometry (fixed at 16 GiB):
#     nchs * luns_per_ch * blks_per_pl * pls_per_lun
#         * pgs_per_blk * secs_per_pg * secsz
#     = 32 * 16 * 16 * 1 * 128 * 32 * 512 = 16 GiB physical
#   devsz_mb = 12288 (12 GiB logical) -> ~25% overprovisioning (matches the
#   reference run-blackbox.sh convention).
# ---------------------------------------------------------------------------
bb_size=12288          # 12 GiB LOGICAL (host-visible). Physical=16 GiB -> ~25% OP.
secsz=512              # sector size (bytes)
secs_per_pg=32         # 16 KiB page  (== HYZNS HYZNS_DEFAULT_PAGE_BYTES)
pgs_per_blk=128        # 2 MiB erase block  (== HYZNS_DEFAULT_PGS_PER_BLK)
blks_per_pl=16         # -> 16 GiB PHYSICAL at the geometry above (NOT logical)
pls_per_lun=1          # no multiplane (HYZNS has none)
luns_per_ch=16         # == HYZNS_DEFAULT_LUNS_PER_CH
nchs=32               # == HYZNS_DEFAULT_NCHS

pg_rd_lat=80000        # == HYZNS_PG_RD_LAT_NS
pg_wr_lat=450000       # == HYZNS_PG_WR_LAT_NS
blk_er_lat=2000000     # == HYZNS_BLK_ER_LAT_NS
ch_xfer_lat=25000      # == HYZNS_CH_XFER_LAT_NS  (BBSSD default is 0; matched on purpose)

gc_thres_pcent=75
gc_thres_pcent_high=95

FEMU_OPTIONS_BB="-device femu"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",devsz_mb=${bb_size}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",namespaces=1"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",femu_mode=1"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",secsz=${secsz}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",nchs=${nchs}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",gc_thres_pcent_high=${gc_thres_pcent_high}"

echo "HH: ${FEMU_OPTIONS_HH}"
echo "BB: ${FEMU_OPTIONS_BB}"

source ./_femu_tune.sh
femu_cpu_plan

sudo ${FEMU_LAUNCH} \
    -name "FEMU-HYZNS-DUAL-VM",debug-threads=on \
    -enable-kvm \
    -cpu host \
    -smp ${FEMU_SMP} \
    -m 32G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    -device virtio-scsi-pci,id=scsi1 \
    -device scsi-hd,drive=hd1 \
    -drive file=$EMTYF,if=none,aio=native,cache=none,id=hd1 \
    -device virtio-scsi-pci,id=scsi2 \
    -device scsi-hd,drive=hd2 \
    -drive file=$EMTYF2,if=none,aio=native,cache=none,id=hd2 \
    ${FEMU_OPTIONS_HH} \
    ${FEMU_OPTIONS_BB} \
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee hyzns_dual_debug.log
