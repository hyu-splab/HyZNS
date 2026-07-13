// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <linux/ioctl.h>
#include <linux/types.h>

// F2FS ioctl definitions for zone management
#define F2FS_IOCTL_MAGIC 0xf5

#define F2FS_IOC_RESIZE_CNS _IOW(F2FS_IOCTL_MAGIC, 26, __u32)
#define F2FS_IOC_GARBAGE_COLLECT_FORCE \
  _IOW(F2FS_IOCTL_MAGIC, 27, struct f2fs_gc_range)
#define F2FS_IOC_GC_FORCE_N_MODIFY \
  _IOW(F2FS_IOCTL_MAGIC, 28, struct f2fs_gc_range)

struct f2fs_gc_range {
  __u32 sync;
  __u64 start;
  __u64 len;
  __u32 force;
};

// F2FS zone size in 4KB blocks. Must match the device zone/line size.
// Current HyZNS geometry: 1 GiB zone = 262144 x 4KB blocks.
#define F2FS_ZONE_BLOCKS 262144
// 256 MiB: 65536, 128 MiB: 32768