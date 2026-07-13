#!/bin/bash
#
# Run FEMU as Hybrid-SSD (HYSSD) - Large Configuration
# 64GB device with 1GB zones, 8ch×8way (full parallelism)
#

# Image directory
# Per-machine paths go in ./femu-local.sh (untracked) — do not hard-code here.
[ -f ./femu-local.sh ] && . ./femu-local.sh
IMGDIR=${IMGDIR:-$HOME/images}
# Virtual machine disk image
EMTYF=${EMTYF:-$IMGDIR/disk.img}
OSIMGF=${OSIMGF:-$IMGDIR/u20s.qcow2}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

# Device Configuration: 64GB device with LARGE zones (1GB)
# Physical device spec (FIXED for both large and small):
#   - 8 channels, 8 LUNs per channel
#   - 512 blocks per plane (per LUN)
#   - Block size = 2MB = 512B × 8 secs/pg × 512 pgs/blk
#   - Total capacity = 8ch × 8lun × 512 blks × 2MB = 64GB
#
# Large zone configuration (8ch×8way - uses ALL positions):
#   - Each zone uses all 64 positions (8ch × 8lun)
#   - Each position contributes 8 blocks per zone
#   - Blocks per zone = 64 positions × 8 blocks/pos = 512 blocks
#   - Zone size = 512 blocks × 2MB = 1GB
#   - Total zones = 512 blocks/lun ÷ 8 blocks/zone = 64 zones

ssd_size_hy=65536        # 64GB = 65536MB
secsz_hy=512             # Sector size: 512 bytes (FIXED)
secs_per_pg_hy=8         # Sectors per page: 8 (FIXED)
pgs_per_blk_hy=512       # Pages per block: 512 (FIXED, gives 2MB block)
blks_per_pl_hy=512       # Blocks per plane: 512 (FIXED for all configs)
pls_per_lun_hy=1         # Planes per LUN: 1
luns_per_ch_hy=8         # LUNs per channel: 8
nchs_hy=8                # Channels: 8
chs_per_line_hy=8        # Channels per line: 8 (full parallelism)
luns_per_line_hy=8       # LUNs per line: 8 (full parallelism)

# Latency in nanoseconds
pg_rd_lat_hy=40000       # page read latency
pg_wr_lat_hy=200000      # page write latency
blk_er_lat_hy=2000000    # block erase latency
ch_xfer_lat_hy=0         # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent_hy=75
gc_thres_pcent_high_hy=95

num_op_line=0            # not used
num_rzones=10            # must be ( 1 < x < blks_per_pl )

FEMU_OPTIONS_HY="-device femu"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",devsz_mb=${ssd_size_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",namespaces=1"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",femu_mode=4"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",secsz=${secsz_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",secs_per_pg=${secs_per_pg_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pgs_per_blk=${pgs_per_blk_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",blks_per_pl=${blks_per_pl_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pls_per_lun=${pls_per_lun_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",luns_per_ch=${luns_per_ch_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",nchs=${nchs_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",chs_per_line=${chs_per_line_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",luns_per_line=${luns_per_line_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pg_rd_lat=${pg_rd_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pg_wr_lat=${pg_wr_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",blk_er_lat=${blk_er_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",ch_xfer_lat=${ch_xfer_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",gc_thres_pcent=${gc_thres_pcent_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",gc_thres_pcent_high=${gc_thres_pcent_high_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",num_rzones=${num_rzones}"

echo "========== HYSSD Large Configuration =========="
echo "Device size: 64GB"
echo "Block size: 2MB (fixed: 512B × 8 × 512)"
echo "Zone size: 1GB (512 blocks per zone)"
echo "Parallelism: 8ch×8way (8 blocks/way)"
echo "Zone count: 64 zones"
echo "================================================"
echo ${FEMU_OPTIONS_HY}

source ./_femu_tune.sh
femu_cpu_plan

sudo ${FEMU_LAUNCH} \
    -name "FEMU-HYSSD-LARGE-VM",debug-threads=on \
    -enable-kvm \
    -cpu host \
    -smp ${FEMU_SMP} \
    -m 8G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    -device virtio-scsi-pci,id=scsi1 \
    -device scsi-hd,drive=hd1 \
    -drive file=$EMTYF,if=none,aio=native,cache=none,id=hd1 \
    ${FEMU_OPTIONS_HY} \
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock-large,server,nowait 2>&1
