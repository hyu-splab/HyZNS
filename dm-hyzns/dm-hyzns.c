// SPDX-License-Identifier: GPL-2.0
/*
 * dm-hyzns - host-managed FTL for the HyZNS FEMU device.
 *
 * Splits the backing namespace at a runtime-movable boundary (r_end, the
 * ABA), all in 512B sectors:
 *
 *     [ 0 .. r_end )            R-region: random-write. Each 4 KiB page
 *                                (8 sectors) is mapped via l2p[lpage] ->
 *                                pblock; backing LBA = pblock * 8. Writes
 *                                are out-of-place with the L2P swap
 *                                deferred to endio; a GC kthread reclaims
 *                                partially-valid lines, an erase kthread
 *                                issues line erases, discards are absorbed
 *                                into the L2P.
 *     [ r_end .. ti->len )       S-region: passthrough to backing.
 *
 * Userland creates the target with:
 *
 *   echo "0 <sectors> hyzns <backing_dev> <r_end_lba> [max_r_end_lba]" \
 *       | dmsetup create hyzns0
 *
 * <r_end_lba> must be zone-aligned (1 GiB). The optional 3rd argument
 * max-provisions the L2P / rings so the boundary can later grow up to it
 * (see hyzns_ctr). The boundary moves at runtime via `dmsetup message ...
 * set_r_end <lba>` or an F2FS-driven REQ_OP_ZONE_MODIFY bio; on a shrink
 * the surrendered lines are force-GC'd first, on device rejection dm state
 * is left untouched.
 *
 * `dmsetup status` emits one key=value line; see hyzns_status() for the
 * field list.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "hyzns"

#define HYZNS_PAGE_SHIFT   3U                          /* 4 KiB / 512B */
#define HYZNS_PAGE_SECTORS (1U << HYZNS_PAGE_SHIFT)
#define HYZNS_PAGE_MASK    (HYZNS_PAGE_SECTORS - 1)
#define HYZNS_PAGE_BYTES   (HYZNS_PAGE_SECTORS << SECTOR_SHIFT)
#define HYZNS_INVALID_PBA  ((sector_t)~0ULL)
#define HYZNS_INVALID_LPA  ((sector_t)~0ULL)

/* NAND geometry. Two nested units:
 *
 *   line = one NAND block per (ch, lun) in parallel = the device erase unit
 *          and the host GC victim/reclaim unit.
 *   zone = an integer number of lines = the logical unit F2FS / ZenFS see and
 *          the granularity the R/S boundary (r_end / ABA) moves on.
 *
 * Geometry (a 64 MiB line keeps a GC victim's valid payload small):
 *   host page      = 4 KiB  (HYZNS_PAGE_SHIFT=3; L2P granularity)
 *   device NAND pg = 16 KiB (4 host pages; FEMU's striping/latency unit)
 *   block          = 128 device pages = 512 host pages = 2 MiB (erase unit)
 *   8 channels x 4 ways (LUNs) parallel  -> line = 8 x 4 x 2 MiB = 64 MiB
 *   zone           = 1 GiB = 16 lines
 *
 * (ch, lun) interleaving must match FEMU's page-major write-pointer order
 * (hyzns.c:hyzns_lba_to_chlun):
 *   device_page_idx = host_page_idx / (16 KiB / 4 KiB) = host_page_idx / 4
 *   ch  = device_page_idx % nchs
 *   lun = (device_page_idx / nchs) % luns_per_ch
 * FEMU strides ch fastest, so within a line consecutive NAND pages fan out
 * across all (ch, lun) before advancing the in-block page; a sequential
 * pblock run lands on every lane. dm's line allocator fills pblocks
 * 0..line_pblocks sequentially, so the two agree page-for-page.
 *
 * HYZNS_PGS_PER_BLK is measured in *host* pages (2 MiB / 4 KiB = 512) so all
 * dm bookkeeping stays in the 4 KiB L2P unit; the 16 KiB device page only
 * matters to FEMU's timing model. */
#define HYZNS_NCHS         8U
#define HYZNS_LUNS_PER_CH  4U
#define HYZNS_PGS_PER_BLK  512U                              /* 2 MiB block / 4 KiB host page */
#define HYZNS_BLOCK_BYTES  (HYZNS_PGS_PER_BLK * HYZNS_PAGE_BYTES)
#define HYZNS_LINE_PBLOCKS                                   \
	(HYZNS_NCHS * HYZNS_LUNS_PER_CH * HYZNS_PGS_PER_BLK)   /* 16,384 host pages = 64 MiB */

/* Logical zone: the unit F2FS/ZenFS see and the granularity r_end moves on.
 * Must equal FEMU's hyzns_zone_size_mb (default 1 GiB) and be an integer
 * number of lines. In host pages: 1 GiB / 4 KiB = 262,144. */
#define HYZNS_ZONE_BYTES   (1ULL << 30)                     /* 1 GiB logical zone */
#define HYZNS_ZONE_PAGES   (HYZNS_ZONE_BYTES >> (SECTOR_SHIFT + HYZNS_PAGE_SHIFT))

/* Coalesce a multi-page R-region write into one contiguous pblock run +
 * one backing bio, instead of splitting it per 4 KiB page. Off (=0) reverts
 * to the legacy per-page dm_accept_partial_bio path for A/B comparison. */
static bool r_coalesce = true;
module_param(r_coalesce, bool, 0644);
MODULE_PARM_DESC(r_coalesce, "coalesce multi-page R writes into one contiguous pblock run (default 1)");

struct hyzns_stats {
	atomic64_t r_bios;
	atomic64_t s_bios;
	atomic64_t splits;
	atomic64_t r_writes;
	atomic64_t r_reads;
	atomic64_t r_reads_unmapped;
	atomic64_t r_overwrites;
	atomic64_t r_inplace;       /* overwrites that reused the existing pblock */
	atomic64_t r_align_rejects;
	atomic64_t r_nospc;
	atomic64_t line_recycles;   /* lines returned (= moved to dirty queue) */
	atomic64_t gc_runs;         /* do_gc_one_line() invocations that picked a victim */
	atomic64_t gc_migrations;   /* valid pblocks moved (write-amplification numerator) */
	atomic64_t gc_skipped;      /* migrations skipped due to L2P race */
	atomic64_t map_requeues;    /* map() stall episodes waiting for GC to free a line */
	atomic64_t r_write_failed;  /* R-region write bios completed with error; alloc rolled back */
	atomic64_t erases;          /* NAND block erases (vendor zsa=0x22) issued */
	atomic64_t erase_failed;    /* erase bios that came back with error */
	atomic64_t discards;        /* REQ_OP_DISCARD bios absorbed at the dm layer */
	atomic64_t discard_pages;   /* L2P entries invalidated by discards */
};

/* Per-bio context for the endio-deferred L2P swap. dm core reserves
 * `ti->per_io_data_size` bytes after the cloned struct bio (accessed
 * via dm_per_bio_data()), so this is per-target-bio with no extra
 * allocation. Initialized to op=HYZNS_OP_NONE in hyzns_map(); the
 * R-region write path sets op=HYZNS_OP_R_WRITE and the rest of the
 * fields. .end_io reads op to decide what to commit.
 */
enum hyzns_pio_op {
	HYZNS_OP_NONE = 0,
	HYZNS_OP_R_WRITE,
};

struct hyzns_pio {
	enum hyzns_pio_op op;
	sector_t           lpage;       /* first lpage of the run */
	sector_t           new_pblock;  /* first pblock of the run (contiguous) */
	unsigned int       n_pages;     /* run length in pages (>= 1) */
};

/* Async ZONE_MODIFY context: created when an F2FS ioctl-driven
 * REQ_OP_ZONE_MODIFY bio arrives via .map(), freed in the endio
 * callback after committing the dm-side r_end.
 */
struct hyzns_c;
struct hyzns_zmod_ctx {
	struct hyzns_c   *hc;
	struct bio        *orig;
	sector_t           new_r_end_lba;
	sector_t           new_r_pages;
	struct work_struct work;
};

/* GC migration batch. One slot per in-flight 4 KiB migration; the slots
 * (and their permanently-allocated data pages) live in hc->gc_items and
 * are reused batch after batch, protected by hc->gc_io_lock.
 */
/* One migration wave submits this many 4 KiB IOs concurrently. The device
 * charges per-(ch,lun) latency in parallel, so the wave's effective parallelism
 * = how many distinct (ch,lun) the batch's destination pblocks span. A NAND
 * page is 16 KiB = 4 host pblocks, so 4 consecutive destination pblocks alias
 * to ONE (ch,lun); at 8x4 (32 lanes) a batch of 32*4 = 128 consecutive
 * destination pblocks touches every (ch,lun), so drain throughput saturates at
 * batch 128 while a co-tenant's 4k-read tail stays clean up to ~32 and
 * degrades past ~64. */
#define HYZNS_GC_BATCH      512U  /* max batch = gc_items[] allocation (runtime-knob cap);
                                    * pins one 4 KiB page per slot -> 2 MiB/target. */
#define HYZNS_GC_BATCH_DEFAULT 128U /* autonomous GC default: the drain knee. Background GC
                                      * runs under free-line pressure (a writer is stalling),
                                      * so max clean drain wins. Runtime: `dmsetup message
                                      * gc_batch`. */
#define HYZNS_GC_BATCH_SHRINK  32U  /* shrink force-GC cap: a background resize must not hurt
                                      * the live co-tenant, so throttle below the tail cliff.
                                      * Urgent resizes can raise it at runtime via `dmsetup
                                      * message gc_batch_shrink`. */
#define HYZNS_GC_SCAN_CHUNK 8192U /* rmap slots scanned per lock hold (>= BATCH) */
#define HYZNS_GC_VICTIM_MAX_VPC 95U /* victim valid% above this = useless (net gain ~0) -> park GC */

struct hyzns_mig_ctx {
	atomic_t          pending;
	struct completion done;
};

struct hyzns_mig_item {
	sector_t               lpage;
	sector_t               old_pblock;
	sector_t               new_pblock;
	struct page           *page;
	bool                   failed; /* IO error in either wave */
	struct hyzns_mig_ctx *ctx;
};

struct hyzns_c {
	struct dm_dev      *dev;
	sector_t            r_end;             /* current logical boundary */
	sector_t            r_max_pages;  /* L2P size, fixed at ctr */
	sector_t            r_max_lines;  /* = r_max_pages / line_pblocks */
	unsigned int        line_pblocks;      /* = HYZNS_LINE_PBLOCKS (64 MiB) */
	unsigned int        zone_pages;        /* = HYZNS_ZONE_PAGES (1 GiB); r_end
	                                        * moves on this granularity, an
	                                        * integer multiple of line_pblocks */
	sector_t           *l2p;               /* INVALID or pblock index */

	/* Line-level free ring. Each entry is a line index 0..r_max_lines-1.
	 * A line covers pblocks [line_idx * line_pblocks, (line_idx + 1) *
	 * line_pblocks). Sequential allocation within the current line cycles
	 * through all (ch, lun) combos.
	 */
	sector_t           *free_lines;
	size_t              ring_head;
	size_t              ring_count;

	/* Exclusive push bound for the free ring, in lines. Steady state ==
	 * r_end; a shrink LOWERS it to the new boundary at force-GC start,
	 * so lines the R-region is about to surrender can never re-enter
	 * circulation mid-drain (the erase-thread recycle would otherwise
	 * hand an evacuated line straight back to the allocator; writes
	 * placed there EIO once the device commits the new ABA, and their
	 * L2P entries are silently dropped at quiesce). Out-of-bound pushes
	 * are parked on quar_lines so a FAILED shrink can restore them. */
	sector_t            push_bound_lines;
	sector_t           *quar_lines;
	size_t              quar_count;

	/* Currently-being-filled write line. cur_line_off is the index of the
	 * next pblock to allocate within the line; when it reaches
	 * line_pblocks the line is full and a new one is popped.
	 */
	bool                cur_line_active;
	sector_t            cur_line_idx;
	unsigned int        cur_line_off;

	/* GC-private destination line, allocated separately from the user
	 * cur line above. Sharing one line let a busy writer eat GC's
	 * destination mid-migration (including the reserve line GC popped),
	 * aborting the victim with migrated=0 -> gc_blocked -> spurious
	 * ENOSPC for stalled writers while reclaimable space still existed.
	 * A victim's valid count is always < line_pblocks, so one private
	 * line is guaranteed to absorb any single victim; the drained
	 * victim then repays the ring. */
	bool                gc_cur_line_active;
	sector_t            gc_cur_line_idx;
	unsigned int        gc_cur_line_off;

	/* Per-line valid pblock count. A pblock is "valid" iff some L2P
	 * entry points at it. When a line's count reaches 0 (and it is not
	 * the current write line), the line is recycled back to free_lines.
	 * u32: a 1 GiB line packs 262144 pblocks, which overflows u16.
	 */
	uint32_t           *line_valid;

	/* Per-line birth jiffies - the moment the line was popped from
	 * free_lines and started receiving writes. Used by Cost-Benefit
	 * victim selection: older lines with low utilization win. */
	unsigned long      *line_birth;

	/* GC victim-selection policy. Switchable at runtime via
	 * `dmsetup message <name> 0 gc_policy {greedy|cb}`.
	 *   greedy: pick the line with the smallest valid count.
	 *   cb    : pick max( age * (line_pblocks - valid) / valid ),
	 *           favoring old + low-utilization lines. (pblk-style.) */
	enum {
		HYZNS_GC_GREEDY = 0,
		HYZNS_GC_COST_BENEFIT,
	}                   gc_policy;

	/* Reverse map: pblock -> lpage (or HYZNS_INVALID_LPA if the pblock
	 * is currently unallocated). Needed by GC to know which lpage's
	 * mapping to update when migrating a victim line's valid pblocks.
	 */
	sector_t           *rmap;

