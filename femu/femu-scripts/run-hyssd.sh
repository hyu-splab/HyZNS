#!/bin/bash
#
# Doeun Kim <doeun96@hanyang.ac.kr>
# Run FEMU as Hybrid-SSD (HYSSD)
#

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

# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=8 # number of sectors in a flash page(16K Page)
pgs_per_blk=512 # number of pages per flash block(2M Block)
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=16 # number of chips per channel
nchs=32 # number of channels

chs_per_line_hy=16    # channels per line (16 of 32 total)
luns_per_line_hy=4    # LUNs per line (4 of 16 total) → 64 blks/line = 128MiB line

#Latency in nanoseconds 
#pg_rd_lat=80000 # page read latency 40000 // NAND_READ_LATENCY  = 65000,  //65us TLC_tREAD(65us : 16K page time)
#pg_wr_lat=650000 # page write latency 200000 // 450us TLC_tProg ,3D time
#blk_er_lat=2000000 # block erase latency2000000 //
#ch_xfer_lat=25000 # channel transfer time, ignored for now

#pg_rd_lat=65000 # page read latency 40000 // NAND_READ_LATENCY  = 65000,  //65us TLC_tREAD(65us : 16K page time)
pg_rd_lat=80000 # page read latency 40000 // NAND_READ_LATENCY  = 65000,  //65us TLC_tREAD(65us : 16K page time)
pg_wr_lat=400000 # page write latency 200000 // 450us TLC_tProg ,3D time
blk_er_lat=2000000 # block erase latency2000000 //
ch_xfer_lat=25000 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=75
gc_thres_pcent_high=95

ssd_size_bb=3072 #4096 * 0.75 / 4G(3G)
blks_per_pl_bb=16

FEMU_OPTIONS_BB="-device femu"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",devsz_mb=${ssd_size_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",namespaces=1"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",femu_mode=1"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",secsz=${secsz}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",blks_per_pl=${blks_per_pl_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",nchs=${nchs}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",gc_thres_pcent_high=${gc_thres_pcent_high}"

#ssd_size_hy=65536     #16G 16384MB / 4GB 4096MB / 64G 65536 / 256G 262144
#blks_per_pl_hy=512     #Rzone+Szone lines 256G

#ssd_size_hy=131072     #16G 16384MB / 4GB 4096MB / 64G 65536 / 128G 131072 / 256G 262144
#blks_per_pl_hy=512     # Rzone+Szone lines 256G

ssd_size_hy=262144     #16G 16384MB / 4GB 4096MB / 64G 65536 / 128G 131072 / 256G 262144
blks_per_pl_hy=256     # zs:bpp = 1GB->256 / 256MB->1024

num_op_line=2   # not used
num_rzones=4 # 256M * 16 = 4G / 1024M * 4 = 4GB

FEMU_OPTIONS_HY="-device femu"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",devsz_mb=${ssd_size_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",namespaces=1"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",femu_mode=4"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",secsz=${secsz}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",blks_per_pl=${blks_per_pl_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",nchs=${nchs}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",num_op_line=${num_op_line}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",num_rzones=${num_rzones}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",chs_per_line=${chs_per_line_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",luns_per_line=${luns_per_line_hy}"


echo ${FEMU_OPTIONS_HY}

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
    ${FEMU_OPTIONS_HY} \
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee hyssd_debug.log
    # 2>"/data/log/error_hy.log"



    #${FEMU_OPTIONS_BB} \
    #-monitor stdio \
    #-serial pty \
    #-gdb tcp::5678,server,nowait \
    #-qmp unix:./qmp-sock,server,nowait 2>&1
