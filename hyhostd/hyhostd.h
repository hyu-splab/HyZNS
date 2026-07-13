/* hyhostd - HyZNS CNS-area autoscaling daemon.
 *
 * Userspace policy daemon for paper §5.6.3 "File System-triggered CNS Area
 * Resizing". Periodically reads the free capacity of the CNS area (R, from
 * dm-hyhost) and the ZNS area (S, from the zone-aware FS or `nvme report-zones`)
 * and triggers an absolute-target ResizeCNS when policy fires. The FS provides
 * the Grow/Shrink mechanism; this daemon owns the policy. See DESIGN.md.
 *
 * All capacities are in ZONES. A zone (1 GiB, the F2FS/ZenFS logical unit and
 * r_end granularity) is 16 dm-hyhost lines (a line is the 64-MiB GC unit), so
 * zone sectors = zone_pblocks*8, NOT line_pblocks*8. dm-hyhost emits both
 * `line_pblocks` and `zone_pblocks`; always convert with zone_pblocks. Older
 * dm builds omit zone_pblocks (there line == zone), hence the fallback to
 * line_pblocks. The wire stays standard NVMe ZNS.
 */
#ifndef HYHOSTD_H
#define HYHOSTD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HYHOSTD_VERSION "0.1"

/* ---- contract: dm-hyhost status keys (dmsetup status <dm>) ---------------- */
#define DM_KEY_R_END        "r_end"        /* current ABA, 512B sectors        */
#define DM_KEY_LINE_PBLOCKS "line_pblocks" /* pages per LINE (64 MiB GC unit)   */
#define DM_KEY_ZONE_PBLOCKS "zone_pblocks" /* pages per ZONE (1 GiB); *8 = zone sectors */
#define DM_KEY_FREE_LINES   "free_lines"   /* free allocator LINES (64 MiB ea)  */
#define DM_KEY_VALID_PAGES  "valid_pages"  /* live-mapped pages in R (L2P)     */
#define DM_KEY_BEST_VICTIM_VPC "best_victim_vpc" /* greedy victim valid%% (GC cost) */
#define DM_KEY_FREE_PAGES   "free_pages"   /* R_C = real free blocks = R - valid - invalid */
#define DM_KEY_GC_BLOCKED   "gc_blocked"   /* dm GC parked (no useful victim) */
#define DM_KEY_NOSPC        "nospc"        /* out-of-space write failures      */

#define HYHOST_PAGE_SECTORS 8u             /* 4 KiB page in 512B sectors       */

/* ---- contract: ZenFS publisher + resize control files (under aux_path) ---- */
#define ZENFS_STATUS_FILE   ".hyzns_status"      /* ZenFS writes; daemon reads */
#define ZENFS_RESIZE_REQ    ".hyzns_resize"      /* daemon writes absolute tgt */
#define ZENFS_RESIZE_ACK    ".hyzns_resize.ack"  /* ZenFS writes "<R> OK|EIO|BUSY" */

/* ---- R_Z source selector ------------------------------------------------- */
typedef enum { RZ_FS = 0, RZ_REPORT = 1 } rz_source_t;

/* ---- target FS ----------------------------------------------------------- */
typedef enum { FS_F2FS = 0, FS_ZENFS = 1 } fs_kind_t;

