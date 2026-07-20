#!/bin/bash
#
# Run FEMU as a Host-managed Hybrid SSD (HYZNS, femu_mode=5).
#
# VM/host plumbing (KVM, multi-disk, qmp,
# tee log). NAND geometry (nchs, luns_per_ch, page/block size) and the
# logical zone size are passed as device options below; latency is still
# baked into hyzns/hyzns.c.

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

# Device sizing (256 GiB default); tweak as needed.
ssd_size_hh=262144              # 16G=16384 / 64G=65536 / 96G = 98304 / 128G=131072 / 256G=262144

# HYZNS NAND geometry. line = nchs*luns_per_ch*pgs_per_blk*page = the
# device erase / host GC unit; zone = the logical unit F2FS/ZenFS see and the
# granularity r_end (ABA) moves on. Defaults below: 8ch x 4way x 128 pg/blk x
# 16 KiB NAND page => 2 MiB block, 64 MiB line, 1 GiB zone (= 16 lines).
# NOTE: nchs/luns_per_ch must equal dm-hyzns's HYZNS_NCHS/HYZNS_LUNS_PER_CH.
# The block size must match too, but the units differ: FEMU counts pgs_per_blk
# in 16 KiB NAND pages (128), dm counts it in 4 KiB host pages (512) — both are
# 2 MiB. So block_bytes = hh_pgs_per_blk*hh_page_bytes here must equal dm's
# HYZNS_PGS_PER_BLK*4 KiB, or the (ch,lun) timing model desyncs.
hh_nchs=8
hh_luns_per_ch=4
hh_page_bytes=16384
hh_pgs_per_blk=128
hh_zone_size_mb=1024

FEMU_OPTIONS_HH="-device femu"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",devsz_mb=${ssd_size_hh}"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",namespaces=1"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",femu_mode=5"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",hyzns_nchs=${hh_nchs}"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",hyzns_luns_per_ch=${hh_luns_per_ch}"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",hyzns_page_bytes=${hh_page_bytes}"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",hyzns_pgs_per_blk=${hh_pgs_per_blk}"
FEMU_OPTIONS_HH=${FEMU_OPTIONS_HH}",hyzns_zone_size_mb=${hh_zone_size_mb}"

echo ${FEMU_OPTIONS_HH}

source ./_femu_tune.sh
femu_cpu_plan

sudo ${FEMU_LAUNCH} \
    -name "FEMU-HYZNS-VM",debug-threads=on \
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
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee hyzns_debug.log
