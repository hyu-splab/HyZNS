#!/bin/bash
#
# run-zns-dual.sh — FEMU with TWO devices for Fig8 "zns-multi" validation:
#
#   /dev/nvme0n1 = plain ZNS  (femu_mode=3, 256 GB, 1 GiB zone -> 256 zones)
#   /dev/nvme1n1 = BBSSD      (femu_mode=1,  16 GiB)  -- conventional aux
#
# Mirrors the ZN540 multi setting (ZNS namespace + a separate conventional
# SSD as the ZenFS aux). The BBSSD plays the aux role. All NAND timing is
# ConfZNS 450/80/2000 us on BOTH devices so the ZNS-vs-aux comparison is on
# identical media (validation intent, matches run-hyhost-dual.sh).
#
# Counterpart of run-hyhost-dual.sh (which does hyzns-multi = HYHOSTSSD+BBSSD).
# Build with femu-compile.sh first.

# Image directory
# Per-machine paths go in ./femu-local.sh (untracked) — do not hard-code here.
[ -f ./femu-local.sh ] && . ./femu-local.sh
IMGDIR=${IMGDIR:-$HOME/images}
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
# Device 0: plain ZNS (femu_mode=3)  ->  /dev/nvme0n1
#   256 GB, zone = 1 GiB (NVME_DEFAULT_ZONE_SIZE in zns/zns.c) -> 256 zones.
#   ch/lun matched to HYHOSTSSD (32x16); ConfZNS 450/80/2000 us latency.
#
#   IMPORTANT: the active zns/zns.c reads ALL geometry+timing from bb_params
#   (zns_init_params: spp->nchs = n->bb_params.nchs, etc.). The zns_num_ch /
#   zns_read / ... properties feed only zns_small.c and are dead here. So the
#   ZNS device must be configured with the BBSSD-style params below.
#   Zone size is fixed at NVME_DEFAULT_ZONE_SIZE (1 GiB); devsz_mb sets the
#   namespace size -> num_zones = devsz / 1 GiB.
# ---------------------------------------------------------------------------
zns_size_mb=262144     # 256 GB -> 256 x 1 GiB zones

FEMU_OPTIONS_ZNS="-device femu"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",devsz_mb=${zns_size_mb}"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",namespaces=1"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",femu_mode=3"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",secsz=512"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",secs_per_pg=32"      # 16 KiB page (== HYHOSTSSD)
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",pgs_per_blk=128"     # 2 MiB block (== HYHOSTSSD)
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",luns_per_ch=16"      # == HYHOSTSSD
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",nchs=32"             # == HYHOSTSSD
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",pg_rd_lat=80000"     # ConfZNS 80 us
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",pg_wr_lat=450000"    # ConfZNS 450 us
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",blk_er_lat=2000000"  # 2 ms
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",ch_xfer_lat=25000"

# ---------------------------------------------------------------------------
# Device 1: BBSSD (femu_mode=1)  ->  /dev/nvme1n1   (conventional aux)
#   Geometry/timing matched to the ZNS device above (== HYHOSTSSD R-region),
#   so aux media == ZNS media.
#   OVERPROVISIONING (see run-hyhost-dual.sh for the full rationale): physical
#   capacity = geometry; devsz_mb = logical (host-visible). They must differ so
#   the FTL has spare blocks for GC. physical==logical (OP 0%) -> GC deadlock +
#   maptbl[] out-of-bounds at the last lpn (ftl_assert off by default) ->
#   silent heap corruption -> delayed qemu crash (suspected dual-crash cause).
#   Physical geometry (fixed at 16 GiB):
#     nchs*luns_per_ch*blks_per_pl*pls_per_lun*pgs_per_blk*secs_per_pg*secsz
#     = 32 * 16 * 16 * 1 * 128 * 32 * 512 = 16 GiB physical
#   devsz_mb = 12288 (12 GiB logical) -> ~25% OP.
# ---------------------------------------------------------------------------
bb_size=12288          # 12 GiB LOGICAL (host-visible). Physical=16 GiB -> ~25% OP.
secsz=512
secs_per_pg=32         # 16 KiB page
pgs_per_blk=128        # 2 MiB erase block
blks_per_pl=16         # -> 16 GiB PHYSICAL (NOT logical)
pls_per_lun=1
luns_per_ch=16
nchs=32

pg_rd_lat=80000        # ConfZNS 80 us
pg_wr_lat=450000       # ConfZNS 450 us
blk_er_lat=2000000     # 2 ms
ch_xfer_lat=25000

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

echo "ZNS: ${FEMU_OPTIONS_ZNS}"
echo "BB : ${FEMU_OPTIONS_BB}"

source ./_femu_tune.sh
femu_cpu_plan

sudo ${FEMU_LAUNCH} \
    -name "FEMU-ZNS-DUAL-VM",debug-threads=on \
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
    ${FEMU_OPTIONS_ZNS} \
    ${FEMU_OPTIONS_BB} \
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee zns_dual_debug.log
