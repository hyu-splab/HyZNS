#!/bin/bash

mkfs.f2fs -f /dev/dm-0
mount /dev/dm-0 /mnt/zns

echo "root@fvm:~# lsblk"
(lsblk -o NAME,MAJ:MIN,RM,SIZE,RO,TYPE,MOUNTPOINT | head -n 1 && lsblk -o NAME,MAJ:MIN,RM,SIZE,RO,TYPE,MOUNTPOINT | grep -E 'nvme1n1|dmzap|nvme2n1')
echo "root@fvm:~# df -h"
(df -h | head -n 1 && df -h | grep -E '/dev/mapper/|/dev/nvme2n1')
