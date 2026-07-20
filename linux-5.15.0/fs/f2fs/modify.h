/*
 * fs/f2fs/modify.c
 *
 * Copyright (c) 2025 Software Platforms Lab.,
 *             https://splab.hanyang.ac.kr/
 */

#ifndef _LINUX_F2FS_MODIFY_H
#define _LINUX_F2FS_MODIFY_H

#include "f2fs.h"

int f2fs_modify_zone(struct f2fs_sb_info *sbi, __u32 new_rzone);
int f2fs_update_super_info(struct f2fs_sb_info *sbi, __u32 new_rzone);

/* ZenFS aux max-provision (F2FS_FEATURE_AUX_MAXPROV) helpers */
int f2fs_aux_gate_to_aba(struct f2fs_sb_info *sbi);
int f2fs_aux_modify(struct f2fs_sb_info *sbi, __u32 new_rzone);

/* Direction-aware online resize: shrink gates the FS usable area down before
 * the device ABA change; grow changes the device ABA first. Single ioctl,
 * race-free. */
int f2fs_resize_cns(struct f2fs_sb_info *sbi, __u32 new_rzone);

#endif /* _LINUX_F2FS_MODIFY_H */