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
OSIMGF=${OSIMGF:-$IMGDIR/u20s.qcow2}

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

#-----------------------------------------------------------------------
# Configurable SSD Controller layout parameters (must be power of 2)
ssd_size_bb=131072 # in megabytes, if you change the above layout parameters, make sure you manually recalculate the ssd size and modify it here, please consider a default 25% overprovisioning ratio.
secsz_bb=512 # sector size in bytes
secs_per_pg_bb=8 # number of sectors in a flash page
pgs_per_blk_bb=512 # number of pages per flash block
blks_per_pl_bb=1024 # number of blocks per plane
pls_per_lun_bb=1 # keep it at one, no multiplanes support
luns_per_ch_bb=8 # number of chips per channel
nchs_bb=8 # number of channels

# Latency in nanoseconds
pg_rd_lat_bb=40000 # page read latency
pg_wr_lat_bb=200000 # page write latency
blk_er_lat_bb=2000000 # block erase latency
ch_xfer_lat_bb=0 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent_bb=75
gc_thres_pcent_high_bb=95

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS_BB="-device femu"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",devsz_mb=${ssd_size_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",namespaces=1"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",femu_mode=1"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",secsz=${secsz_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",secs_per_pg=${secs_per_pg_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pgs_per_blk=${pgs_per_blk_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",blks_per_pl=${blks_per_pl_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pls_per_lun=${pls_per_lun_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",luns_per_ch=${luns_per_ch_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",nchs=${nchs_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pg_rd_lat=${pg_rd_lat_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",pg_wr_lat=${pg_wr_lat_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",blk_er_lat=${blk_er_lat_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",ch_xfer_lat=${ch_xfer_lat_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",gc_thres_pcent=${gc_thres_pcent_bb}"
FEMU_OPTIONS_BB=${FEMU_OPTIONS_BB}",gc_thres_pcent_high=${gc_thres_pcent_high_bb}"
#-----------------------------------------------------------------------
# Zoned-Namespace (ZNS) SSDs Controller layout parameters
SSD_SIZE_MB=131072
NUM_CHANNELS=8
NUM_CHIPS_PER_CHANNEL=8
READ_LATENCY_NS=${pg_rd_lat_bb}
WRITE_LATENCY_NS=${pg_wr_lat_bb}

FEMU_OPTIONS_ZNS="-device femu"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",devsz_mb=${SSD_SIZE_MB}"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",namespaces=1"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",zns_num_ch=${NUM_CHANNELS}"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",zns_num_lun=${NUM_CHIPS_PER_CHANNEL}"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",zns_read=${READ_LATENCY_NS}"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",zns_write=${WRITE_LATENCY_NS}"
FEMU_OPTIONS_ZNS=${FEMU_OPTIONS_ZNS}",femu_mode=3"
#-----------------------------------------------------------------------
# Hybrid-SSD (HYSSD) SSDs Controller layout parameters
ssd_size_hy=131072     #16G 16384MB / 4GB 4096MB
secsz_hy=512         #128MiB per zone
secs_per_pg_hy=8
pgs_per_blk_hy=512   # does not affect the delay model
blks_per_pl_hy=1024    #Rzone+Szone lines
pls_per_lun_hy=1
luns_per_ch_hy=8
nchs_hy=8

# Latency in nanoseconds
pg_rd_lat_hy=${pg_rd_lat_bb} # page read latency
pg_wr_lat_hy=${pg_wr_lat_bb} # page write latency
blk_er_lat_hy=${blk_er_lat_bb} # block erase latency
ch_xfer_lat_hy=${ch_xfer_lat_bb} # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent_hy=${gc_thres_pcent_bb}
gc_thres_pcent_high_hy=${gc_thres_pcent_high_bb}

num_rzones=32   # must be ( 1 < x < blks_per_pl )

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
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pg_rd_lat=${pg_rd_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",pg_wr_lat=${pg_wr_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",blk_er_lat=${blk_er_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",ch_xfer_lat=${ch_xfer_lat_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",gc_thres_pcent=${gc_thres_pcent_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",gc_thres_pcent_high=${gc_thres_pcent_high_hy}"
FEMU_OPTIONS_HY=${FEMU_OPTIONS_HY}",num_rzones=${num_rzones}"
#-----------------------------------------------------------------------

echo ${FEMU_OPTIONS_BB}
echo ${FEMU_OPTIONS_ZNS}
echo ${FEMU_OPTIONS_HY}

source ./_femu_tune.sh
femu_cpu_plan

sudo ${FEMU_LAUNCH} \
    -name "FEMU-ALL-SSDs-VM",debug-threads=on \
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
    ${FEMU_OPTIONS_BB} \
    ${FEMU_OPTIONS_ZNS} \
    ${FEMU_OPTIONS_HY} \
    -net user,hostfwd=tcp::${SSH_HOSTFWD:-8080}-:22 \
    -net nic,model=virtio \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1
