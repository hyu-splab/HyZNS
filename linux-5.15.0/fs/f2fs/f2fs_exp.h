/*
 * fs/f2fs/
 *
 * Copyright (c) 2025 Software Platforms Lab.,
 *             https://splab.hanyang.ac.kr/
 */

#ifndef _LINUX_F2FS_EXP_H
#define _LINUX_F2FS_EXP_H

#include "f2fs.h"

#define DEBUG_GC 1

//super.c
int init_exp_cnt_cmd(struct f2fs_sb_info *sbi); 
int print_exp_cnt_cmd(struct f2fs_sb_info *sbi);
int print_heat_level_distribution(struct f2fs_sb_info *sbi);

//gc.c
void calclock(struct timespec64 *ts, unsigned long long *time, unsigned long long *count);

/* ResizeCNS instrumentation (gc.c): bracket one resize so per-function
 * timing + work accounting is collected and a summary is printed. */
void f2fs_rz_begin(bool shrink, unsigned int secs);
void f2fs_rz_end(void);
u64 f2fs_rz_now(void);
void f2fs_rz_dev_aba(u64 ns);

/* L0-on-CNS discard/invalidate tracing (segment.c). Counts the L0 lifecycle on
 * the aux F2FS: block invalidation, add_discard_addrs decisions (why discard is
 * or isn't issued, incl. partial-valid), prefree transitions, and actual issue.
 * Printed as one summary line per checkpoint. Gate: F2FS_L0DBG. */
#define F2FS_L0DBG 1
struct f2fs_l0dbg {
	u64 inval_blks;          /* f2fs_invalidate_blocks() calls */
	u64 dadd_calls;          /* add_discard_addrs() entered */
	u64 dadd_skip_fullvalid; /* skipped: segment fully valid */
	u64 dadd_skip_norealtime;/* skipped: realtime discard disabled */
	u64 dadd_skip_novalid;   /* skipped: segment has 0 valid (handled as prefree) */
	u64 dadd_skip_full;      /* skipped: nr_discards >= max_discards */
	u64 dadd_cand_blks;      /* discard candidate blocks actually queued */
	u64 dadd_partial_segs;   /* segments that were partial-valid at add_discard time */
	u64 prefree_made;        /* segments moved dirty->prefree */
	u64 disc_issued_blks;    /* blocks actually issued as discard to the device */
};
extern struct f2fs_l0dbg g_l0dbg;
void f2fs_l0dbg_cp_summary(struct f2fs_sb_info *sbi);  /* call at checkpoint */

#endif /* _LINUX_F2FS_EXP_H */