/* ---- daemon configuration (file + CLI overrides) ------------------------- */
typedef struct {
	char        dm[64];          /* dm-hyhost target name (e.g. hyhost0)     */
	char        backing[128];    /* backing namespace (e.g. /dev/nvme0n1)    */
	char        mnt[256];        /* FS mount point (F2FS) - resize fd source */
	char        aux[256];        /* aux/control dir (ZenFS status+resize)    */
	char        f2fs_io[256];    /* f2fs_io binary (patched w/ resize_cns)   */
	fs_kind_t   fs;              /* f2fs | zenfs                             */
	rz_source_t rz_source;       /* fs | report                             */

	uint32_t    poll_ms;         /* monitor period (idle)                   */
	uint32_t    poll_min_ms;     /* monitor period under pressure (R_C low)  */

	/* FTR policy thresholds (paper §5.6.3; the only policy). R_C = dm
	 * free_pages (blocks), R_Z = remaining ZNS zones, S_Z = zone size:
	 *   grow   +1 iff R_C < ftr_grow_rc*S_Z   && R_Z >= ftr_grow_rz
	 *   shrink -1 iff R_C > ftr_shrink_rc*S_Z && R_Z <  ftr_shrink_rz
	 * Paper defaults 2/5/3/4; "the conditions can be configured by the
	 * administrator". ------------------------------------------------------ */
	uint32_t    ftr_grow_rc;     /* FTR grow R_C threshold, in zones (paper 2)   */
	uint32_t    ftr_grow_rz;     /* FTR grow R_Z guard, in zones (paper 5)       */
	uint32_t    ftr_shrink_rc;   /* FTR shrink R_C threshold, in zones (paper 3) */
	uint32_t    ftr_shrink_rz;   /* FTR shrink R_Z bound, in zones (paper 4)     */
	uint32_t    ftr_gcu_resize;  /* 1: gc_urgent ON while a grow/shrink fires    *
	                              * (paper: flush delayed discards before        *
	                              * ResizeCNS). 0: never.                        */
	uint32_t    ftr_gcu_park;    /* 1: gc_urgent ON while dm GC is parked        *
	                              * (victims ~fully valid -> force F2FS GC +     *
	                              * discard). 0: off (paper default).            */
	uint32_t    r_min;           /* clamp: never shrink below               */
	uint32_t    r_max;           /* clamp: never grow above                 */
	uint32_t    s_end_zone;      /* rz=report only: exclusive zone bound of
	                              * this instance's S-region. Required when two
	                              * instances share a device, else R_Z counts
	                              * the other instance's zones. 0 = device end. */

	uint32_t    cooldown_ms;     /* min gap between successful resizes       */
	uint32_t    backoff_ms;      /* initial backoff after a failed resize   */
	uint32_t    backoff_max_ms;  /* backoff ceiling                         */
	uint32_t    ack_timeout_ms;  /* ZenFS resize ack wait                   */

	/* run modes ----------------------------------------------------------- */
	bool        once;            /* one cycle then exit                     */
	bool        dry_run;         /* decide + log, do not act                */
	int         verbose;         /* log verbosity                           */
	char        logfile[256];    /* "" = stderr                             */
	char        snapfile[256];   /* "" = off. CSV appended with fs/dm
	                              * valid/invalid/free BEFORE and AFTER every
	                              * resize act (grow/shrink), for the
	                              * resize-window analysis.                 */
	char        mock_dm[256];    /* file with canned dmsetup status line    */
	char        mock_rz[256];    /* file with canned report-zones / status  */
} hyd_cfg;

/* ---- live device snapshot ------------------------------------------------ */
typedef struct {
	uint32_t r_c;            /* free CNS capacity in ZONES (= free_pages/zone_pblk) */
	uint32_t cur_r;          /* current R-zone count (r_end/zone)  */
	uint32_t r_z;            /* free ZNS zones                     */
	uint32_t zone_sectors;   /* sectors per zone (zone_pblocks*8)  */
	uint32_t zone_pblocks;   /* pages per zone (1 GiB); zone capacity */
	uint32_t line_pblocks;   /* pages per line (64 MiB GC unit)    */
	uint32_t free_lines;     /* raw free allocator lines (display) */
	uint64_t valid_pages;    /* live-mapped pages in R (dm L2P)    */
	uint64_t free_pages;     /* R_C = real free blocks (R - valid - invalid) */
	bool     gc_blocked;     /* dm GC parked: victims all ~fully valid    */
	uint32_t best_victim_vpc;/* greedy victim valid%% (GC cost proxy) */
	uint64_t dev_sectors;    /* dm target length (S ends here)     */
	uint64_t nospc;          /* dm nospc counter                   */
	bool     dm_ok;          /* R_C/cur_r valid                    */
	bool     rz_ok;          /* R_Z valid                          */
} hyd_state;

/* ---- policy decision ----------------------------------------------------- */
typedef enum { ACT_NONE = 0, ACT_GROW = 1, ACT_SHRINK = 2 } hyd_action;

typedef struct {
	hyd_action action;
	uint32_t   target_r;     /* absolute new R-zone count          */
	const char *reason;      /* human-readable why                 */
	int        gc_urgent;    /* hybrid valid-gate: -1=leave, 0=off, *
	                          * 1=on. When CNS is pressured but live *
	                          * valid is low (fragmentation, not real *
	                          * demand), turn gc_urgent ON to recycle *
	                          * lines instead of growing.            */
} hyd_decision;

/* ---- monitor.c ----------------------------------------------------------- */
int  hyd_read_dm(const hyd_cfg *cfg, hyd_state *st);   /* R_C, cur_r, zone_sectors */
int  hyd_read_rz(const hyd_cfg *cfg, hyd_state *st);   /* R_Z (fs|report)          */

/* ---- policy.c ------------------------------------------------------------ */
hyd_decision hyd_decide(const hyd_cfg *cfg, const hyd_state *st);
int        hyd_act(const hyd_cfg *cfg, const hyd_decision *d, char *resmsg, size_t n);

/* ---- util (hyhostd.c) ---------------------------------------------------- */
uint64_t hyd_now_ms(void);
int      hyd_run_capture(const char *cmd, char *out, size_t n);  /* popen → out */
long     hyd_kv_lookup(const char *line, const char *key);       /* "key=<int>" */

#endif /* HYHOSTD_H */
