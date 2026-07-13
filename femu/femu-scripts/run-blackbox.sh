#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)

# Image directory
# Per-machine paths go in ./femu-local.sh (untracked) — do not hard-code here.
[ -f ./femu-local.sh ] && . ./femu-local.sh
IMGDIR=${IMGDIR:-$HOME/images}
# Virtual machine disk image
EMTYF=${EMTYF:-$IMGDIR/disk.img}
EMTYF2=${EMTYF2:-$IMGDIR/disk2.img}
OSIMGF=${OSIMGF:-$IMGDIR/u20s.qcow2}

# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=32 # number of sectors in a flash page(16K Page)
pgs_per_blk=128 # number of pages per flash block(2M Block)
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=16 # number of chips per channel
nchs=32 # number of channels

ssd_size=65536 #16G 16384MB / 4GB 4096MB / 64G 65536 / 128G 131072 / 256G 262144
blks_per_pl=64

chs_per_line=32    # channels per line (16 of 32 total)
luns_per_line=16    # LUNs per line (4 of 16 total) → 64 blks/line = 128MiB line

# Latency in nanoseconds 
#pg_rd_lat=80000 # page read latency 40000 // NAND_READ_LATENCY  = 65000,  //65us TLC_tREAD(65us : 16K page time)
#pg_wr_lat=650000 # page write latency 200000 // 450us TLC_tProg ,3D time
#blk_er_lat=2000000 # block erase latency2000000 //
#ch_xfer_lat=25000 # channel transfer time, ignored for now

pg_rd_lat=65000 # page read latency 40000 // NAND_READ_LATENCY  = 65000,  //65us TLC_tREAD(65us : 16K page time)
pg_wr_lat=400000 # page write latency 200000 // 450us TLC_tProg ,3D time
blk_er_lat=2000000 # block erase latency2000000 //
ch_xfer_lat=25000 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

#-----------------------------------------------------------------------

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"

echo ${FEMU_OPTIONS}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

source ./_femu_tune.sh
femu_cpu_plan

sudo ${FEMU_LAUNCH} \
    -name "FEMU-HYSSD-VM",debug-threads=on \
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
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee bbssd_debug.log