	/* Dirty-line ring. Lines that have just been recycled (valid count
	 * dropped to zero) wait here until the erase kthread issues a single
	 * vendor R-line-erase (zsa=0x23) that the device fans out across every
	 * (ch, lun) block making up the line (HYZNS_LINE_PBLOCKS /
	 * HYZNS_PGS_PER_BLK = 32 blocks per line at 8x4, one block per lane).
	 * Then pushes the line onto free_lines. Maintains the
	 * NAND-realistic invariant "no line goes back to free_lines without
	 * being erased first". Same array shape as free_lines. */
	sector_t           *dirty_lines;
	size_t              dirty_head;
	size_t              dirty_count;

	/* Erase kthread. Sleeps when dirty_count == 0; wakes when
	 * line_push_dirty() enqueues. Drains dirty_lines, issues one
	 * hyzns_line_erase (zsa=0x23) per line, then pushes onto free_lines
	 * and wakes the gc thread / map() requeues. */
	struct task_struct *erase_thread;
	wait_queue_head_t   erase_wait;

	/* ZONE_MODIFY worker queue. F2FS ioctl path arrives at map() in a
	 * non-sleeping context, so we hand off the actual work (shrink
	 * force GC + synchronous device ModifyZone + quiesce + commit) to
	 * an ordered single-threaded workqueue. The dmsetup set_r_end path
	 * does not use this - it already runs in process context. */
	struct workqueue_struct *zmod_wq;

	/* GC kthread + watermarks. map() write path wakes the thread when
	 * the live free-line count drops below gc_low_watermark; the
	 * thread runs until it reaches gc_high_watermark or finds no
	 * useful victim.
	 */
	struct task_struct *gc_thread;
	wait_queue_head_t   gc_wait;
	/* Batch migration buffers (HYZNS_GC_BATCH slots + data pages).
	 * Shared by the GC kthread and the shrink-force-GC path (zmod
	 * worker) - gc_io_lock serializes whole-line GC runs. */
	struct hyzns_mig_item *gc_items;
	u32                 gc_batch;   /* runtime GC migration batch cap (<=HYZNS_GC_BATCH);
	                                 * smaller => GC floods the device queue less per wave,
	                                 * leaving bandwidth for the co-tenant. dmsetup message. */
	u32                 gc_batch_shrink; /* batch cap applied ONLY during shrink force-GC
	                                 * (min'd with gc_batch); protects the live co-tenant. */
	struct mutex        gc_io_lock;
	/* Writers that found no poppable line AND an unmapped lpage sleep
	 * here in map() until the erase kthread recycles a line (or GC
	 * parks victimless - then they fail with ENOSPC). DM_MAPIO_REQUEUE
	 * is not usable for this: bio-based dm honours it only during a
	 * noflush suspend and otherwise completes the bio with EIO
	 * (dm_io_dec_pending). */
	wait_queue_head_t   alloc_wait;
	size_t              gc_low_watermark;
	size_t              gc_high_watermark;
	/* Lines reserved for GC. The user-write fast path is forbidden
	 * from popping when ring_count <= gc_reserve_lines, so GC always
	 * has a destination to migrate into even when the workload would
	 * otherwise drain the ring to zero. Without this the system
	 * wedges at "cur_line full + ring=0": neither user nor GC can
	 * allocate a new line, and DM_MAPIO_REQUEUE on the user side
	 * drives the workload to EIO.
	 */
	size_t              gc_reserve_lines;
	/* GC backoff flag. Set true by the kthread after a no-progress
	 * sweep (n <= 0) and cleared by any user-write endio path that
	 * actually recycles a line. Without this we'd hot-spin: when
	 * ring_count stays at 0 the watermark predicate is always true,
	 * so wait_event_interruptible returns immediately and GC re-runs
	 * with the same outcome forever. With it, GC sleeps until
	 * forward progress is observable.
	 */
	bool                gc_blocked;

	spinlock_t          lock;
	struct hyzns_stats stats;
};

/* -------------------------------------------------------------------------
 * Free-line ring (caller holds hc->lock for both ops).
 * ------------------------------------------------------------------------- */

static int hyzns_line_pop(struct hyzns_c *hc, sector_t *out)
{
	sector_t line;

	if (!hc->ring_count)
		return -ENOSPC;
	line = hc->free_lines[hc->ring_head];
	hc->ring_head = (hc->ring_head + 1) % hc->r_max_lines;
	hc->ring_count--;
	if (hc->line_birth)
		hc->line_birth[line] = jiffies;
	*out = line;
	return 0;
}

static void hyzns_line_push(struct hyzns_c *hc, sector_t line)
{
	size_t tail;

	if (line >= hc->push_bound_lines) {
		/* Shrink in progress (or a straggler erase completing right
		 * after a commit): park it. Returned to the ring only by
		 * hyzns_shrink_restore() on a failed shrink; dropped at the
		 * next quarantine reset otherwise (the line left the
		 * R-region). */
		BUG_ON(hc->quar_count >= hc->r_max_lines);
		hc->quar_lines[hc->quar_count++] = line;
		return;
	}
	tail = (hc->ring_head + hc->ring_count) % hc->r_max_lines;
	BUG_ON(hc->ring_count >= hc->r_max_lines);
	hc->free_lines[tail] = line;
	hc->ring_count++;
}

/* Is the line sitting in the dirty (awaiting-erase) ring? Linear - the
 * dirty ring is nearly always a handful of entries. Caller holds hc->lock. */
static bool hyzns_line_in_dirty(struct hyzns_c *hc, sector_t line)
{
	size_t i;

	for (i = 0; i < hc->dirty_count; i++)
		if (hc->dirty_lines[(hc->dirty_head + i) % hc->r_max_lines] ==
		    line)
			return true;
	return false;
}

/* Dirty-ring ops. Caller holds hc->lock (IRQ-safe). Dirty ring carries
 * lines that have been recycled but not yet NAND-erased - the erase
 * kthread drains it and is the only producer for free_lines. */
static void hyzns_line_push_dirty(struct hyzns_c *hc, sector_t line)
{
	size_t tail = (hc->dirty_head + hc->dirty_count) % hc->r_max_lines;

	BUG_ON(hc->dirty_count >= hc->r_max_lines);
	hc->dirty_lines[tail] = line;
	hc->dirty_count++;
}

static int hyzns_line_pop_dirty(struct hyzns_c *hc, sector_t *out)
{
	if (!hc->dirty_count)
		return -ENOSPC;
	*out = hc->dirty_lines[hc->dirty_head];
	hc->dirty_head = (hc->dirty_head + 1) % hc->r_max_lines;
	hc->dirty_count--;
	return 0;
}

/* A line that is currently open for allocation (user cur or GC cur) must
 * never be pushed to the dirty ring on a zero-valid transition, nor be
 * picked as a GC victim - its offset keeps advancing. Caller holds
 * hc->lock. */
static inline bool hyzns_line_is_open(struct hyzns_c *hc, sector_t line)
{
	return (hc->cur_line_active && line == hc->cur_line_idx) ||
	       (hc->gc_cur_line_active && line == hc->gc_cur_line_idx);
}

/* Erase one full line with a single vendor R-line-erase (zsa = 0x23):
 * the device receives one LBA (= line start), looks up the line index,
 * and advances every (ch, lun)'s next_avail_ns for the block at that
 * line_idx, charging the whole line as one parallel erase round.
 * Sleepable - must run from the erase kthread. */
static int hyzns_line_erase(struct hyzns_c *hc, sector_t line)
{
	struct bio *bio = bio_alloc(GFP_KERNEL, 0);
	sector_t line_lba;
	int ret;

	if (!bio)
		return -ENOMEM;
	line_lba = (line * hc->line_pblocks) << HYZNS_PAGE_SHIFT;
	bio_set_dev(bio, hc->dev->bdev);
	bio->bi_opf = REQ_OP_ZONE_R_LINE_ERASE | REQ_SYNC;
	bio->bi_iter.bi_sector = line_lba;
	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret) {
		atomic64_inc(&hc->stats.erase_failed);
		DMWARN("line erase line=%llu lba=%llu failed: %d",
		       (unsigned long long)line,
		       (unsigned long long)line_lba, ret);
	} else {
		atomic64_inc(&hc->stats.erases);
	}
	return ret;
}

static int hyzns_erase_thread(void *data)
{
	struct hyzns_c *hc = data;
	unsigned long flags;

	while (!kthread_should_stop()) {
		sector_t line;
		bool have;

		wait_event_interruptible(hc->erase_wait,
			kthread_should_stop() ||
			READ_ONCE(hc->dirty_count) > 0);
		if (kthread_should_stop())
			break;

		spin_lock_irqsave(&hc->lock, flags);
		have = (hyzns_line_pop_dirty(hc, &line) == 0);
		spin_unlock_irqrestore(&hc->lock, flags);
		if (!have)
			continue;

		/* erase off-lock - one zsa=0x23 bio per line; the device charges
		 * an erase on every (ch, lun) in parallel and returns the max, so
		 * a whole 64 MiB line costs ~one erase round (~2 ms) regardless of
		 * its 32-block span. */
		hyzns_line_erase(hc, line);

		spin_lock_irqsave(&hc->lock, flags);
		hyzns_line_push(hc, line);
		if (READ_ONCE(hc->gc_blocked))
			pr_info("dm-hyzns: [GC] unparked (line %llu recycled)\n",
				(unsigned long long)line);
		WRITE_ONCE(hc->gc_blocked, false);
		spin_unlock_irqrestore(&hc->lock, flags);
		wake_up(&hc->gc_wait);
		wake_up(&hc->alloc_wait);
		cond_resched();
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Boundary reshape helpers.  Caller holds hc->lock.
 *
 * Shrink: backing pblocks at [new_r_pages, old_r_pages) are about to
 * become S-region zones (we've already told the device via Zone Mgmt
 * Send action 0x20). We must:
 *   (1) clear L2P entries at lpage >= new_r_pages OR pointing at a
 *       pblock >= new_r_pages, recycling reclaimable pblocks;
 *   (2) prune the free ring of any entry that has just become S-region.
 *
 * Grow (within capacity): the same pblock range becomes R-region again.
 *   (1) confirm L2P entries [old_r_pages, new_r_pages) are INVALID
 *       (they should be - set at ctr or during a prior shrink);
 *   (2) push the newly-available pblocks back to the ring.
 * ------------------------------------------------------------------------- */

/* Absorb a REQ_OP_DISCARD at the dm layer: invalidate every L2P mapping
 * fully covered by the range so the upper filesystem's deletes translate
 * into line_valid drops (and line recycling) instead of dead data that
 * GC would otherwise migrate. Discard is advisory - partial pages at the
 * range edges are skipped, and the S-region part (>= r_end) is ignored
 * since S zones are reclaimed via zone reset. The bio never reaches the
 * device: HyZM's DSM handler is a no-op ack, so the entire useful effect
 * is this host-side bookkeeping.
 *
 * Called from .map() (process context, sleepable under dm's SRCU). Takes
 * hc->lock in bounded batches so a multi-GiB fstrim cannot monopolize
 * the lock against the write path.
 */
static void hyzns_discard_range(struct hyzns_c *hc, sector_t lba,
				 unsigned int n_sectors, sector_t r_end)
{
	sector_t end = lba + n_sectors;
	sector_t lpage, last_page;
	unsigned long flags;
	bool wake_erase = false;
	long freed = 0;

	if (end > r_end)
		end = r_end;
	/* First fully-covered page (round start up), last+1 (round end down). */
	lpage = (lba + HYZNS_PAGE_SECTORS - 1) >> HYZNS_PAGE_SHIFT;
	last_page = end >> HYZNS_PAGE_SHIFT;

	while (lpage < last_page) {
		sector_t batch_end = min_t(sector_t, lpage + 1024, last_page);

		spin_lock_irqsave(&hc->lock, flags);
		for (; lpage < batch_end; lpage++) {
			sector_t p = hc->l2p[lpage];
			sector_t line;

			if (p == HYZNS_INVALID_PBA)
				continue;
			line = p / hc->line_pblocks;
			hc->l2p[lpage] = HYZNS_INVALID_PBA;
			hc->rmap[p]    = HYZNS_INVALID_LPA;
			BUG_ON(hc->line_valid[line] == 0);
			hc->line_valid[line]--;
			freed++;
			if (hc->line_valid[line] == 0 &&
			    !hyzns_line_is_open(hc, line)) {
				hyzns_line_push_dirty(hc, line);
				atomic64_inc(&hc->stats.line_recycles);
				wake_erase = true;
			}
		}
		spin_unlock_irqrestore(&hc->lock, flags);
		cond_resched();
	}

	if (freed) {
		atomic64_add(freed, &hc->stats.discard_pages);
		/* Same re-arm as the endio displacement path: freed pages
		 * may have turned a full line into a GC victim. */
		if (READ_ONCE(hc->gc_blocked))
			pr_info("dm-hyzns: [GC] unparked (discard freed %llu pages)\n",
				(unsigned long long)freed);
		WRITE_ONCE(hc->gc_blocked, false);
		if (!wake_erase &&
		    READ_ONCE(hc->ring_count) < hc->gc_low_watermark)
			wake_up(&hc->gc_wait);
	}
	if (wake_erase)
		wake_up(&hc->erase_wait);
}

static void hyzns_quiesce_shrink(struct hyzns_c *hc, sector_t new_r_pages)
{
	sector_t old_r_pages = hc->r_end >> HYZNS_PAGE_SHIFT;
	sector_t new_r_lines = new_r_pages / hc->line_pblocks;
	sector_t i;
	size_t   kept;

	/* Pass 1: drop L2P mappings that fall outside the new R-region or
	 * point at a pblock past the boundary. Decrement line_valid for
	 * each cleared mapping. */
	for (i = 0; i < old_r_pages; i++) {
		sector_t p = hc->l2p[i];
		sector_t old_line;

		if (p == HYZNS_INVALID_PBA)
			continue;
		if (i < new_r_pages && p < new_r_pages)
			continue;

		old_line = p / hc->line_pblocks;
		BUG_ON(hc->line_valid[old_line] == 0);
		hc->line_valid[old_line]--;
		hc->l2p[i] = HYZNS_INVALID_PBA;
		/* p may be inside or past the new boundary; either way no
		 * lpage references it anymore. */
		if (p < hc->r_max_pages)
			hc->rmap[p] = HYZNS_INVALID_LPA;
	}

	/* If an open write line (user cur or GC cur) is past the new
	 * boundary, abandon it. Its unfilled tail is irrelevant; anything
	 * already written has had its L2P entry cleared above. */
	if (hc->cur_line_active && hc->cur_line_idx >= new_r_lines) {
		hc->cur_line_active = false;
		hc->cur_line_off    = 0;
	}
	if (hc->gc_cur_line_active && hc->gc_cur_line_idx >= new_r_lines) {
		hc->gc_cur_line_active = false;
		hc->gc_cur_line_off    = 0;
	}

	/* Purge dirty (awaiting-erase) entries past the boundary - those
	 * lines leave the R-region and the device reshape owns their state
	 * now; letting the erase thread touch them post-commit would fire
	 * an R-line erase into what is about to be an S-zone. */
	{
		size_t d, kept_d = 0;

		for (d = 0; d < hc->dirty_count; d++) {
			sector_t l = hc->dirty_lines[(hc->dirty_head + d) %
						     hc->r_max_lines];
			if (l < new_r_lines)
				hc->dirty_lines[kept_d++] = l;
		}
		hc->dirty_head  = 0;
		hc->dirty_count = kept_d;
	}

	/* Pass 2: rebuild the free-line ring from scratch - every line
	 * inside the new R-region that has zero valid pblocks and isn't
	 * the active write line is free.  O(new_r_lines), which is the
	 * same order as Pass 1. Lines still queued for erase are NOT free
	 * yet - the erase thread pushes them when done; adding them here
	 * too would put the same line on the ring twice (two writers on
	 * one line). */
	kept = 0;
	for (i = 0; i < new_r_lines; i++) {
		if (hc->line_valid[i] != 0)
			continue;
		if (hyzns_line_is_open(hc, i))
			continue;
		if (hyzns_line_in_dirty(hc, i))
			continue;
		hc->free_lines[kept++] = i;
	}
	hc->ring_head  = 0;
	hc->ring_count = kept;
	/* Quarantined lines are all >= new_r_lines: they left the R-region
	 * with this commit. Drop them. push_bound_lines already equals
	 * new_r_lines (set at force-GC start) - the new steady state. */
	hc->quar_count = 0;
}

static void hyzns_quiesce_grow(struct hyzns_c *hc, sector_t new_r_pages)
{
	sector_t old_r_lines = (hc->r_end >> HYZNS_PAGE_SHIFT) /
			       hc->line_pblocks;
	sector_t new_r_lines = new_r_pages / hc->line_pblocks;
	sector_t i;

	/* Raise the push bound FIRST or the pushes below would be
	 * quarantined as out-of-bound. */
	hc->push_bound_lines = new_r_lines;
	for (i = old_r_lines; i < new_r_lines; i++) {
		/* These lines were S-region zones - pblocks have no L2P
		 * entries pointing at them, line_valid should be 0. (If a
		 * prior shrink left a stale non-zero, reset it.) */
		hc->line_valid[i] = 0;
		hyzns_line_push(hc, i);
	}
}

/* Recompute the GC free-line watermarks from the LIVE R size (lines). Called
 * after every resize commit so the band scales with the current R instead of
 * staying frozen at the ctr-time initial size (which starves a grown R of
 * headroom). */
static void hyzns_set_watermarks(struct hyzns_c *hc, sector_t active_lines)
{
	/* GC trigger = ceil(R * 0.25) free lines. Predicate is ring_count <
	 * gc_low_watermark, so low = T+1 makes "free_lines <= T" wake GC. high =
	 * low => GC reclaims one line at a time, no hoarding band (a %-based high
	 * hoards several lines at tiny R and can deadlock). */
	size_t T = ((size_t)active_lines + 3) / 4;   /* ceil(R/4) */
	if (T < 1)
		T = 1;
	WRITE_ONCE(hc->gc_low_watermark,  T + 1);
	WRITE_ONCE(hc->gc_high_watermark, T + 1);
	WRITE_ONCE(hc->gc_reserve_lines,  active_lines > 1 ? 1 : 0);
}

/* -------------------------------------------------------------------------
 * Garbage collection.  Background kthread migrates valid pblocks
 * out of partially-invalid victim lines so the line can be recycled.
 * Triggered when free_lines drops below gc_low_watermark; runs until it
 * reaches gc_high_watermark or no useful victim remains. Mirrors bbssd's
 * select_victim_line / clean_one_block / mark_line_free flow.
 *
 * Concurrency model:
 *  - GC thread takes hc->lock for state changes only; data IO (read/
 *    write bios) happens with the lock dropped.
 *  - During the IO drop window a concurrent map() write can displace the
 *    lpage we're migrating. We detect this on lock reacquire by checking
 *    that l2p[lpage] is still our old pblock; if not, skip the swap and
 *    leak the new_pblock allocation (it will be reclaimed when its line
 *    fully invalidates). gc_skipped counter tracks this.
 *  - The same read-after-write race window that exists for normal map()
 *    writes also exists during GC migration: if a reader observes the
 *    new mapping before the WRITE bio lands, it sees stale data. Same
 *    deferral as the existing race.
 * ------------------------------------------------------------------------- */

static int hyzns_gc_items_alloc(struct hyzns_c *hc)
{
	unsigned int i;

	hc->gc_items = kcalloc(HYZNS_GC_BATCH, sizeof(*hc->gc_items),
			       GFP_KERNEL);
	if (!hc->gc_items)
		return -ENOMEM;
	for (i = 0; i < HYZNS_GC_BATCH; i++) {
		hc->gc_items[i].page = alloc_page(GFP_KERNEL);
		if (!hc->gc_items[i].page) {
			while (i--)
				__free_page(hc->gc_items[i].page);
			kfree(hc->gc_items);
			hc->gc_items = NULL;
			return -ENOMEM;
		}
	}
	return 0;
}

static void hyzns_gc_items_free(struct hyzns_c *hc)
{
	unsigned int i;

	if (!hc->gc_items)
		return;
	for (i = 0; i < HYZNS_GC_BATCH; i++)
		__free_page(hc->gc_items[i].page);
	kfree(hc->gc_items);
	hc->gc_items = NULL;
}

static void hyzns_mig_end_io(struct bio *bio)
{
	struct hyzns_mig_item *it  = bio->bi_private;
	struct hyzns_mig_ctx  *ctx = it->ctx;

	if (bio->bi_status)
		it->failed = true;
	bio_put(bio);
	if (atomic_dec_and_test(&ctx->pending))
		complete(&ctx->done);
}

/* Submit one wave of concurrent 4 KiB IOs over gc_items[0..n) and wait
 * for all of them: REQ_OP_READ pulls each item's old_pblock into its
 * page, REQ_OP_WRITE pushes the page to new_pblock. Items already marked
 * failed are skipped, so a failed read naturally drops out of the write
 * wave. The concurrency is what makes GC fast - the device charges the
 * per-(ch, lun) latencies in parallel, so a wave costs roughly one IO
 * latency instead of n of them. */
static void hyzns_mig_wave(struct hyzns_c *hc, unsigned int n,
			    unsigned int op)
{
	struct hyzns_mig_ctx ctx;
	struct blk_plug plug;
	unsigned int j;

	/* Self-reference so an early completion can't fire before every
	 * bio has been submitted. */
	atomic_set(&ctx.pending, 1);
	init_completion(&ctx.done);

	/* Plug so the block layer batches the whole wave's submissions
	 * (merge/queue) instead of dispatching each bio one at a time. */
	blk_start_plug(&plug);
	for (j = 0; j < n; j++) {
		struct hyzns_mig_item *it = &hc->gc_items[j];
		sector_t pblock = (op == REQ_OP_READ) ? it->old_pblock
						      : it->new_pblock;
		struct bio *bio;

		if (it->failed)
			continue;
		bio = bio_alloc(GFP_KERNEL, 1);
		if (!bio) {
			it->failed = true;
			continue;
		}
		bio_set_dev(bio, hc->dev->bdev);
		bio->bi_opf            = op | REQ_SYNC;
		bio->bi_iter.bi_sector = pblock << HYZNS_PAGE_SHIFT;
		bio_add_page(bio, it->page, HYZNS_PAGE_BYTES, 0);
		it->ctx         = &ctx;
		bio->bi_private = it;
		bio->bi_end_io  = hyzns_mig_end_io;
		atomic_inc(&ctx.pending);
		submit_bio(bio);
	}
	blk_finish_plug(&plug);

	if (!atomic_dec_and_test(&ctx.pending))
		wait_for_completion(&ctx.done);
}

/* Pick the in-use line with the lowest valid count. Returns
 * HYZNS_INVALID_PBA if no useful victim (no line has valid pblocks
 * worth migrating). Caller must hold hc->lock.
 *
 * "Useful" = line_valid > 0 (something to migrate) AND
 * line_valid < line_pblocks (not fully valid; migration buys nothing
 * from a line where every pblock is still mapped, since we'd just be
 * relocating valid data without reclaiming any).
 */
static sector_t hyzns_select_victim(struct hyzns_c *hc)
{
	sector_t best_line = HYZNS_INVALID_PBA;
	sector_t l;

	if (hc->gc_policy == HYZNS_GC_COST_BENEFIT) {
		/* score = age * (line_pblocks - valid) / valid; pick max.
		 * Use u64 to avoid overflow at line_pblocks=16384, age in
		 * jiffies (32-bit jiffies wraps every ~50 days at HZ=1000;
		 * the subtraction handles wrap correctly via unsigned). */
		u64 best_score = 0;
		unsigned long now = jiffies;

		for (l = 0; l < hc->r_max_lines; l++) {
			uint32_t v = hc->line_valid[l];
			u64 age, score;

			if (v == 0 || v >= hc->line_pblocks)
				continue;
			if (hyzns_line_is_open(hc, l))
				continue;

			age = (u64)(now - hc->line_birth[l]);
			score = age * (u64)(hc->line_pblocks - v) / (u64)v;
			if (score > best_score) {
				best_score = score;
				best_line  = l;
			}
		}
		return best_line;
	}

	/* HYZNS_GC_GREEDY (default): smallest valid count wins. */
	{
		uint32_t best_valid = hc->line_pblocks;

		for (l = 0; l < hc->r_max_lines; l++) {
			uint32_t v = hc->line_valid[l];

			if (v == 0 || v >= hc->line_pblocks)
				continue;
			if (hyzns_line_is_open(hc, l))
				continue;
			if (v < best_valid) {
				best_valid = v;
				best_line  = l;
				if (v == 1)
					break;        /* can't beat 1 */
			}
		}
		return best_line;
	}
}

/* Migrate all valid pblocks of one victim line. Returns the number of
 * pblocks migrated, or -1 if no victim was available.
 */
/* Internal worker: migrate all valid pblocks of a specific victim line.
 * The caller has already committed to this victim (no select_victim call
 * here). Used by both the policy-driven path (hyzns_do_gc_one_line) and
 * the shrink force-GC path (hyzns_prepare_shrink_force_gc), which needs
 * to evacuate specific out-of-range lines regardless of their valid count.
 */
/* GC event accumulator: lets the shrink force-GC path roll per-line migration
 * stats into one summary, kept distinct from autonomous background GC. */
struct hyzns_gc_acc {
	int       lines;     /* victim lines that actually migrated >0 */
	long long pages;     /* total pblocks relocated */
	s64       wall_ns;   /* sum of per-line GC wall time */
	s64       read_ns;   /* sum of read-wave */
	s64       write_ns;  /* sum of write-wave */
};

static int hyzns_do_gc_one_line_at(struct hyzns_c *hc, sector_t victim,
				    const char *reason, struct hyzns_gc_acc *acc)
{
	sector_t base_pblock = victim * hc->line_pblocks;
	int migrated = 0;
	unsigned int i = 0;
	unsigned long flags;
	bool out_of_lines = false;
	/* per-line GC wall-clock (analog of BBSSD femu do_gc gc_wall_ns) - host-FTL
	 * GC relocation latency. throughput = migrated*4KiB / gc_wall_ns. */
	ktime_t gc_wall_t0 = ktime_get();
	/* split barrier accounting: how much of gc_wall is read-wave vs write-wave
	 * (two-barrier). If write_ns >> read_ns the cap is in the write/device path
	 * (pipelining read+write buys nothing); if comparable, two-barrier is real. */
	s64 read_wave_ns = 0, write_wave_ns = 0;

	/* The batch buffers are shared between the GC kthread and the
	 * shrink-force-GC path (zmod worker) - serialize whole-line runs. */
	mutex_lock(&hc->gc_io_lock);

	while (i < hc->line_pblocks && !out_of_lines) {
		unsigned int n = 0, j, scanned = 0, io_failed = 0;
		bool wake_erase = false;

		/* Collect phase: reserve up to HYZNS_GC_BATCH destination
		 * pblocks under one lock hold (bounded by SCAN_CHUNK so a
		 * mostly-invalid victim doesn't pin the lock while we skip
		 * dead slots). The L2P swap is deferred to the commit phase
		 * so concurrent readers keep seeing the old data. */
		spin_lock_irqsave(&hc->lock, flags);
		while (i < hc->line_pblocks && n < READ_ONCE(hc->gc_batch) &&
		       scanned < HYZNS_GC_SCAN_CHUNK) {
			sector_t old_pblock = base_pblock + i;
			sector_t lpage      = hc->rmap[old_pblock];
			struct hyzns_mig_item *it;

			i++;
			scanned++;
			if (lpage == HYZNS_INVALID_LPA)
				continue;
			if (hc->l2p[lpage] != old_pblock) {
				atomic64_inc(&hc->stats.gc_skipped);
				continue;
			}

			if (!hc->gc_cur_line_active ||
			    hc->gc_cur_line_off >= hc->line_pblocks) {
				sector_t new_line;

				if (hyzns_line_pop(hc, &new_line) < 0) {
					i--; /* slot not consumed */
					out_of_lines = true;
					break;
				}
				/* GC-private destination - this pop may take
				 * the reserve line, which is exactly what
				 * the reserve exists for. Users never
				 * allocate from gc_cur, so a busy writer
				 * can't starve the migration mid-victim. */
				hc->gc_cur_line_idx    = new_line;
				hc->gc_cur_line_off    = 0;
				hc->gc_cur_line_active = true;
			}
			it = &hc->gc_items[n++];
			it->lpage      = lpage;
			it->old_pblock = old_pblock;
			it->new_pblock = hc->gc_cur_line_idx *
					 hc->line_pblocks +
					 hc->gc_cur_line_off;
			it->failed     = false;
			hc->gc_cur_line_off++;
			hc->line_valid[hc->gc_cur_line_idx]++;
		}
		spin_unlock_irqrestore(&hc->lock, flags);

		if (n) {
			ktime_t wv = ktime_get();
			hyzns_mig_wave(hc, n, REQ_OP_READ);
			read_wave_ns += ktime_to_ns(ktime_sub(ktime_get(), wv));
			wv = ktime_get();
			hyzns_mig_wave(hc, n, REQ_OP_WRITE);
			write_wave_ns += ktime_to_ns(ktime_sub(ktime_get(), wv));

			/* Commit phase: one lock pass over the batch. */
			spin_lock_irqsave(&hc->lock, flags);
			for (j = 0; j < n; j++) {
				struct hyzns_mig_item *it = &hc->gc_items[j];
				sector_t new_line;

				if (!it->failed &&
				    hc->l2p[it->lpage] == it->old_pblock) {
					BUG_ON(hc->line_valid[victim] == 0);
					hc->line_valid[victim]--;
					hc->l2p[it->lpage]       = it->new_pblock;
					hc->rmap[it->new_pblock] = it->lpage;
					hc->rmap[it->old_pblock] =
							HYZNS_INVALID_LPA;
					atomic64_inc(&hc->stats.gc_migrations);
					migrated++;
					/* Whoever decrements a line to zero
					 * under the lock pushes it - doing
					 * it here (not in a trailing "is it
					 * zero now?" pass) means the user
					 * endio displacement path and GC
					 * can't both push the victim. */
					if (hc->line_valid[victim] == 0) {
						hyzns_line_push_dirty(hc,
								       victim);
						atomic64_inc(
						    &hc->stats.line_recycles);
						wake_erase = true;
					}
					continue;
				}

				/* IO failed, or a user write raced past us
				 * and L2P already points elsewhere. Drop the
				 * line_valid reservation for new_pblock; the
				 * old mapping (if still ours) stays intact
				 * so user reads remain correct. */
				new_line = it->new_pblock / hc->line_pblocks;
				BUG_ON(hc->line_valid[new_line] == 0);
				hc->line_valid[new_line]--;
				if (hc->line_valid[new_line] == 0 &&
				    !hyzns_line_is_open(hc, new_line)) {
					hyzns_line_push_dirty(hc, new_line);
					atomic64_inc(&hc->stats.line_recycles);
					wake_erase = true;
				}
				if (it->failed)
					io_failed++;
				else
					atomic64_inc(&hc->stats.gc_skipped);
			}
			spin_unlock_irqrestore(&hc->lock, flags);

			if (wake_erase)
				wake_up(&hc->erase_wait);
			if (io_failed)
				DMERR("GC: %u/%u migrations failed IO on line %llu",
				      io_failed, n,
				      (unsigned long long)victim);
		}
		cond_resched();
	}

	mutex_unlock(&hc->gc_io_lock);
	if (migrated > 0) {
		s64 gc_wall_ns = ktime_to_ns(ktime_sub(ktime_get(), gc_wall_t0));
		pr_info("dm-hyzns: [%s] GC line=%llu migrated_pages=%d gc_wall_ns=%lld read_wave_ns=%lld write_wave_ns=%lld\n",
			reason, (unsigned long long)victim, migrated, gc_wall_ns,
			read_wave_ns, write_wave_ns);
		if (acc) {
			acc->lines++;
			acc->pages    += migrated;
			acc->wall_ns  += gc_wall_ns;
			acc->read_ns  += read_wave_ns;
			acc->write_ns += write_wave_ns;
		}
	}
	return migrated;
}

/* Policy-driven GC: pick a victim via hyzns_select_victim (Greedy or
 * Cost-Benefit) and migrate its valid pblocks. Used by the background
 * GC kthread and the dmsetup "gc" message.
 */
static int hyzns_do_gc_one_line(struct hyzns_c *hc)
{
	sector_t victim;
	unsigned long flags;

	spin_lock_irqsave(&hc->lock, flags);
	victim = hyzns_select_victim(hc);
	/* A victim that is nearly fully valid is a useless victim: migrating it
	 * writes ~1 line to free ~1 line (net gain ~0), so GC would spin forever
	 * saturating the device when the free-line target is unreachable (e.g.
	 * valid data legitimately occupies most of R). Treat it like "no victim"
	 * -> caller parks in gc_blocked until a discard/recycle changes the map.
	 * Shrink's forced GC does NOT come through here (it must evacuate even
	 * fully-valid lines). */
	if (victim != HYZNS_INVALID_PBA &&
	    hc->line_valid[victim] >= hc->line_pblocks / 100 * HYZNS_GC_VICTIM_MAX_VPC) {
		atomic64_inc(&hc->stats.gc_skipped);
		victim = HYZNS_INVALID_PBA;
	}
	if (victim == HYZNS_INVALID_PBA) {
		spin_unlock_irqrestore(&hc->lock, flags);
		return -1;
	}
	atomic64_inc(&hc->stats.gc_runs);
	spin_unlock_irqrestore(&hc->lock, flags);

	return hyzns_do_gc_one_line_at(hc, victim, "GC", NULL);
}

/* Modify-shrink force GC: evacuate every out-of-range line that holds
 * any valid pblocks. Position-driven (not policy-driven) - selects by
 * line index, not by IPC/age - to guarantee that no L2P entry points
 * past the new boundary by the time quiesce_shrink runs.
 *
 * Step 1 cleans up the free-line ring and the active write line so that
 * subsequent pblock allocations during migration stay inside the new
 * R-region. Step 2 walks the [new_r_lines, old_r_lines) range from the
 * top down and migrates each non-empty line via hyzns_do_gc_one_line_at.
 *
 * Caller MUST NOT hold hc->lock. Sleeps in migrate_data.
 */
/* A shrink aborted after the force-GC lowered the push bound: raise the
 * bound back to the LIVE boundary and return every quarantined line to
 * the free ring. Caller must NOT hold hc->lock. */
static void hyzns_shrink_restore(struct hyzns_c *hc)
{
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&hc->lock, flags);
	hc->push_bound_lines = (hc->r_end >> HYZNS_PAGE_SHIFT) /
			       hc->line_pblocks;
	for (i = 0; i < hc->quar_count; i++) {
		size_t tail = (hc->ring_head + hc->ring_count) %
			      hc->r_max_lines;

		BUG_ON(hc->ring_count >= hc->r_max_lines);
		hc->free_lines[tail] = hc->quar_lines[i];
		hc->ring_count++;
	}
	if (hc->quar_count)
		pr_info("dm-hyzns: [SHRINK abort] restored %zu quarantined lines\n",
			hc->quar_count);
	hc->quar_count = 0;
	spin_unlock_irqrestore(&hc->lock, flags);
	wake_up(&hc->alloc_wait);
	wake_up(&hc->gc_wait);
}

static int hyzns_prepare_shrink_force_gc(struct hyzns_c *hc,
					   sector_t new_r_pages)
{
	sector_t new_r_lines = new_r_pages / hc->line_pblocks;
	sector_t old_r_lines = (hc->r_end >> HYZNS_PAGE_SHIFT) /
			       hc->line_pblocks;
	unsigned long flags;
	sector_t line;
	int ret = 0;
	struct hyzns_gc_acc acc = {0};
	ktime_t t0 = ktime_get();
	u32 saved_gc_batch = READ_ONCE(hc->gc_batch);

	/* Step 1: prune out-of-range lines from the free ring and
	 * abandon the active write line if it sits past the new
	 * boundary. After this, hyzns_line_pop() can only hand out
	 * lines inside [0, new_r_lines). */
	spin_lock_irqsave(&hc->lock, flags);
	{
		size_t i, kept = 0;

		/* From here until commit (quiesce) or abort (restore), no
		 * line >= new_r_lines may re-enter the free ring - the
		 * erase-thread recycle of the very lines this force GC
		 * empties would otherwise hand them back to the allocator
		 * mid-drain. */
		hc->push_bound_lines = new_r_lines;
		hc->quar_count = 0;

		for (i = 0; i < hc->ring_count; i++) {
			size_t idx = (hc->ring_head + i) %
				     hc->r_max_lines;
			sector_t l = hc->free_lines[idx];

			if (l < new_r_lines)
				hc->free_lines[kept++] = l;
			else
				hc->quar_lines[hc->quar_count++] = l;
		}
		hc->ring_head  = 0;
		hc->ring_count = kept;

		if (hc->cur_line_active &&
		    hc->cur_line_idx >= new_r_lines) {
			hc->cur_line_active = false;
			hc->cur_line_off    = 0;
		}
		if (hc->gc_cur_line_active &&
		    hc->gc_cur_line_idx >= new_r_lines) {
			hc->gc_cur_line_active = false;
			hc->gc_cur_line_off    = 0;
		}
	}
	spin_unlock_irqrestore(&hc->lock, flags);

	/* Throttle the GC migration batch for the duration of the shrink drain so a
	 * live co-tenant keeps device bandwidth (min so a manually-lowered gc_batch is
	 * not raised). Restored at every return below. */
	WRITE_ONCE(hc->gc_batch, min(saved_gc_batch, READ_ONCE(hc->gc_batch_shrink)));

	/* Step 2: walk out-of-range lines top-down and force-evacuate
	 * any that still hold valid pblocks. */
	for (line = old_r_lines; line > new_r_lines; line--) {
		sector_t victim = line - 1;
		uint32_t v;

		spin_lock_irqsave(&hc->lock, flags);
		v = hc->line_valid[victim];
		spin_unlock_irqrestore(&hc->lock, flags);

		/* Position-driven eviction: a FULLY-valid line must be
		 * evacuated too (cold data that landed high) - skipping it
		 * would leave L2P entries pointing past the boundary for
		 * quiesce to silently drop. Only empty lines are skipped. */
		if (v == 0)
			continue;

		atomic64_inc(&hc->stats.gc_runs);
		ret = hyzns_do_gc_one_line_at(hc, victim, "SHRINK", &acc);
		spin_lock_irqsave(&hc->lock, flags);
		v = hc->line_valid[victim];
		spin_unlock_irqrestore(&hc->lock, flags);
		if (ret < 0 || v != 0) {
			/* Also catches the silent out-of-lines early exit in
			 * do_gc_one_line_at (returns migrated >= 0 with the
			 * victim only partially drained). Boundary stays. */
			DMWARN("shrink force-GC: line %llu evacuation failed (ret=%d valid_left=%u)",
			       (unsigned long long)victim, ret, v);
			WRITE_ONCE(hc->gc_batch, saved_gc_batch);   /* restore */
			hyzns_shrink_restore(hc);
			return ret < 0 ? ret : -ENOSPC;
		}
		cond_resched();
	}
	WRITE_ONCE(hc->gc_batch, saved_gc_batch);   /* restore autonomous batch */

	pr_info("dm-hyzns: [SHRINK force-GC] TOTAL lines=%d migrated_pages=%lld (%lld MiB) event_ns=%lld gc_wall_ns=%lld read_ns=%lld write_ns=%lld range=[%llu..%llu)\n",
		acc.lines, acc.pages, (acc.pages * 4) / 1024,
		ktime_to_ns(ktime_sub(ktime_get(), t0)),
		acc.wall_ns, acc.read_ns, acc.write_ns,
		(unsigned long long)new_r_lines, (unsigned long long)old_r_lines);
	return 0;
}

static int hyzns_gc_thread(void *data)
{
	struct hyzns_c *hc = data;

	while (!kthread_should_stop()) {
		wait_event_interruptible(hc->gc_wait,
			kthread_should_stop() ||
			(READ_ONCE(hc->ring_count) < hc->gc_low_watermark &&
			 !READ_ONCE(hc->gc_blocked)));

		if (kthread_should_stop())
			break;

		while (!kthread_should_stop() &&
		       READ_ONCE(hc->ring_count) < hc->gc_high_watermark) {
			int n = hyzns_do_gc_one_line(hc);

			/* n < 0  : no useful victim (every line is fully
			 *          valid or empty) - back off until a user
			 *          write recycles something.
			 * n == 0 : picked a victim but every migration was
			 *          skipped (raced with user writes that
			 *          remapped the lpages). Same - back off.
			 * Setting gc_blocked here parks us in wait_event;
			 * the user-write endio recycle path clears it.
			 */
			if (n <= 0) {
				pr_info("dm-hyzns: [GC] parked (no useful victim, n=%d, free_lines=%zu)\n",
					n, READ_ONCE(hc->ring_count));
				WRITE_ONCE(hc->gc_blocked, true);
				/* Writers stalled in map() are waiting for
				 * a line GC will now never free - let them
				 * observe gc_blocked and fail with ENOSPC. */
				wake_up(&hc->alloc_wait);
				break;
			}
			cond_resched();
		}
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Constructor / destructor
 * ------------------------------------------------------------------------- */

static int hyzns_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct hyzns_c *hc;
	unsigned long long tmp;
	char dummy;
	size_t l2p_bytes;
	sector_t r_max_end;        /* max-provisioned R boundary (sectors) */
	sector_t active_lines;     /* lines usable now = r_end / line */
	int ret;

	if (argc < 2 || argc > 3) {
		ti->error = "Usage: hyzns <dev> <r_end_lba> [max_r_end_lba]";
		return -EINVAL;
	}

	hc = kzalloc(sizeof(*hc), GFP_KERNEL);
	if (!hc) {
		ti->error = "Cannot allocate hyzns context";
		return -ENOMEM;
	}
	spin_lock_init(&hc->lock);

	if (sscanf(argv[1], "%llu%c", &tmp, &dummy) != 1) {
		ti->error = "Invalid r_end_lba";
		ret = -EINVAL;
		goto bad_arg;
	}
	hc->r_end = (sector_t)tmp;
	r_max_end = hc->r_end;

	/* Optional 3rd argument: the MAX-provisioned R boundary LBA. The dm
	 * sizes its L2P / reverse map / line rings for this maximum so that an
	 * online grow (F2FS ABA-modify, dmsetup set_r_end) can re-expose tail
	 * lines without reallocating - the dm-side analogue of F2FS's mkfs
	 * -A -P max-provision. The current usable boundary is r_end (<= max);
	 * the [r_end, max) lines map onto S-region zones until a grow hands
	 * them to R. If omitted (or < r_end), max defaults to r_end and the
	 * boundary cannot grow past it.
	 */
	if (argc == 3) {
		unsigned long long max_tmp;

		if (sscanf(argv[2], "%llu%c", &max_tmp, &dummy) != 1) {
			ti->error = "Invalid max_r_end_lba (3rd argument)";
			ret = -EINVAL;
			goto bad_arg;
		}
		if ((sector_t)max_tmp >= hc->r_end)
			r_max_end = (sector_t)max_tmp;
	}

	if ((hc->r_end & HYZNS_PAGE_MASK) || (r_max_end & HYZNS_PAGE_MASK)) {
		ti->error = "r_end / max_r_end must be page-aligned (multiple of 8 sectors)";
		ret = -EINVAL;
		goto bad_arg;
	}

	hc->line_pblocks = HYZNS_LINE_PBLOCKS;
	hc->zone_pages   = HYZNS_ZONE_PAGES;
	/* Physical capacity is max-provisioned; the active boundary is r_end.
	 * r_end / max_r_end must land on a ZONE boundary (1 GiB) - the unit F2FS
	 * and FEMU's hyzns_set_r_end agree on. Zone-aligned implies line-aligned
	 * because a zone is an integer number of 64 MiB lines, so the free-line
	 * ring math below stays exact. */
	hc->r_max_pages = r_max_end >> HYZNS_PAGE_SHIFT;
	if (hc->r_max_pages % hc->zone_pages ||
	    (hc->r_end >> HYZNS_PAGE_SHIFT) % hc->zone_pages) {
		ti->error = "r_end / max_r_end must be zone-aligned (pages % 262144 == 0, i.e. 1 GiB)";
		ret = -EINVAL;
		goto bad_arg;
	}
	hc->r_max_lines = hc->r_max_pages / hc->line_pblocks;
	active_lines = (hc->r_end >> HYZNS_PAGE_SHIFT) / hc->line_pblocks;
	hc->cur_line_active = false;
	hc->cur_line_off    = 0;
	hc->gc_cur_line_active = false;
	hc->gc_cur_line_off    = 0;

	if (hc->r_max_pages) {
		size_t i;
		size_t free_lines_bytes;
		size_t line_valid_bytes;
		size_t rmap_bytes;

		l2p_bytes = (size_t)hc->r_max_pages * sizeof(sector_t);
		hc->l2p = vmalloc(l2p_bytes);
		if (!hc->l2p) {
			ti->error = "Cannot allocate L2P table";
			ret = -ENOMEM;
			goto bad_arg;
		}
		/* HYZNS_INVALID_PBA == ~0 → memset 0xff initialises the
		 * whole table to "unmapped" in one pass. */
		memset(hc->l2p, 0xff, l2p_bytes);

		free_lines_bytes = (size_t)hc->r_max_lines * sizeof(sector_t);
		hc->free_lines = vmalloc(free_lines_bytes);
		if (!hc->free_lines) {
			ti->error = "Cannot allocate free-line ring";
			ret = -ENOMEM;
			goto bad_l2p;
		}
		/* Seed only the ACTIVE lines [0, active_lines) - these are the
		 * R-region zones usable now. The tail [active_lines, r_max_lines)
		 * is reserved for grow and belongs to S until then; it must NOT
		 * be on the free ring or the allocator would place R data into
		 * the S-region. Grow (hyzns_quiesce_grow) pushes those lines on
		 * later. Allocations pop from head; fully-invalidated lines push
		 * back to tail. */
		for (i = 0; i < active_lines; i++)
			hc->free_lines[i] = (sector_t)i;
		hc->ring_head  = 0;
		hc->ring_count = active_lines;
		hc->push_bound_lines = active_lines;

		/* Quarantine parking lot for shrink (same shape as the free
		 * ring; holds out-of-bound pushes between force-GC start and
		 * commit/abort). */
		hc->quar_lines = vmalloc(free_lines_bytes);
		if (!hc->quar_lines) {
			ti->error = "Cannot allocate shrink quarantine list";
			ret = -ENOMEM;
			goto bad_ring;
		}
		hc->quar_count = 0;

		line_valid_bytes = (size_t)hc->r_max_lines * sizeof(uint32_t);
		hc->line_valid = vmalloc(line_valid_bytes);
		if (!hc->line_valid) {
			ti->error = "Cannot allocate line_valid array";
			ret = -ENOMEM;
			goto bad_quar;
		}
		memset(hc->line_valid, 0, line_valid_bytes);

		rmap_bytes = l2p_bytes;
		hc->rmap = vmalloc(rmap_bytes);
		if (!hc->rmap) {
			ti->error = "Cannot allocate reverse map";
			ret = -ENOMEM;
			goto bad_line_valid;
		}
		memset(hc->rmap, 0xff, rmap_bytes);

		/* Dirty-line ring. Same shape as free_lines but starts empty -
		 * lines flow into it when recycled, the erase kthread drains
		 * them. */
		hc->dirty_lines = vmalloc(free_lines_bytes);
		if (!hc->dirty_lines) {
			ti->error = "Cannot allocate dirty-line ring";
			ret = -ENOMEM;
			goto bad_rmap;
		}
		hc->dirty_head  = 0;
		hc->dirty_count = 0;

		/* Per-line birth jiffies for Cost-Benefit GC. */
		hc->line_birth = vmalloc((size_t)hc->r_max_lines *
					 sizeof(unsigned long));
		if (!hc->line_birth) {
			ti->error = "Cannot allocate line_birth array";
			ret = -ENOMEM;
			goto bad_dirty;
		}
		{
			size_t k;
			unsigned long now = jiffies;
			for (k = 0; k < hc->r_max_lines; k++)
				hc->line_birth[k] = now;
		}
	}
	hc->gc_policy = HYZNS_GC_GREEDY;
	hc->gc_batch        = HYZNS_GC_BATCH_DEFAULT;  /* drain knee */
	hc->gc_batch_shrink = HYZNS_GC_BATCH_SHRINK;   /* below the co-tenant cliff */

	/* GC watermarks, seeded from the initial R and recomputed on every
	 * resize so the band scales with the live R, not the provisioned max
	 * (see hyzns_set_watermarks). */
	hyzns_set_watermarks(hc, active_lines);
	init_waitqueue_head(&hc->gc_wait);
	init_waitqueue_head(&hc->erase_wait);
	init_waitqueue_head(&hc->alloc_wait);
	hc->gc_blocked = false;
	mutex_init(&hc->gc_io_lock);

	ret = hyzns_gc_items_alloc(hc);
	if (ret) {
		ti->error = "Cannot allocate GC migration batch";
		goto bad_gc_items;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &hc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad_dev;
	}

	ti->num_flush_bios        = 1;
	/* Accept discards so upper-filesystem deletes (F2FS checkpoint
	 * discard, fstrim) invalidate our L2P instead of leaving dead-but-
	 * valid pages for GC to migrate. discards_supported forces queue-
	 * level support even if the backing device didn't advertise DSM -
	 * the bio is absorbed in map() and never reaches the device. */
	ti->num_discard_bios      = 1;
	ti->discards_supported    = true;
	ti->num_write_zeroes_bios = 0;
	ti->per_io_data_size      = sizeof(struct hyzns_pio);
	ti->private               = hc;

	hc->erase_thread = kthread_run(hyzns_erase_thread, hc,
				       "hyzns-erase/%s", hc->dev->name);
	if (IS_ERR(hc->erase_thread)) {
		ret = PTR_ERR(hc->erase_thread);
		hc->erase_thread = NULL;
		ti->error = "Cannot start erase thread";
		dm_put_device(ti, hc->dev);
		goto bad_dev;
	}

	hc->gc_thread = kthread_run(hyzns_gc_thread, hc, "hyzns-gc/%s",
				    hc->dev->name);
	if (IS_ERR(hc->gc_thread)) {
		ret = PTR_ERR(hc->gc_thread);
		hc->gc_thread = NULL;
		ti->error = "Cannot start GC thread";
		kthread_stop(hc->erase_thread);
		hc->erase_thread = NULL;
		dm_put_device(ti, hc->dev);
		goto bad_dev;
	}

	/* Ordered single-thread workqueue for ZONE_MODIFY processing.
	 * Ordered so two concurrent F2FS ioctl modify_zone calls serialize;
	 * single-thread so we never race two boundary changes against each
	 * other. WQ_MEM_RECLAIM because the worker calls submit_bio_wait
	 * which may be on the writeback path. */
	hc->zmod_wq = alloc_ordered_workqueue("hyzns-zmod/%s",
					      WQ_MEM_RECLAIM, hc->dev->name);
	if (!hc->zmod_wq) {
		ret = -ENOMEM;
		ti->error = "Cannot allocate zmod workqueue";
		kthread_stop(hc->gc_thread);
		hc->gc_thread = NULL;
		kthread_stop(hc->erase_thread);
		hc->erase_thread = NULL;
		dm_put_device(ti, hc->dev);
		goto bad_dev;
	}

	DMINFO("ctr: backing=%s r_end=%llu len=%llu r_pages=%llu r_lines=%llu line_pblocks=%u l2p=%lluMiB gc_wm=%zu/%zu",
	       argv[0],
	       (unsigned long long)hc->r_end,
	       (unsigned long long)ti->len,
	       (unsigned long long)hc->r_max_pages,
	       (unsigned long long)hc->r_max_lines,
	       hc->line_pblocks,
	       (unsigned long long)((hc->r_max_pages * sizeof(sector_t)) >> 20),
	       hc->gc_low_watermark, hc->gc_high_watermark);
	return 0;

bad_dev:
	hyzns_gc_items_free(hc);
bad_gc_items:
	vfree(hc->line_birth);
bad_dirty:
	vfree(hc->dirty_lines);
bad_rmap:
	vfree(hc->rmap);
bad_line_valid:
	vfree(hc->line_valid);
bad_quar:
	vfree(hc->quar_lines);
bad_ring:
	vfree(hc->free_lines);
bad_l2p:
	vfree(hc->l2p);
bad_arg:
	kfree(hc);
	return ret;
}

static void hyzns_dtr(struct dm_target *ti)
{
	struct hyzns_c *hc = ti->private;

	/* Drain in-flight ZONE_MODIFY work before stopping GC/erase threads.
	 * The worker may still be holding pending references to L2P / rings
	 * which the kthread_stop teardown below will free. */
	if (hc->zmod_wq) {
		destroy_workqueue(hc->zmod_wq);
		hc->zmod_wq = NULL;
	}
	if (hc->gc_thread) {
		kthread_stop(hc->gc_thread);
		hc->gc_thread = NULL;
	}
	if (hc->erase_thread) {
		kthread_stop(hc->erase_thread);
		hc->erase_thread = NULL;
	}
	dm_put_device(ti, hc->dev);
	hyzns_gc_items_free(hc);
	vfree(hc->line_birth);
	vfree(hc->dirty_lines);
	vfree(hc->rmap);
	vfree(hc->line_valid);
	vfree(hc->quar_lines);
	vfree(hc->free_lines);
	vfree(hc->l2p);
	kfree(hc);
}

/* -------------------------------------------------------------------------
 * IO mapping
 * ------------------------------------------------------------------------- */

static int hyzns_map_r_page(struct hyzns_c *hc, struct bio *bio,
			     sector_t bio_lba)
{
	sector_t lpage = bio_lba >> HYZNS_PAGE_SHIFT;
	sector_t pblock_lba;

	if (op_is_write(bio_op(bio))) {
		sector_t new_pblock = 0;
		bool inplace = false;
		bool wake_gc = false;
		bool stall = false;
		bool stalled = false;
		struct hyzns_pio *pio;
		unsigned long flags;

retry_alloc:
		spin_lock_irqsave(&hc->lock, flags);

		/* Need a fresh pblock from the current write line; pop a new
		 * line if the current one is exhausted (or unset). User writes
		 * are gated on ring_count > gc_reserve_lines so GC keeps a
		 * private pool to migrate into. */
		if (!hc->cur_line_active ||
		    hc->cur_line_off >= hc->line_pblocks) {
			if (hc->ring_count > hc->gc_reserve_lines &&
			    hyzns_line_pop(hc, &hc->cur_line_idx) == 0) {
				hc->cur_line_off    = 0;
				hc->cur_line_active = true;
			} else if (hc->l2p[lpage] != HYZNS_INVALID_PBA) {
				/* Ring exhausted, no current line, but the
				 * lpage is already mapped → fall back to
				 * in-place. The L2P stays the same, so the
				 * write goes to the existing pblock. No
				 * endio swap needed; line_valid unchanged.
				 */
				new_pblock = hc->l2p[lpage];
				inplace    = true;
			} else {
				/* No free line and unmapped lpage. Wake GC
				 * (via the watermark check below) and stall
				 * until a line frees up. */
				stall = true;
			}
		}

		if (!stall && !inplace) {
			/* Out-of-place: reserve the next pblock in the
			 * current line. line_valid is incremented at alloc
			 * (= "reserved"); the L2P swap and the displacement
			 * of the previous pblock happen at endio so a
			 * concurrent reader of `lpage` keeps seeing the old
			 * mapping until the new data has actually landed.
			 */
			new_pblock = hc->cur_line_idx * hc->line_pblocks +
				     hc->cur_line_off;
			hc->cur_line_off++;
			hc->line_valid[hc->cur_line_idx]++;
		}

		if (hc->ring_count < hc->gc_low_watermark)
			wake_gc = true;
		spin_unlock_irqrestore(&hc->lock, flags);

		if (wake_gc)
			wake_up(&hc->gc_wait);

		if (stall) {
			/* Block the submitter (map() runs in process
			 * context, sleeping is allowed for bio-based
			 * targets) until allocation becomes possible: the
			 * erase kthread recycles a line onto the ring, a
			 * concurrent writer pops a fresh cur line, or a
			 * grow adds lines. (GC's destination line is
			 * private - gc_cur - and never user-allocatable.)
			 * Fail with ENOSPC only when we already waited
			 * once, re-checked under the lock, and GC is
			 * parked victimless - at that point nothing will
			 * free a line until a discard or a displacing
			 * overwrite arrives, and the wait would be
			 * unbounded. */
			if (bio->bi_opf & REQ_NOWAIT) {
				bio_wouldblock_error(bio);
				return DM_MAPIO_SUBMITTED;
			}
			/* gc_blocked means GC can't help. That is terminal ONLY if R
			 * is also at its max-provisioned size: with r_end < max, a
			 * GrowCNS can still add lines any moment, so keep sleeping
			 * (grow commit / discard-recycle wake alloc_wait) instead of
			 * failing the writer. Static (non-provisioned) targets keep
			 * the old fail-fast ENOSPC. */
			if (stalled && READ_ONCE(hc->gc_blocked) &&
			    (READ_ONCE(hc->r_end) >> HYZNS_PAGE_SHIFT) >= hc->r_max_pages) {
				atomic64_inc(&hc->stats.r_nospc);
				bio->bi_status = BLK_STS_NOSPC;
				bio_endio(bio);
				return DM_MAPIO_SUBMITTED;
			}
			atomic64_inc(&hc->stats.map_requeues);
			/* killable + bounded: a growable target waits for GrowCNS, but
			 * if the whole stack is wedged (S can't donate a zone, F2FS GC
			 * can't write, discards never arrive) an unbounded wait_event
			 * leaves the writer in unkillable D-state forever. 60 s with
			 * no line and no grow => ENOSPC like the static case; SIGKILL
			 * is honored immediately. */
			{
				long wr = wait_event_killable_timeout(hc->alloc_wait,
					   (READ_ONCE(hc->cur_line_active) &&
					    READ_ONCE(hc->cur_line_off) <
							hc->line_pblocks) ||
					   READ_ONCE(hc->ring_count) >
						   hc->gc_reserve_lines ||
					   (READ_ONCE(hc->gc_blocked) &&
					    (READ_ONCE(hc->r_end) >> HYZNS_PAGE_SHIFT) >=
							hc->r_max_pages),
					   60 * HZ);
				if (wr <= 0) {
					atomic64_inc(&hc->stats.r_nospc);
					bio->bi_status = BLK_STS_NOSPC;
					bio_endio(bio);
					return DM_MAPIO_SUBMITTED;
				}
			}
			stalled = true;
			stall   = false;
			wake_gc = false;
			goto retry_alloc;
		}

		if (inplace) {
			atomic64_inc(&hc->stats.r_writes);
			atomic64_inc(&hc->stats.r_overwrites);
			atomic64_inc(&hc->stats.r_inplace);
			/* No endio commit needed; pio stays at OP_NONE. */
		} else {
			pio = dm_per_bio_data(bio, sizeof(struct hyzns_pio));
			pio->op         = HYZNS_OP_R_WRITE;
			pio->lpage      = lpage;
			pio->new_pblock = new_pblock;
			pio->n_pages    = 1;
			/* Stats incremented at endio so failures don't
			 * inflate the success counters. */
		}
		pblock_lba = new_pblock << HYZNS_PAGE_SHIFT;
	} else if (bio_op(bio) == REQ_OP_READ) {
		sector_t mapping = READ_ONCE(hc->l2p[lpage]);

		atomic64_inc(&hc->stats.r_reads);
		if (mapping == HYZNS_INVALID_PBA) {
			atomic64_inc(&hc->stats.r_reads_unmapped);
			zero_fill_bio(bio);
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		pblock_lba = mapping << HYZNS_PAGE_SHIFT;
	} else {
		/* write_zeroes / unknown ops are not translated (discards were
		 * already absorbed in hyzns_map()); stay defensive in case
		 * the op table grows.
		 */
		atomic64_inc(&hc->stats.r_align_rejects);
		bio->bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	bio->bi_iter.bi_sector = pblock_lba;
	return DM_MAPIO_REMAPPED;
}

/* Multi-page R-region write fast path. Reserve a CONTIGUOUS run of pblocks
 * from the current write line (pblocks within a line are allocated in
 * index order, so the run maps to one contiguous backing LBA range and the
 * device stripes it across (ch, lun) in parallel - the legacy per-page
 * dm_accept_partial_bio path serialised this into one bio per 4 KiB).
 *
 * The run is capped to the current line (so all reserved pblocks share one
 * line_valid counter); if the bio is longer, dm core reissues the tail and
 * it lands here again on the next line. The L2P swap for every page in the
 * run is still deferred to .end_io (read-after-write safety preserved).
 *
 * Out-of-place only: if no fresh line is available (ring exhausted), this
 * falls back to the single-page path which handles in-place / stall. */
static int hyzns_map_r_write(struct hyzns_c *hc, struct bio *bio,
			      sector_t bio_lba, unsigned int n)
{
	sector_t lpage  = bio_lba >> HYZNS_PAGE_SHIFT;
	unsigned int npages = n >> HYZNS_PAGE_SHIFT;
	unsigned int k = 0;
	sector_t pblock_start = 0;
	bool wake_gc = false;
	struct hyzns_pio *pio;
	unsigned long flags;

	spin_lock_irqsave(&hc->lock, flags);
	if (!hc->cur_line_active || hc->cur_line_off >= hc->line_pblocks) {
		if (hc->ring_count > hc->gc_reserve_lines &&
		    hyzns_line_pop(hc, &hc->cur_line_idx) == 0) {
			hc->cur_line_off    = 0;
			hc->cur_line_active = true;
		}
	}
	if (hc->cur_line_active && hc->cur_line_off < hc->line_pblocks) {
		k = hc->line_pblocks - hc->cur_line_off;
		if (k > npages)
			k = npages;
		pblock_start = hc->cur_line_idx * hc->line_pblocks +
			       hc->cur_line_off;
		hc->cur_line_off                  += k;
		hc->line_valid[hc->cur_line_idx]  += k;
	}
	if (hc->ring_count < hc->gc_low_watermark)
		wake_gc = true;
	spin_unlock_irqrestore(&hc->lock, flags);

	if (wake_gc)
		wake_up(&hc->gc_wait);

	/* No fresh line - degrade to the single-page path (in-place / stall). */
	if (k == 0) {
		if (n > HYZNS_PAGE_SECTORS)
			dm_accept_partial_bio(bio, HYZNS_PAGE_SECTORS);
		return hyzns_map_r_page(hc, bio, bio_lba);
	}

	/* Reserved fewer pages than the bio holds (line boundary): let dm
	 * core reissue the remainder, which re-enters .map() on the next line. */
	if (k < npages) {
		dm_accept_partial_bio(bio, (unsigned int)(k << HYZNS_PAGE_SHIFT));
		atomic64_inc(&hc->stats.splits);
	}

	pio = dm_per_bio_data(bio, sizeof(*pio));
	pio->op         = HYZNS_OP_R_WRITE;
	pio->lpage      = lpage;
	pio->new_pblock = pblock_start;
	pio->n_pages    = k;

	bio->bi_iter.bi_sector = pblock_start << HYZNS_PAGE_SHIFT;
	return DM_MAPIO_REMAPPED;
}

/* End-io for the cloned bio used to forward REQ_OP_ZONE_MODIFY to the
 * backing device. Runs in IRQ / softirq context - keep it light, lock
 * with spin_lock_irqsave, no sleeping. Commits the dm-side r_end on
 * device success and ends the original bio with the device's status. */
/* Workqueue worker for F2FS-ioctl-driven REQ_OP_ZONE_MODIFY. Runs in
 * process context so it can (1) execute the position-driven shrink
 * force GC (which sleeps in migrate_data) before issuing the device
 * command, (2) submit the actual ModifyZone bio synchronously, and
 * (3) commit dm-side r_end + quiesce. The original bio from F2FS is
 * only completed at the very end, so F2FS's submit_bio_wait stays
 * blocked until everything is durable.
 *
 * The dmsetup `set_r_end` path follows the same sequence but inline
 * because it already runs in process context (message callback).
 */
static void hyzns_zmod_worker(struct work_struct *work)
{
	struct hyzns_zmod_ctx *ctx =
		container_of(work, struct hyzns_zmod_ctx, work);
	struct hyzns_c *hc = ctx->hc;
	struct bio *orig = ctx->orig;
	struct bio *clone;
	sector_t cur_r_end;
	unsigned long flags;
	bool is_shrink;
	blk_status_t status = BLK_STS_OK;
	int ret;

	/* zmod stage timing - breaks the F2FS-side "device ABA" number down into
	 * force-GC vs device submit (FEMU ZSA 0x20) vs dm quiesce, so a large
	 * device-ABA with zero force-GC migration can be attributed correctly. */
	ktime_t z_t0 = ktime_get(), z_gc, z_sub, z_q;

	spin_lock_irqsave(&hc->lock, flags);
	cur_r_end = hc->r_end;
	spin_unlock_irqrestore(&hc->lock, flags);
	is_shrink = ctx->new_r_end_lba < cur_r_end;

	if (is_shrink) {
		ret = hyzns_prepare_shrink_force_gc(hc, ctx->new_r_pages);
		if (ret) {
			DMWARN("ZONE_MODIFY shrink force-GC failed (%d); boundary unchanged",
			       ret);
			status = BLK_STS_IOERR;
			goto done;
		}
	}
	z_gc = ktime_get();

	clone = bio_alloc(GFP_NOIO, 0);
	if (!clone) {
		DMWARN("ZONE_MODIFY clone alloc failed");
		if (is_shrink)
			hyzns_shrink_restore(hc);
		status = BLK_STS_RESOURCE;
		goto done;
	}
	bio_set_dev(clone, hc->dev->bdev);
	clone->bi_opf = REQ_OP_ZONE_MODIFY | REQ_SYNC;
	clone->bi_iter.bi_sector = ctx->new_r_end_lba;
	ret = submit_bio_wait(clone);
	bio_put(clone);
	if (ret) {
		DMWARN("ZONE_MODIFY backing rejected (ret=%d); dm state unchanged",
		       ret);
		if (is_shrink)
			hyzns_shrink_restore(hc);
		status = BLK_STS_IOERR;
		goto done;
	}
	z_sub = ktime_get();

	/* Device accepted - commit dm-side state. */
	spin_lock_irqsave(&hc->lock, flags);
	if (is_shrink)
		hyzns_quiesce_shrink(hc, ctx->new_r_pages);
	else if (ctx->new_r_end_lba > hc->r_end)
		hyzns_quiesce_grow(hc, ctx->new_r_pages);
	WRITE_ONCE(hc->r_end, ctx->new_r_end_lba);
	hyzns_set_watermarks(hc, ctx->new_r_pages / hc->line_pblocks); /* scale GC band to new R */
	spin_unlock_irqrestore(&hc->lock, flags);
	/* A grow pushed reclaimed lines onto the free ring. */
	wake_up(&hc->alloc_wait);
	z_q = ktime_get();

	pr_info("dm-hyzns: [ZMOD %s] forcegc=%lld us device_submit(ZSA)=%lld us quiesce=%lld us total=%lld us\n",
		is_shrink ? "shrink" : "grow",
		ktime_to_ns(ktime_sub(z_gc,  z_t0)) / 1000,
		ktime_to_ns(ktime_sub(z_sub, z_gc)) / 1000,
		ktime_to_ns(ktime_sub(z_q,   z_sub)) / 1000,
		ktime_to_ns(ktime_sub(z_q,   z_t0)) / 1000);

	DMINFO("ZONE_MODIFY (.map worker) %llu -> %llu",
	       (unsigned long long)cur_r_end,
	       (unsigned long long)ctx->new_r_end_lba);

done:
	orig->bi_status = status;
	bio_endio(orig);
	kfree(ctx);
}

static int hyzns_map(struct dm_target *ti, struct bio *bio)
{
	struct hyzns_c *hc = ti->private;
	struct hyzns_pio *pio;
	sector_t bio_lba, bio_end, r_end;
	unsigned int n;

	/* All bios get a per-io context; the R-region write path fills it
	 * in to defer the L2P swap to endio. Default = OP_NONE so .end_io
	 * does nothing for reads / S-region IO / in-place writes. */
	pio = dm_per_bio_data(bio, sizeof(struct hyzns_pio));
	pio->op = HYZNS_OP_NONE;

	bio_set_dev(bio, hc->dev->bdev);
	bio_lba = dm_target_offset(ti, bio->bi_iter.bi_sector);
	n = bio_sectors(bio);

	/* REQ_OP_ZONE_MODIFY arriving via .map() (e.g. F2FS ioctl #28
	 * f2fs_modify_zone_ratio) must NOT just be forwarded - the
	 * device-side r_end would change but the dm-side L2P / free ring
	 * stays at the old boundary, leaving L2P entries pointing at
	 * pblocks now in the S-region. The work needed to keep both
	 * sides consistent (shrink force GC + device submit + quiesce)
	 * requires process context and sleeping, so we hand it off to
	 * the zmod workqueue and return DM_MAPIO_SUBMITTED. The original
	 * bio is held until the worker completes, so F2FS's
	 * submit_bio_wait stays blocked end-to-end.
	 *
	 * bi_sector carries the new ABA LBA directly (= new first S-zone's
	 * zslba). The dmsetup-message set_r_end path uses the same wire
	 * encoding and doesn't traverse .map(); its handler runs the same
	 * sequence inline. */
	if (bio_op(bio) == REQ_OP_ZONE_MODIFY) {
		sector_t new_r_end_lba = bio->bi_iter.bi_sector;
		sector_t new_r_pages;
		struct hyzns_zmod_ctx *ctx;

		if (new_r_end_lba & HYZNS_PAGE_MASK) {
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		new_r_pages = new_r_end_lba >> HYZNS_PAGE_SHIFT;
		/* r_end moves zone-granular (1 GiB), matching FEMU's hyzns_set_r_end
		 * (% zone_size). A mid-zone boundary would leave a zone split between
		 * R and S semantics and the device would reject it anyway; reject
		 * early so the shrink/grow quiesce always sees a clean zone edge. */
		if (new_r_pages % hc->zone_pages) {
			bio->bi_status = BLK_STS_NOTSUPP;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		if (new_r_pages > hc->r_max_pages) {
			bio->bi_status = BLK_STS_IOERR;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}

		ctx = kzalloc(sizeof(*ctx), GFP_NOIO);
		if (!ctx) {
			bio->bi_status = BLK_STS_RESOURCE;
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
		ctx->hc = hc;
		ctx->orig = bio;
		ctx->new_r_end_lba = new_r_end_lba;
		ctx->new_r_pages = new_r_pages;
		INIT_WORK(&ctx->work, hyzns_zmod_worker);
		queue_work(hc->zmod_wq, &ctx->work);
		return DM_MAPIO_SUBMITTED;
	}

	/* BLKREPORTABA (REQ_OP_ZONE_RRZONE): answer locally from our own
	 * r_end instead of forwarding. Forwarding cannot work - dm core
	 * clones zone-mgmt bios with the length clamped to 0, stripping
	 * the 4-byte response buffer, so the op reaches the device with
	 * phys_seg 0 and fails. And the dm layer owns the authoritative
	 * boundary for the stack above it anyway (set_r_end keeps the
	 * device in sync). The clone still shares the submitter's bio_vec
	 * table (__bio_clone_fast), so the response page is reachable even
	 * though bi_iter.bi_size was clamped.
	 */
	if (bio_op(bio) == REQ_OP_ZONE_RRZONE) {
		struct bio_vec *bv = bio->bi_io_vec;
		sector_t zone_sectors = bdev_zone_sectors(hc->dev->bdev);

		if (bv && bv->bv_page && zone_sectors &&
		    bv->bv_len >= sizeof(u32)) {
			u32 nr_rzones = div64_u64(READ_ONCE(hc->r_end),
						  zone_sectors);

			memcpy(page_address(bv->bv_page) + bv->bv_offset,
			       &nr_rzones, sizeof(u32));
			bio->bi_status = BLK_STS_OK;
		} else {
			bio->bi_status = BLK_STS_NOTSUPP;
		}
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* Empty / pure-flush / zone-reset-all bios cannot be split via
	 * dm_accept_partial_bio(); just forward them with bi_sector
	 * remapped if the op carries an LBA target.
	 */
	if (n == 0 || bio_op(bio) == REQ_OP_ZONE_RESET_ALL ||
	    (bio->bi_opf & REQ_PREFLUSH)) {
		if (n || op_is_zone_mgmt(bio_op(bio)))
			bio->bi_iter.bi_sector = bio_lba;
		return DM_MAPIO_REMAPPED;
	}

	r_end = READ_ONCE(hc->r_end);
	bio_end = bio_lba + n;

	/* REQ_OP_DISCARD: absorb entirely at the dm layer. Must be handled
	 * before the generic R-path below - REQ_OP_DISCARD is an odd opcode
	 * so op_is_write() is true for it, and letting it fall through would
	 * make the R-write path allocate a pblock for a payload-less bio.
	 * The range is invalidated in the L2P (R-region part only) and the
	 * bio completes here; nothing is sent to the device. */
	if (bio_op(bio) == REQ_OP_DISCARD) {
		atomic64_inc(&hc->stats.discards);
		hyzns_discard_range(hc, bio_lba, n, r_end);
		bio->bi_status = BLK_STS_OK;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* R/S boundary cross: trim head, let dm reissue the tail. */
	if (bio_lba < r_end && bio_end > r_end) {
		unsigned int head = (unsigned int)(r_end - bio_lba);

		dm_accept_partial_bio(bio, head);
		atomic64_inc(&hc->stats.splits);
		bio_end = bio_lba + head;
		n = head;
	}

	/* S-region: pure passthrough. */
	if (bio_lba >= r_end) {
		atomic64_inc(&hc->stats.s_bios);
		bio->bi_iter.bi_sector = bio_lba;
		return DM_MAPIO_REMAPPED;
	}

	/* R-region from here on. */
	atomic64_inc(&hc->stats.r_bios);

	/* Reject sub-page or unaligned IO. */
	if ((bio_lba & HYZNS_PAGE_MASK) || (n & HYZNS_PAGE_MASK)) {
		atomic64_inc(&hc->stats.r_align_rejects);
		bio->bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);
		return DM_MAPIO_SUBMITTED;
	}

	/* Writes: coalesce a multi-page bio into one contiguous pblock run
	 * (one backing bio → device-parallel). Reads (and the !r_coalesce
	 * legacy path) still split per page, since a page's pblock is
	 * scattered by prior out-of-place writes and can't be remapped as a
	 * contiguous range. dm core resubmits the unconsumed tail back
	 * through .map() with bio_lba advanced. */
	if (r_coalesce && op_is_write(bio_op(bio)))
		return hyzns_map_r_write(hc, bio, bio_lba, n);

	if (n > HYZNS_PAGE_SECTORS)
		dm_accept_partial_bio(bio, HYZNS_PAGE_SECTORS);

	return hyzns_map_r_page(hc, bio, bio_lba);
}

/* End-of-IO callback. Runs after the target's bio finishes; commits
 * the L2P swap for R-region out-of-place writes (and rolls back on
 * failure). dm core invokes this for every bio that .map() returned
 * DM_MAPIO_REMAPPED for, so the OP_NONE early-out is the common case.
 */
static int hyzns_end_io(struct dm_target *ti, struct bio *bio,
			 blk_status_t *error)
{
	struct hyzns_c   *hc  = ti->private;
	struct hyzns_pio *pio = dm_per_bio_data(bio, sizeof(*pio));
	sector_t lpage, new_pblock, new_line;
	unsigned int npages, i;

	if (pio->op != HYZNS_OP_R_WRITE)
		return DM_ENDIO_DONE;

	lpage      = pio->lpage;
	new_pblock = pio->new_pblock;
	npages     = pio->n_pages;            /* contiguous run, all in one line */
	new_line   = new_pblock / hc->line_pblocks;

	if (*error == BLK_STS_OK) {
		sector_t old, old_line;
		unsigned long flags;
		bool recycled = false;

		spin_lock_irqsave(&hc->lock, flags);
		for (i = 0; i < npages; i++) {
			old = hc->l2p[lpage + i];
			hc->l2p[lpage + i]       = new_pblock + i;
			hc->rmap[new_pblock + i] = lpage + i;
			if (old != HYZNS_INVALID_PBA) {
				old_line = old / hc->line_pblocks;
				BUG_ON(hc->line_valid[old_line] == 0);
				hc->line_valid[old_line]--;
				hc->rmap[old] = HYZNS_INVALID_LPA;
				if (hc->line_valid[old_line] == 0 &&
				    !hyzns_line_is_open(hc, old_line)) {
					hyzns_line_push_dirty(hc, old_line);
					atomic64_inc(&hc->stats.line_recycles);
					recycled = true;
				}
				/* This displacement may have created the first
				 * GC-eligible victim since the GC thread parked
				 * (gc_blocked is only otherwise cleared on a full
				 * line recycle, which the in-place fallback regime
				 * can starve indefinitely). Re-arm GC; if there is
				 * still no victim it just parks again. */
				WRITE_ONCE(hc->gc_blocked, false);
				atomic64_inc(&hc->stats.r_overwrites);
			}
		}
		spin_unlock_irqrestore(&hc->lock, flags);

		atomic64_add(npages, &hc->stats.r_writes);

		if (recycled)
			wake_up(&hc->erase_wait);
		else if (READ_ONCE(hc->ring_count) < hc->gc_low_watermark)
			wake_up(&hc->gc_wait);
	} else {
		unsigned long flags;
		bool recycled = false;

		/* Backing write failed. Roll back the line_valid reservation
		 * we did at .map() time (whole run is in one line). L2P was
		 * never updated, so the lpages still map to whatever they did
		 * before - no consistency issue. The user sees the original
		 * failure status.
		 */
		spin_lock_irqsave(&hc->lock, flags);
		BUG_ON(hc->line_valid[new_line] < npages);
		hc->line_valid[new_line] -= npages;
		if (hc->line_valid[new_line] == 0 &&
		    !hyzns_line_is_open(hc, new_line)) {
			hyzns_line_push_dirty(hc, new_line);
			atomic64_inc(&hc->stats.line_recycles);
			recycled = true;
		}
		spin_unlock_irqrestore(&hc->lock, flags);
		atomic64_add(npages, &hc->stats.r_write_failed);

		if (recycled)
			wake_up(&hc->erase_wait);
	}

	return DM_ENDIO_DONE;
}

/* -------------------------------------------------------------------------
 * Plumbing required by dm core
 * ------------------------------------------------------------------------- */

static int hyzns_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct hyzns_c *hc = ti->private;

	return fn(ti, hc->dev, 0, ti->len, data);
}

/* Discards are absorbed in map() at FTL-page granularity and never reach
 * the backing device, so the limits must be ours, not the stacked ones.
 * With the backing device's 512B granularity, blk_bio_discard_split()
 * cuts large discards at max_discard_sectors (8388607 sectors - not
 * page-aligned), and hyzns_discard_range()'s inward rounding then leaks
 * one untrimmed page per split boundary. Page-sized granularity makes
 * every split page-aligned. */
static void hyzns_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	limits->discard_granularity = HYZNS_PAGE_BYTES;
	limits->max_discard_sectors = UINT_MAX & ~HYZNS_PAGE_MASK;
	limits->max_hw_discard_sectors = UINT_MAX & ~HYZNS_PAGE_MASK;
}

static int hyzns_prepare_ioctl(struct dm_target *ti, struct block_device **bdev)
{
	struct hyzns_c *hc = ti->private;

	*bdev = hc->dev->bdev;
	if (ti->len != i_size_read(hc->dev->bdev->bd_inode) >> SECTOR_SHIFT)
		return 1;
	return 0;
}

#ifdef CONFIG_BLK_DEV_ZONED
static int hyzns_report_zones(struct dm_target *ti,
			       struct dm_report_zones_args *args,
			       unsigned int nr_zones)
{
	struct hyzns_c *hc = ti->private;

	return dm_report_zones(hc->dev->bdev, 0, args->next_sector,
			       args, nr_zones);
}
#else
#define hyzns_report_zones NULL
#endif

static void hyzns_status(struct dm_target *ti, status_type_t type,
			  unsigned int status_flags, char *result,
			  unsigned int maxlen)
{
	struct hyzns_c *hc = ti->private;
	size_t sz = 0;

	switch (type) {
	case STATUSTYPE_INFO: {
		size_t   free_lines;
		size_t   dirty_lines_count;
		bool     cur_active, gc_active;
		unsigned int cur_off, gc_off;
		unsigned long flags;
			u64 valid_sum = 0, cur_lines, free_pages;
			u32 vi, vh[5] = {0, 0, 0, 0, 0};
			u32 bv_valid = ~0u;   /* min valid among used lines = greedy GC victim */
			u32 bv_vpc = 0;       /* that victim's valid% (GC cost proxy)          */

		spin_lock_irqsave(&hc->lock, flags);
		free_lines = hc->ring_count;
		dirty_lines_count = hc->dirty_count;
		cur_active = hc->cur_line_active;
		cur_off    = cur_active ? hc->cur_line_off : 0;
		gc_active  = hc->gc_cur_line_active;
		gc_off     = gc_active ? hc->gc_cur_line_off : 0;
		spin_unlock_irqrestore(&hc->lock, flags);

		/* R_C = real free blocks = R_blocks - (valid + invalid) = truly-unwritten
		 * pblocks: whole free-ring lines + the unwritten tails of the open write
		 * line and the GC destination line. (invalid = written-but-dead pblocks
		 * are NOT free; they count as occupied until GC/erase recycles the line.) */
		free_pages = (u64)free_lines * hc->line_pblocks;
		if (cur_active && hc->line_pblocks > cur_off)
			free_pages += hc->line_pblocks - cur_off;
		if (gc_active && hc->line_pblocks > gc_off)
			free_pages += hc->line_pblocks - gc_off;

		/* per-line valid snapshot: total live pages in R + coarse validity
		 * histogram vh = [empty,<25,<50,<75,>=75]%. best-effort lock-free. */
		cur_lines = hc->line_pblocks ? (hc->r_end >> 3) / hc->line_pblocks : 0;
		if (cur_lines > hc->r_max_lines)
			cur_lines = hc->r_max_lines;
		for (vi = 0; vi < cur_lines; vi++) {
			u32 v = hc->line_valid[vi];
			u32 pct = hc->line_pblocks ? (u32)((u64)v * 100 / hc->line_pblocks) : 0;
			valid_sum += v;
			if (v == 0)        vh[0]++;
			else if (pct < 25) vh[1]++;
			else if (pct < 50) vh[2]++;
			else if (pct < 75) vh[3]++;
			else               vh[4]++;
			if (v > 0 && v < bv_valid) bv_valid = v;  /* greedy victim */
		}
		bv_vpc = (bv_valid != ~0u && hc->line_pblocks) ?
			(u32)((u64)bv_valid * 100 / hc->line_pblocks) : 0;

		DMEMIT("r_end=%llu pages=%llu lines=%llu line_pblocks=%u zone_pblocks=%u block_pages=%u gc_policy=%s free_lines=%llu dirty=%llu cur_off=%u r_bios=%llu s_bios=%llu splits=%llu w=%llu r=%llu unmapped=%llu ovr=%llu inplace=%llu recycles=%llu gc_runs=%llu gc_mig=%llu gc_skip=%llu erases=%llu erase_fail=%llu requeue=%llu nospc=%llu wfail=%llu discards=%llu discard_pgs=%llu valid_pages=%llu valid_lines=%llu vh=%u:%u:%u:%u:%u best_victim_vpc=%u free_pages=%llu gc_blocked=%u",
		       (unsigned long long)hc->r_end,
		       (unsigned long long)hc->r_max_pages,
		       (unsigned long long)hc->r_max_lines,
		       hc->line_pblocks,
		       hc->zone_pages,
		       HYZNS_PGS_PER_BLK,
		       hc->gc_policy == HYZNS_GC_COST_BENEFIT ? "cb" : "greedy",
		       (unsigned long long)free_lines,
		       (unsigned long long)dirty_lines_count,
		       cur_off,
		       (unsigned long long)atomic64_read(&hc->stats.r_bios),
		       (unsigned long long)atomic64_read(&hc->stats.s_bios),
		       (unsigned long long)atomic64_read(&hc->stats.splits),
		       (unsigned long long)atomic64_read(&hc->stats.r_writes),
		       (unsigned long long)atomic64_read(&hc->stats.r_reads),
		       (unsigned long long)atomic64_read(&hc->stats.r_reads_unmapped),
		       (unsigned long long)atomic64_read(&hc->stats.r_overwrites),
		       (unsigned long long)atomic64_read(&hc->stats.r_inplace),
		       (unsigned long long)atomic64_read(&hc->stats.line_recycles),
		       (unsigned long long)atomic64_read(&hc->stats.gc_runs),
		       (unsigned long long)atomic64_read(&hc->stats.gc_migrations),
		       (unsigned long long)atomic64_read(&hc->stats.gc_skipped),
		       (unsigned long long)atomic64_read(&hc->stats.erases),
		       (unsigned long long)atomic64_read(&hc->stats.erase_failed),
		       (unsigned long long)atomic64_read(&hc->stats.map_requeues),
		       (unsigned long long)atomic64_read(&hc->stats.r_nospc),
		       (unsigned long long)atomic64_read(&hc->stats.r_write_failed),
		       (unsigned long long)atomic64_read(&hc->stats.discards),
		       (unsigned long long)atomic64_read(&hc->stats.discard_pages),
		       (unsigned long long)valid_sum,
		       (unsigned long long)cur_lines,
		       vh[0], vh[1], vh[2], vh[3], vh[4], bv_vpc,
		       (unsigned long long)free_pages,
		       READ_ONCE(hc->gc_blocked) ? 1U : 0U);
		break;
	}
	case STATUSTYPE_TABLE:
		DMEMIT("%s %llu", hc->dev->name, (unsigned long long)hc->r_end);
		break;
	case STATUSTYPE_IMA:
		result[0] = '\0';
		break;
	}
}

/*
 * dmsetup message <name> 0 <args>
 *   set_r_end <lba>     # reshape the R/S boundary atomically.
 *                       # Issues a Zone Mgmt Send (action 0x20,
 *                       # NVME_ZONE_MODIFY_ZONE) against the backing
 *                       # device first via REQ_OP_ZONE_MODIFY; on
 *                       # success quiesces the L2P + free ring and
 *                       # commits the new boundary in dm state.
 *                       # If the device rejects the new boundary
 *                       # (e.g. non-EMPTY zone in the affected range)
 *                       # the dm state is left untouched.
 *                       # Caller is expected to suspend/resume the
 *                       # target if there may be concurrent IO; the
 *                       # message handler does not gate IO itself.
 */
static int hyzns_message(struct dm_target *ti, unsigned int argc, char **argv,
			  char *result, unsigned int maxlen)
{
	struct hyzns_c *hc = ti->private;
	unsigned long long val;
	char dummy;

	if (argc < 1)
		return -EINVAL;

	if (!strcmp(argv[0], "set_r_end")) {
		sector_t new_r_pages;
		sector_t cur_r_end;
		struct bio *bio;
		unsigned long flags2;
		bool is_shrink;
		int ret;

		if (argc != 2 || sscanf(argv[1], "%llu%c", &val, &dummy) != 1)
			return -EINVAL;
		if (val & HYZNS_PAGE_MASK) {
			DMWARN("set_r_end %llu not page-aligned", val);
			return -EINVAL;
		}
		new_r_pages = (sector_t)val >> HYZNS_PAGE_SHIFT;
		/* Zone-granular (1 GiB), matching FEMU's hyzns_set_r_end. */
		if (new_r_pages % hc->zone_pages) {
			DMWARN("set_r_end %llu not zone-aligned (1 GiB)", val);
			return -EINVAL;
		}
		if (new_r_pages > hc->r_max_pages) {
			DMWARN("set_r_end %llu exceeds L2P capacity (%llu pages)",
			       val,
			       (unsigned long long)hc->r_max_pages);
			return -EINVAL;
		}

		/* Snapshot direction before touching anything. Shrink needs a
		 * position-driven force GC first so that quiesce_shrink later
		 * sees no L2P entry pointing past the new boundary. */
		spin_lock_irqsave(&hc->lock, flags2);
		cur_r_end = hc->r_end;
		spin_unlock_irqrestore(&hc->lock, flags2);
		is_shrink = ((sector_t)val < cur_r_end);

		if (is_shrink) {
			ret = hyzns_prepare_shrink_force_gc(hc, new_r_pages);
			if (ret) {
				DMWARN("set_r_end %llu: force GC failed (%d); boundary unchanged",
				       val, ret);
				return ret;
			}
		}

		/* Fire Zone Mgmt Send action 0x20 (NVME_ZONE_MODIFY_ZONE) at
		 * the backing namespace using the standard block-layer
		 * REQ_OP_ZONE_MODIFY path. bi_sector carries the new ABA LBA
		 * directly; the NVMe driver maps it to the standard SLBA
		 * field (see drivers/nvme/host/zns.c::nvme_setup_zone_mgmt_send).
		 * Sleeps - must not be holding hc->lock.
		 */
		bio = bio_alloc(GFP_KERNEL, 0);
		if (!bio) {
			if (is_shrink)
				hyzns_shrink_restore(hc);
			return -ENOMEM;
		}
		bio_set_dev(bio, hc->dev->bdev);
		bio->bi_opf = REQ_OP_ZONE_MODIFY | REQ_SYNC;
		bio->bi_iter.bi_sector = (sector_t)val;
		ret = submit_bio_wait(bio);
		bio_put(bio);
		if (ret) {
			DMWARN("set_r_end %llu: device rejected (ret=%d); dm state unchanged",
			       val, ret);
			if (is_shrink)
				hyzns_shrink_restore(hc);
			return ret;
		}

		/* Device accepted the new boundary. Commit dm-side state. */
		{
			unsigned long flags;

			spin_lock_irqsave(&hc->lock, flags);
			cur_r_end = hc->r_end;
			if ((sector_t)val < cur_r_end)
				hyzns_quiesce_shrink(hc, new_r_pages);
			else if ((sector_t)val > cur_r_end)
				hyzns_quiesce_grow(hc, new_r_pages);
			WRITE_ONCE(hc->r_end, (sector_t)val);
			hyzns_set_watermarks(hc, new_r_pages / hc->line_pblocks); /* scale GC band to new R */
			spin_unlock_irqrestore(&hc->lock, flags);
			/* A grow pushed reclaimed lines onto the free ring. */
			wake_up(&hc->alloc_wait);
		}

		DMINFO("set_r_end %llu -> %llu (pages=%llu)",
		       (unsigned long long)cur_r_end, val,
		       (unsigned long long)new_r_pages);
		return 0;
	}

	if (!strcmp(argv[0], "gc_policy")) {
		if (argc != 2)
			return -EINVAL;
		if (!strcmp(argv[1], "greedy")) {
			WRITE_ONCE(hc->gc_policy, HYZNS_GC_GREEDY);
		} else if (!strcmp(argv[1], "cb")) {
			WRITE_ONCE(hc->gc_policy, HYZNS_GC_COST_BENEFIT);
		} else {
			DMWARN("gc_policy: unknown '%s' (use greedy|cb)", argv[1]);
			return -EINVAL;
		}
		DMINFO("gc_policy = %s", argv[1]);
		return 0;
	}

	if (!strcmp(argv[0], "gc_batch")) {
		/* Runtime GC migration batch cap (1..HYZNS_GC_BATCH). Smaller =>
		 * fewer bios flooded to the device per wave => leaves queue slots
		 * for the co-tenant during shrink force-GC. */
		u32 v;
		if (argc != 2 || kstrtou32(argv[1], 10, &v))
			return -EINVAL;
		if (v < 1) v = 1;
		if (v > HYZNS_GC_BATCH) v = HYZNS_GC_BATCH;
		WRITE_ONCE(hc->gc_batch, v);
		DMINFO("gc_batch = %u", v);
		return 0;
	}

	if (!strcmp(argv[0], "gc_batch_shrink")) {
		/* Batch cap applied only during shrink force-GC (co-tenant protection). */
		u32 v;
		if (argc != 2 || kstrtou32(argv[1], 10, &v))
			return -EINVAL;
		if (v < 1) v = 1;
		if (v > HYZNS_GC_BATCH) v = HYZNS_GC_BATCH;
		WRITE_ONCE(hc->gc_batch_shrink, v);
		DMINFO("gc_batch_shrink = %u", v);
		return 0;
	}

	if (!strcmp(argv[0], "gc")) {
		/* Force one round of GC: keep migrating victim lines until no
		 * useful victim remains. Useful for benchmarking and for
		 * tests that don't naturally cross gc_low_watermark. */
		int total = 0;

		for (;;) {
			int n = hyzns_do_gc_one_line(hc);

			if (n < 0)
				break;
			total += n;
			cond_resched();
		}
		DMINFO("manual gc: migrated %d pblocks", total);
		return 0;
	}

	DMWARN("unknown message: %s", argv[0]);
	return -EINVAL;
}

/* -------------------------------------------------------------------------
 * Target type registration
 * ------------------------------------------------------------------------- */

static struct target_type hyzns_target = {
	.name            = "hyzns",
	.version         = {0, 15, 0},
	.features        = DM_TARGET_PASSES_INTEGRITY | DM_TARGET_ZONED_HM,
	.module          = THIS_MODULE,
	.ctr             = hyzns_ctr,
	.dtr             = hyzns_dtr,
	.map             = hyzns_map,
	.end_io          = hyzns_end_io,
	.status          = hyzns_status,
	.message         = hyzns_message,
	.prepare_ioctl   = hyzns_prepare_ioctl,
	.iterate_devices = hyzns_iterate_devices,
	.io_hints        = hyzns_io_hints,
	.report_zones    = hyzns_report_zones,
};

static int __init dm_hyzns_init(void)
{
	int r = dm_register_target(&hyzns_target);

	if (r < 0)
		DMERR("register failed %d", r);
	return r;
}

static void __exit dm_hyzns_exit(void)
{
	dm_unregister_target(&hyzns_target);
}

module_init(dm_hyzns_init);
module_exit(dm_hyzns_exit);

MODULE_AUTHOR("HYSSD project");
MODULE_DESCRIPTION("dm target for the HyZNS host-managed hybrid FTL");
MODULE_LICENSE("GPL");
