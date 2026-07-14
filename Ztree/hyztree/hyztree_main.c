/*
 * ctree_main.c  –  CTree variant: Dynamic CNS
 *
 gcc -O2 -g -Wall -Wextra -std=c11 -pthread -I ctree \
      ctree/ctree_nlt.c ctree/ctree_zone.c \
      ctree/variants/dynamic/ctree_main.c \
      ctree/bench_main_ctree.c \
      -o build/ctree_dynamic -lzbd -lnvme -lpthread
 *
 * ────────────────────────────────────────────────────────────────────────── */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#include "hyztree_main.h"

/* Filesystem name of a mount point (for the CNS startup log). */
static const char *cns_fstype_name(const char *path)
{
    struct statfs sf;
    if (statfs(path, &sf) != 0) return "unknown";
    switch ((unsigned long)sf.f_type) {
    case 0xF2F52010UL: return "f2fs";
    case 0xEF53UL:     return "ext4";
    case 0x58465342UL: return "xfs";
    default:           return "other";
    }
}

/* ── Zone budget overrides for this variant ────────────────────────────────
 * ILayer is dormant (internals on CNS).  3 freed active slots → +2 hot / +1 cold.
 * Active budget: 0 IZ + 9 Hot + 3 Cold + 1 Meta = 13 (within device limit). */
#undef  ZTREE_LZGROUP_HOT_INIT
#define ZTREE_LZGROUP_HOT_INIT     10U
#undef  ZTREE_LZGROUP_COLD_INIT
#define ZTREE_LZGROUP_COLD_INIT    3U

/* CNS I/O mode toggle (env var CNS_ODIRECT=1 to enable). */
static int g_cns_odirect = 0;

/* ── Periodic evict+GC thread ───────────────────────────────────────────────
 * Background thread drives evict→punch concurrently with inserts; both phases
 * are non-blocking (per-node node_trywrlock), so no global insert quiesce is
 * needed.  Cadence via env CTREE_DYNAMIC_GC_INTERVAL_MS / CNS HWM trigger. */
static pthread_t        g_gc_tid;
static _Atomic bool     g_gc_running     = false;
static _Atomic bool     g_gc_stop        = false;
static unsigned         g_gc_interval_ms = 0;

/* Independent periodic thread for ZNS region GC.  Enabled via
 * CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS=<ms> (default 0 = disabled).  Also
 * requires CTREE_DYNAMIC_ZNS_GC=1 to actually do work (cow_gc_zns is
 * env-gated).  Non-blocking: serialises with foreground per-leaf via
 * node_latch_for_id. */
static pthread_t        g_zns_gc_tid;
static _Atomic bool     g_zns_gc_running     = false;
static _Atomic bool     g_zns_gc_stop        = false;
static unsigned         g_zns_gc_interval_ms = 0;

/* CNS high-water-mark trigger.  dynamic_gc_thread also fires evict+gc_cns
 * when F2FS usage >= g_cns_hwm_ratio (env CTREE_DYNAMIC_CNS_HWM_RATIO,
 * default 0.90; 0 disables). */
static double           g_cns_hwm_ratio   = 0.0;
static unsigned         g_cns_hwm_poll_ms = 200;

/* Config-3 experiment: once CNS usage reaches g_cns_freeze_pct, stop spilling
 * leaves to CNS (force them to ZNS, blocking) and run NO CNS GC; internal
 * nodes still go to CNS.  env CTREE_DYNAMIC_CNS_FREEZE_PCT (0 = off).  One-way:
 * with GC off CNS never drops, so g_leaf_cns_frozen stays set once tripped. */
static double           g_cns_freeze_pct  = 0.0;
static _Atomic bool     g_leaf_cns_frozen = false;

/* Freeze leaf->CNS spill during each GC cycle so spill can't outrun evict to
 * ENOSPC. env CTREE_DYNAMIC_CNS_GC_FREEZE=0 to disable. */
static int              g_gc_freeze_spill = 1;

/* Leaf-spill (CNS fallback) accounting — leaf-only (internal nodes return
 * before the leaf write section).  spill_ratio = cns / (zns + cns). */
static _Atomic(uint64_t) g_leaf_zns = 0;   /* leaf flush landed on a ZNS zone */
static _Atomic(uint64_t) g_leaf_cns = 0;   /* leaf flush spilled to CNS (fallback) */

/* Stop-the-world CNS GC (env CNS_GC_BLOCKING=1, for comparison): GC thread
 * takes the wrlock around evict+punch, inserts take the rdlock — so inserts
 * pause entirely during a GC cycle (the pre-non-blocking behavior). */
static int              g_cns_gc_blocking = 0;
static pthread_rwlock_t g_insert_pause;
static _Atomic bool     g_insert_pause_initialised = false;

/* R/S boundary state.  Grow is daemon-driven only (no in-process HWM grow); see
 * the g_daemon_grow block comment below and cns_grow_prepare/commit. */
static _Atomic(uint32_t)  g_cur_rzones = 0;   /* current R-zone count */
static uint32_t           g_max_rzones = 0;   /* L2P cap (max R-zones) */
static _Atomic(uint64_t)  g_grow_count = 0;   /* # grows performed */

/* daemon-driven grow (Style-1, 2-phase handshake): the hyhostd daemon owns the
 * grow policy and calls the F2FS resize ioctl itself (f2fs_io resize_cns); ctree
 * only frees/reserves the boundary S-zones (prepare) and finalizes the pool
 * (commit).  Control files under g_resize_ctrl_dir (default = cns_dir()):
 *   .hyzns_prepare       daemon writes absolute target R
 *   .hyzns_prepare.ack   ctree writes "READY <K>" | "BUSY"  (atomic temp+rename)
 *   .hyzns_commit        daemon writes "COMMIT" | "ABORT" after its f2fs_io
 * Ordering: ctree must free+freeze the boundary BEFORE the daemon moves the ABA,
 * so a prepare freezes the K zones (removes them from the allocator) and only a
 * later COMMIT advances g_cur_rzones.  See cns_grow_prepare. */
static int                g_daemon_grow = 0;             /* env CTREE_DAEMON_GROW */
static char               g_resize_ctrl_dir[256] = "";   /* env CTREE_RESIZE_CTRL_DIR */
static uint64_t           g_resize_commit_timeout_ms = 30000; /* daemon-death fallback */
static _Atomic(int)       g_resize_pending = 0;          /* a prepare is frozen */
static uint32_t           g_resize_k = 0;                /* frozen boundary zones */
static uint32_t           g_resize_target = 0;           /* absolute target R */
static uint64_t           g_resize_since_ms = 0;         /* prepare timestamp */
static uint64_t           g_resize_fblocks0 = 0;         /* statvfs f_blocks @prepare */

static double cns_used_ratio(void);
static size_t cns_physical_bytes(ztree_t *t);
static size_t cns_dm_physical_bytes(void);
static long   cns_log_dm_valid(void);
static inline uint64_t monotonic_ns(void);
static int    cns_grow_prepare(ztree_t *t, uint32_t target);
static void   cns_grow_commit(ztree_t *t, int ok);
static void   ctree_resize_poll(ztree_t *t);
static uint64_t cns_fs_total_blocks(void);

/* Periodic trigger (g_gc_interval_ms) + HWM trigger (g_cns_hwm_ratio).
 * Poll cadence = min(periodic interval, HWM poll), bounded below by 50ms. */
static void *dynamic_gc_thread(void *arg)
{
    cow_tree *t = (cow_tree *)arg;

    unsigned poll_ms;
    if (g_gc_interval_ms > 0 && g_cns_hwm_ratio > 0.0)
        poll_ms = g_gc_interval_ms < g_cns_hwm_poll_ms
                      ? g_gc_interval_ms : g_cns_hwm_poll_ms;
    else if (g_gc_interval_ms > 0)
        poll_ms = g_gc_interval_ms;
    else
        poll_ms = g_cns_hwm_poll_ms;
    if (poll_ms < 50) poll_ms = 50;

    unsigned elapsed_periodic = 0;

    while (!atomic_load_explicit(&g_gc_stop, memory_order_acquire))
    {
        unsigned slept = 0;
        while (slept < poll_ms
            && !atomic_load_explicit(&g_gc_stop, memory_order_acquire))
        {
            unsigned step = (poll_ms - slept > 50) ? 50 : (poll_ms - slept);
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step * 1000000L };
            nanosleep(&ts, NULL);
            slept += step;
            if (g_daemon_grow) ctree_resize_poll(t);  /* ~50ms handshake cadence */
        }
        if (atomic_load_explicit(&g_gc_stop, memory_order_acquire))
            break;
        elapsed_periodic += slept;

        /* Config-3 freeze mode: monitor CNS, trip the leaf-spill freeze, and
         * never run CNS GC.  Once frozen we still poll (cheap) but stay set. */
        if (g_cns_freeze_pct > 0.0)
        {
            if (!atomic_load_explicit(&g_leaf_cns_frozen, memory_order_acquire))
            {
                double r = cns_used_ratio();
                if (r * 100.0 >= g_cns_freeze_pct)
                {
                    atomic_store_explicit(&g_leaf_cns_frozen, true,
                                          memory_order_release);
                    fprintf(stderr,
                            "[ctree_dynamic] leaf CNS fallback FROZEN at %.1f%% "
                            "(>= %.1f%%): leaves -> ZNS only, CNS GC off\n",
                            r * 100.0, g_cns_freeze_pct);
                }
            }
            continue;
        }

        /* Daemon-driven grow: the daemon absorbs CNS pressure by growing R, and
         * epunch frees departed CNS slots immediately on the CNS->ZNS path
         * (cns_free_slot), so no HWM evict + batched punch is needed.  The maint
         * loop stays alive only to service the resize handshake, which runs in
         * the sleep loop above (ctree_resize_poll). */
        if (g_daemon_grow) { elapsed_periodic = 0; continue; }

        const char *reason = NULL;
        double ratio = 0.0;
        if (g_cns_hwm_ratio > 0.0)
        {
            ratio = cns_used_ratio();
            if (ratio >= g_cns_hwm_ratio)
                reason = "hwm";
        }
        if (!reason && g_gc_interval_ms > 0
            && elapsed_periodic >= g_gc_interval_ms)
            reason = "periodic";
        if (!reason)
            continue;

        if (strcmp(reason, "hwm") == 0)
            fprintf(stderr,
                    "[ctree_dynamic] CNS HWM trigger: %.1f%% >= %.1f%%\n",
                    ratio * 100.0, g_cns_hwm_ratio * 100.0);

        /* Pre-reclaim CNS breakdown: dm_physical = FTL_stale + ctree_invalid + live. */
        {
            size_t dm_phys   = cns_dm_physical_bytes();
            size_t f2fs_phys = cns_physical_bytes(t);
            size_t live      = (size_t)atomic_load_explicit(&t->stat_cns_current,
                                   memory_order_relaxed) * ZTREE_PAGE_SIZE;
            size_t ftl_inv   = dm_phys   > f2fs_phys ? dm_phys   - f2fs_phys : 0;
            size_t ctree_inv = f2fs_phys > live      ? f2fs_phys - live      : 0;
            fprintf(stderr,
                    "[ctree_dynamic] CNS breakdown (pre-reclaim): dm_phys=%zu KB  "
                    "ftl_invalid=%zu KB  ctree_invalid=%zu KB  live=%zu KB\n",
                    dm_phys / 1024, ftl_inv / 1024, ctree_inv / 1024, live / 1024);
            long vpages = cns_log_dm_valid();   /* prints valid_pages/lines/vh */
            if (vpages > 0) {
                /* 3 views, sum == dm_phys exactly:
                 *   valid = F2FS-valid shard data (st_blocks)
                 *   B     = F2FS-invalid but dm still maps valid (double-CoW
                 *           stale + F2FS meta)
                 *   C     = dm-invalid, awaiting line-GC erase */
                size_t dm_valid = (size_t)vpages * ZTREE_PAGE_SIZE;
                size_t bb = dm_valid > f2fs_phys ? dm_valid - f2fs_phys : 0;
                size_t cc = dm_phys  > dm_valid  ? dm_phys  - dm_valid  : 0;
                fprintf(stderr,
                        "[ctree_dynamic] CNS 3-view: valid_f2fs=%zu KB  "
                        "B_f2fsinval_dmvalid=%zu KB  C_dminval=%zu KB  "
                        "[ctree_live=%zu KB]\n",
                        f2fs_phys / 1024, bb / 1024, cc / 1024, live / 1024);
            }
        }

        int is_hwm = (strcmp(reason, "hwm") == 0);

        /* Daemon-driven grow variant: the daemon absorbs CNS pressure by growing
         * (see ctree_resize_poll), so we do NOT evict live leaves to ZNS.  We
         * only reclaim dead CoW-stale CNS pages (punch) so CNS doesn't bloat and
         * force needless grows.  Non-blocking (per-node latch); inserts run. */
        if (g_cns_gc_blocking)
            pthread_rwlock_wrlock(&g_insert_pause);   /* stop-the-world */
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        cow_phase_mark(t, is_hwm ? "begin:cns_hwm_punch" : "begin:cns_periodic_punch");
        cow_gc_cns(t);                 /* non-blocking punch (per-node latch) */
        cow_phase_mark(t, is_hwm ? "end:cns_hwm_punch" : "end:cns_periodic_punch");
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (g_cns_gc_blocking)
            pthread_rwlock_unlock(&g_insert_pause);
        double punch_s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr,
                "[ctree_daemon] CNS reclaim (%s): punch=%.3fs (grow-only, no evict)\n",
                reason, punch_s);
        elapsed_periodic = 0;
    }
    return NULL;
}

static void *dynamic_zns_gc_thread(void *arg)
{
    cow_tree *t = (cow_tree *)arg;
    while (!atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire))
    {
        unsigned slept = 0;
        while (slept < g_zns_gc_interval_ms
            && !atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire))
        {
            unsigned step = (g_zns_gc_interval_ms - slept > 50)
                                ? 50 : (g_zns_gc_interval_ms - slept);
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step * 1000000L };
            nanosleep(&ts, NULL);
            slept += step;
        }
        if (atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire))
            break;

        /* Non-blocking: cow_gc_zns serialises with foreground per-leaf
         * via node_latch_for_id. */
        cow_phase_mark(t, "begin:zns_gc_periodic");
        cow_gc_zns(t);
        cow_phase_mark(t, "end:zns_gc_periodic");
    }
    return NULL;
}

/* hyhost F2FS variant: CNS lives on an F2FS mount over the dm-hyhost
 * R-region (sparse files, so cns_physical_bytes() reads st_blocks and GC
 * punches holes — same as the dynamic variant).  Mount defaults to
 * /mnt/hyhost; override with CTREE_CNS_DIR. */
static const char *cns_dir(void)
{
    const char *d = getenv("CTREE_CNS_DIR");
    return (d && *d) ? d : "/mnt/hyhost";
}

/* back-to-front layout: meta moved to the TOP two S-zones, so LLayer leaf
 * zones span [0, nr_zones-2).  Internals live on CNS (IZ pool dormant). */
#define CTREE_DYNAMIC_LLAYER_BASE  0U
/* first meta zone = exclusive upper bound of the leaf-zone range */
#define CTREE_LEAF_ZONE_END(t)     ((uint32_t)((t)->info.nr_zones - 2))

/* Trace every TRACE_SAMPLE_INTERVAL page_appends to /tmp/ctree_dynamic_trace.csv */
#define TRACE_SAMPLE_INTERVAL 10000U

/* Bytes physically allocated to the CNS file on disk.  st_blocks counts
 * 512-byte sectors actually backed by storage (sparse holes excluded).
 * This is the metric we want to plot over time — it shrinks when GC
 * (FALLOC_FL_PUNCH_HOLE) frees orphan ranges. */
static inline size_t cns_physical_bytes(ztree_t *t)
{
    if (t->cns_fd < 0) return 0;
    size_t bytes = 0;
    for (int k = 0; k < CTREE_CNS_SHARDS; k++) {
        struct stat st;
        if (t->cns_fd_shard[k] >= 0 && fstat(t->cns_fd_shard[k], &st) == 0)
            bytes += (size_t)st.st_blocks * 512;
    }
    return bytes;
}

/* F2FS-level used ratio (this file + everything else under the mount).
 * Used by the HWM trigger in dynamic_gc_thread. */
/* When F2FS sits on a dm-hyhost target, the real ENOSPC pressure is dm's
 * PHYSICAL line occupancy (F2FS LFS + dm out-of-place = double CoW), not the
 * F2FS-logical statvfs view — statvfs stays low while dm lines fill with stale.
 * Read dm's own accounting (dmsetup status <name>); the dm device name comes
 * from CTREE_CNS_DM_NAME (default hyhost0).  If dmsetup yields nothing (e.g.
 * F2FS-A direct on the raw device, no dm), fall back to statvfs. */
static double cns_used_ratio(void)
{
    const char *name = getenv("CTREE_CNS_DM_NAME");
    if (!name || !*name) name = "hyhost0";
    char cmd[160];
    snprintf(cmd, sizeof cmd, "dmsetup status %s 2>/dev/null", name);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof buf, fp)) {
            char *p;
            long lines = 0, lpb = 0, fl = 0, co = 0;
            if ((p = strstr(buf, " lines=")))      lines = atol(p + 7);
            if ((p = strstr(buf, "line_pblocks="))) lpb  = atol(p + 13);
            if ((p = strstr(buf, "free_lines=")))   fl   = atol(p + 11);
            if ((p = strstr(buf, "cur_off=")))      co   = atol(p + 8);
            /* denominator = current R/S boundary (g_cur_rzones), not the L2P
             * cap (lines) — they differ once grow is provisioned. */
            uint32_t bnd = atomic_load_explicit(&g_cur_rzones, memory_order_relaxed);
            if (bnd == 0 || bnd > (uint32_t)lines) bnd = (uint32_t)lines;
            if (bnd > 0 && lpb > 0) {
                double used = (fl >= bnd) ? 0.0
                            : ((double)(bnd - fl - 1) * lpb + co);
                pclose(fp);
                return used / ((double)bnd * lpb);
            }
        }
        pclose(fp);
    }

    /* Fallback: F2FS-logical usage (no dm layer, e.g. -A direct). */
    struct statvfs vfs;
    if (statvfs(cns_dir(), &vfs) != 0) return 0.0;
    uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    if (total == 0) return 0.0;
    return (double)(total - avail) / (double)total;
}

/* dm PHYSICAL bytes in use (lines occupancy × 4KB) — includes F2FS LFS + dm
 * out-of-place stale.  Fallback: F2FS-logical used. */
static size_t cns_dm_physical_bytes(void)
{
    const char *name = getenv("CTREE_CNS_DM_NAME");
    if (!name || !*name) name = "hyhost0";
    char cmd[160];
    snprintf(cmd, sizeof cmd, "dmsetup status %s 2>/dev/null", name);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char buf[512];
        if (fgets(buf, sizeof buf, fp)) {
            char *p;
            long lines = 0, lpb = 0, fl = 0, co = 0;
            if ((p = strstr(buf, " lines=")))       lines = atol(p + 7);
            if ((p = strstr(buf, "line_pblocks=")))  lpb  = atol(p + 13);
            if ((p = strstr(buf, "free_lines=")))    fl   = atol(p + 11);
            if ((p = strstr(buf, "cur_off=")))       co   = atol(p + 8);
            uint32_t bnd = atomic_load_explicit(&g_cur_rzones, memory_order_relaxed);
            if (bnd == 0 || bnd > (uint32_t)lines) bnd = (uint32_t)lines;
            if (bnd > 0 && lpb > 0) {
                double used = (fl >= bnd) ? 0.0
                            : ((double)(bnd - fl - 1) * lpb + co);
                pclose(fp);
                return (size_t)(used * (double)ZTREE_PAGE_SIZE);
            }
        }
        pclose(fp);
    }
    struct statvfs vfs;
    if (statvfs(cns_dir(), &vfs) != 0) return 0;
    return (size_t)((vfs.f_blocks - vfs.f_bavail) * vfs.f_frsize);
}

/* Log dm per-line valid stats (valid_pages/valid_lines/vh histogram) — new dm
 * only; silently skips if the field is absent (old module). */
static long cns_log_dm_valid(void)
{
    const char *name = getenv("CTREE_CNS_DM_NAME");
    if (!name || !*name) name = "hyhost0";
    char cmd[160];
    snprintf(cmd, sizeof cmd, "dmsetup status %s 2>/dev/null", name);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    long pages = -1;
    char buf[512];
    if (fgets(buf, sizeof buf, fp)) {
        char *vp = strstr(buf, "valid_pages=");
        if (vp) {
            pages = atol(vp + 12);
            char *vl = strstr(buf, "valid_lines=");
            char *vh = strstr(buf, "vh=");
            char vhs[64] = "?";
            if (vh) sscanf(vh + 3, "%63s", vhs);
            fprintf(stderr,
                    "[ctree_dynamic] dm valid: pages=%ld lines=%ld "
                    "vh(e:<25:<50:<75:>=75)=%s\n",
                    pages, vl ? atol(vl + 12) : -1, vhs);
        }
    }
    pclose(fp);
    return pages;
}

/* F2FS usable-area size (statvfs total blocks).  Used on commit-timeout to tell
 * whether the daemon's ABA move actually landed before it died. */
static uint64_t cns_fs_total_blocks(void)
{
    struct statvfs vfs;
    if (statvfs(cns_dir(), &vfs) != 0) return 0;
    return (uint64_t)vfs.f_blocks;
}

/* Phase 1 (prepare): freeze the deepest K = target-cur leaf zones so the daemon
 * can safely move the ABA.  Under a brief insert-pause: verify the K boundary
 * zones are empty, reset them, and remove them from the hot pool (hot_pool_base
 * += K) so no future insert targets them.  g_cur_rzones is NOT advanced yet (the
 * boundary has not moved).  Returns K on READY, 0 on BUSY (boundary busy / at
 * cap) so the daemon backs off and retries. */
static int cns_grow_prepare(ztree_t *t, uint32_t target)
{
    uint32_t cur = atomic_load_explicit(&g_cur_rzones, memory_order_relaxed);
    if (target <= cur) return 0;
    uint32_t K = target - cur;
    if (g_max_rzones == 0 || target > g_max_rzones) return 0;

    pthread_rwlock_wrlock(&g_insert_pause);
    uint32_t base = t->za.hot_pool_base;           /* deepest K leaf zones */
    if (K > t->za.hot_pool_size) {                 /* refuse to empty the pool */
        pthread_rwlock_unlock(&g_insert_pause);
        return 0;
    }
    for (uint32_t z = base; z < base + K; z++) {
        if (atomic_load_explicit(&t->zone_full[z], memory_order_acquire)
            || atomic_load_explicit(&t->zone_wp_bytes[z], memory_order_acquire)
               > t->zones[z].start) {
            pthread_rwlock_unlock(&g_insert_pause);
            return 0;                              /* boundary busy → BUSY */
        }
    }
    for (uint32_t z = base; z < base + K; z++)
        zbd_reset_zones(t->fd, (off_t)t->zones[z].start, (off_t)t->info.zone_size);
    t->za.hot_pool_base += K;
    t->za.hot_pool_size -= K;
    g_resize_k        = K;
    g_resize_target   = target;
    g_resize_fblocks0 = cns_fs_total_blocks();
    g_resize_since_ms = monotonic_ns() / 1000000ULL;
    atomic_store_explicit(&g_resize_pending, 1, memory_order_release);
    pthread_rwlock_unlock(&g_insert_pause);
    fprintf(stderr, "[ctree_daemon] grow PREPARE: R %u -> %u  froze %u boundary "
            "zones (hot_base=%u)\n", cur, target, K, t->za.hot_pool_base);
    return (int)K;
}

/* Phase 2 (commit): COMMIT (ok!=0) — the daemon's f2fs_io moved the ABA, so
 * advance g_cur_rzones (the K zones already left the pool at prepare).  ABORT
 * (ok==0) — the ABA did NOT move, so return the frozen zones to the pool. */
static void cns_grow_commit(ztree_t *t, int ok)
{
    if (!atomic_load_explicit(&g_resize_pending, memory_order_acquire)) return;
    pthread_rwlock_wrlock(&g_insert_pause);
    if (ok) {
        atomic_store_explicit(&g_cur_rzones, g_resize_target, memory_order_release);
        atomic_fetch_add_explicit(&g_grow_count, 1, memory_order_relaxed);
        fprintf(stderr, "[ctree_daemon] grow COMMIT: R now %u\n", g_resize_target);
    } else {
        t->za.hot_pool_base -= g_resize_k;
        t->za.hot_pool_size += g_resize_k;
        fprintf(stderr, "[ctree_daemon] grow ABORT: returned %u zones to pool "
                "(hot_base=%u)\n", g_resize_k, t->za.hot_pool_base);
    }
    g_resize_k = 0;
    g_resize_target = 0;
    atomic_store_explicit(&g_resize_pending, 0, memory_order_release);
    pthread_rwlock_unlock(&g_insert_pause);
}

/* Poll the daemon resize control files (called ~every 50ms from the maint loop).
 * One resize in flight: while a prepare is frozen, wait for .hyzns_commit (or
 * resolve via statvfs on timeout if the daemon died); otherwise pick up a fresh
 * .hyzns_prepare, execute it, and ack READY|BUSY (atomic temp+rename). */
static void ctree_resize_poll(ztree_t *t)
{
    const char *dir = g_resize_ctrl_dir[0] ? g_resize_ctrl_dir : cns_dir();
    char prep[320], pack[320], ptmp[320], commit[320];
    snprintf(prep,   sizeof prep,   "%s/.hyzns_prepare", dir);
    snprintf(pack,   sizeof pack,   "%s/.hyzns_prepare.ack", dir);
    snprintf(ptmp,   sizeof ptmp,   "%s/.hyzns_prepare.ack.tmp", dir);
    snprintf(commit, sizeof commit, "%s/.hyzns_commit", dir);

    if (atomic_load_explicit(&g_resize_pending, memory_order_acquire)) {
        FILE *cf = fopen(commit, "r");
        if (cf) {
            char verdict[16] = {0};
            int k = fscanf(cf, "%15s", verdict);
            fclose(cf);
            unlink(commit);
            cns_grow_commit(t, (k == 1 && strncmp(verdict, "COMMIT", 6) == 0));
            return;
        }
        uint64_t now = monotonic_ns() / 1000000ULL;
        if (now - g_resize_since_ms > g_resize_commit_timeout_ms) {
            uint64_t nowblk = cns_fs_total_blocks();
            int grew = (nowblk > g_resize_fblocks0);
            fprintf(stderr, "[ctree_daemon] commit TIMEOUT: fblocks %llu -> %llu "
                    "=> %s\n", (unsigned long long)g_resize_fblocks0,
                    (unsigned long long)nowblk, grew ? "COMMIT" : "ABORT");
            cns_grow_commit(t, grew);
        }
        return;
    }

    FILE *pf = fopen(prep, "r");
    if (!pf) return;
    unsigned target = 0;
    int k = fscanf(pf, "%u", &target);
    fclose(pf);
    unlink(prep);
    if (k != 1 || target == 0) return;

    int r = cns_grow_prepare(t, target);
    FILE *af = fopen(ptmp, "w");
    if (af) {
        if (r > 0) fprintf(af, "READY %d\n", r);
        else       fprintf(af, "BUSY\n");
        fflush(af);
        fclose(af);
        rename(ptmp, pack);
    }
}

/* True iff zone_id is an LLayer ZNS leaf zone (excludes INVALID, CNS,
 * ILayer, and meta zones).  Used to gate zone_valid_leaves accounting. */
static inline bool is_zns_llayer(ztree_t *t, uint32_t zone_id)
{
    return zone_id < CTREE_LEAF_ZONE_END(t);   /* [0, nr_zones-2); meta excluded */
}

/* Move a leaf's live residency from prev_zone to target_zone in the
 * per-zone counter.  No-op when prev==target (sticky re-append).  Non-ZNS
 * endpoints (INVALID, CNS) are simply skipped, so new-leaf / spill / evict
 * transitions all fall out of the same call. */
static inline void zone_valid_leaves_move(ztree_t *t,
                                          uint32_t prev_zone,
                                          uint32_t target_zone)
{
    if (prev_zone == target_zone)
        return;
    if (is_zns_llayer(t, prev_zone))
        atomic_fetch_sub_explicit(&t->zone_valid_leaves[prev_zone], 1,
                                  memory_order_relaxed);
    if (is_zns_llayer(t, target_zone))
        atomic_fetch_add_explicit(&t->zone_valid_leaves[target_zone], 1,
                                  memory_order_relaxed);
}

/* Bytes physically appended to ZNS leaf zones (cumulative WP - zone_start).
 * Monotonically non-decreasing without ZNS GC.  Diff against (leaves on ZNS)
 * × 4KB gives the stale-page volume that a future ZNS GC would reclaim. */
static inline size_t zns_physical_bytes(ztree_t *t)
{
    size_t total = 0;
    for (uint32_t z = CTREE_DYNAMIC_LLAYER_BASE; z < CTREE_LEAF_ZONE_END(t); z++) {
        uint64_t wp    = atomic_load_explicit(&t->zone_wp_bytes[z],
                                              memory_order_relaxed);
        uint64_t start = t->zones[z].start;
        if (wp > start) total += (size_t)(wp - start);
    }
    return total;
}

/* ── CNS bitmap helpers ────────────────────────────────────────────────────
 * 1 bit per node_id.  bit=1 means "this leaf currently lives on CNS".
 * Internal nodes are always on CNS in this variant, so we don't track them. */
static inline void cns_bitmap_set(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (byte < t->cns_bitmap_bytes)
        atomic_fetch_or_explicit(&t->cns_bitmap[byte], bit, memory_order_relaxed);
}

static inline void cns_bitmap_clear(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (byte < t->cns_bitmap_bytes)
        atomic_fetch_and_explicit(&t->cns_bitmap[byte], ~bit, memory_order_relaxed);
}

static inline int cns_bitmap_test(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (byte >= t->cns_bitmap_bytes)
        return 0;
    return (atomic_load_explicit(&t->cns_bitmap[byte], memory_order_relaxed) & bit) != 0;
}

/* ── CNS "needs-punch" bitmap ───────────────────────────────────────────────
 * 1 bit per node_id, set on a genuine CNS-slot death (live 1->0), cleared
 * when the slot is re-written live or after the non-blocking GC punches it.
 * Lets cow_gc_cns reclaim only freshly-dead slots without latching every nid
 * in [0, max_node) or re-punching slots that were never on CNS. */
static inline void cns_dirty_set(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (t->cns_dirty_bitmap && byte < t->cns_bitmap_bytes)
        atomic_fetch_or_explicit(&t->cns_dirty_bitmap[byte], bit, memory_order_relaxed);
}

static inline void cns_dirty_clear(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (t->cns_dirty_bitmap && byte < t->cns_bitmap_bytes)
        atomic_fetch_and_explicit(&t->cns_dirty_bitmap[byte], ~bit, memory_order_relaxed);
}

static inline int cns_dirty_test(ztree_t *t, ztree_node_id_t nid)
{
    uint32_t byte = nid / 8;
    uint8_t  bit  = 1U << (nid % 8);
    if (!t->cns_dirty_bitmap || byte >= t->cns_bitmap_bytes)
        return 0;
    return (atomic_load_explicit(&t->cns_dirty_bitmap[byte], memory_order_relaxed) & bit) != 0;
}

/* CNS-full backpressure cap (env CTREE_CNS_BACKPRESSURE_MS): retry CNS write on
 * nospc-EIO up to this long, else give up. */
static uint64_t g_cns_bp_max_ms = 30000;

/* epunch mode (env CTREE_CNS_EPUNCH=1): free a departed CNS slot IMMEDIATELY
 * via fallocate(PUNCH_HOLE) instead of flagging it dirty for batched reclaim.
 * Defined after cns_shard_fd; declared here for the earlier callers. */
static int g_cns_epunch = 0;
static inline void cns_punch_slot(ztree_t *t, ztree_node_id_t nid);
static inline void cns_free_slot(ztree_t *t, ztree_node_id_t nid)
{
    if (g_cns_epunch) cns_punch_slot(t, nid);
    else              cns_dirty_set(t, nid);
}

/* Retire a node removed from the tree.  Frees its CNS slot (epunch: now;
 * default: deferred via dirty bitmap + batched cow_gc_cns). */
static inline void retire_cns_node(ztree_t *t, uint32_t zone_id,
                                   ztree_node_id_t nid)
{
    nlt_remove(&t->nlt, zone_id, nid);
    if (zone_id == CTREE_CNS_ZONE_ID) {
        if (cns_bitmap_test(t, nid)) {
            atomic_fetch_sub_explicit(&t->stat_cns_current, 1,
                                      memory_order_relaxed);
            cns_free_slot(t, nid);
        }
        cns_bitmap_clear(t, nid);
    } else if (is_zns_llayer(t, zone_id)) {
        /* Leaf removed from a ZNS zone — ZNS GC accounting. */
        atomic_fetch_sub_explicit(&t->zone_valid_leaves[zone_id], 1,
                                  memory_order_relaxed);
    }
}

/* ── Zone busy counter (used in CNS spill stat sampling) ─────────────────── */
static inline void zones_busy_inc(ztree_t *t)
{
    atomic_fetch_add_explicit(&t->cns_zones_busy, 1, memory_order_relaxed);
}
static inline void zones_busy_dec(ztree_t *t)
{
    atomic_fetch_sub_explicit(&t->cns_zones_busy, 1, memory_order_relaxed);
}

static inline void zones_busy_sample(ztree_t *t)
{
    uint32_t b = atomic_load_explicit(&t->cns_zones_busy, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->stat_zones_busy_sum, b, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->stat_zones_busy_samples, 1, memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Internal constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_HEIGHT 32
#define RIGHTMOST_IDX UINT32_MAX

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW insert path state
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct
{
    ztree_node_id_t node_id;
    uint32_t zone_id;
    uint32_t slot_id;
    ztree_page page;
    uint32_t cidx_from_parent; /* RIGHTMOST_IDX when linked via ptr */
} insert_path_frame;

typedef struct
{
    ztree_node_id_t left_id;
    uint32_t left_zone;
    uint32_t left_slot;
    int left_zone_changed;

    int split;
    int64_t promote_key;
    ztree_node_id_t right_id;
    uint32_t right_zone;
    uint32_t right_slot;
} propagate_state;

#define MAX_BATCH_PAGES ZTREE_MAX_BATCH_PAGES
#define MAX_NVME_PAGES ZTREE_MAX_NVME_PAGES

/* ═══════════════════════════════════════════════════════════════════════════
 * Utility
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void emit_trace_row(ztree_t *t)
{
    if (!t->trace_fp)
        return;
    uint64_t total = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    double elapsed = (double)(monotonic_ns() - t->trace_start_ns) / 1e9;
    int64_t internals = atomic_load_explicit(&t->stat_cns_current, memory_order_relaxed);
    uint32_t total_nodes = atomic_load_explicit(&t->next_node_id, memory_order_relaxed) - 1;
    int64_t leaves = (int64_t)total_nodes - internals;
    uint64_t cns_w = atomic_load_explicit(&t->stat_cns_writes, memory_order_relaxed);
    uint32_t height = atomic_load_explicit(&t->tree_height, memory_order_relaxed);
    size_t cns_phys = cns_physical_bytes(t);
    size_t zns_phys = zns_physical_bytes(t);
    fprintf(t->trace_fp, "%.3f,%lld,%lld,%llu,%llu,%u,%zu,%zu\n",
            elapsed,
            (long long)leaves,
            (long long)internals,
            (unsigned long long)total,
            (unsigned long long)cns_w,
            height,
            cns_phys,
            zns_phys);
}

static inline void maybe_trace_sample(ztree_t *t)
{
    if (!t->trace_fp)
        return;
    uint64_t total = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    if (total % TRACE_SAMPLE_INTERVAL != 0)
        return;
    emit_trace_row(t);
}

/* Unconditional trace sample — used inside maintenance (evict + gc) where
 * stat_page_appends doesn't advance, so the modulo-gated sampler would
 * otherwise stay silent and the M-band on the plot would have no data
 * points to draw the CNS drop or ZNS rise. */
static inline void force_trace_sample(ztree_t *t)
{
    emit_trace_row(t);
    if (t->trace_fp)
        fflush(t->trace_fp);
}

static inline uint64_t ztree_hash64(ztree_node_id_t id)
{
    uint64_t x = id;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 33;
    return x;
}

/* ── Exact per-node latch table (k3 ORWC) ──────────────────────────────────
 * ORWC coupling holds two latches at once; with a HASHED table two unrelated
 * parent→child pairs can alias to crossed buckets and deadlock.  Exact
 * per-node latches make every wait follow tree order (parent before child),
 * which is acyclic.  Chunked, lazily allocated, indexed by node_id (ids are
 * sequential and never reused).  Writer-preferred so preemptive splits are
 * not starved by the shared-lock descent traffic on hot parents. */
#define K3_LATCH_CHUNK_BITS  16
#define K3_LATCH_CHUNK_SIZE  (1U << K3_LATCH_CHUNK_BITS)
#define K3_LATCH_MAX_CHUNKS  (1U << (32 - K3_LATCH_CHUNK_BITS))

static _Atomic(pthread_rwlock_t *) g_latch_chunk[K3_LATCH_MAX_CHUNKS];

static pthread_rwlock_t *node_latch_for_id(ztree_t *t, ztree_node_id_t id)
{
    (void)t;
    uint32_t c   = id >> K3_LATCH_CHUNK_BITS;
    uint32_t off = id & (K3_LATCH_CHUNK_SIZE - 1);

    pthread_rwlock_t *ch = atomic_load_explicit(&g_latch_chunk[c],
                                                memory_order_acquire);
    if (!ch)
    {
        pthread_rwlock_t *neu = malloc(sizeof(*neu) * K3_LATCH_CHUNK_SIZE);
        if (!neu)
        {
            perror("node_latch chunk malloc");
            exit(EXIT_FAILURE);
        }
        pthread_rwlockattr_t at;
        pthread_rwlockattr_init(&at);
        pthread_rwlockattr_setkind_np(&at, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        for (uint32_t i = 0; i < K3_LATCH_CHUNK_SIZE; i++)
            pthread_rwlock_init(&neu[i], &at);
        pthread_rwlockattr_destroy(&at);

        pthread_rwlock_t *expect = NULL;
        if (atomic_compare_exchange_strong_explicit(&g_latch_chunk[c], &expect, neu,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire))
        {
            ch = neu;
        }
        else
        {
            for (uint32_t i = 0; i < K3_LATCH_CHUNK_SIZE; i++)
                pthread_rwlock_destroy(&neu[i]);
            free(neu);
            ch = expect;
        }
    }
    return &ch[off];
}

/* Atomic monotonic-max — used by all three lock-profile buckets. */
static inline void prof_update_max(_Atomic(uint64_t) *target, uint64_t sample)
{
    uint64_t cur = atomic_load_explicit(target, memory_order_relaxed);
    while (sample > cur)
    {
        if (atomic_compare_exchange_weak_explicit(target, &cur, sample,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return;
    }
}

/* CNS slot mapping for internal nodes: slot_id == node_id. */
static inline uint32_t cns_slot_for_node(ztree_node_id_t node_id)
{
    return (uint32_t)node_id;
}

/* Sharded CNS: slot_id (== node_id) → shard (slot & N-1), dense offset within. */
static inline off_t cns_slot_offset(uint32_t slot_id)
{
    return (off_t)(slot_id / CTREE_CNS_SHARDS) * (off_t)ZTREE_PAGE_SIZE;
}

static inline int cns_shard_fd(ztree_t *t, uint32_t slot_id)
{
    return t->cns_fd_shard[slot_id & (CTREE_CNS_SHARDS - 1)];
}

/* epunch: punch a departed CNS slot's 4KB immediately (F2FS may discard it to
 * the dm layer, freeing the R-region line eagerly). */
static inline void cns_punch_slot(ztree_t *t, ztree_node_id_t nid)
{
    fallocate(cns_shard_fd(t, (uint32_t)nid),
              FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
              cns_slot_offset((uint32_t)nid), (off_t)ZTREE_PAGE_SIZE);
}

/* CNS cache tag: bit 63 set + slot_id keeps CNS pages disjoint from ZNS pgnum. */
static inline ztree_pagenum_t cns_cache_tag(uint32_t slot_id)
{
    return (ztree_pagenum_t)slot_id | (1ULL << 63);
}

/* Per-zone write mutex (ZWL) profile recorders, broken out by zone group.
 * Routing: ilayer_pool_base..hot_pool_base → IZ, hot..cold → Hot, cold+ → Cold.
 * In this variant ilayer_pool_base == hot_pool_base, so IZ never matches.
 * Meta zones (0,1) bypass this path. Aggregated at print time. */

typedef enum
{
    ZWL_GROUP_IZ   = 0,
    ZWL_GROUP_HOT  = 1,
    ZWL_GROUP_COLD = 2,
    ZWL_GROUP_NONE = 3,   /* meta or otherwise unclassified */
} zwl_group_t;

static inline zwl_group_t zone_group_of(ztree_t *t, uint32_t zone_id)
{
    if (zone_id >= t->za.cold_pool_base)
        return ZWL_GROUP_COLD;
    if (zone_id >= t->za.hot_pool_base)
        return ZWL_GROUP_HOT;
    if (zone_id >= t->za.ilayer_pool_base)
        return ZWL_GROUP_IZ;
    return ZWL_GROUP_NONE;
}

static inline void record_zwl_wait(ztree_t *t, uint32_t zone_id, uint64_t wait_ns)
{
    _Atomic(uint64_t) *wait_p, *cnt_p, *max_p;
    switch (zone_group_of(t, zone_id))
    {
    case ZWL_GROUP_IZ:
        wait_p = &t->prof_zwl_iz_wait_ns_sum;
        cnt_p  = &t->prof_zwl_iz_acquire_count;
        max_p  = &t->prof_zwl_iz_max_wait_ns;
        break;
    case ZWL_GROUP_HOT:
        wait_p = &t->prof_zwl_hot_wait_ns_sum;
        cnt_p  = &t->prof_zwl_hot_acquire_count;
        max_p  = &t->prof_zwl_hot_max_wait_ns;
        break;
    case ZWL_GROUP_COLD:
        wait_p = &t->prof_zwl_cold_wait_ns_sum;
        cnt_p  = &t->prof_zwl_cold_acquire_count;
        max_p  = &t->prof_zwl_cold_max_wait_ns;
        break;
    default:
        return;
    }
    atomic_fetch_add_explicit(wait_p, wait_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(cnt_p,  1,       memory_order_relaxed);
    prof_update_max(max_p, wait_ns);
}

static inline void record_zwl_hold(ztree_t *t, uint32_t zone_id, uint64_t hold_ns)
{
    _Atomic(uint64_t) *hold_p;
    switch (zone_group_of(t, zone_id))
    {
    case ZWL_GROUP_IZ:   hold_p = &t->prof_zwl_iz_hold_ns_sum;   break;
    case ZWL_GROUP_HOT:  hold_p = &t->prof_zwl_hot_hold_ns_sum;  break;
    case ZWL_GROUP_COLD: hold_p = &t->prof_zwl_cold_hold_ns_sum; break;
    default: return;
    }
    atomic_fetch_add_explicit(hold_p, hold_ns, memory_order_relaxed);
}

/* Instrumented node locking: records wait time for contention profiling. */
static inline void node_wrlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID)
        return;
    uint64_t t0 = monotonic_ns();
    pthread_rwlock_wrlock(node_latch_for_id(t, id));
    uint64_t t1 = monotonic_ns();
    uint64_t wait = t1 - t0;
    atomic_fetch_add_explicit(&t->prof_nl_wr_wait_ns_sum, wait, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->prof_nl_wr_acquire_count, 1, memory_order_relaxed);
    prof_update_max(&t->prof_nl_wr_max_wait_ns, wait);
}

/* Non-blocking variant — used by ZNS GC to avoid foreground deadlock. */
static inline int node_trywrlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID) return 1;
    if (pthread_rwlock_trywrlock(node_latch_for_id(t, id)) != 0)
        return 0;
    atomic_fetch_add_explicit(&t->prof_nl_wr_acquire_count, 1, memory_order_relaxed);
    return 1;
}

static inline void node_rdlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID)
        return;
    uint64_t t0 = monotonic_ns();
    pthread_rwlock_rdlock(node_latch_for_id(t, id));
    uint64_t t1 = monotonic_ns();
    uint64_t wait = t1 - t0;
    atomic_fetch_add_explicit(&t->prof_nl_rd_wait_ns_sum, wait, memory_order_relaxed);
    atomic_fetch_add_explicit(&t->prof_nl_rd_acquire_count, 1, memory_order_relaxed);
    prof_update_max(&t->prof_nl_rd_max_wait_ns, wait);
}

static inline void node_unlock(ztree_t *t, ztree_node_id_t id)
{
    if (id == ZTREE_INVALID_NODE_ID)
        return;
    pthread_rwlock_unlock(node_latch_for_id(t, id));
}

/* Helper: compute physical page number from zone + slot */
static inline ztree_pagenum_t zone_slot_to_pn(ztree_t *t,
                                              uint32_t zone_id,
                                              uint32_t slot_id)
{
    uint64_t off = t->zones[zone_id].start + (uint64_t)slot_id * ZTREE_PAGE_SIZE;
    return (ztree_pagenum_t)(off / ZTREE_PAGE_SIZE);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 4-Way Set-Associative Global Page Cache
 * ═══════════════════════════════════════════════════════════════════════════ */

static void cache_init(ztree_t *t)
{
    t->global_cache = calloc(ZTREE_CACHE_NUM_SETS, sizeof(ztree_cache_set));
    if (!t->global_cache)
    {
        perror("cache_init calloc");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < ZTREE_CACHE_NUM_SETS; i++)
    {
        if (pthread_mutex_init(&t->global_cache[i].lock, NULL) != 0)
        {
            perror("cache_init mutex");
            exit(EXIT_FAILURE);
        }
        for (int j = 0; j < ZTREE_CACHE_WAYS; j++)
        {
            t->global_cache[i].ways[j].valid = 0;
            t->global_cache[i].ways[j].tag = ZTREE_INVALID_PGN;
        }
    }
    atomic_store_explicit(&t->cache_lru_clock, 0, memory_order_relaxed);
}

static void cache_destroy(ztree_t *t)
{
    if (!t->global_cache)
        return;
    for (size_t i = 0; i < ZTREE_CACHE_NUM_SETS; i++)
        pthread_mutex_destroy(&t->global_cache[i].lock);
    free(t->global_cache);
    t->global_cache = NULL;
}

static int cache_lookup(ztree_t *t, ztree_pagenum_t pn, ztree_page *dst)
{
    size_t set_idx = (size_t)(ztree_hash64((uint64_t)pn) % ZTREE_CACHE_NUM_SETS);
    ztree_cache_set *set = &t->global_cache[set_idx];

    pthread_mutex_lock(&set->lock);
    for (int i = 0; i < ZTREE_CACHE_WAYS; i++)
    {
        if (set->ways[i].valid && set->ways[i].tag == pn)
        {
            set->ways[i].lru_counter = atomic_fetch_add_explicit(
                &t->cache_lru_clock, 1, memory_order_relaxed);
            *dst = set->ways[i].data;
            pthread_mutex_unlock(&set->lock);
            atomic_fetch_add_explicit(&t->stat_cache_hit, 1, memory_order_relaxed);
            return 1;
        }
    }
    pthread_mutex_unlock(&set->lock);
    return 0;
}

static void cache_insert(ztree_t *t, ztree_pagenum_t pn, const ztree_page *src)
{
    size_t set_idx = (size_t)(ztree_hash64((uint64_t)pn) % ZTREE_CACHE_NUM_SETS);
    ztree_cache_set *set = &t->global_cache[set_idx];
    uint64_t clock = atomic_fetch_add_explicit(&t->cache_lru_clock, 1,
                                               memory_order_relaxed);
    pthread_mutex_lock(&set->lock);

    /* Dedupe by tag: same pn must occupy at most one way (otherwise
     * cache_lookup may return a stale duplicate). */
    int victim = -1;
    for (int i = 0; i < ZTREE_CACHE_WAYS; i++)
    {
        if (set->ways[i].valid && set->ways[i].tag == pn)
        {
            victim = i;
            break;
        }
    }

    if (victim < 0)
    {
        victim = 0;
        uint64_t min_lru = set->ways[0].lru_counter;
        for (int i = 0; i < ZTREE_CACHE_WAYS; i++)
        {
            if (!set->ways[i].valid)
            {
                victim = i;
                break;
            }
            if (set->ways[i].lru_counter < min_lru)
            {
                min_lru = set->ways[i].lru_counter;
                victim = i;
            }
        }
    }

    set->ways[victim].valid = 1;
    set->ways[victim].tag = pn;
    set->ways[victim].lru_counter = clock;
    set->ways[victim].data = *src;

    pthread_mutex_unlock(&set->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page I/O (NLT-aware load)
 * ═══════════════════════════════════════════════════════════════════════════ */

static void load_page_by_pn(ztree_t *t, ztree_pagenum_t pn, ztree_page *dst)
{
    if (cache_lookup(t, pn, dst))
        return;

    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    off_t off = (off_t)pn * ZTREE_PAGE_SIZE;
    int rfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;

    if (t->direct_fd >= 0)
    {
        void *raw;
        if (posix_memalign(&raw, ZTREE_PAGE_SIZE, ZTREE_PAGE_SIZE) != 0)
        {
            perror("load_page_by_pn: posix_memalign");
            exit(EXIT_FAILURE);
        }
        ssize_t n = pread(rfd, raw, ZTREE_PAGE_SIZE, off);
        if (n != (ssize_t)ZTREE_PAGE_SIZE)
        {
            fprintf(stderr, "load_page_by_pn: pread ret=%ld err=%d off=%lu\n",
                    (long)n, errno, (unsigned long)off);
            free(raw);
            exit(EXIT_FAILURE);
        }
        memcpy(dst, raw, ZTREE_PAGE_SIZE);
        free(raw);
    }
    else
    {
        if (pread(t->fd, dst, ZTREE_PAGE_SIZE, off) != (ssize_t)ZTREE_PAGE_SIZE)
        {
            perror("load_page_by_pn: pread");
            exit(EXIT_FAILURE);
        }
    }

    cache_insert(t, pn, dst);

    /* DO NOT call nlt_update() here.  A loaded page's header carries the
     * (zone_id, slot_id) it was flushed at, which may be an OLD slot —
     * publishing that back into the NLT can revert the live entry for
     * this node_id to a stale slot, orphaning the latest content.  NLT
     * is owned solely by the flush path (flush_page_immediate). */
}

/* Read a page from CNS.  O_DIRECT path uses an aligned bounce buffer. */
static void load_page_from_cns(ztree_t *t, uint32_t slot_id, ztree_page *dst)
{
    ztree_pagenum_t pn = cns_cache_tag(slot_id);

    if (cache_lookup(t, pn, dst))
        return;

    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    ssize_t n;
    if (g_cns_odirect)
    {
        _Alignas(ZTREE_PAGE_SIZE) char raw[ZTREE_PAGE_SIZE];
        n = pread(cns_shard_fd(t, slot_id), raw, ZTREE_PAGE_SIZE, cns_slot_offset(slot_id));
        if (n == (ssize_t)ZTREE_PAGE_SIZE)
            memcpy(dst, raw, ZTREE_PAGE_SIZE);
    }
    else
    {
        n = pread(cns_shard_fd(t, slot_id), dst, ZTREE_PAGE_SIZE, cns_slot_offset(slot_id));
    }
    if (n != (ssize_t)ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "load_page_from_cns: pread ret=%ld err=%d slot=%u\n",
                (long)n, errno, slot_id);
        exit(EXIT_FAILURE);
    }

    cache_insert(t, pn, dst);
}

static int load_page_by_nlt(ztree_t *t, const nlt_location_t *loc,
                            ztree_page *dst)
{
    if (!loc || loc->zone_id == ZTREE_INVALID_ZONE_ID ||
        loc->node_id == ZTREE_INVALID_NODE_ID)
    {
        return 0;
    }

    if (loc->zone_id == CTREE_CNS_ZONE_ID)
    {
        load_page_from_cns(t, loc->slot_id, dst);
        return 1;
    }

    ztree_pagenum_t pn = zone_slot_to_pn(t, loc->zone_id, loc->slot_id);
    load_page_by_pn(t, pn, dst);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Zone write helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void zone_finish_if_full(ztree_t *t, uint32_t zone_id)
{
    if (zone_id >= t->info.nr_zones)
        return;
    if (!atomic_load_explicit(&t->zone_full[zone_id], memory_order_acquire))
        return;
    off_t zstart = (off_t)t->zones[zone_id].start;
    int rc = zbd_finish_zones(t->fd, zstart, (off_t)t->info.zone_size);
    if (rc != 0)
        fprintf(stderr,
                "[zone_finish_if_full] zone=%u start=0x%llx size=0x%llx "
                "rc=%d errno=%d (%s)\n",
                zone_id,
                (unsigned long long)t->zones[zone_id].start,
                (unsigned long long)t->info.zone_size,
                rc, errno, strerror(errno));
}

static int zone_append_page(ztree_t *t, uint32_t zone_id,
                            const void *buf,
                            uint32_t *out_slot_id,
                            ztree_pagenum_t *out_pn)
{
    uint64_t cur_wp = atomic_fetch_add_explicit(
        &t->zone_wp_bytes[zone_id], ZTREE_PAGE_SIZE, memory_order_acq_rel);

    int wfd;
    const void *wbuf;
    _Alignas(ZTREE_PAGE_SIZE) char local_bounce[ZTREE_PAGE_SIZE];
    if (t->direct_fd >= 0)
    {
        memcpy(local_bounce, buf, ZTREE_PAGE_SIZE);
        wfd = t->direct_fd;
        wbuf = local_bounce;
    }
    else
    {
        wfd = t->fd;
        wbuf = buf;
    }
    if (pwrite(wfd, wbuf, ZTREE_PAGE_SIZE, (off_t)cur_wp) != (ssize_t)ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "pwrite failed: cur_wp=%llu\n", (unsigned long long)cur_wp);
        return -1;
    }

    uint32_t slot = (uint32_t)((cur_wp - t->zones[zone_id].start) / ZTREE_PAGE_SIZE);
    if (out_slot_id)
        *out_slot_id = slot;
    if (out_pn)
        *out_pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);

    uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
    uint64_t zone_end = t->zones[zone_id].start + t->zones[zone_id].capacity;
    if (new_wp >= zone_end)
    {
        zone_mark_full(&t->za, zone_id);
        zone_finish_if_full(t, zone_id);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Meta zone management (RLayer – superblock ping-pong on ZNS zones 0/1)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* back-to-front layout: meta lives at the TOP two S-zones (farthest from the
 * R/S boundary) so dm grow can absorb the empty low zones without evacuation. */
#define META_Z0(t) ((uint32_t)((t)->info.nr_zones - 2))
#define META_Z1(t) ((uint32_t)((t)->info.nr_zones - 1))

static inline uint32_t other_meta_zone(ztree_t *t, uint32_t z)
{
    return (z == META_Z0(t)) ? META_Z1(t) : META_Z0(t);
}

static void activate_meta_zone(ztree_t *t, uint32_t zone_id, uint64_t version)
{
    off_t zstart = (off_t)t->zones[zone_id].start;
    fdatasync(t->fd);
    zbd_finish_zones(t->fd, zstart, (off_t)t->info.zone_size);
    if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
    {
        perror("activate_meta_zone: zbd_reset_zones");
        exit(EXIT_FAILURE);
    }

    atomic_store_explicit(&t->zone_wp_bytes[zone_id],
                          t->zones[zone_id].start, memory_order_release);
    atomic_store_explicit(&t->zone_full[zone_id], 0, memory_order_release);

    ztree_zone_header zh;
    memset(&zh, 0, sizeof zh);
    zh.magic = ZTREE_ZH_MAGIC;
    zh.state = ZTREE_ZH_ACTIVE;
    zh.version = version;

    uint32_t ignored_slot;
    if (zone_append_page(t, zone_id, &zh, &ignored_slot, NULL) != 0)
    {
        perror("activate_meta_zone: pwrite header");
        exit(EXIT_FAILURE);
    }

    t->active_meta_zone = zone_id;
    t->meta_wp = 1;
    t->meta_version = version;
}

static void rotate_meta_zone(ztree_t *t)
{
    activate_meta_zone(t, other_meta_zone(t, t->active_meta_zone),
                       t->meta_version + 1);
}

static uint64_t scan_meta_zone(int fd, uint32_t zone_id __attribute__((unused)),
                               uint64_t zone_start, uint64_t zone_size,
                               ztree_superblock_entry *out)
{
    uint64_t n_slots = zone_size / ZTREE_PAGE_SIZE;
    uint64_t last_wp = 0;

    for (uint64_t i = 1; i < n_slots; i++)
    {
        ztree_superblock_entry tmp;
        off_t off = (off_t)(zone_start + i * ZTREE_PAGE_SIZE);
        if (pread(fd, &tmp, ZTREE_PAGE_SIZE, off) != (ssize_t)ZTREE_PAGE_SIZE)
            break;
        if (tmp.magic != ZTREE_SB_MAGIC)
            break;
        *out = tmp;
        last_wp = i;
    }
    return last_wp;
}

static void load_superblock(ztree_t *t)
{
    ztree_zone_header zh0, zh1;
    uint64_t wp0 = atomic_load_explicit(&t->zone_wp_bytes[META_Z0(t)],
                                        memory_order_acquire);
    uint64_t wp1 = atomic_load_explicit(&t->zone_wp_bytes[META_Z1(t)],
                                        memory_order_acquire);

    int v0 = (wp0 > t->zones[META_Z0(t)].start) &&
             (pread(t->fd, &zh0, ZTREE_PAGE_SIZE, 0) == (ssize_t)ZTREE_PAGE_SIZE) &&
             (zh0.magic == ZTREE_ZH_MAGIC);
    int v1 = (wp1 > t->zones[META_Z1(t)].start) &&
             (pread(t->fd, &zh1, ZTREE_PAGE_SIZE,
                    (off_t)t->zones[META_Z1(t)].start) == (ssize_t)ZTREE_PAGE_SIZE) &&
             (zh1.magic == ZTREE_ZH_MAGIC);

    ztree_superblock_entry sb0, sb1;
    uint64_t sbwp0 = 0, sbwp1 = 0;

    if (v0)
        sbwp0 = scan_meta_zone(t->fd, META_Z0(t),
                               t->zones[META_Z0(t)].start,
                               t->info.zone_size, &sb0);
    if (v1)
        sbwp1 = scan_meta_zone(t->fd, META_Z1(t),
                               t->zones[META_Z1(t)].start,
                               t->info.zone_size, &sb1);

    if (sbwp0 == 0 && sbwp1 == 0)
    {
        memset(&t->durable_sb, 0, sizeof t->durable_sb);
        t->durable_sb.root_node_id = ZTREE_INVALID_NODE_ID;
        t->durable_sb.root_zone_id = ZTREE_INVALID_ZONE_ID;
        t->durable_sb.root_slot_id = ZTREE_INVALID_SLOT_ID;
        t->durable_sb.next_node_id = 1;
        t->durable_sb.leaf_order = ZTREE_LEAF_ORDER;
        t->durable_sb.internal_order = ZTREE_INTERNAL_ORDER;
        activate_meta_zone(t, META_Z0(t), 0);
    }
    else
    {
        ztree_superblock_entry *best;
        uint32_t best_zone;
        uint64_t best_wp;
        uint64_t best_meta_version;

        if (sbwp0 > 0 && sbwp1 > 0)
        {
            if (sb0.seq_no >= sb1.seq_no)
            {
                best = &sb0;
                best_zone = META_Z0(t);
                best_wp = sbwp0;
                best_meta_version = zh0.version;
            }
            else
            {
                best = &sb1;
                best_zone = META_Z1(t);
                best_wp = sbwp1;
                best_meta_version = zh1.version;
            }
        }
        else if (sbwp0 > 0)
        {
            best = &sb0;
            best_zone = META_Z0(t);
            best_wp = sbwp0;
            best_meta_version = zh0.version;
        }
        else
        {
            best = &sb1;
            best_zone = META_Z1(t);
            best_wp = sbwp1;
            best_meta_version = zh1.version;
        }

        t->durable_sb = *best;
        t->active_meta_zone = best_zone;
        t->meta_wp = best_wp + 1;
        t->meta_version = best_meta_version;
    }

    atomic_store_explicit(&t->volatile_sb.root_node_id,
                          t->durable_sb.root_node_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id,
                          t->durable_sb.root_zone_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id,
                          t->durable_sb.root_slot_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no,
                          t->durable_sb.seq_no * 2, memory_order_release);

    atomic_store_explicit(&t->next_node_id,
                          t->durable_sb.next_node_id, memory_order_relaxed);

    if (t->durable_sb.root_node_id != ZTREE_INVALID_NODE_ID)
    {
        nlt_location_t loc = {
            .zone_id = t->durable_sb.root_zone_id,
            .node_id = t->durable_sb.root_node_id,
            .slot_id = t->durable_sb.root_slot_id,
        };
        nlt_update(&t->nlt, &loc);
    }
}

static void write_superblock_sync(ztree_t *t)
{
    pthread_mutex_lock(&t->sb_lock);

    t->durable_sb.magic = ZTREE_SB_MAGIC;

    if (t->meta_wp >= t->zones[t->active_meta_zone].capacity / ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "[ctree_dynamic] meta zone full, rotating\n");
        rotate_meta_zone(t);
    }

    if (zone_append_page(t, t->active_meta_zone, &t->durable_sb, NULL, NULL) != 0)
    {
        perror("write_superblock_sync: pwrite");
        exit(EXIT_FAILURE);
    }
    t->meta_wp++;

    pthread_mutex_unlock(&t->sb_lock);
}

static void *sb_flusher_thread(void *arg)
{
    ztree_t *t = (ztree_t *)arg;

    while (!atomic_load_explicit(&t->stop_flusher, memory_order_acquire))
    {
        usleep(ZTREE_FLUSH_INTERVAL_MS * 1000);

        if (!atomic_exchange_explicit(&t->dirty_sb, false, memory_order_acq_rel))
            continue;

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot;
        uint64_t seq;
        for (;;)
        {
            uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 & 1ULL)
                continue;
            root_nid = atomic_load_explicit(&t->volatile_sb.root_node_id,
                                            memory_order_acquire);
            root_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id,
                                             memory_order_acquire);
            root_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id,
                                             memory_order_acquire);
            uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 == s2 && (s2 & 1ULL) == 0)
            {
                seq = s2;
                break;
            }
        }

        pthread_mutex_lock(&t->sb_lock);
        t->durable_sb.root_node_id = root_nid;
        t->durable_sb.root_zone_id = root_zone;
        t->durable_sb.root_slot_id = root_slot;
        t->durable_sb.seq_no = seq / 2;
        t->durable_sb.next_node_id =
            atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
        pthread_mutex_unlock(&t->sb_lock);

        write_superblock_sync(t);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW insert helpers (no overlay/temp_id)
 * ═══════════════════════════════════════════════════════════════════════════ */

static ztree_node_id_t assign_stable_node_id(ztree_t *t)
{
    return atomic_fetch_add_explicit(&t->next_node_id, 1, memory_order_acq_rel);
}

static int load_latest_node(ztree_t *t,
                            uint32_t zone_id,
                            ztree_node_id_t node_id,
                            uint32_t *out_zone,
                            uint32_t *out_slot,
                            ztree_page *out)
{
    nlt_location_t query = {
        .zone_id = zone_id,
        .node_id = node_id,
        .slot_id = ZTREE_INVALID_SLOT_ID,
    };
    nlt_location_t result;
    if (!nlt_lookup(&t->nlt, &query, &result))
        return 0;
    if (!load_page_by_nlt(t, &result, out))
        return 0;
    if (out_zone)
        *out_zone = result.zone_id;
    if (out_slot)
        *out_slot = result.slot_id;
    return 1;
}

/*
 * flush_page_immediate
 *   Internal node → CNS at slot_id == node_id (always). out_zone_changed=0:
 *     parent's (child_zone_id, child_node_id) link is permanent, so structural
 *     parent rewrite happens only via split propagation, never via CoW.
 *   Leaf node → ZNS, sticky-on-prev_zone or zone_alloc_llayer + blocking lock.
 */
static void flush_page_immediate(ztree_t *t,
                                 ztree_page *pg,
                                 uint32_t prev_zone,
                                 uint32_t avoid_zone,
                                 int *out_zone_changed,
                                 uint32_t *out_zone,
                                 uint32_t *out_slot)
{
    if (!pg->is_leaf)
    {
        /* ── Internal node → CNS write (slot = node_id) ─────────────────── */
        uint32_t slot_id = cns_slot_for_node(pg->node_id);
        ztree_pagenum_t pn = cns_cache_tag(slot_id);

        pg->zone_id = CTREE_CNS_ZONE_ID;
        pg->slot_id = slot_id;

        /* O_DIRECT requires page-aligned buffer; buffered mode can use pg directly. */
        _Alignas(ZTREE_PAGE_SIZE) char bounce[ZTREE_PAGE_SIZE];
        const void *wbuf = pg;
        if (g_cns_odirect) { memcpy(bounce, pg, ZTREE_PAGE_SIZE); wbuf = bounce; }
        int cfd = cns_shard_fd(t, slot_id);
        off_t coff = cns_slot_offset(slot_id);
        /* backpressure: internal must live on CNS, so on nospc wait for a line to
         * free (gc_force/dm-GC/grow, all outside our lock) and retry. */
        uint64_t bp0 = 0;
        for (;;)
        {
            ssize_t pwr = pwrite(cfd, wbuf, ZTREE_PAGE_SIZE, coff);
            if (pwr == (ssize_t)ZTREE_PAGE_SIZE) break;
            int e = errno;
            uint64_t now = monotonic_ns() / 1000000ULL;
            if ((e == ENOSPC || e == EIO) && (bp0 == 0 || now - bp0 <= g_cns_bp_max_ms))
            {
                if (bp0 == 0) bp0 = now;
                usleep(1000);
                continue;
            }
            fprintf(stderr,
                    "flush_page_immediate: pwrite CNS slot=%u node_id=%u errno=%d (%s)%s\n",
                    slot_id, pg->node_id, e, strerror(e),
                    (e == ENOSPC || e == EIO) ? " (backpressure timeout)" : "");
            exit(EXIT_FAILURE);
        }

        cache_insert(t, pn, pg);

        nlt_location_t loc = {
            .zone_id = CTREE_CNS_ZONE_ID,
            .node_id = pg->node_id,
            .slot_id = slot_id,
        };
        nlt_update_migrate(&t->nlt, &loc, prev_zone);

        /* Mark this slot occupied in the bitmap so GC knows not to punch it. */
        if (!cns_bitmap_test(t, pg->node_id))
            atomic_fetch_add_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
        cns_bitmap_set(t, pg->node_id);
        cns_dirty_clear(t, pg->node_id);

        atomic_fetch_add_explicit(&t->stat_cns_writes, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->stat_page_appends, 1, memory_order_relaxed);
        maybe_trace_sample(t);

        if (out_zone_changed)
            *out_zone_changed = 0;
        if (out_zone)
            *out_zone = CTREE_CNS_ZONE_ID;
        if (out_slot)
            *out_slot = slot_id;
        return;
    }

    /* ── Leaf node: base-ctree-style sticky + dynamic alloc + CNS spill ───
     * Step 1   sticky on prev_zone (trylock; contention → step 3)
     * Step 1.5 CNS-resident leaf trying to return to its home ZNS zone
     * Step 2   dynamic alloc (trylock; contention → step 3)
     * Step 3   CNS spill at slot_id = node_id, bitmap tracks residency */
    uint32_t target_zone;
    uint64_t cur_wp;
    bool     sticky_ok = false;
    bool     cns_path  = false;
    uint64_t zwl_hold_start = 0;
    int      eoverflow_retries = 0;
    bool     leaf_reroute_zns = false;  /* set on CNS-full: force leaf to ZNS (Step 2b) */

retry_flush:
    sticky_ok = false;
    cns_path  = false;

    /* Step 1: sticky on prev_zone (skip if prev is CNS or INVALID). */
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && prev_zone != CTREE_CNS_ZONE_ID
        && !atomic_load_explicit(&t->zone_full[prev_zone], memory_order_acquire))
    {
        int rc = pthread_mutex_trylock(&t->zone_write_locks[prev_zone]);
        if (rc == 0)
        {
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, prev_zone, 0);
            zones_busy_inc(t);
            zwl_hold_start = lock_t1;

            uint64_t zone_end = t->zones[prev_zone].start
                                + t->zones[prev_zone].capacity;
            uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[prev_zone],
                                               memory_order_relaxed);

            bool sealed = atomic_load_explicit(&t->zone_full[prev_zone],
                                               memory_order_relaxed)
                          || (wp + ZTREE_PAGE_SIZE > zone_end);

            if (!sealed)
            {
                cur_wp      = wp;
                target_zone = prev_zone;
                atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                       wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
                sticky_ok = true;
            }
            else
            {
                record_zwl_hold(t, prev_zone, monotonic_ns() - zwl_hold_start);
                zones_busy_dec(t);
                pthread_mutex_unlock(&t->zone_write_locks[prev_zone]);
                zone_mark_full(&t->za, prev_zone);
                nlt_set_zone_sealed(&t->nlt, prev_zone, true);
                zone_seal_and_replace(&t->za, prev_zone);
            }
        }
        /* else: trylock failed → fall through */
    }

    /* Step 1.5: CNS leaf returning to its home zone (preserved in pg->zone_id). */
    bool cns_returned_home = false;
    if (!sticky_ok && !cns_path && prev_zone == CTREE_CNS_ZONE_ID)
    {
        uint32_t home_zone = pg->zone_id;
        bool home_valid = (home_zone < CTREE_LEAF_ZONE_END(t));  /* [0,nr-2) */
        bool home_full = !home_valid
                      || atomic_load_explicit(&t->zone_full[home_zone],
                                              memory_order_acquire);
        /* Skip Step 1.5 if the home zone is EMPTY (post-GC-reset).  Opening
         * it here bypasses the active-zone cap in rr_pick_zone; let Step 2
         * route through zone_alloc_llayer instead. */
        bool home_empty = home_valid
                       && atomic_load_explicit(&t->zone_wp_bytes[home_zone],
                                               memory_order_acquire)
                          <= t->zones[home_zone].start;
        if (home_empty)
            home_full = true;  /* force fall-through */

        if (!home_full)
        {
            int rc = pthread_mutex_trylock(&t->zone_write_locks[home_zone]);
            if (rc == 0)
            {
                uint64_t lock_t1 = monotonic_ns();
                record_zwl_wait(t, home_zone, 0);
                zones_busy_inc(t);
                zwl_hold_start = lock_t1;

                uint64_t zone_end = t->zones[home_zone].start
                                    + t->zones[home_zone].capacity;
                uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[home_zone],
                                                   memory_order_relaxed);

                if (wp + ZTREE_PAGE_SIZE <= zone_end)
                {
                    cur_wp      = wp;
                    target_zone = home_zone;
                    atomic_store_explicit(&t->zone_wp_bytes[home_zone],
                                           wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
                    sticky_ok          = true;
                    cns_returned_home  = true;
                    atomic_fetch_add_explicit(&t->stat_cns_return_home, 1,
                                              memory_order_relaxed);
                }
                else
                {
                    home_full = true;
                    atomic_fetch_add_explicit(&t->stat_cns_home_full, 1,
                                              memory_order_relaxed);
                }
                if (!sticky_ok)
                {
                    record_zwl_hold(t, home_zone, monotonic_ns() - zwl_hold_start);
                    zones_busy_dec(t);
                    pthread_mutex_unlock(&t->zone_write_locks[home_zone]);
                }
            }
            else
            {
                atomic_fetch_add_explicit(&t->stat_cns_home_contend, 1,
                                          memory_order_relaxed);
            }
        }
        else
        {
            atomic_fetch_add_explicit(&t->stat_cns_home_full, 1, memory_order_relaxed);
        }

        if (!sticky_ok && home_full)
        {
            target_zone = zone_alloc_llayer(&t->za, pg->node_id, avoid_zone);
            /* INVALID = at active-zone cap — skip this step, fall to Step 3 CNS spill. */
            if (target_zone != ZTREE_INVALID_ZONE_ID)
            {
            pg->zone_id = target_zone;  /* update home zone for future return */
            int rc = pthread_mutex_trylock(&t->zone_write_locks[target_zone]);
            if (rc == 0)
            {
                uint64_t lock_t1 = monotonic_ns();
                record_zwl_wait(t, target_zone, 0);
                zones_busy_inc(t);
                zwl_hold_start = lock_t1;

                if (!atomic_load_explicit(&t->zone_full[target_zone],
                                          memory_order_acquire))
                {
                    uint64_t zone_end = t->zones[target_zone].start
                                        + t->zones[target_zone].capacity;
                    uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                                       memory_order_relaxed);
                    if (wp + ZTREE_PAGE_SIZE <= zone_end
                        && (wp != t->zones[target_zone].start
                            || zone_admission_acquire(&t->za, target_zone)))
                    {
                        cur_wp = wp;
                        atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                               wp + ZTREE_PAGE_SIZE,
                                               memory_order_relaxed);
                        sticky_ok = true;
                        atomic_fetch_add_explicit(&t->stat_cns_return_new, 1,
                                                  memory_order_relaxed);
                    }
                }
                if (!sticky_ok)
                {
                    record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                    zones_busy_dec(t);
                    pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                }
            }
            }  /* end if (target_zone != INVALID) */
        }
    }

    /* Step 2: dynamic alloc with trylock (new node, or sticky was zone-full). */
    if (!sticky_ok && !cns_path
        && (prev_zone == ZTREE_INVALID_ZONE_ID
            || (prev_zone != CTREE_CNS_ZONE_ID
                && atomic_load_explicit(&t->zone_full[prev_zone],
                                        memory_order_acquire))))
    {
        target_zone = zone_alloc_llayer(&t->za, pg->node_id, avoid_zone);
        if (target_zone != ZTREE_INVALID_ZONE_ID)  /* INVALID = at active cap → spill */
        {
        pg->zone_id = target_zone;  /* preserve home zone for future CNS return */

        int rc = pthread_mutex_trylock(&t->zone_write_locks[target_zone]);
        if (rc == 0)
        {
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, target_zone, 0);
            zones_busy_inc(t);
            zwl_hold_start = lock_t1;

            if (!atomic_load_explicit(&t->zone_full[target_zone],
                                      memory_order_acquire))
            {
                uint64_t zone_end = t->zones[target_zone].start
                                    + t->zones[target_zone].capacity;
                uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                                   memory_order_relaxed);
                if (wp + ZTREE_PAGE_SIZE <= zone_end
                    && (wp != t->zones[target_zone].start
                        || zone_admission_acquire(&t->za, target_zone)))
                {
                    cur_wp = wp;
                    atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                           wp + ZTREE_PAGE_SIZE,
                                           memory_order_relaxed);
                    sticky_ok = true;
                }
            }
            if (!sticky_ok)
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                zones_busy_dec(t);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            }
        }
        }  /* end if (target_zone != INVALID) */
    }

    /* Step 2b: leaf-CNS-freeze (config-3 freeze_pct, or during a GC cycle) —
     * leaves never spill to CNS; block on a ZNS zone until one accepts the page.
     * Deadlock-safe: all writers (foreground, evict, ZNS-GC) take node-latch
     * before zone-lock, so none holds a zone lock while waiting on this leaf's
     * node latch.  Internal nodes are unaffected (they took the CNS path). */
    if (!sticky_ok && !cns_path
        && (leaf_reroute_zns
            || atomic_load_explicit(&g_leaf_cns_frozen, memory_order_acquire)))
    {
        long freeze_spins = 0;
        while (!sticky_ok)
        {
            target_zone = zone_alloc_llayer(&t->za, pg->node_id, avoid_zone);
            if (target_zone == ZTREE_INVALID_ZONE_ID)
            {
                /* All leaf zones at the active-zone cap — wait for a seal. */
                if (++freeze_spins > 5000000)
                {
                    fprintf(stderr,
                            "flush_page_immediate(leaf): frozen ZNS placement "
                            "stuck (node_id=%llu) — no zone freed\n",
                            (unsigned long long)pg->node_id);
                    exit(EXIT_FAILURE);
                }
                usleep(20);
                continue;
            }
            pg->zone_id = target_zone;
            pthread_mutex_lock(&t->zone_write_locks[target_zone]);
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, target_zone, 0);
            zones_busy_inc(t);
            zwl_hold_start = lock_t1;

            uint64_t zone_end = t->zones[target_zone].start
                                + t->zones[target_zone].capacity;
            uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                               memory_order_relaxed);
            if (!atomic_load_explicit(&t->zone_full[target_zone],
                                      memory_order_acquire)
                && wp + ZTREE_PAGE_SIZE <= zone_end
                && (wp != t->zones[target_zone].start
                    || zone_admission_acquire(&t->za, target_zone)))
            {
                cur_wp = wp;
                atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                      wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
                sticky_ok = true;
                break;
            }

            /* No space / full / at cap — seal if full, drop lock, retry. */
            bool no_space = (wp + ZTREE_PAGE_SIZE > zone_end);
            record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
            zones_busy_dec(t);
            pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            if (no_space)
            {
                zone_mark_full(&t->za, target_zone);
                zone_seal_and_replace(&t->za, target_zone);
            }
            else
            {
                usleep(5);  /* at cap — let a sealer make room */
            }
        }
    }

    /* Step 3: CNS spill (every trylock above failed). */
    if (!sticky_ok)
    {
        cns_path = true;
        cur_wp = (uint64_t)cns_slot_offset(pg->node_id);
        target_zone = CTREE_CNS_ZONE_ID;
        zones_busy_sample(t);
        if (prev_zone == CTREE_CNS_ZONE_ID)
            atomic_fetch_add_explicit(&t->stat_cns_stay, 1, memory_order_relaxed);
    }

    /* ── Compute slot_id, write, post-write bookkeeping ─────────────── */
    uint32_t slot_id;
    ztree_pagenum_t pn;

    if (!cns_path)
    {
        slot_id = (uint32_t)((cur_wp - t->zones[target_zone].start) / ZTREE_PAGE_SIZE);
        pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);
        pg->zone_id = target_zone;
    }
    else
    {
        slot_id = pg->node_id;
        pn = (ztree_pagenum_t)pg->node_id | (1ULL << 63);
        /* pg->zone_id keeps "home zone" so the next flush can attempt return */
    }
    pg->slot_id = slot_id;

    int wfd;
    const void *wbuf;
    _Alignas(ZTREE_PAGE_SIZE) char local_bounce[ZTREE_PAGE_SIZE];

    if (cns_path)
    {
        memcpy(local_bounce, pg, ZTREE_PAGE_SIZE);
        wfd = cns_shard_fd(t, pg->node_id);
        wbuf = local_bounce;
    }
    else if (t->direct_fd >= 0)
    {
        memcpy(local_bounce, pg, ZTREE_PAGE_SIZE);
        wfd = t->direct_fd;
        wbuf = local_bounce;
    }
    else
    {
        wfd = t->fd;
        wbuf = pg;
    }

    if (!cns_path)
    {
        uint64_t zs = t->zones[target_zone].start;
        uint64_t ze = zs + t->zones[target_zone].capacity;
        if (cur_wp < zs || cur_wp + ZTREE_PAGE_SIZE > ze)
        {
            fprintf(stderr,
                    "flush_page_immediate(leaf): cur_wp out of range "
                    "target_zone=%u prev_zone=%u node_id=%llu cur_wp=0x%llx "
                    "zone_start=0x%llx zone_end=0x%llx\n",
                    target_zone, prev_zone,
                    (unsigned long long)pg->node_id,
                    (unsigned long long)cur_wp,
                    (unsigned long long)zs,
                    (unsigned long long)ze);
            exit(EXIT_FAILURE);
        }
    }

    ssize_t pwr = pwrite(wfd, wbuf, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
    {
        int e = errno;
        /* leaf hit a full CNS: don't wait on CNS — reroute to ZNS (has space;
         * contention is transient), Step 2b blocks until placed. */
        if (cns_path && (e == ENOSPC || e == EIO))
        {
            leaf_reroute_zns = true;
            goto retry_flush;
        }
        if (!cns_path && e == EOVERFLOW)
        {
            /* zone_wp_bytes drifted from the device's real write pointer
             * (async zbd_finish_zones, or a stale WP left behind by ZNS
             * GC).  Re-sync from the device under the zone lock we still
             * hold — a blind -PAGE retry can spin forever, and this runs
             * with every node latch on the delete/insert path held, so an
             * unbounded spin deadlocks the whole tree. */
            struct zbd_zone zinfo;
            unsigned int nz = 1;
            if (zbd_report_zones(t->fd,
                                 (off_t)t->zones[target_zone].start,
                                 (off_t)t->info.zone_size,
                                 ZBD_RO_ALL, &zinfo, &nz) == 0 && nz > 0)
            {
                atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                      (uint64_t)zinfo.wp, memory_order_release);
                if (zinfo.cond == ZBD_ZONE_COND_FULL)
                {
                    zone_mark_full(&t->za, target_zone);
                    zone_admission_release_zone(&t->za, target_zone);
                }
            }
            else
            {
                atomic_fetch_sub_explicit(&t->zone_wp_bytes[target_zone],
                                          ZTREE_PAGE_SIZE, memory_order_relaxed);
            }
            record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
            zones_busy_dec(t);
            pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            if (++eoverflow_retries > 1000)
            {
                fprintf(stderr,
                        "flush_page_immediate(leaf): EOVERFLOW unrecoverable "
                        "after %d retries — target_zone=%u cur_wp=0x%llx "
                        "prev_zone=%u node_id=%llu\n",
                        eoverflow_retries, target_zone,
                        (unsigned long long)cur_wp, prev_zone,
                        (unsigned long long)pg->node_id);
                exit(EXIT_FAILURE);
            }
            usleep(500);
            goto retry_flush;
        }
        fprintf(stderr,
                "flush_page_immediate(leaf): pwrite at 0x%llx ret=%zd errno=%d (%s) "
                "target_zone=%u prev_zone=%u cns=%d\n",
                (unsigned long long)cur_wp, pwr, e, strerror(e),
                target_zone, prev_zone, cns_path ? 1 : 0);
        exit(EXIT_FAILURE);
    }

    if (!cns_path)
    {
        atomic_fetch_add_explicit(&g_leaf_zns, 1, memory_order_relaxed);
        record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
        zones_busy_dec(t);
        pthread_mutex_unlock(&t->zone_write_locks[target_zone]);

        uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
        uint64_t zone_end = t->zones[target_zone].start
                            + t->zones[target_zone].capacity;
        if (new_wp >= zone_end)
        {
            zone_mark_full(&t->za, target_zone);
            zone_seal_and_replace(&t->za, target_zone);
        }

        if (cns_bitmap_test(t, pg->node_id)) {
            atomic_fetch_sub_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
            cns_bitmap_clear(t, pg->node_id);
            cns_free_slot(t, pg->node_id);  /* CNS->ZNS free (epunch=now / deferred) */
        }
    }
    else
    {
        atomic_fetch_add_explicit(&g_leaf_cns, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&t->stat_cns_writes, 1, memory_order_relaxed);
        if (!cns_bitmap_test(t, pg->node_id))
            atomic_fetch_add_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
        cns_bitmap_set(t, pg->node_id);
        cns_dirty_clear(t, pg->node_id);
    }

    cache_insert(t, pn, pg);

    nlt_location_t loc = {
        .zone_id = target_zone,
        .node_id = pg->node_id,
        .slot_id = slot_id,
    };
    nlt_update_migrate(&t->nlt, &loc, prev_zone);

    /* Per-zone live-leaf accounting for ZNS GC victim selection. */
    zone_valid_leaves_move(t, prev_zone, target_zone);

    if (!cns_path)
    {
        uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
        uint64_t zone_bytes_used = new_wp - t->zones[target_zone].start;
        uint64_t seal_threshold = (t->zones[target_zone].capacity * 95ULL) / 100ULL;
        if (zone_bytes_used >= seal_threshold)
        {
            zone_mark_full(&t->za, target_zone);
            nlt_set_zone_sealed(&t->nlt, target_zone, true);
            zone_seal_and_replace(&t->za, target_zone);
        }
    }

    /* Heat tracking — leaves only, on real zone migration. */
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && prev_zone != CTREE_CNS_ZONE_ID
        && prev_zone != target_zone)
        zone_heat_reset(&t->za, pg->node_id);
    zone_heat_record_write(&t->za, pg->node_id);

    atomic_fetch_add_explicit(&t->stat_page_appends, 1, memory_order_relaxed);
    maybe_trace_sample(t);

    /* zone_changed: parent rewrite needed only on real ZNS zone migration. */
    if (cns_path || cns_returned_home
        || (prev_zone != ZTREE_INVALID_ZONE_ID
            && prev_zone != CTREE_CNS_ZONE_ID
            && prev_zone == target_zone))
    {
        if (out_zone_changed) *out_zone_changed = 0;
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
    }
    else
    {
        if (out_zone_changed) *out_zone_changed = 1;
        atomic_fetch_add_explicit(&t->stat_zone_changes, 1, memory_order_relaxed);
    }

    if (out_zone) *out_zone = target_zone;
    if (out_slot) *out_slot = slot_id;
}

static uint32_t child_pos_for_key(const ztree_page *p, int64_t key)
{
    for (uint32_t i = 0; i < p->num_keys; i++)
    {
        if (key < (int64_t)p->internal[i].key)
            return i;
    }
    return RIGHTMOST_IDX;
}

static uint32_t child_pos_for_id(const ztree_page *p, ztree_node_id_t child_id)
{
    for (uint32_t i = 0; i < p->num_keys; i++)
    {
        if (p->internal[i].child_node_id == child_id)
            return i;
    }
    if (p->ptr_node_id == child_id)
        return RIGHTMOST_IDX;
    return UINT32_MAX;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Direct Concurrent Insert Path
 * ═══════════════════════════════════════════════════════════════════════════ */

static int try_publish_root_if_unchanged(ztree_t *t,
                                         uint64_t expected_seq_even,
                                         ztree_node_id_t root_nid,
                                         uint32_t root_zone,
                                         uint32_t root_slot)
{
    uint64_t expected = expected_seq_even;
    int cas_success = atomic_compare_exchange_strong_explicit(&t->volatile_sb.seq_no,
                                                              &expected,
                                                              expected_seq_even + 1,
                                                              memory_order_acq_rel,
                                                              memory_order_acquire);
    if (!cas_success)
        return 0;

    atomic_store_explicit(&t->volatile_sb.root_node_id, root_nid, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id, root_zone, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id, root_slot, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no, expected_seq_even + 2, memory_order_release);
    atomic_store_explicit(&t->dirty_sb, true, memory_order_release);
    (void)atomic_fetch_add_explicit(&t->txg_next, 1, memory_order_acq_rel);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ORWC insert path — El-Shaikh/Seeger/Soisalon-Soininen, "Lightweight Latches
 * for B-Trees to Cope with High Contention" (DEXA 2024), Sect. 3.
 * Ported from ztree_main_k3.c.  Replaces the b1 optimistic descent + pser
 * (fix A): preemptive splits make parent-serialization unnecessary.
 * ctree adaptations vs ztree k3:
 *   - flush may land a leaf on CNS (out_zone_changed=0 there, so no hint fix
 *     is queued — parent hints keep the leaf's home ZNS zone, matching the
 *     Step-1.5 return-home protocol).
 *   - new INTERNAL nodes bump stat_cns_current (internals always live on CNS).
 *   - tree height lives in t->tree_height (not volatile_sb); it is stored and
 *     read INSIDE the superblock seqlock window, so (root, height) pairs are
 *     consistent.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define K3_ESCALATE_AFTER 3U   /* optimistic restarts before exclusive coupling */

static _Atomic(uint64_t) g_k3_restarts = 0;
static _Atomic(uint64_t) g_k3_upgrade_child_fail = 0;
static _Atomic(uint64_t) g_k3_upgrade_parent_fail = 0;
static _Atomic(uint64_t) g_k3_upgrade_validate_fail = 0;
static _Atomic(uint64_t) g_k3_escalations = 0;
static _Atomic(uint64_t) g_k3_preemptive_leaf_splits = 0;
static _Atomic(uint64_t) g_k3_preemptive_int_splits = 0;
static _Atomic(uint64_t) g_k3_root_splits = 0;
static _Atomic(uint64_t) g_k3_hint_fix_applied = 0;
static _Atomic(uint64_t) g_k3_hint_fix_skipped = 0;

/* "unsafe" per the paper == cannot absorb one more entry (insert-only). */
static inline int page_is_full(const ztree_page *p)
{
    return p->is_leaf ? (p->num_keys >= ZTREE_LEAF_ORDER - 1)
                      : (p->num_keys >= ZTREE_INTERNAL_ORDER - 1);
}

/* In-memory split of a full page: the upper half moves to *right (fresh
 * node_id assigned here).  Returns the separator key for the parent. */
static int64_t split_page_mem(ztree_t *t, ztree_page *left, ztree_page *right)
{
    memset(right, 0, sizeof *right);
    right->node_id = assign_stable_node_id(t);
    int64_t sep;

    if (left->is_leaf)
    {
        right->is_leaf = 1;
        uint32_t sp = (left->num_keys + 1) / 2;
        right->num_keys = left->num_keys - sp;
        for (uint32_t i = 0; i < right->num_keys; i++)
            right->leaf[i] = left->leaf[sp + i];
        left->num_keys = sp;
        /* leaf chain */
        right->ptr_node_id = left->ptr_node_id;
        right->ptr_zone_id = left->ptr_zone_id;
        left->ptr_node_id = right->node_id;
        left->ptr_zone_id = ZTREE_INVALID_ZONE_ID;
        sep = (int64_t)right->leaf[0].key;
        /* Inherit heat so the sibling isn't misclassified cold. */
        zone_heat_inherit(&t->za, right->node_id, left->node_id);
    }
    else
    {
        right->is_leaf = 0;
        /* new internal node → CNS resident (parity with baseline accounting) */
        atomic_fetch_add_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
        uint32_t mi = left->num_keys / 2;          /* promoted key index */
        sep = (int64_t)left->internal[mi].key;
        right->num_keys = left->num_keys - mi - 1;
        for (uint32_t i = 0; i < right->num_keys; i++)
            right->internal[i] = left->internal[mi + 1 + i];
        right->ptr_node_id = left->ptr_node_id;
        right->ptr_zone_id = left->ptr_zone_id;
        left->ptr_node_id = left->internal[mi].child_node_id;
        left->ptr_zone_id = left->internal[mi].child_zone_id;
        left->num_keys = mi;
    }
    return sep;
}

/* Insert (sep, left, right) into a NON-FULL parent at routing position cidx
 * (the position child_pos_for_key(par, key) returned for the split child). */
static void parent_insert_sep(ztree_page *par, uint32_t cidx,
                              int64_t sep,
                              ztree_node_id_t left_id, uint32_t left_zone,
                              ztree_node_id_t right_id, uint32_t right_zone)
{
    uint32_t pos = (cidx == RIGHTMOST_IDX) ? par->num_keys : cidx;
    for (int64_t j = (int64_t)par->num_keys - 1; j >= (int64_t)pos; j--)
        par->internal[j + 1] = par->internal[j];
    par->internal[pos].key = (uint64_t)sep;
    par->internal[pos].child_node_id = left_id;
    par->internal[pos].child_zone_id = left_zone;
    if (pos == par->num_keys)
    {
        par->ptr_node_id = right_id;
        par->ptr_zone_id = right_zone;
    }
    else
    {
        par->internal[pos + 1].child_node_id = right_id;
        par->internal[pos + 1].child_zone_id = right_zone;
    }
    par->num_keys++;
}

/* Publish root (id, zone, slot, height).  Caller must hold the CURRENT root's
 * wrlock, so no competing locked publisher exists; the CAS spin only arbitrates
 * against GC's optimistic try_publish_root_if_unchanged.  t->tree_height is
 * stored inside the seqlock window so snapshots see consistent pairs. */
static void publish_root_locked(ztree_t *t, ztree_node_id_t nid,
                                uint32_t zone, uint32_t slot, uint32_t height)
{
    for (;;)
    {
        uint64_t s = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s & 1ULL)
            continue;
        if (!atomic_compare_exchange_weak_explicit(&t->volatile_sb.seq_no, &s, s + 1,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire))
            continue;
        atomic_store_explicit(&t->volatile_sb.root_node_id, nid, memory_order_release);
        atomic_store_explicit(&t->volatile_sb.root_zone_id, zone, memory_order_release);
        atomic_store_explicit(&t->volatile_sb.root_slot_id, slot, memory_order_release);
        atomic_store_explicit(&t->tree_height, height, memory_order_release);
        atomic_store_explicit(&t->volatile_sb.seq_no, s + 2, memory_order_release);
        atomic_store_explicit(&t->dirty_sb, true, memory_order_release);
        (void)atomic_fetch_add_explicit(&t->txg_next, 1, memory_order_acq_rel);
        return;
    }
}

/* Seqlock read of (root id, zone, slot, height).  Returns the even seq_no. */
static uint64_t k3_root_snapshot(ztree_t *t, ztree_node_id_t *nid,
                                 uint32_t *zone, uint32_t *slot, uint32_t *height)
{
    for (;;)
    {
        uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 & 1ULL)
            continue;
        *nid    = atomic_load_explicit(&t->volatile_sb.root_node_id, memory_order_acquire);
        *zone   = atomic_load_explicit(&t->volatile_sb.root_zone_id, memory_order_acquire);
        *slot   = atomic_load_explicit(&t->volatile_sb.root_slot_id, memory_order_acquire);
        *height = atomic_load_explicit(&t->tree_height, memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 == s2 && (s2 & 1ULL) == 0)
            return s2;
    }
}

/* Deferred two-stage-tracking refresh: child (cid) now lives in zone cz;
 * update the ancestors' child_zone_id hints bottom-up, one wrlock at a time,
 * cascading while the ancestor's own flush migrates it too.  Best-effort:
 * a lookup/route miss just leaves an advisory-stale hint behind. */
static void fix_zone_hint_up(ztree_t *t, const ztree_node_id_t *path_ids,
                             int top_lvl, int from_lvl,
                             ztree_node_id_t child_id, uint32_t child_zone)
{
    ztree_node_id_t cid = child_id;
    uint32_t cz = child_zone;

    for (int L = from_lvl + 1; L <= top_lvl; L++)
    {
        ztree_node_id_t pid = path_ids[L];
        if (pid == ZTREE_INVALID_NODE_ID)
            return;
        node_wrlock(t, pid);

        insert_path_frame pf;
        pf.node_id = pid;
        if (!load_latest_node(t, ZTREE_INVALID_ZONE_ID, pid,
                              &pf.zone_id, &pf.slot_id, &pf.page)
            || pf.page.is_leaf)
        {
            node_unlock(t, pid);
            atomic_fetch_add_explicit(&g_k3_hint_fix_skipped, 1, memory_order_relaxed);
            return;
        }

        uint32_t cidx = child_pos_for_id(&pf.page, cid);
        if (cidx == UINT32_MAX)
        {
            node_unlock(t, pid);
            atomic_fetch_add_explicit(&g_k3_hint_fix_skipped, 1, memory_order_relaxed);
            return;
        }

        uint32_t curz = (cidx == RIGHTMOST_IDX) ? pf.page.ptr_zone_id
                                                : pf.page.internal[cidx].child_zone_id;
        if (curz == cz)
        {
            node_unlock(t, pid);
            return;
        }
        if (cidx == RIGHTMOST_IDX)
            pf.page.ptr_zone_id = cz;
        else
            pf.page.internal[cidx].child_zone_id = cz;

        int zchg;
        flush_page_immediate(t, &pf.page, pf.zone_id, ZTREE_INVALID_ZONE_ID,
                             &zchg, &pf.zone_id, &pf.slot_id);
        atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_k3_hint_fix_applied, 1, memory_order_relaxed);

        int is_root = (atomic_load_explicit(&t->volatile_sb.root_node_id,
                                            memory_order_acquire) == pid);
        if (is_root)
            publish_root_locked(t, pid, pf.zone_id, pf.slot_id,
                                atomic_load_explicit(&t->tree_height,
                                                     memory_order_acquire));
        node_unlock(t, pid);

        if (!zchg || is_root)
            return;
        cid = pid;
        cz = pf.zone_id;
    }
}

static void do_single_insert(ztree_t *t, int64_t key, const char *value)
{
    int x_level = 0;   /* levels <= x_level are latched exclusively (leaf = 0) */

    /* Deferred zone-hint fix-ups; survive restarts (their flushes are done). */
    struct { ztree_node_id_t id; uint32_t zone; int lvl; } pending[MAX_HEIGHT];
    int n_pending = 0;
    ztree_node_id_t path_ids[MAX_HEIGHT] = {0};
    int top_lvl = 0;
    int success = 0;

    for (uint32_t restart = 0; !success; restart++)
    {
        if (restart > 0)
        {
            atomic_fetch_add_explicit(&g_k3_restarts, 1, memory_order_relaxed);
            if (restart > K3_ESCALATE_AFTER && x_level != INT_MAX)
            {
                x_level = INT_MAX;   /* classical exclusive coupling from root */
                atomic_fetch_add_explicit(&g_k3_escalations, 1, memory_order_relaxed);
            }
            if ((restart & 0xFU) == 0)
                sched_yield();
            if (restart >= 1000000U)
            {
                fprintf(stderr, "cow_insert: excessive restarts (key=%ld)\n", (long)key);
                exit(EXIT_FAILURE);
            }
        }

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot, height;
        uint64_t seq_snapshot = k3_root_snapshot(t, &root_nid, &root_zone,
                                                 &root_slot, &height);
        uint64_t apply_t0 = monotonic_ns();

        /* Empty tree: create the root leaf and publish it (CAS arbitrates
         * concurrent creators; the loser's flushed page is unreachable). */
        if (root_nid == ZTREE_INVALID_NODE_ID)
        {
            ztree_page root;
            memset(&root, 0, sizeof root);
            root.is_leaf = 1;
            root.node_id = assign_stable_node_id(t);
            root.num_keys = 1;
            root.leaf[0].key = (uint64_t)key;
            memcpy(root.leaf[0].record.value, value, 120);

            int zchg;
            uint32_t rz, rs;
            flush_page_immediate(t, &root, ZTREE_INVALID_ZONE_ID,
                                 ZTREE_INVALID_ZONE_ID, &zchg, &rz, &rs);

            if (try_publish_root_if_unchanged(t, seq_snapshot, root.node_id, rz, rs))
            {
                uint64_t apply_dt = monotonic_ns() - apply_t0;
                atomic_fetch_add_explicit(&t->stat_apply_ns_sum, apply_dt, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->stat_apply_ns_samples, 1, memory_order_relaxed);
                success = 1;
            }
            continue;
        }

        if (height >= MAX_HEIGHT)
        {
            fprintf(stderr, "do_single_insert: height overflow\n");
            exit(EXIT_FAILURE);
        }

        int lvl = (int)height;
        top_lvl = lvl;
        int m_wr = (lvl <= x_level || lvl == 0);

        if (m_wr) node_wrlock(t, root_nid);
        else      node_rdlock(t, root_nid);

        /* Still the root?  A root split publishes its replacement BEFORE
         * releasing the old root's wrlock, so this check is stable while we
         * hold any latch on root_nid. */
        if (atomic_load_explicit(&t->volatile_sb.root_node_id,
                                 memory_order_acquire) != root_nid)
        {
            node_unlock(t, root_nid);
            continue;
        }

        insert_path_frame f;   /* current node */
        f.node_id = root_nid;
        if (!load_latest_node(t, root_zone, root_nid, &f.zone_id, &f.slot_id, &f.page))
        {
            node_unlock(t, root_nid);
            continue;
        }
        path_ids[lvl] = root_nid;

        /* ── Preemptive ROOT split (paper's root special case: blocking
         *    upgrade + unsafe-condition re-validation after acquiring). ── */
        if (page_is_full(&f.page))
        {
            if (!m_wr)
            {
                node_unlock(t, root_nid);
                node_wrlock(t, root_nid);   /* blocking OK: nothing else held */
                m_wr = 1;
                if (atomic_load_explicit(&t->volatile_sb.root_node_id,
                                         memory_order_acquire) != root_nid
                    || !load_latest_node(t, f.zone_id, root_nid,
                                         &f.zone_id, &f.slot_id, &f.page))
                {
                    node_unlock(t, root_nid);
                    continue;
                }
            }
            if (page_is_full(&f.page))
            {
                /* Split under w(root): old root becomes the left child, a
                 * fresh internal node becomes the new root.  Persistence
                 * order: sibling → original → new root; publish before the
                 * old root's latch is released. */
                insert_path_frame rf;
                int is_leaf_split = f.page.is_leaf;
                int64_t sep = split_page_mem(t, &f.page, &rf.page);
                rf.node_id = rf.page.node_id;
                node_wrlock(t, rf.node_id);   /* pre-latch before NLT-visible */

                int rzchg, lzchg, nrchg;
                flush_page_immediate(t, &rf.page, ZTREE_INVALID_ZONE_ID, f.zone_id,
                                     &rzchg, &rf.zone_id, &rf.slot_id);
                flush_page_immediate(t, &f.page, f.zone_id, ZTREE_INVALID_ZONE_ID,
                                     &lzchg, &f.zone_id, &f.slot_id);

                ztree_page nr;
                memset(&nr, 0, sizeof nr);
                nr.is_leaf = 0;
                nr.node_id = assign_stable_node_id(t);
                /* new internal root → CNS resident */
                atomic_fetch_add_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
                nr.num_keys = 1;
                nr.internal[0].key = (uint64_t)sep;
                nr.internal[0].child_node_id = f.node_id;
                nr.internal[0].child_zone_id = f.zone_id;
                nr.ptr_node_id = rf.node_id;
                nr.ptr_zone_id = rf.zone_id;

                uint32_t nrz, nrs;
                flush_page_immediate(t, &nr, ZTREE_INVALID_ZONE_ID,
                                     ZTREE_INVALID_ZONE_ID, &nrchg, &nrz, &nrs);
                publish_root_locked(t, nr.node_id, nrz, nrs, height + 1);
                (void)rzchg; (void)lzchg; (void)nrchg;

                atomic_fetch_add_explicit(&g_k3_root_splits, 1, memory_order_relaxed);
                if (is_leaf_split)
                    atomic_fetch_add_explicit(&g_k3_preemptive_leaf_splits, 1, memory_order_relaxed);
                else
                    atomic_fetch_add_explicit(&g_k3_preemptive_int_splits, 1, memory_order_relaxed);

                root_nid = nr.node_id;
                height  += 1;
                if (top_lvl + 1 < MAX_HEIGHT)
                {
                    top_lvl += 1;
                    path_ids[top_lvl] = nr.node_id;
                }

                if (key < sep)
                {
                    node_unlock(t, rf.node_id);
                }
                else
                {
                    node_unlock(t, f.node_id);
                    f = rf;
                }
                path_ids[lvl] = f.node_id;
                /* keep descending at the same level, exclusively latched */
            }
        }

        /* ── Coupled descent ── */
        int failed = 0;
        for (;;)
        {
            if (lvl == 0)
            {
                /* Leaf: exclusively latched and guaranteed non-full. */
                ztree_page *leaf = &f.page;
                int updated = 0;
                for (uint32_t i = 0; i < leaf->num_keys; i++)
                {
                    if ((int64_t)leaf->leaf[i].key == key)
                    {
                        memcpy(leaf->leaf[i].record.value, value, 120);
                        updated = 1;
                        break;
                    }
                }
                if (!updated)
                {
                    uint32_t pos = 0;
                    while (pos < leaf->num_keys && (int64_t)leaf->leaf[pos].key < key)
                        pos++;
                    for (int64_t i = (int64_t)leaf->num_keys - 1; i >= (int64_t)pos; i--)
                        leaf->leaf[i + 1] = leaf->leaf[i];
                    leaf->leaf[pos].key = (uint64_t)key;
                    memcpy(leaf->leaf[pos].record.value, value, 120);
                    leaf->num_keys++;
                }

                int zchg;
                flush_page_immediate(t, leaf, f.zone_id, ZTREE_INVALID_ZONE_ID,
                                     &zchg, &f.zone_id, &f.slot_id);
                if (f.node_id == root_nid)
                    publish_root_locked(t, root_nid, f.zone_id, f.slot_id, height);
                else if (zchg && n_pending < MAX_HEIGHT)
                {
                    pending[n_pending].id   = f.node_id;
                    pending[n_pending].zone = f.zone_id;
                    pending[n_pending].lvl  = 0;
                    n_pending++;
                }
                node_unlock(t, f.node_id);

                uint64_t apply_dt = monotonic_ns() - apply_t0;
                atomic_fetch_add_explicit(&t->stat_apply_ns_sum, apply_dt, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->stat_apply_ns_samples, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->stat_flush_ns_sum, apply_dt, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->stat_flush_ns_samples, 1, memory_order_relaxed);
                success = 1;
                break;
            }

            /* Internal node: route to the child. */
            uint32_t cidx = child_pos_for_key(&f.page, key);
            ztree_node_id_t child_id = (cidx == RIGHTMOST_IDX)
                                           ? f.page.ptr_node_id
                                           : f.page.internal[cidx].child_node_id;
            uint32_t child_zone = (cidx == RIGHTMOST_IDX)
                                      ? f.page.ptr_zone_id
                                      : f.page.internal[cidx].child_zone_id;

            int child_lvl = lvl - 1;
            int cm_wr = (child_lvl <= x_level || child_lvl == 0);

            if (cm_wr) node_wrlock(t, child_id);
            else       node_rdlock(t, child_id);

            insert_path_frame cf;
            cf.node_id = child_id;
            if (!load_latest_node(t, child_zone, child_id,
                                  &cf.zone_id, &cf.slot_id, &cf.page))
            {
                node_unlock(t, child_id);
                node_unlock(t, f.node_id);
                failed = 1;
                break;
            }

            if (!page_is_full(&cf.page))
            {
                node_unlock(t, f.node_id);
                f = cf;
                lvl = child_lvl;
                m_wr = cm_wr;
                path_ids[lvl] = f.node_id;
                continue;
            }

            /* ── Child is FULL → preemptive split. ── */
            if (!cm_wr)
            {
                /* Optimistic upgrade of the child (paper State 0→1): while we
                 * hold the parent latch no split of the child can COMPLETE
                 * (it needs w(parent)), so winning the trywrlock after
                 * dropping our own rd is decisive. */
                node_unlock(t, child_id);
                if (!node_trywrlock(t, child_id))
                {
                    atomic_fetch_add_explicit(&g_k3_upgrade_child_fail, 1, memory_order_relaxed);
                    node_unlock(t, f.node_id);
                    if (lvl > x_level) x_level = lvl;
                    failed = 1;
                    break;
                }
                cm_wr = 1;
                if (!load_latest_node(t, cf.zone_id, child_id,
                                      &cf.zone_id, &cf.slot_id, &cf.page))
                {
                    node_unlock(t, child_id);
                    node_unlock(t, f.node_id);
                    failed = 1;
                    break;
                }
                if (!page_is_full(&cf.page))   /* defensive; cannot happen */
                {
                    node_unlock(t, f.node_id);
                    f = cf;
                    lvl = child_lvl;
                    m_wr = 1;
                    path_ids[lvl] = f.node_id;
                    continue;
                }
            }
            if (!m_wr)
            {
                /* Optimistic upgrade of the parent (paper State 1→2 with the
                 * case I/II/III analysis): release rd, trywrlock, then
                 * re-validate — not full and still routing the key to the
                 * exclusively-held child. */
                node_unlock(t, f.node_id);
                if (!node_trywrlock(t, f.node_id))
                {
                    atomic_fetch_add_explicit(&g_k3_upgrade_parent_fail, 1, memory_order_relaxed);
                    node_unlock(t, child_id);
                    if (lvl > x_level) x_level = lvl;
                    failed = 1;
                    break;
                }
                m_wr = 1;
                int valid = load_latest_node(t, f.zone_id, f.node_id,
                                             &f.zone_id, &f.slot_id, &f.page)
                            && !f.page.is_leaf
                            && !page_is_full(&f.page);
                if (valid)
                {
                    cidx = child_pos_for_key(&f.page, key);
                    ztree_node_id_t routed = (cidx == RIGHTMOST_IDX)
                                                 ? f.page.ptr_node_id
                                                 : f.page.internal[cidx].child_node_id;
                    valid = (routed == child_id);
                }
                if (!valid)
                {
                    atomic_fetch_add_explicit(&g_k3_upgrade_validate_fail, 1, memory_order_relaxed);
                    node_unlock(t, child_id);
                    node_unlock(t, f.node_id);
                    if (lvl > x_level) x_level = lvl;
                    failed = 1;
                    break;
                }
            }

            /* Both exclusively latched: split the child.  Persistence order:
             * sibling → original → parent, so the parent entries carry the
             * children's POST-flush zones (accurate two-stage hints). */
            insert_path_frame rf;
            int is_leaf_split = cf.page.is_leaf;
            int64_t sep = split_page_mem(t, &cf.page, &rf.page);
            rf.node_id = rf.page.node_id;
            node_wrlock(t, rf.node_id);   /* pre-latch before NLT-visible */

            int rzchg, lzchg, pzchg;
            flush_page_immediate(t, &rf.page, ZTREE_INVALID_ZONE_ID, cf.zone_id,
                                 &rzchg, &rf.zone_id, &rf.slot_id);
            flush_page_immediate(t, &cf.page, cf.zone_id, ZTREE_INVALID_ZONE_ID,
                                 &lzchg, &cf.zone_id, &cf.slot_id);

            parent_insert_sep(&f.page, cidx, sep,
                              cf.node_id, cf.zone_id,
                              rf.node_id, rf.zone_id);
            flush_page_immediate(t, &f.page, f.zone_id, ZTREE_INVALID_ZONE_ID,
                                 &pzchg, &f.zone_id, &f.slot_id);
            (void)rzchg; (void)lzchg;

            if (is_leaf_split)
                atomic_fetch_add_explicit(&g_k3_preemptive_leaf_splits, 1, memory_order_relaxed);
            else
                atomic_fetch_add_explicit(&g_k3_preemptive_int_splits, 1, memory_order_relaxed);

            if (f.node_id == root_nid)
                publish_root_locked(t, root_nid, f.zone_id, f.slot_id, height);
            else if (pzchg && n_pending < MAX_HEIGHT)
            {
                pending[n_pending].id   = f.node_id;
                pending[n_pending].zone = f.zone_id;
                pending[n_pending].lvl  = lvl;
                n_pending++;
            }

            /* Descend into the covering half; release the other two. */
            node_unlock(t, f.node_id);
            if (key < sep)
            {
                node_unlock(t, rf.node_id);
                f = cf;
            }
            else
            {
                node_unlock(t, cf.node_id);
                f = rf;
            }
            lvl = child_lvl;
            m_wr = 1;
            path_ids[lvl] = f.node_id;
        }
        (void)failed;
        (void)seq_snapshot;
    }

    /* Deferred two-stage-tracking refresh — no latches held anymore. */
    for (int i = 0; i < n_pending; i++)
        fix_zone_hint_up(t, path_ids, top_lvl, pending[i].lvl,
                         pending[i].id, pending[i].zone);

    atomic_fetch_add_explicit(&t->stat_inserts, 1, memory_order_relaxed);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW delete path (paper Algorithm 4 + cascade + root collapse)
 *
 * Same b1 concurrency model as base ctree.  In ilayer, internal-node CoW
 * always lands on CNS at slot_id == node_id, so flush_page_immediate
 * returns out_zone_changed == 0 for internals — propagate-zone-up will
 * naturally short-circuit at the leaf's immediate parent.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    ztree_node_id_t node_id;
    uint32_t zone_id;
    uint32_t slot_id;
    ztree_page page;
    uint32_t cidx_from_parent;  /* RIGHTMOST_IDX if linked via ptr_* */
} delete_path_frame;

/* Deferred retirement list.  Used for nodes whose retirement is NOT safe
 * inline because the OLD superblock still references them at the time
 * we'd want to retire — specifically root collapse (old root) and
 * root-leaf-empty (the now-empty root leaf).  cow_delete drains this
 * list AFTER try_publish_root_if_unchanged() succeeds, at which point
 * the new sb has committed and the old root is unreachable from any
 * fresh reader.  Inline merge-sibling retirement bypasses this list. */
#define DELETE_RETIRE_MAX 4
typedef struct {
    int count;
    uint32_t zone[DELETE_RETIRE_MAX];
    ztree_node_id_t nid[DELETE_RETIRE_MAX];
} delete_retire_list;

static inline void retire_list_push(delete_retire_list *r,
                                    uint32_t zone, ztree_node_id_t nid)
{
    if (r->count < DELETE_RETIRE_MAX) {
        r->zone[r->count] = zone;
        r->nid[r->count] = nid;
        r->count++;
    }
}

#define DEL_OK          0
#define DEL_NOT_FOUND   1
#define DEL_RACE        2
#define DEL_NEEDS_MERGE 3

static void delete_release_path_locks(ztree_t *t, delete_path_frame *path, int n)
{
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) {
            if (node_latch_for_id(t, path[i].node_id) ==
                node_latch_for_id(t, path[j].node_id)) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            node_unlock(t, path[i].node_id);
    }
}

static int delete_propagate_zone_up(ztree_t *t,
                                    delete_path_frame *path, int depth,
                                    int64_t key,
                                    propagate_state *prop)
{
    for (int level = depth - 2; level >= 0 && prop->left_zone_changed; level--) {
        delete_path_frame *pf = &path[level];
        node_wrlock(t, pf->node_id);
        if (!load_latest_node(t, pf->zone_id, pf->node_id,
                              &pf->zone_id, &pf->slot_id, &pf->page)) {
            node_unlock(t, pf->node_id);
            return -1;
        }
        ztree_page *par = &pf->page;
        uint32_t cidx = child_pos_for_id(par, prop->left_id);
        if (cidx == UINT32_MAX) {
            cidx = child_pos_for_key(par, key);
            ztree_node_id_t expected = (cidx == RIGHTMOST_IDX)
                                           ? par->ptr_node_id
                                           : par->internal[cidx].child_node_id;
            if (expected != prop->left_id) {
                node_unlock(t, pf->node_id);
                return -1;
            }
        }
        if (cidx == RIGHTMOST_IDX) {
            par->ptr_zone_id = prop->left_zone;
        } else {
            par->internal[cidx].child_zone_id = prop->left_zone;
        }
        flush_page_immediate(t, par, pf->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop->left_zone_changed,
                             &prop->left_zone, &prop->left_slot);
        prop->left_id = par->node_id;
        if (prop->left_zone_changed)
            atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1,
                                      memory_order_relaxed);
        node_unlock(t, pf->node_id);
    }
    return 0;
}

static int try_optimistic_delete(ztree_t *t, int64_t key,
                                 ztree_node_id_t root_nid, uint32_t root_zone,
                                 uint32_t root_slot, uint64_t seq_snapshot,
                                 ztree_node_id_t *out_root_nid,
                                 uint32_t *out_root_zone,
                                 uint32_t *out_root_slot,
                                 int *out_root_changed,
                                 delete_retire_list *retire)
{
    delete_path_frame path[MAX_HEIGHT];
    int depth = 0;

    ztree_node_id_t cur_id = root_nid;
    uint32_t cur_zone = root_zone;
    node_rdlock(t, cur_id);

    while (1) {
        if (depth >= MAX_HEIGHT) {
            node_unlock(t, cur_id);
            fprintf(stderr, "delete: depth overflow\n");
            exit(EXIT_FAILURE);
        }
        delete_path_frame *f = &path[depth];
        f->node_id = cur_id;
        f->cidx_from_parent = (depth == 0) ? RIGHTMOST_IDX
                                            : path[depth - 1].cidx_from_parent;
        if (!load_latest_node(t, cur_zone, cur_id,
                              &f->zone_id, &f->slot_id, &f->page)) {
            node_unlock(t, cur_id);
            return DEL_RACE;
        }
        if (f->page.is_leaf) {
            node_unlock(t, cur_id);
            node_wrlock(t, cur_id);
            if (!load_latest_node(t, cur_zone, cur_id,
                                  &f->zone_id, &f->slot_id, &f->page)) {
                node_unlock(t, cur_id);
                return DEL_RACE;
            }
            if (!f->page.is_leaf) {
                node_unlock(t, cur_id);
                return DEL_RACE;
            }
            if (depth >= 1) {
                delete_path_frame *pf = &path[depth - 1];
                ztree_page latest_par;
                uint32_t pzone, pslot;
                if (!load_latest_node(t, pf->zone_id, pf->node_id,
                                      &pzone, &pslot, &latest_par)
                    || latest_par.is_leaf) {
                    node_unlock(t, cur_id);
                    return DEL_RACE;
                }
                uint32_t new_cidx = child_pos_for_key(&latest_par, key);
                ztree_node_id_t routed = (new_cidx == RIGHTMOST_IDX)
                                              ? latest_par.ptr_node_id
                                              : latest_par.internal[new_cidx].child_node_id;
                if (routed != cur_id) {
                    node_unlock(t, cur_id);
                    return DEL_RACE;
                }
            }
            depth++;
            break;
        }
        uint32_t cidx = child_pos_for_key(&f->page, key);
        ztree_node_id_t next_id = (cidx == RIGHTMOST_IDX)
                                       ? f->page.ptr_node_id
                                       : f->page.internal[cidx].child_node_id;
        if (node_latch_for_id(t, next_id) != node_latch_for_id(t, cur_id)) {
            node_rdlock(t, next_id);
            node_unlock(t, cur_id);
        }
        cur_zone = (cidx == RIGHTMOST_IDX) ? f->page.ptr_zone_id
                                            : f->page.internal[cidx].child_zone_id;
        cur_id = next_id;
        path[depth + 1].cidx_from_parent = cidx;
        depth++;
    }

    delete_path_frame *leaff = &path[depth - 1];
    ztree_page *leaf = &leaff->page;
    int found_idx = -1;
    for (uint32_t i = 0; i < leaf->num_keys; i++) {
        if ((int64_t)leaf->leaf[i].key == key) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        node_unlock(t, leaff->node_id);
        return DEL_NOT_FOUND;
    }

    int is_root_leaf = (depth == 1);
    uint32_t new_count = leaf->num_keys - 1;
    int needs_merge = (!is_root_leaf && new_count < ZTREE_LEAF_MIN);
    if (needs_merge) {
        node_unlock(t, leaff->node_id);
        return DEL_NEEDS_MERGE;
    }

    for (uint32_t i = (uint32_t)found_idx; i + 1 < leaf->num_keys; i++)
        leaf->leaf[i] = leaf->leaf[i + 1];
    leaf->num_keys = new_count;

    if (is_root_leaf && leaf->num_keys == 0) {
        node_unlock(t, leaff->node_id);
        *out_root_nid = ZTREE_INVALID_NODE_ID;
        *out_root_zone = ZTREE_INVALID_ZONE_ID;
        *out_root_slot = ZTREE_INVALID_SLOT_ID;
        *out_root_changed = 1;
        (void)seq_snapshot;
        /* Tree becomes empty.  Old root-leaf is still referenced by the
         * current sb until publish_root commits root=INVALID; defer. */
        retire_list_push(retire, leaff->zone_id, leaff->node_id);
        return DEL_OK;
    }

    propagate_state prop;
    memset(&prop, 0, sizeof prop);
    flush_page_immediate(t, leaf, leaff->zone_id, ZTREE_INVALID_ZONE_ID,
                         &prop.left_zone_changed,
                         &prop.left_zone, &prop.left_slot);
    prop.left_id = leaf->node_id;
    node_unlock(t, leaff->node_id);

    if (delete_propagate_zone_up(t, path, depth, key, &prop) != 0)
        return DEL_RACE;

    if (prop.left_id == root_nid) {
        *out_root_nid = root_nid;
        *out_root_zone = prop.left_zone;
        *out_root_slot = prop.left_slot;
        *out_root_changed = 1;
    } else {
        *out_root_nid = root_nid;
        *out_root_zone = root_zone;
        *out_root_slot = root_slot;
        *out_root_changed = 0;
    }
    return DEL_OK;
}

static int try_pessimistic_delete(ztree_t *t, int64_t key,
                                  ztree_node_id_t root_nid, uint32_t root_zone,
                                  uint32_t root_slot,
                                  ztree_node_id_t *out_root_nid,
                                  uint32_t *out_root_zone,
                                  uint32_t *out_root_slot,
                                  int *out_root_changed,
                                  delete_retire_list *retire)
{
    delete_path_frame path[MAX_HEIGHT];
    int depth = 0;

    ztree_node_id_t cur_id = root_nid;
    uint32_t cur_zone = root_zone;
    uint32_t pending_cidx = RIGHTMOST_IDX;
    node_wrlock(t, cur_id);

    while (1) {
        if (depth >= MAX_HEIGHT) {
            delete_release_path_locks(t, path, depth);
            node_unlock(t, cur_id);
            fprintf(stderr, "delete: depth overflow (pessimistic)\n");
            exit(EXIT_FAILURE);
        }
        delete_path_frame *f = &path[depth];
        f->node_id = cur_id;
        f->cidx_from_parent = pending_cidx;
        if (!load_latest_node(t, cur_zone, cur_id,
                              &f->zone_id, &f->slot_id, &f->page)) {
            delete_release_path_locks(t, path, depth + 1);
            return DEL_RACE;
        }
        if (f->page.is_leaf) {
            depth++;
            break;
        }
        uint32_t cidx = child_pos_for_key(&f->page, key);
        ztree_node_id_t next_id = (cidx == RIGHTMOST_IDX)
                                       ? f->page.ptr_node_id
                                       : f->page.internal[cidx].child_node_id;
        uint32_t next_zone = (cidx == RIGHTMOST_IDX)
                                  ? f->page.ptr_zone_id
                                  : f->page.internal[cidx].child_zone_id;

        int already_held = 0;
        for (int j = 0; j <= depth; j++) {
            if (node_latch_for_id(t, next_id) ==
                node_latch_for_id(t, path[j].node_id)) {
                already_held = 1;
                break;
            }
        }
        if (!already_held)
            node_wrlock(t, next_id);

        cur_id = next_id;
        cur_zone = next_zone;
        pending_cidx = cidx;
        depth++;
    }

    delete_path_frame *leaff = &path[depth - 1];
    ztree_page *leaf = &leaff->page;
    int found_idx = -1;
    for (uint32_t i = 0; i < leaf->num_keys; i++) {
        if ((int64_t)leaf->leaf[i].key == key) {
            found_idx = (int)i;
            break;
        }
    }
    if (found_idx < 0) {
        delete_release_path_locks(t, path, depth);
        return DEL_NOT_FOUND;
    }

    for (uint32_t i = (uint32_t)found_idx; i + 1 < leaf->num_keys; i++)
        leaf->leaf[i] = leaf->leaf[i + 1];
    leaf->num_keys--;

    if (depth == 1 && leaf->num_keys == 0) {
        /* Tree becomes empty; old root-leaf still referenced by current
         * sb until publish_root commits root=INVALID.  Defer retire. */
        retire_list_push(retire, path[0].zone_id, path[0].node_id);
        delete_release_path_locks(t, path, depth);
        *out_root_nid = ZTREE_INVALID_NODE_ID;
        *out_root_zone = ZTREE_INVALID_ZONE_ID;
        *out_root_slot = ZTREE_INVALID_SLOT_ID;
        *out_root_changed = 1;
        return DEL_OK;
    }

    propagate_state prop;
    memset(&prop, 0, sizeof prop);
    int level = depth - 1;
    int merge_done_at_level = -1;
    int root_collapsed = 0;
    ztree_node_id_t collapsed_root_nid = ZTREE_INVALID_NODE_ID;
    uint32_t collapsed_root_zone = ZTREE_INVALID_ZONE_ID;
    uint32_t collapsed_root_slot = ZTREE_INVALID_SLOT_ID;

    while (level > 0) {
        delete_path_frame *cur_f = &path[level];
        delete_path_frame *par_f = &path[level - 1];
        ztree_page *cur = &cur_f->page;
        ztree_page *par = &par_f->page;
        uint32_t cidx = cur_f->cidx_from_parent;

        uint32_t min_keys = cur->is_leaf ? ZTREE_LEAF_MIN : ZTREE_INTERNAL_MIN;
        if (cur->num_keys >= min_keys) {
            break;
        }

        ztree_node_id_t sib_id = ZTREE_INVALID_NODE_ID;
        uint32_t sib_zone = ZTREE_INVALID_ZONE_ID;
        uint32_t sib_cidx = UINT32_MAX;
        int sib_is_right = 0;
        int sib_was_rightmost = 0;
        if (cidx == RIGHTMOST_IDX) {
            if (par->num_keys == 0) break;
            sib_cidx = par->num_keys - 1;
            sib_id = par->internal[sib_cidx].child_node_id;
            sib_zone = par->internal[sib_cidx].child_zone_id;
            sib_is_right = 0;
        } else if (cidx + 1 < par->num_keys) {
            sib_cidx = cidx + 1;
            sib_id = par->internal[sib_cidx].child_node_id;
            sib_zone = par->internal[sib_cidx].child_zone_id;
            sib_is_right = 1;
        } else if (cidx + 1 == par->num_keys) {
            sib_cidx = par->num_keys;
            sib_id = par->ptr_node_id;
            sib_zone = par->ptr_zone_id;
            sib_is_right = 1;
            sib_was_rightmost = 1;
        } else if (cidx > 0) {
            sib_cidx = cidx - 1;
            sib_id = par->internal[sib_cidx].child_node_id;
            sib_zone = par->internal[sib_cidx].child_zone_id;
            sib_is_right = 0;
        } else {
            break;
        }

        int sib_already_held = 0;
        for (int j = 0; j < depth; j++) {
            if (node_latch_for_id(t, sib_id) ==
                node_latch_for_id(t, path[j].node_id)) {
                sib_already_held = 1;
                break;
            }
        }
        if (!sib_already_held)
            node_wrlock(t, sib_id);

        ztree_page sib_page;
        uint32_t sib_actual_zone, sib_actual_slot;
        if (!load_latest_node(t, sib_zone, sib_id,
                              &sib_actual_zone, &sib_actual_slot, &sib_page)) {
            if (!sib_already_held) node_unlock(t, sib_id);
            delete_release_path_locks(t, path, depth);
            return DEL_RACE;
        }

        uint32_t cur_keys = cur->num_keys;
        uint32_t sib_keys = sib_page.num_keys;
        uint32_t merged_keys, max_keys;
        if (cur->is_leaf) {
            merged_keys = cur_keys + sib_keys;
            max_keys = ZTREE_LEAF_ORDER - 1;
        } else {
            merged_keys = cur_keys + 1 + sib_keys;
            max_keys = ZTREE_INTERNAL_ORDER - 1;
        }

        if (merged_keys > max_keys) {
            if (!sib_already_held) node_unlock(t, sib_id);
            break;
        }

        if (cur->is_leaf) {
            if (sib_is_right) {
                for (uint32_t i = 0; i < sib_keys; i++)
                    cur->leaf[cur_keys + i] = sib_page.leaf[i];
                cur->num_keys = cur_keys + sib_keys;
                cur->ptr_node_id = sib_page.ptr_node_id;
                cur->ptr_zone_id = sib_page.ptr_zone_id;
            } else {
                ztree_leaf_entity tmp[ZTREE_LEAF_ORDER];
                for (uint32_t i = 0; i < sib_keys; i++) tmp[i] = sib_page.leaf[i];
                for (uint32_t i = 0; i < cur_keys; i++) tmp[sib_keys + i] = cur->leaf[i];
                for (uint32_t i = 0; i < sib_keys + cur_keys; i++) cur->leaf[i] = tmp[i];
                cur->num_keys = sib_keys + cur_keys;
            }
        } else {
            uint64_t separator;
            if (sib_is_right) {
                separator = par->internal[cidx].key;
            } else {
                separator = par->internal[sib_cidx].key;
            }
            ztree_page *left_p = sib_is_right ? cur : &sib_page;
            ztree_page *right_p = sib_is_right ? &sib_page : cur;

            ztree_internal_entity tmp_ent[ZTREE_INTERNAL_ORDER];
            uint32_t pos = 0;
            for (uint32_t i = 0; i < left_p->num_keys; i++)
                tmp_ent[pos++] = left_p->internal[i];
            tmp_ent[pos].key = separator;
            tmp_ent[pos].child_node_id = left_p->ptr_node_id;
            tmp_ent[pos].child_zone_id = left_p->ptr_zone_id;
            pos++;
            for (uint32_t i = 0; i < right_p->num_keys; i++)
                tmp_ent[pos++] = right_p->internal[i];

            for (uint32_t i = 0; i < pos; i++) cur->internal[i] = tmp_ent[i];
            cur->num_keys = pos;
            cur->ptr_node_id = right_p->ptr_node_id;
            cur->ptr_zone_id = right_p->ptr_zone_id;
        }

        flush_page_immediate(t, cur, cur_f->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed,
                             &prop.left_zone, &prop.left_slot);
        prop.left_id = cur->node_id;

        uint32_t k_idx = sib_is_right ? cidx : sib_cidx;
        uint32_t c_idx = sib_is_right ? (sib_was_rightmost ? par->num_keys
                                                            : sib_cidx)
                                       : sib_cidx;
        for (uint32_t j = k_idx; j + 1 < par->num_keys; j++)
            par->internal[j].key = par->internal[j + 1].key;
        if (sib_was_rightmost) {
            par->ptr_node_id = cur->node_id;
            par->ptr_zone_id = prop.left_zone;
        } else {
            for (uint32_t j = c_idx; j + 1 < par->num_keys; j++) {
                par->internal[j].child_node_id =
                    par->internal[j + 1].child_node_id;
                par->internal[j].child_zone_id =
                    par->internal[j + 1].child_zone_id;
            }
        }
        par->num_keys--;

        if (sib_is_right && !sib_was_rightmost) {
            par->internal[cidx].child_zone_id = prop.left_zone;
            par->internal[cidx].child_node_id = cur->node_id;
        } else if (!sib_is_right) {
            par->internal[sib_cidx].child_node_id = cur->node_id;
            par->internal[sib_cidx].child_zone_id = prop.left_zone;
        }

        if (!sib_already_held) node_unlock(t, sib_id);

        atomic_fetch_add_explicit(&t->stat_delete_merges, 1, memory_order_relaxed);

        if (level - 1 == 0 && par->num_keys == 0) {
            nlt_location_t q = { .zone_id = par->ptr_zone_id,
                                 .node_id = par->ptr_node_id,
                                 .slot_id = ZTREE_INVALID_SLOT_ID };
            nlt_location_t r;
            if (!nlt_lookup(&t->nlt, &q, &r)) {
                delete_release_path_locks(t, path, depth);
                return DEL_RACE;
            }
            root_collapsed = 1;
            collapsed_root_nid = r.node_id;
            collapsed_root_zone = r.zone_id;
            collapsed_root_slot = r.slot_id;
            /* par (old root) was NOT flushed in this branch — its on-disk
             * content still references sib.  Until publish_root swings
             * sb.root to collapsed_root_nid, the old sb keeps the path
             * old_root → {cur, sib} reachable.  Defer BOTH retirements
             * until cow_delete confirms the publish. */
            retire_list_push(retire, par_f->zone_id, par->node_id);
            retire_list_push(retire, sib_actual_zone, sib_id);
            atomic_fetch_add_explicit(&t->stat_delete_root_collapses, 1,
                                      memory_order_relaxed);
            level = 0;
            break;
        }

        flush_page_immediate(t, par, par_f->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed,
                             &prop.left_zone, &prop.left_slot);
        prop.left_id = par->node_id;

        /* par is now persisted without referencing sib_id.  Any fresh
         * reader navigating from par will skip sib_id; old-sb readers
         * holding stale par content can still find sib via NLT until
         * the very next moment — accepted race (existing model). */
        retire_cns_node(t, sib_actual_zone, sib_id);

        if ((level - 1) < merge_done_at_level || merge_done_at_level < 0)
            merge_done_at_level = level - 1;

        if (level - 1 == 0) {
            break;
        }

        if (par->num_keys < ZTREE_INTERNAL_MIN) {
            atomic_fetch_add_explicit(&t->stat_delete_cascades, 1,
                                      memory_order_relaxed);
            level--;
            continue;
        }
        break;
    }

    if (!root_collapsed && merge_done_at_level < 0) {
        delete_path_frame *lf = &path[depth - 1];
        flush_page_immediate(t, &lf->page, lf->zone_id,
                             ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed,
                             &prop.left_zone, &prop.left_slot);
        prop.left_id = lf->page.node_id;
        merge_done_at_level = depth - 1;
    }

    if (!root_collapsed) {
        int start_level = merge_done_at_level - 1;
        for (int lvl = start_level; lvl >= 0 && prop.left_zone_changed; lvl--) {
            delete_path_frame *pf = &path[lvl];
            ztree_page *par = &pf->page;
            uint32_t cidx = child_pos_for_id(par, prop.left_id);
            if (cidx == UINT32_MAX) {
                cidx = child_pos_for_key(par, key);
                ztree_node_id_t expected = (cidx == RIGHTMOST_IDX)
                                                ? par->ptr_node_id
                                                : par->internal[cidx].child_node_id;
                if (expected != prop.left_id) {
                    delete_release_path_locks(t, path, depth);
                    return DEL_RACE;
                }
            }
            if (cidx == RIGHTMOST_IDX) {
                par->ptr_zone_id = prop.left_zone;
            } else {
                par->internal[cidx].child_zone_id = prop.left_zone;
            }
            flush_page_immediate(t, par, pf->zone_id, ZTREE_INVALID_ZONE_ID,
                                 &prop.left_zone_changed,
                                 &prop.left_zone, &prop.left_slot);
            prop.left_id = par->node_id;
            if (prop.left_zone_changed)
                atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1,
                                          memory_order_relaxed);
        }
    }

    delete_release_path_locks(t, path, depth);

    if (root_collapsed) {
        *out_root_nid = collapsed_root_nid;
        *out_root_zone = collapsed_root_zone;
        *out_root_slot = collapsed_root_slot;
        *out_root_changed = 1;
    } else if (prop.left_id == root_nid) {
        *out_root_nid = root_nid;
        *out_root_zone = prop.left_zone;
        *out_root_slot = prop.left_slot;
        *out_root_changed = 1;
    } else {
        *out_root_nid = root_nid;
        *out_root_zone = root_zone;
        *out_root_slot = root_slot;
        *out_root_changed = 0;
    }
    return DEL_OK;
}

static int do_single_delete(cow_tree *t, int64_t key)
{
    for (uint32_t retry = 0;; retry++) {
        if (retry >= 1000000U) {
            fprintf(stderr, "cow_delete: excessive retries (key=%ld)\n", (long)key);
            exit(EXIT_FAILURE);
        }
        if (retry > 0 && (retry & 0xFU) == 0) sched_yield();

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot;
        uint64_t seq_snapshot;
        for (;;) {
            uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 & 1ULL) continue;
            root_nid = atomic_load_explicit(&t->volatile_sb.root_node_id,
                                            memory_order_acquire);
            root_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id,
                                             memory_order_acquire);
            root_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id,
                                             memory_order_acquire);
            uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no,
                                               memory_order_acquire);
            if (s1 == s2 && (s2 & 1ULL) == 0) {
                seq_snapshot = s2;
                break;
            }
        }
        if (root_nid == ZTREE_INVALID_NODE_ID) return 0;

        ztree_node_id_t out_nid = root_nid;
        uint32_t out_zone = root_zone;
        uint32_t out_slot = root_slot;
        int root_changed = 0;
        delete_retire_list retire = { .count = 0 };

        int res = try_optimistic_delete(t, key, root_nid, root_zone, root_slot,
                                        seq_snapshot,
                                        &out_nid, &out_zone, &out_slot,
                                        &root_changed, &retire);
        if (res == DEL_NOT_FOUND) return 0;
        if (res == DEL_RACE) continue;
        if (res == DEL_NEEDS_MERGE) {
            res = try_pessimistic_delete(t, key, root_nid, root_zone, root_slot,
                                         &out_nid, &out_zone, &out_slot,
                                         &root_changed, &retire);
            if (res == DEL_NOT_FOUND) return 0;
            if (res == DEL_RACE) continue;
        }

        if (root_changed &&
            !(out_nid == root_nid && out_zone == root_zone && out_slot == root_slot))
        {
            if (!try_publish_root_if_unchanged(t, seq_snapshot,
                                               out_nid, out_zone, out_slot))
                continue;  /* publish race → retire list dropped, leak this attempt */
        }
        /* Publish (if any) committed, or no publish needed.  Drain the
         * deferred retirements: old root from collapse / empty-root-leaf,
         * plus the sib that was paired with a non-flushed old root. */
        for (int i = 0; i < retire.count; i++)
            retire_cns_node(t, retire.zone[i], retire.nid[i]);
        atomic_fetch_add_explicit(&t->stat_deletes, 1, memory_order_relaxed);
        return 1;
    }
}

int cow_delete(cow_tree *t, int64_t key)
{
    return do_single_delete(t, key);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * cow_open / cow_insert / cow_close
 * ═══════════════════════════════════════════════════════════════════════════ */

cow_tree *cow_open(const char *path)
{
    fprintf(stderr, "[ctree_dynamic] opening %s  cache_sets=%d ways=%d\n",
            path, ZTREE_CACHE_NUM_SETS, ZTREE_CACHE_WAYS);

    ztree_t *t = calloc(1, sizeof *t);
    if (!t)
    {
        perror("cow_open calloc");
        return NULL;
    }

    if (pthread_mutex_init(&t->sb_lock, NULL) != 0)
    {
        perror("cow_open sb_lock");
        free(t);
        return NULL;
    }

    t->fd = zbd_open(path, O_RDWR, &t->info);
    if (t->fd < 0)
    {
        perror("zbd_open");
        free(t);
        return NULL;
    }


    t->direct_fd = open(path, O_RDWR | O_DIRECT);

    /* CNS device is REQUIRED — internal nodes have no ZNS fallback.
     * Default: buffered I/O (page cache absorbs hot-LBA rewrites).
     * Override with env CNS_ODIRECT=1 for O_DIRECT (no kernel cache). */
    {
        const char *od = getenv("CNS_ODIRECT");        /* default ON; CNS_ODIRECT=0 to disable */
        g_cns_odirect = (od && atoi(od) == 0) ? 0 : 1;
        const char *ep = getenv("CTREE_CNS_EPUNCH");   /* default ON; CTREE_CNS_EPUNCH=0 to disable */
        g_cns_epunch = (ep && atoi(ep) == 0) ? 0 : 1;
        fprintf(stderr, "[ctree_hyhost_f2fs] CNS reclaim: %s\n",
                g_cns_epunch ? "epunch (immediate punch on CNS->ZNS)"
                             : "deferred (dirty bitmap + batched cow_gc_cns)");
        const char *bp = getenv("CTREE_CNS_BACKPRESSURE_MS");
        if (bp && atoi(bp) >= 0) g_cns_bp_max_ms = (uint64_t)atoi(bp);
        fprintf(stderr, "[ctree_hyhost_f2fs] CNS backpressure cap: %llu ms\n",
                (unsigned long long)g_cns_bp_max_ms);
    }
    int cns_flags = O_RDWR | O_CREAT | (g_cns_odirect ? O_DIRECT : 0);
    for (int k = 0; k < CTREE_CNS_SHARDS; k++)
    {
        char path_k[256];
        snprintf(path_k, sizeof path_k, "%s/nodes.%d.dat", cns_dir(), k);
        int fd = open(path_k, cns_flags, 0644);
        if (fd < 0)
        {
            fprintf(stderr,
                    "[ctree_hyhost_f2fs] FATAL: cannot open CNS file %s: %s\n"
                    "  Ensure F2FS is mounted on %s (dm-hyhost R-region):\n"
                    "    sudo ~/HYSSD/dm-hyhost/scripts/f2fs_setup.sh /dev/nvme0n1 <r_end> %s\n",
                    path_k, strerror(errno), cns_dir(), cns_dir());
            for (int j = 0; j < k; j++) close(t->cns_fd_shard[j]);
            if (t->direct_fd >= 0) close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            exit(EXIT_FAILURE);
        }
        if (ftruncate(fd, 0) != 0)
            fprintf(stderr, "[ctree_dynamic] WARNING: ftruncate(0) on %s: %s\n",
                    path_k, strerror(errno));
        t->cns_fd_shard[k] = fd;
    }
    t->cns_fd = t->cns_fd_shard[0];  /* alias for validity checks */
    fprintf(stderr, "[ctree_hyhost_f2fs] CNS mount: %s (%s)\n",
            cns_dir(), cns_fstype_name(cns_dir()));
    fprintf(stderr,
            "[ctree_hyhost_f2fs] CNS mode: %s (%d shards in %s)\n",
            g_cns_odirect ? "O_DIRECT" : "buffered I/O",
            CTREE_CNS_SHARDS, cns_dir());

    /* No CNS bitmap in this variant — location is determined by pg->is_leaf. */
    /* Allocate cns_bitmap so leaf-spill can track which leaves currently
     * live on CNS.  Internals are always on CNS (separate from the bitmap). */
    t->cns_bitmap_bytes = CTREE_CNS_BITMAP_MAX_NODES / 8;
    t->cns_bitmap = calloc(t->cns_bitmap_bytes, sizeof(*t->cns_bitmap));
    if (!t->cns_bitmap)
    {
        fprintf(stderr, "[ctree_dynamic] FATAL: cannot allocate cns_bitmap\n");
        close(t->cns_fd);
        if (t->direct_fd >= 0) close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        exit(EXIT_FAILURE);
    }
    t->cns_dirty_bitmap = calloc(t->cns_bitmap_bytes, sizeof(*t->cns_dirty_bitmap));
    if (!t->cns_dirty_bitmap)
    {
        fprintf(stderr, "[ctree_dynamic] FATAL: cannot allocate cns_dirty_bitmap\n");
        free(t->cns_bitmap);
        close(t->cns_fd);
        if (t->direct_fd >= 0) close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        exit(EXIT_FAILURE);
    }

    /* Trace: time-stepped sample of node counts, append totals, and the
     * CNS file's *physical* size (cns_phys_bytes — the metric that drops
     * when GC punches holes).  Path differs from ilayer to avoid clashes.
     * Override default with env CTREE_DYNAMIC_TRACE_PATH so buffered/ODIRECT
     * runs can keep their own CSVs side-by-side. */
    {
        const char *trace_path = getenv("CTREE_DYNAMIC_TRACE_PATH");
        if (!trace_path || !*trace_path)
            trace_path = "/tmp/ctree_dynamic_trace.csv";
        t->trace_fp = fopen(trace_path, "w");
        if (!t->trace_fp)
            fprintf(stderr,
                    "[ctree_dynamic] WARNING: cannot open trace %s: %s\n",
                    trace_path, strerror(errno));
        else
            fprintf(stderr,
                    "[ctree_dynamic] trace -> %s\n", trace_path);
    }
    t->trace_start_ns = monotonic_ns();
    if (t->trace_fp)
        fprintf(t->trace_fp,
                "time_sec,zns_current,cns_current,appends,cns_writes,height,cns_phys_bytes,zns_phys_bytes\n");
    atomic_store_explicit(&t->stat_cns_current, 0, memory_order_relaxed);
    atomic_store_explicit(&t->stat_cns_writes, 0, memory_order_relaxed);
    atomic_store_explicit(&t->tree_height, 0, memory_order_relaxed);  /* k3: root LEVEL, leaf=0 */

    t->zones = calloc(t->info.nr_zones, sizeof *t->zones);
    t->zone_wp_bytes = calloc(t->info.nr_zones, sizeof *t->zone_wp_bytes);
    t->zone_full = calloc(t->info.nr_zones, sizeof *t->zone_full);
    t->zone_valid_leaves = calloc(t->info.nr_zones, sizeof *t->zone_valid_leaves);
    if (!t->zones || !t->zone_wp_bytes || !t->zone_full || !t->zone_valid_leaves)
    {
        perror("calloc zones");
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        free(t->zone_valid_leaves);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    unsigned int nr = t->info.nr_zones;
    if (zbd_report_zones(t->fd, 0, 0, ZBD_RO_ALL, t->zones, &nr) != 0)
    {
        perror("zbd_report_zones");
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        free(t->zone_valid_leaves);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    /* hyhost: the device's front zones are the R-region (CNS, owned by
     * dm-hyhost + F2FS).  ctree's ZNS structures (meta z0/1, LLayer z2+) must
     * live in the S-region only, so shift the reported zone array past the
     * reserved (R) zones and shrink nr_zones — logical zone 0 == first S-zone.
     * reserved==0 (real full-ZNS namespace) makes this a no-op. */
    {
        uint32_t reserved = t->info.nr_rzones;
        if (reserved > 0 && reserved < t->info.nr_zones)
        {
            memmove(t->zones, t->zones + reserved,
                    (size_t)(t->info.nr_zones - reserved) * sizeof *t->zones);
            t->info.nr_zones -= reserved;
            fprintf(stderr,
                    "[ctree_hyhost_f2fs] reserved(R)=%u zones skipped; ZNS uses %u "
                    "S-zones (logical 0 = phys zone %u)\n",
                    reserved, t->info.nr_zones, reserved);
        }
        else
        {
            fprintf(stderr, "[ctree_hyhost_f2fs] reserved(R)=%u; no zone offset "
                    "(ZNS uses all %u zones)\n", reserved, t->info.nr_zones);
        }
    }

    for (uint32_t z = 0; z < t->info.nr_zones; z++)
    {
        atomic_store_explicit(&t->zone_wp_bytes[z], t->zones[z].wp, memory_order_relaxed);
        atomic_store_explicit(&t->zone_full[z],
                              (t->zones[z].cond == ZBD_ZONE_COND_FULL) ? 1 : 0,
                              memory_order_relaxed);
    }

    cache_init(t);

    uint64_t zone_pages = (t->info.nr_zones > 0)
                              ? (t->zones[0].capacity / ZTREE_PAGE_SIZE)
                              : 65536ULL;
    size_t tracker_cap = (size_t)zone_pages * 4ULL;
    if (tracker_cap < 4096) tracker_cap = 4096;
    size_t zones_cap = (size_t)t->info.nr_zones * 2ULL;
    if (zones_cap < 256) zones_cap = 256;
    nlt_init(&t->nlt, zones_cap, tracker_cap);

    t->node_latches = calloc(ZTREE_NODE_LATCH_BUCKETS, sizeof(*t->node_latches));
    if (!t->node_latches)
    {
        perror("cow_open node_latches");
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        free(t->zone_valid_leaves);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }
    for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
    {
        if (pthread_rwlock_init(&t->node_latches[i], NULL) != 0)
        {
            perror("cow_open node_latches init");
            for (size_t j = 0; j < i; j++)
                pthread_rwlock_destroy(&t->node_latches[j]);
            free(t->node_latches);
            cache_destroy(t);
            nlt_destroy(&t->nlt);
            free(t->zones);
            free(t->zone_wp_bytes);
            free(t->zone_full);
            free(t->zone_valid_leaves);
            close(t->cns_fd);
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    t->zone_write_locks = calloc(t->info.nr_zones, sizeof(*t->zone_write_locks));
    if (!t->zone_write_locks)
    {
        perror("cow_open zone_write_locks");
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        free(t->zone_valid_leaves);
        close(t->cns_fd);
        if (t->direct_fd >= 0)
            close(t->direct_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }
    for (uint32_t z = 0; z < t->info.nr_zones; z++)
    {
        if (pthread_mutex_init(&t->zone_write_locks[z], NULL) != 0)
        {
            perror("cow_open zone_write_locks init");
            for (uint32_t j = 0; j < z; j++)
                pthread_mutex_destroy(&t->zone_write_locks[j]);
            free(t->zone_write_locks);
            for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
                pthread_rwlock_destroy(&t->node_latches[i]);
            free(t->node_latches);
            cache_destroy(t);
            nlt_destroy(&t->nlt);
            free(t->zones);
            free(t->zone_wp_bytes);
            free(t->zone_full);
            free(t->zone_valid_leaves);
            close(t->cns_fd);
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    /* back-to-front layout: leaf LLayer = [0, nr_zones-2), meta = top 2 zones.
     * Allocator fills high→low so the low (R/S-boundary) zones stay empty for
     * dm grow.  ILayer pool dormant (internals on CNS). */
    uint32_t ilayer_pool_base  = 0U;   /* dormant; overlaps hot pool start */
    uint32_t ilayer_pool_size  = 1U;   /* allocator clamps to >=1, never used */
    uint32_t ilayer_init       = 1U;

    uint32_t llayer_pool_base  = 0U;   /* meta moved to top 2 zones */
    uint32_t llayer_pool_total = (t->info.nr_zones > 2)
                                     ? (t->info.nr_zones - 2) : 1;

    uint32_t hot_pool_size  = (llayer_pool_total * 80U) / 100U;
    uint32_t cold_pool_size = llayer_pool_total - hot_pool_size;
    if (hot_pool_size  < ZTREE_LZGROUP_HOT_INIT)  hot_pool_size  = ZTREE_LZGROUP_HOT_INIT;
    if (cold_pool_size < ZTREE_LZGROUP_COLD_INIT) cold_pool_size = ZTREE_LZGROUP_COLD_INIT;

    uint32_t hot_pool_base  = llayer_pool_base;
    uint32_t cold_pool_base = llayer_pool_base + hot_pool_size;

    zone_alloc_init(&t->za,
                    ilayer_pool_base, ilayer_pool_size, ilayer_init,
                    hot_pool_base,  hot_pool_size,  ZTREE_LZGROUP_HOT_INIT,
                    cold_pool_base, cold_pool_size, ZTREE_LZGROUP_COLD_INIT,
                    t->zone_full, t->zone_wp_bytes, t->zones, t->info.nr_zones,
                    t->fd, (uint64_t)t->info.zone_size,
                    t->zone_write_locks);

    /* back-to-front: fill leaf pools high→low so low (R/S-boundary) zones stay
     * empty for dm grow.  meta is at the top 2 zones (out of the pools). */
    t->za.reverse = 1;

    /* Active-zone admission: 13 leaf + 1 meta zone = device max 14. */
    t->za.admission_enabled = 1;
    t->za.active_cap = 13;
    atomic_store_explicit(&t->za.active_zones, 0, memory_order_relaxed);

    fprintf(stderr,
            "[ctree_dynamic] LLayer hot-pool [%u, %u)  init_group=%u"
            "  cold-pool [%u, %u)  init_group=%u\n",
            hot_pool_base,  hot_pool_base  + hot_pool_size,  ZTREE_LZGROUP_HOT_INIT,
            cold_pool_base, cold_pool_base + cold_pool_size, ZTREE_LZGROUP_COLD_INIT);

    load_superblock(t);

    /* k3 ORWC descends by level (leaf=0), so the root level must be known
     * before the first insert.  Fresh device -> INVALID root -> height 0;
     * best-effort walk for a reopened tree (needs a populated NLT). */
    {
        uint32_t h = 0;
        ztree_node_id_t hn = atomic_load_explicit(&t->volatile_sb.root_node_id,
                                                  memory_order_acquire);
        uint32_t hz = atomic_load_explicit(&t->volatile_sb.root_zone_id,
                                           memory_order_acquire);
        ztree_page hp;
        uint32_t wz, ws;
        while (hn != ZTREE_INVALID_NODE_ID && h < MAX_HEIGHT
               && load_latest_node(t, hz, hn, &wz, &ws, &hp)
               && !hp.is_leaf)
        {
            hn = (hp.num_keys > 0) ? hp.internal[0].child_node_id : hp.ptr_node_id;
            hz = (hp.num_keys > 0) ? hp.internal[0].child_zone_id : hp.ptr_zone_id;
            h++;
        }
        atomic_store_explicit(&t->tree_height, h, memory_order_relaxed);
    }

    atomic_store_explicit(&t->dirty_sb, false, memory_order_relaxed);
    atomic_store_explicit(&t->stop_flusher, false, memory_order_relaxed);
    atomic_store_explicit(&t->txg_next, 0, memory_order_relaxed);
    atomic_store_explicit(&t->txg_synced, 0, memory_order_relaxed);

    if (pthread_create(&t->flusher_tid, NULL, sb_flusher_thread, t) != 0)
    {
        perror("pthread_create sb_flusher_thread");
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        zone_alloc_destroy(&t->za);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        free(t->zone_valid_leaves);
        close(t->cns_fd);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    /* dynamic_gc_thread triggers: periodic interval and/or CNS HWM.
     *   CTREE_DYNAMIC_GC_INTERVAL_MS   periodic cadence (default 0 = off)
     *   CTREE_DYNAMIC_CNS_HWM_RATIO    HWM as fraction (default 0.90; 0 off)
     *   CTREE_DYNAMIC_CNS_HWM_POLL_MS  HWM poll cadence (default 200)
     * Thread starts if either trigger is enabled. */
    {
        const char *env = getenv("CTREE_DYNAMIC_GC_INTERVAL_MS");
        long ms = env ? atol(env) : 0;
        if (ms < 0) ms = 0;
        g_gc_interval_ms = (unsigned)ms;
    }
    {
        const char *env = getenv("CTREE_DYNAMIC_CNS_HWM_RATIO");
        double r = env ? atof(env) : 0.90;
        if (r < 0.0 || r > 1.0) r = 0.0;
        g_cns_hwm_ratio = r;
    }
    {
        const char *env = getenv("CTREE_DYNAMIC_CNS_HWM_POLL_MS");
        long ms = env ? atol(env) : 200;
        if (ms < 50) ms = 50;
        g_cns_hwm_poll_ms = (unsigned)ms;
    }
    {
        const char *env = getenv("CTREE_DYNAMIC_CNS_FREEZE_PCT");
        double p = env ? atof(env) : 0.0;
        if (p < 0.0 || p > 100.0) p = 0.0;
        g_cns_freeze_pct = p;
        /* Freeze implies no CNS GC: silence the HWM/periodic triggers. */
        if (g_cns_freeze_pct > 0.0)
        {
            g_cns_hwm_ratio  = 0.0;
            g_gc_interval_ms = 0;
        }
    }
    {
        const char *env = getenv("CTREE_DYNAMIC_CNS_GC_FREEZE");
        g_gc_freeze_spill = (env && atoi(env) == 0) ? 0 : 1;
    }
    {
        const char *env = getenv("CNS_GC_BLOCKING");
        g_cns_gc_blocking = (env && atoi(env) != 0) ? 1 : 0;
        if (g_cns_gc_blocking
            && !atomic_load_explicit(&g_insert_pause_initialised,
                                     memory_order_acquire))
        {
            pthread_rwlockattr_t attr;
            pthread_rwlockattr_init(&attr);
            /* Writer preference: GC's wrlock must not starve under the
             * continuous worker rdlock stream. */
            pthread_rwlockattr_setkind_np(
                &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
            pthread_rwlock_init(&g_insert_pause, &attr);
            pthread_rwlockattr_destroy(&attr);
            atomic_store_explicit(&g_insert_pause_initialised, true,
                                  memory_order_release);
        }
    }
    {
        /* Daemon-driven grow (HyZNS §5.6.3).  The hyhostd daemon owns the grow
         * policy + calls the F2FS resize ioctl; ctree executes the 2-phase
         * boundary handshake (prepare/commit) via control files. */
        const char *mv = getenv("CTREE_CNS_MAX_RZONES");
        g_max_rzones = (mv && atoi(mv) > 0) ? (uint32_t)atoi(mv) : 0;
        const char *dg = getenv("CTREE_DAEMON_GROW");
        g_daemon_grow = (dg && atoi(dg) != 0 && g_max_rzones > 0) ? 1 : 0;
        const char *cd = getenv("CTREE_RESIZE_CTRL_DIR");
        if (cd && cd[0])
            snprintf(g_resize_ctrl_dir, sizeof g_resize_ctrl_dir, "%s", cd);
        const char *ct = getenv("CTREE_RESIZE_COMMIT_TIMEOUT_MS");
        if (ct && atoi(ct) > 0) g_resize_commit_timeout_ms = (uint64_t)atoi(ct);
        atomic_store_explicit(&g_cur_rzones, t->info.nr_rzones, memory_order_relaxed);
        if (g_daemon_grow
            && !atomic_load_explicit(&g_insert_pause_initialised,
                                     memory_order_acquire))
        {
            pthread_rwlockattr_t attr;
            pthread_rwlockattr_init(&attr);
            pthread_rwlockattr_setkind_np(
                &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
            pthread_rwlock_init(&g_insert_pause, &attr);
            pthread_rwlockattr_destroy(&attr);
            atomic_store_explicit(&g_insert_pause_initialised, true,
                                  memory_order_release);
        }
        if (g_daemon_grow)
            fprintf(stderr,
                    "[ctree_daemon] daemon-driven grow ENABLED: cur=%u max=%u zones  "
                    "ctrl_dir=%s  commit_timeout=%llums\n",
                    t->info.nr_rzones, g_max_rzones,
                    g_resize_ctrl_dir[0] ? g_resize_ctrl_dir : cns_dir(),
                    (unsigned long long)g_resize_commit_timeout_ms);
    }
    if (g_gc_interval_ms > 0 || g_cns_hwm_ratio > 0.0 || g_cns_freeze_pct > 0.0
        || g_daemon_grow)
    {
        atomic_store_explicit(&g_gc_stop, false, memory_order_relaxed);
        atomic_store_explicit(&g_gc_running, true, memory_order_release);
        if (pthread_create(&g_gc_tid, NULL, dynamic_gc_thread, t) != 0)
        {
            perror("pthread_create dynamic_gc_thread");
            atomic_store_explicit(&g_gc_running, false, memory_order_release);
        }
        else
        {
            if (g_cns_freeze_pct > 0.0)
                fprintf(stderr,
                        "[ctree_dynamic] CNS FREEZE mode: leaf spill stops at "
                        "%.1f%% CNS, no CNS GC (poll %ums)\n",
                        g_cns_freeze_pct, g_cns_hwm_poll_ms);
            else
                fprintf(stderr,
                        "[ctree_dynamic] CNS GC thread enabled: periodic=%ums  "
                        "hwm=%.1f%%  mode=%s\n",
                        g_gc_interval_ms, g_cns_hwm_ratio * 100.0,
                        g_cns_gc_blocking ? "BLOCKING" : "non-blocking");
        }
    }
    else
    {
        fprintf(stderr,
                "[ctree_dynamic] CNS GC thread disabled (no periodic, no HWM)\n");
    }

    /* Periodic ZNS GC.  Default 0 (disabled).  Also requires
     * CTREE_DYNAMIC_ZNS_GC=1 for cow_gc_zns to do real work. */
    {
        const char *env = getenv("CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS");   /* default 5000ms */
        long ms = env ? atol(env) : 5000;
        if (ms < 0) ms = 0;
        g_zns_gc_interval_ms = (unsigned)ms;
    }
    if (g_zns_gc_interval_ms > 0)
    {
        atomic_store_explicit(&g_zns_gc_stop, false, memory_order_relaxed);
        atomic_store_explicit(&g_zns_gc_running, true, memory_order_release);
        if (pthread_create(&g_zns_gc_tid, NULL,
                           dynamic_zns_gc_thread, t) != 0)
        {
            perror("pthread_create dynamic_zns_gc_thread");
            atomic_store_explicit(&g_zns_gc_running, false,
                                  memory_order_release);
        }
        else
        {
            fprintf(stderr,
                    "[ctree_dynamic] periodic ZNS GC enabled: every %u ms\n",
                    g_zns_gc_interval_ms);
        }
    }

    return t;
}

void cow_insert(cow_tree *t, int64_t key, const char *value)
{
    /* Default: CNS/ZNS GC are non-blocking (per-node latches), inserts run free.
     * CNS_GC_BLOCKING=1: pause inserts under the rdlock while CNS GC holds the
     * wrlock (stop-the-world, for comparison). */
    if (g_daemon_grow
        || (g_cns_gc_blocking
            && atomic_load_explicit(&g_gc_running, memory_order_acquire)))
    {
        pthread_rwlock_rdlock(&g_insert_pause);   /* grow/commit holds wrlock briefly */
        do_single_insert(t, key, value);
        pthread_rwlock_unlock(&g_insert_pause);
        return;
    }
    do_single_insert(t, key, value);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dynamic CNS — eviction + GC
 *
 * cow_evict_cns_leaves():
 *   Walk the bitmap.  For each CNS-resident node that is a *leaf*, force-
 *   migrate it back to its home ZNS zone (or a freshly allocated one).
 *   Internal nodes are left on CNS (they always live there).  Non-blocking:
 *   serialises with foreground inserts per-node via node_trywrlock (same as
 *   the ZNS GC migrate); contended nids are skipped and retried next cycle.
 *
 * cow_gc_cns():
 *   Punch (FALLOC_FL_PUNCH_HOLE) the CNS slot of every freshly-dead leaf
 *   (cns_dirty bit set) so F2FS reclaims the blocks.  Non-blocking: re-verifies
 *   the slot dead under the node latch before punching.  cns_physical_bytes()
 *   shrinks as a result.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Verbosity gate for noisy diagnostic prints (NLT miss in ztree_find,
 * GC SAFETY warnings in cow_gc_cns).  Both are symptoms of the known
 * residual 0.5% concurrent-split race (see project_dynamic_insert_race
 * memory).  Quiet by default; set CTREE_DYNAMIC_VERBOSE=1 to re-enable
 * (e.g., when debugging a fresh race lead). */
static inline int dynamic_verbose(void)
{
    static int inited = 0, on = 0;
    if (!inited) {
        const char *e = getenv("CTREE_DYNAMIC_VERBOSE");
        on = (e && *e && *e != '0');
        inited = 1;
    }
    return on;
}

/* Phase marker: append (time_sec_since_cow_open, name) to a side file
 * for plot annotation.  Path: CTREE_DYNAMIC_PHASE_PATH env, or default
 * "<trace_path>.phases".  Caller (binding's Maintenance) invokes this
 * at workload phase boundaries; plotter draws a vline + region label
 * at each marker. */
void cow_phase_mark(cow_tree *t, const char *name)
{
    static FILE *phase_fp = NULL;
    static pthread_mutex_t phase_mu = PTHREAD_MUTEX_INITIALIZER;
    static int tried_open = 0;
    if (!t || !name) return;
    pthread_mutex_lock(&phase_mu);
    if (!phase_fp && !tried_open) {
        tried_open = 1;
        const char *path = getenv("CTREE_DYNAMIC_PHASE_PATH");
        char buf[1024];
        if (!path || !*path) {
            const char *tp = getenv("CTREE_DYNAMIC_TRACE_PATH");
            if (tp && *tp) {
                snprintf(buf, sizeof buf, "%s.phases", tp);
                path = buf;
            }
        }
        if (path && *path) {
            phase_fp = fopen(path, "w");
            if (phase_fp) {
                fprintf(phase_fp, "time_sec,phase\n");
                fprintf(stderr, "[ctree_dynamic] phase log -> %s\n", path);
            }
        }
    }
    if (phase_fp) {
        double sec = (double)(monotonic_ns() - t->trace_start_ns) / 1e9;
        fprintf(phase_fp, "%.3f,%s\n", sec, name);
        fflush(phase_fp);
    }
    pthread_mutex_unlock(&phase_mu);
}

/* Parallel maintenance: split nid range across N workers.  Knob:
 * CTREE_DYNAMIC_MAINT_THREADS (default 8).  Foreground writes assumed
 * quiesced (chain runner gates phases). */
static int dyn_maint_threads(void)
{
    const char *e = getenv("CTREE_DYNAMIC_MAINT_THREADS");
    int n = e ? atoi(e) : 8;
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return n;
}

struct dyn_evict_arg {
    cow_tree *t;
    uint32_t  nid_start, nid_end;
    size_t    evicted, skipped_internal, skipped_full;
};

static void *dyn_evict_worker(void *p)
{
    struct dyn_evict_arg *a = p;
    cow_tree *t = a->t;

    for (uint32_t nid = a->nid_start; nid < a->nid_end; nid++)
    {
        if (nid == 0) continue;
        if (!cns_bitmap_test(t, nid)) continue;     /* cheap pre-filter */
        if (!node_trywrlock(t, nid)) continue;      /* contended — next cycle */

        /* Re-verify under the latch that the leaf still lives on CNS (stable
         * vs. a concurrent foreground CoW of this nid, which also holds this
         * latch).  NLT[nid] only changes while its node latch is held. */
        nlt_location_t q = { .zone_id = CTREE_CNS_ZONE_ID, .node_id = nid,
                             .slot_id = ZTREE_INVALID_SLOT_ID };
        nlt_location_t r;
        if (!nlt_lookup(&t->nlt, &q, &r) || r.zone_id != CTREE_CNS_ZONE_ID)
        {
            cns_bitmap_clear(t, nid);
            cns_free_slot(t, nid);
            node_unlock(t, nid);
            continue;
        }

        ztree_page p;
        load_page_from_cns(t, r.slot_id, &p);
        if (!p.is_leaf) { node_unlock(t, nid); a->skipped_internal++; continue; }

        uint32_t home_zone = p.zone_id;
        bool home_in_range = (home_zone < CTREE_LEAF_ZONE_END(t));  /* [0,nr-2) */
        bool home_full = !home_in_range
                      || atomic_load_explicit(&t->zone_full[home_zone],
                                              memory_order_acquire);
        /* Treat post-GC-reset (EMPTY) home as not-valid so we go through
         * zone_alloc_llayer and its active-zone cap.  Direct-pick here
         * would bypass the cap and trip the device's max_active_zones. */
        bool home_empty = home_in_range
                       && atomic_load_explicit(&t->zone_wp_bytes[home_zone],
                                               memory_order_acquire)
                          <= t->zones[home_zone].start;
        bool home_valid = !home_full && !home_empty;
        uint32_t target = home_valid
                              ? home_zone
                              : zone_alloc_llayer(&t->za, p.node_id, ZTREE_INVALID_ZONE_ID);
        if (target == ZTREE_INVALID_ZONE_ID)
        {
            /* At active cap — skip this leaf, stays on CNS. */
            node_unlock(t, nid);
            a->skipped_full++;
            continue;
        }

        /* Blocking — workers on same target zone queue, never CNS-fallback. */
        pthread_mutex_lock(&t->zone_write_locks[target]);
        if (atomic_load_explicit(&t->zone_full[target], memory_order_acquire))
        {
            pthread_mutex_unlock(&t->zone_write_locks[target]);
            node_unlock(t, nid);
            a->skipped_full++;
            continue;
        }

        uint64_t zone_end = t->zones[target].start + t->zones[target].capacity;
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target],
                                           memory_order_relaxed);
        if (wp + ZTREE_PAGE_SIZE > zone_end)
        {
            pthread_mutex_unlock(&t->zone_write_locks[target]);
            zone_mark_full(&t->za, target);
            zone_seal_and_replace(&t->za, target);
            node_unlock(t, nid);
            a->skipped_full++;
            continue;
        }
        if (wp == t->zones[target].start && !zone_admission_acquire(&t->za, target))
        {
            /* At active cap — leaf stays on CNS, retried next GC. */
            pthread_mutex_unlock(&t->zone_write_locks[target]);
            node_unlock(t, nid);
            a->skipped_full++;
            continue;
        }

        atomic_store_explicit(&t->zone_wp_bytes[target], wp + ZTREE_PAGE_SIZE,
                              memory_order_relaxed);
        uint64_t cur_wp = wp;
        uint32_t slot_id = (uint32_t)((cur_wp - t->zones[target].start) / ZTREE_PAGE_SIZE);
        p.zone_id = target;
        p.slot_id = slot_id;

        _Alignas(ZTREE_PAGE_SIZE) char bounce[ZTREE_PAGE_SIZE];
        memcpy(bounce, &p, ZTREE_PAGE_SIZE);
        int wfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;
        ssize_t pwr = pwrite(wfd, bounce, ZTREE_PAGE_SIZE, (off_t)cur_wp);
        if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
        {
            fprintf(stderr,
                    "cow_evict_cns_leaves: pwrite nid=%u target=%u err=%s\n",
                    nid, target, strerror(errno));
            pthread_mutex_unlock(&t->zone_write_locks[target]);
            node_unlock(t, nid);
            continue;
        }
        pthread_mutex_unlock(&t->zone_write_locks[target]);

        nlt_location_t newloc = { .zone_id = target, .node_id = nid, .slot_id = slot_id };
        nlt_update_migrate(&t->nlt, &newloc, CTREE_CNS_ZONE_ID);

        cns_bitmap_clear(t, nid);
        cns_free_slot(t, nid);
        atomic_fetch_sub_explicit(&t->stat_cns_current, 1, memory_order_relaxed);
        /* CNS → ZNS: leaf now resident in 'target' for ZNS GC accounting. */
        zone_valid_leaves_move(t, CTREE_CNS_ZONE_ID, target);

        node_unlock(t, nid);
        a->evicted++;
    }
    return NULL;
}

size_t cow_evict_cns_leaves(cow_tree *t)
{
    if (!t || t->cns_fd < 0 || !t->cns_bitmap) return 0;
    force_trace_sample(t);  /* pre-evict snapshot for plot M-band */

    uint32_t max_node = atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
    int N = dyn_maint_threads();
    if ((uint32_t)N > max_node && max_node > 0) N = (int)max_node;
    if (N < 1) N = 1;

    pthread_t tids[32];
    struct dyn_evict_arg args[32];
    uint32_t per = (max_node + N - 1) / N;
    for (int i = 0; i < N; i++)
    {
        uint32_t s = (uint32_t)i * per;
        uint32_t e = ((uint32_t)(i + 1) * per > max_node) ? max_node : ((uint32_t)(i + 1) * per);
        args[i] = (struct dyn_evict_arg){ .t = t, .nid_start = s, .nid_end = e };
        pthread_create(&tids[i], NULL, dyn_evict_worker, &args[i]);
    }
    size_t evicted = 0, skipped_internal = 0, skipped_full = 0;
    for (int i = 0; i < N; i++)
    {
        pthread_join(tids[i], NULL);
        evicted += args[i].evicted;
        skipped_internal += args[i].skipped_internal;
        skipped_full += args[i].skipped_full;
    }

    force_trace_sample(t);
    fprintf(stderr,
            "[ctree_dynamic] cow_evict_cns_leaves: evicted=%zu  internal_skipped=%zu  "
            "zone_full_skipped=%zu  cns_phys=%zu KB  (workers=%d)\n",
            evicted, skipped_internal, skipped_full,
            cns_physical_bytes(t) / 1024, N);
    return evicted;
}

void cow_count_cns_residents(cow_tree *t, size_t *out_internals, size_t *out_leaves)
{
    *out_internals = 0;
    *out_leaves = 0;
    if (!t || !t->cns_bitmap) return;
    uint32_t max_node = atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
    for (uint32_t nid = 1; nid < max_node; nid++)
    {
        if (!cns_bitmap_test(t, nid)) continue;
        nlt_location_t q = { .zone_id = CTREE_CNS_ZONE_ID, .node_id = nid,
                             .slot_id = ZTREE_INVALID_SLOT_ID };
        nlt_location_t r;
        if (!nlt_lookup(&t->nlt, &q, &r) || r.zone_id != CTREE_CNS_ZONE_ID)
            continue;
        ztree_page p;
        load_page_from_cns(t, r.slot_id, &p);
        if (p.is_leaf) (*out_leaves)++;
        else (*out_internals)++;
    }
}

struct dyn_gc_arg {
    cow_tree *t;
    uint32_t  nid_start, nid_end, max_node;
    size_t    total_punched_pages;
    size_t    punched_internal_caught;
};

/* Non-blocking punch of one dead CNS slot.  Serialises with foreground CNS
 * writes via the per-node latch (held across flush_page_immediate): the slot
 * is re-verified dead under the latch, so a concurrent re-spill to the same
 * nid can never have its just-written page punched.  Skips contended nids
 * (trylock fail) — they are retried next GC cycle.  Only nids flagged dirty
 * (a live 1->0 transition) are considered, so never-CNS leaves and already-
 * punched slots cost a single cheap bitmap read, not a latch + syscall. */
static void *dyn_gc_worker(void *p)
{
    struct dyn_gc_arg *a = p;
    cow_tree *t = a->t;

    for (uint32_t nid = a->nid_start; nid < a->nid_end; nid++)
    {
        if (nid == 0 || nid >= a->max_node) continue;
        if (!cns_dirty_test(t, nid)) continue;          /* no pending death */
        if (cns_bitmap_test(t, nid)) continue;          /* live again */
        if (!node_trywrlock(t, nid)) continue;          /* contended — next cycle */

        /* Re-verify dead under the latch (stable vs. concurrent CNS write). */
        if (cns_bitmap_test(t, nid)) {
            cns_dirty_clear(t, nid);
            node_unlock(t, nid);
            continue;
        }
        nlt_location_t q = { .zone_id = CTREE_CNS_ZONE_ID, .node_id = nid,
                             .slot_id = ZTREE_INVALID_SLOT_ID };
        nlt_location_t r;
        if (nlt_lookup(&t->nlt, &q, &r) && r.zone_id == CTREE_CNS_ZONE_ID) {
            a->punched_internal_caught++;               /* still mapped to CNS */
        } else if (fallocate(cns_shard_fd(t, nid),
                             FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                             cns_slot_offset(nid), (off_t)ZTREE_PAGE_SIZE) == 0) {
            a->total_punched_pages += 1;
        }
        cns_dirty_clear(t, nid);
        node_unlock(t, nid);
    }
    return NULL;
}

size_t cow_gc_cns(cow_tree *t)
{
    if (!t || t->cns_fd < 0 || !t->cns_bitmap) return 0;
    size_t before = cns_physical_bytes(t);

    uint32_t max_node = atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
    int N = dyn_maint_threads();
    if ((uint32_t)N > max_node && max_node > 0) N = (int)max_node;
    if (N < 1) N = 1;

    pthread_t tids[32];
    struct dyn_gc_arg args[32];
    uint32_t per = (max_node + N - 1) / N;
    for (int i = 0; i < N; i++) {
        uint32_t s = (uint32_t)i * per;
        uint32_t e = ((uint32_t)(i + 1) * per > max_node) ? max_node : ((uint32_t)(i + 1) * per);
        args[i] = (struct dyn_gc_arg){ .t = t, .nid_start = s, .nid_end = e,
                                       .max_node = max_node };
        pthread_create(&tids[i], NULL, dyn_gc_worker, &args[i]);
    }
    size_t total_punched_pages = 0, punched_internal_caught = 0;
    for (int i = 0; i < N; i++) {
        pthread_join(tids[i], NULL);
        total_punched_pages    += args[i].total_punched_pages;
        punched_internal_caught += args[i].punched_internal_caught;
    }

    if (punched_internal_caught && dynamic_verbose())
        fprintf(stderr,
                "[ctree_dynamic] GC: %zu unsafe slots caught\n",
                punched_internal_caught);

    size_t after = cns_physical_bytes(t);
    force_trace_sample(t);
    fprintf(stderr,
            "[ctree_dynamic] cow_gc_cns: punched_pages=%zu  cns_phys: %zu → %zu KB  "
            "(freed %zu KB, workers=%d)\n",
            total_punched_pages,
            before / 1024, after / 1024,
            (before > after ? (before - after) / 1024 : 0), N);
    return (before > after) ? (before - after) : 0;
}

/* ZNS GC: reclaim stale data by migrating live leaves from sealed zones
 * with high stale ratios, then reset the victim zone. */
#define ZNS_GC_STALE_THRESHOLD 0.5

/* Migrate one live leaf to a new ZNS zone.
 * Takes the per-leaf wrlock to serialize with concurrent foreground CoW.
 * After locking, recheck NLT[nid]; skip if the leaf was already moved. */
/* Read root snapshot via seqlock. */
static uint64_t gc_root_snapshot(ztree_t *t,
                                 ztree_node_id_t *out_nid,
                                 uint32_t *out_zone,
                                 uint32_t *out_slot) {
    for (;;) {
        uint64_t s1 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 & 1ULL) continue;
        *out_nid  = atomic_load_explicit(&t->volatile_sb.root_node_id, memory_order_acquire);
        *out_zone = atomic_load_explicit(&t->volatile_sb.root_zone_id, memory_order_acquire);
        *out_slot = atomic_load_explicit(&t->volatile_sb.root_slot_id, memory_order_acquire);
        uint64_t s2 = atomic_load_explicit(&t->volatile_sb.seq_no, memory_order_acquire);
        if (s1 == s2 && (s2 & 1ULL) == 0) return s2;
    }
}

/* Descend from root using `key`, find parent of target_nid, rewrite its
 * child_zone_id to NLT-current.  Parent is CNS — no further cascade. */
static int gc_cascade_parent(ztree_t *t,
                             ztree_node_id_t target_nid,
                             int64_t key) {
    ztree_node_id_t root_nid;
    uint32_t root_zone, root_slot;
    uint64_t seq_snapshot = gc_root_snapshot(t, &root_nid, &root_zone, &root_slot);
    if (root_nid == ZTREE_INVALID_NODE_ID) return 0;

    nlt_location_t want_q = { .zone_id = ZTREE_INVALID_ZONE_ID,
                              .node_id = target_nid,
                              .slot_id = ZTREE_INVALID_SLOT_ID };
    nlt_location_t want;
    if (!nlt_lookup(&t->nlt, &want_q, &want)) return 0;

    if (root_nid == target_nid) {
        if (root_zone == want.zone_id && root_slot == want.slot_id) return 1;
        return try_publish_root_if_unchanged(t, seq_snapshot,
                                             target_nid, want.zone_id, want.slot_id);
    }

    insert_path_frame path[MAX_HEIGHT];
    int depth = 0;
    ztree_node_id_t cur_id   = root_nid;
    uint32_t        cur_zone = root_zone;

    while (1) {
        if (depth >= MAX_HEIGHT) return 0;
        insert_path_frame *f = &path[depth++];
        f->node_id = cur_id;
        if (!load_latest_node(t, cur_zone, cur_id,
                              &f->zone_id, &f->slot_id, &f->page))
            return 0;
        if (f->page.is_leaf) return 0;
        uint32_t cidx = child_pos_for_key(&f->page, key);
        ztree_node_id_t child_nid;
        uint32_t        child_zone;
        if (cidx == RIGHTMOST_IDX) {
            child_nid  = f->page.ptr_node_id;
            child_zone = f->page.ptr_zone_id;
        } else {
            child_nid  = f->page.internal[cidx].child_node_id;
            child_zone = f->page.internal[cidx].child_zone_id;
        }
        if (child_nid == target_nid) break;
        cur_id   = child_nid;
        cur_zone = child_zone;
    }

    insert_path_frame *pf = &path[depth - 1];
    if (!node_trywrlock(t, pf->node_id)) return 0;
    if (!load_latest_node(t, pf->zone_id, pf->node_id,
                          &pf->zone_id, &pf->slot_id, &pf->page)) {
        node_unlock(t, pf->node_id); return 0;
    }
    uint32_t cidx = child_pos_for_id(&pf->page, target_nid);
    if (cidx == UINT32_MAX) {
        node_unlock(t, pf->node_id); return 0;
    }
    uint32_t cur_child_zone = (cidx == RIGHTMOST_IDX)
                                  ? pf->page.ptr_zone_id
                                  : pf->page.internal[cidx].child_zone_id;
    if (cur_child_zone == want.zone_id) {
        node_unlock(t, pf->node_id); return 1;
    }
    if (cidx == RIGHTMOST_IDX)
        pf->page.ptr_zone_id = want.zone_id;
    else
        pf->page.internal[cidx].child_zone_id = want.zone_id;

    propagate_state prop;
    memset(&prop, 0, sizeof prop);
    flush_page_immediate(t, &pf->page, pf->zone_id, ZTREE_INVALID_ZONE_ID,
                         &prop.left_zone_changed, &prop.left_zone, &prop.left_slot);
    node_unlock(t, pf->node_id);
    return 1;
}

static int zns_gc_migrate_leaf(cow_tree *t, uint32_t victim,
                               ztree_node_id_t nid, uint32_t slot)
{
    if (!node_trywrlock(t, nid)) return 0;  /* skip contended; next cycle */

    /* Re-verify the leaf still lives at (victim, slot). */
    nlt_location_t query = { .zone_id = victim, .node_id = nid,
                             .slot_id = ZTREE_INVALID_SLOT_ID };
    nlt_location_t actual;
    if (!nlt_lookup(&t->nlt, &query, &actual) || actual.zone_id != victim)
    {
        /* Stale per-zone tracker phantom: the leaf already moved out of this
         * zone.  Prune it so the victim can drain (else seen>migrated forever
         * and the zone never resets). */
        nlt_remove(&t->nlt, victim, nid);
        node_unlock(t, nid);
        return 2;
    }
    /* Use the NLT-authoritative slot, not the (possibly stale/duplicate) tracker
     * hint — else a stale-slot entry skips forever (seen>migrated, no reset). */
    slot = actual.slot_id;

    ztree_page p;
    ztree_pagenum_t pn = zone_slot_to_pn(t, victim, slot);
    load_page_by_pn(t, pn, &p);
    if (!p.is_leaf || p.num_keys == 0)
    {
        node_unlock(t, nid);
        return 0;
    }
    int64_t cascade_key = (int64_t)p.leaf[0].key;

    /* Prefer an open zone (no admission slot); open new only if none has space. */
    uint32_t target = zone_alloc_llayer_existing(&t->za, nid, victim);
    if (target == ZTREE_INVALID_ZONE_ID)
        target = zone_alloc_llayer(&t->za, nid, victim);
    if (target == ZTREE_INVALID_ZONE_ID)
    {
        node_unlock(t, nid);
        return 0;  /* at active cap — defer */
    }
    pthread_mutex_lock(&t->zone_write_locks[target]);
    if (atomic_load_explicit(&t->zone_full[target], memory_order_acquire))
    {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid);
        return 0;
    }
    uint64_t zone_end = t->zones[target].start + t->zones[target].capacity;
    uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target],
                                       memory_order_relaxed);
    if (wp + ZTREE_PAGE_SIZE > zone_end)
    {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        zone_mark_full(&t->za, target);
        zone_seal_and_replace(&t->za, target);
        node_unlock(t, nid);
        return 0;
    }
    if (wp == t->zones[target].start && !zone_admission_acquire(&t->za, target))
    {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid);
        return 0;
    }
    atomic_store_explicit(&t->zone_wp_bytes[target], wp + ZTREE_PAGE_SIZE,
                          memory_order_relaxed);
    uint64_t cur_wp = wp;
    uint32_t new_slot = (uint32_t)((cur_wp - t->zones[target].start) / ZTREE_PAGE_SIZE);
    p.zone_id = target;
    p.slot_id = new_slot;

    _Alignas(ZTREE_PAGE_SIZE) char bounce[ZTREE_PAGE_SIZE];
    memcpy(bounce, &p, ZTREE_PAGE_SIZE);
    int wfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;
    ssize_t pwr = pwrite(wfd, bounce, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
    {
        /* Roll back the WP reservation, drop locks, caller summarises. */
        atomic_fetch_sub_explicit(&t->zone_wp_bytes[target], ZTREE_PAGE_SIZE,
                                  memory_order_relaxed);
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid);
        return 0;
    }
    pthread_mutex_unlock(&t->zone_write_locks[target]);

    nlt_location_t newloc = { .zone_id = target, .node_id = nid, .slot_id = new_slot };
    nlt_update_migrate(&t->nlt, &newloc, victim);
    zone_valid_leaves_move(t, victim, target);
    node_unlock(t, nid);
    (void)gc_cascade_parent(t, nid, cascade_key);
    return 1;
}

struct zns_gc_ctx {
    cow_tree *t;
    uint32_t  victim;
    size_t    seen;       /* live entries enumerated in victim's NLT tracker */
    size_t    migrated;   /* successfully relocated out of the victim        */
};
static void zns_gc_migrate_cb(ztree_node_id_t nid, uint32_t slot, void *vp)
{
    struct zns_gc_ctx *c = vp;
    c->seen++;
    int r = zns_gc_migrate_leaf(c->t, c->victim, nid, slot);
    if (r == 1) c->migrated++;
    else if (r == 2) c->seen--;   /* phantom pruned — not a real occupant */
}

/* Migrate one victim's live leaves out.  Records seen/migrated into the
 * per-victim result slots.  Does NOT reset the zone — resets are deferred
 * to phase 2 so that during migration every victim stays sealed and is
 * therefore never picked by zone_alloc_llayer as a relocation target
 * (zone_has_space() is false for sealed zones). */
struct zns_victim_result {
    uint32_t zone;
    uint32_t counter_before;
    size_t   seen;
    size_t   migrated;
    int      done;
};

static void zns_gc_migrate_victim(cow_tree *t, struct zns_victim_result *r)
{
    r->counter_before =
        atomic_load_explicit(&t->zone_valid_leaves[r->zone],
                             memory_order_relaxed);
    struct zns_gc_ctx ctx = { .t = t, .victim = r->zone,
                              .seen = 0, .migrated = 0 };
    nlt_zone_for_each(&t->nlt, r->zone, zns_gc_migrate_cb, &ctx);
    r->seen     = ctx.seen;
    r->migrated = ctx.migrated;
}

struct dyn_zns_gc_arg {
    cow_tree                 *t;
    struct zns_victim_result *results;
    int                       v_start, v_end;
};

static void *dyn_zns_gc_worker(void *p)
{
    struct dyn_zns_gc_arg *a = p;
    for (int v = a->v_start; v < a->v_end; v++)
        zns_gc_migrate_victim(a->t, &a->results[v]);
    return NULL;
}

size_t cow_gc_zns(cow_tree *t)
{
    if (!t) return 0;
    /* Off by default while the post-GC RUN-G hang is being debugged.
     * Set CTREE_DYNAMIC_ZNS_GC=1 to exercise it.  The migrate/reset core
     * works (victims reset, stale GB reclaimed); the open issue is that
     * post-GC zone state can leave foreground flushes spinning on the
     * EOVERFLOW retry path. */
    {
        const char *e = getenv("CTREE_DYNAMIC_ZNS_GC");   /* default ON; =0 to disable */
        if (e && e[0] == '0')
            return 0;
    }
    force_trace_sample(t);
    size_t before = zns_physical_bytes(t);

    /* 1. Victim selection: sealed LLayer zones over the stale threshold. */
    uint32_t *victims = malloc(sizeof(uint32_t) * t->info.nr_zones);
    if (!victims) return 0;
    int nvictims = 0;
    for (uint32_t z = CTREE_DYNAMIC_LLAYER_BASE; z < t->info.nr_zones; z++)
    {
        if (!atomic_load_explicit(&t->zone_full[z], memory_order_acquire))
            continue;  /* only sealed zones — never the active write target */
        uint64_t start = t->zones[z].start;
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[z],
                                           memory_order_relaxed);
        if (wp <= start) continue;  /* unused */
        uint64_t used  = wp - start;
        uint64_t valid = (uint64_t)atomic_load_explicit(&t->zone_valid_leaves[z],
                                                        memory_order_relaxed)
                         * ZTREE_PAGE_SIZE;
        double stale_ratio = (used > valid)
                                 ? (1.0 - (double)valid / (double)used)
                                 : 0.0;
        if (stale_ratio > ZNS_GC_STALE_THRESHOLD)
            victims[nvictims++] = z;
    }

    if (nvictims > 0)
    {
        fprintf(stderr, "[ctree_dynamic] cow_gc_zns: %d victim(s):", nvictims);
        for (int v = 0; v < nvictims; v++)
            fprintf(stderr, " %u", victims[v]);
        fputc('\n', stderr);
    }

    /* Phase 1: migrate every victim's live leaves out, in parallel.  All
     *    victims stay sealed throughout, so zone_alloc_llayer never routes
     *    a relocation back into a victim (zone_has_space()==0 for sealed).
     *    Each victim is an independent zone + NLT bucket; shared state is
     *    per-zone atomic or already thread-safe. */
    struct zns_victim_result *results =
        malloc(sizeof(*results) * (size_t)nvictims);
    if (!results) { free(victims); return 0; }
    for (int v = 0; v < nvictims; v++)
        results[v] = (struct zns_victim_result){ .zone = victims[v] };

    int N = dyn_maint_threads();
    if (N > nvictims) N = (nvictims > 0) ? nvictims : 1;
    if (N < 1) N = 1;

    pthread_t tids[32];
    struct dyn_zns_gc_arg args[32];
    int per = (nvictims + N - 1) / N;
    for (int i = 0; i < N; i++)
    {
        int s = i * per;
        int e = ((i + 1) * per > nvictims) ? nvictims : (i + 1) * per;
        args[i] = (struct dyn_zns_gc_arg){
            .t = t, .results = results, .v_start = s, .v_end = e
        };
        pthread_create(&tids[i], NULL, dyn_zns_gc_worker, &args[i]);
    }
    for (int i = 0; i < N; i++)
        pthread_join(tids[i], NULL);

    /* Phase 2: reset victims whose live leaves were fully migrated.  Done
     *    single-threaded after all migration completes — no worker can be
     *    writing into a zone while it is being reset.  A victim with
     *    leftover leaves (target-full / I/O error) is left sealed-as-is. */
    size_t total_migrated = 0, zones_reset = 0, drift_zones = 0;
    for (int v = 0; v < nvictims; v++)
    {
        struct zns_victim_result *r = &results[v];
        total_migrated += r->migrated;
        if (r->seen != (size_t)r->counter_before)
            drift_zones++;
    }

    /* Reset drained victims; re-migrate stragglers (transient trylock holds)
     * over a few short passes — foreground releases latches in between. */
    for (int pass = 0; pass <= 3; pass++)
    {
        int pending = 0;
        for (int v = 0; v < nvictims; v++)
        {
            struct zns_victim_result *r = &results[v];
            if (r->done) continue;
            /* Drained iff every NLT-tracked leaf was migrated out (seen==migrated).
             * Use NLT truth, not zone_valid_leaves, which can drift high and then
             * block reset forever (victim stuck, zns_phys runaway). */
            if (r->seen == r->migrated)
            {
                off_t zstart = (off_t)t->zones[r->zone].start;
                if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
                {
                    perror("cow_gc_zns: zbd_reset_zones");
                    continue;
                }
                atomic_store_explicit(&t->zone_wp_bytes[r->zone],
                                      t->zones[r->zone].start, memory_order_release);
                atomic_store_explicit(&t->zone_valid_leaves[r->zone], 0,
                                      memory_order_release);  /* correct drift */
                zone_admission_release_zone(&t->za, r->zone);
                atomic_store_explicit(&t->zone_full[r->zone], 0, memory_order_release);
                nlt_set_zone_sealed(&t->nlt, r->zone, false);
                r->done = 1; zones_reset++;
            }
            else
            {
                pending = 1;
            }
        }
        if (!pending || pass == 3) break;
        usleep(20000);
        for (int v = 0; v < nvictims; v++)
        {
            struct zns_victim_result *r = &results[v];
            if (r->done) continue;
            zns_gc_migrate_victim(t, r);
            total_migrated += r->migrated;
        }
    }
    free(results);
    free(victims);
    if (drift_zones)
        fprintf(stderr,
                "[ctree_dynamic] cow_gc_zns: %zu zone(s) had counter drift "
                "(corrected)\n", drift_zones);

    size_t after = zns_physical_bytes(t);
    force_trace_sample(t);
    if (nvictims > 0)
        fprintf(stderr,
                "[ctree_dynamic] cow_gc_zns: victims=%d  zones_reset=%zu  "
                "pages_migrated=%zu  zns_phys: %zu → %zu KB  (freed %zu KB, workers=%d)\n",
                nvictims, zones_reset, total_migrated,
                before / 1024, after / 1024,
                (before > after ? (before - after) / 1024 : 0), N);
    return (before > after) ? (before - after) : 0;
}

void cow_close(cow_tree *t)
{
    if (!t)
        return;

    if (atomic_load_explicit(&g_zns_gc_running, memory_order_acquire))
    {
        atomic_store_explicit(&g_zns_gc_stop, true, memory_order_release);
        pthread_join(g_zns_gc_tid, NULL);
        atomic_store_explicit(&g_zns_gc_running, false, memory_order_release);
    }

    if (atomic_load_explicit(&g_gc_running, memory_order_acquire))
    {
        atomic_store_explicit(&g_gc_stop, true, memory_order_release);
        pthread_join(g_gc_tid, NULL);
        atomic_store_explicit(&g_gc_running, false, memory_order_release);

        /* Final evict+GC at quiescence so the trace ends with the sawtooth's
         * trailing valley rather than mid-burst. */
        cow_evict_cns_leaves(t);
        cow_gc_cns(t);
    }

    atomic_store_explicit(&t->stop_flusher, true, memory_order_release);
    pthread_join(t->flusher_tid, NULL);

    uint64_t inserts = atomic_load_explicit(&t->stat_inserts, memory_order_relaxed);
    uint64_t deletes = atomic_load_explicit(&t->stat_deletes, memory_order_relaxed);
    uint64_t del_merges = atomic_load_explicit(&t->stat_delete_merges, memory_order_relaxed);
    uint64_t del_cascades = atomic_load_explicit(&t->stat_delete_cascades, memory_order_relaxed);
    uint64_t del_root_collapses = atomic_load_explicit(&t->stat_delete_root_collapses, memory_order_relaxed);
    uint64_t ch = atomic_load_explicit(&t->stat_cache_hit, memory_order_relaxed);
    uint64_t cm = atomic_load_explicit(&t->stat_cache_miss, memory_order_relaxed);
    uint64_t appends = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    uint64_t nlt_only = atomic_load_explicit(&t->stat_nlt_only_updates, memory_order_relaxed);
    uint64_t zone_chg = atomic_load_explicit(&t->stat_zone_changes, memory_order_relaxed);
    uint64_t par_rew = atomic_load_explicit(&t->stat_parent_rewrites, memory_order_relaxed);
    uint64_t fl_sum = atomic_load_explicit(&t->stat_flush_ns_sum, memory_order_relaxed);
    uint64_t fl_samp = atomic_load_explicit(&t->stat_flush_ns_samples, memory_order_relaxed);
    uint64_t cns_writes = atomic_load_explicit(&t->stat_cns_writes, memory_order_relaxed);

    fprintf(stderr,
            "\n[ctree_dynamic profile]\n"
            "  inserts        = %llu\n"
            "  deletes        = %llu  (merges=%llu cascades=%llu root_collapses=%llu)\n"
            "  cache_hit      = %llu  miss = %llu  hit_rate = %.1f%%\n"
            "  page_appends   = %llu\n"
            "  internal_writes_to_cns = %llu  (%.1f%% of page_appends)\n"
            "  leaf two-stage tracking:\n"
            "    nlt_only_updates  = %llu  (parent skipped, same zone OR internal CoW)\n"
            "    zone_changes      = %llu  (leaf moved to new zone)\n"
            "    parent_rewrites   = %llu\n"
            "  leaf-spill: zns=%llu cns=%llu  spill_ratio=%.2f%%\n"
            "  avg_flush_us   = %.1f\n",
            (unsigned long long)inserts,
            (unsigned long long)deletes,
            (unsigned long long)del_merges,
            (unsigned long long)del_cascades,
            (unsigned long long)del_root_collapses,
            (unsigned long long)ch,
            (unsigned long long)cm,
            (ch + cm > 0) ? 100.0 * (double)ch / (double)(ch + cm) : 0.0,
            (unsigned long long)appends,
            (unsigned long long)cns_writes,
            (appends > 0) ? 100.0 * (double)cns_writes / (double)appends : 0.0,
            (unsigned long long)nlt_only,
            (unsigned long long)zone_chg,
            (unsigned long long)par_rew,
            (unsigned long long)atomic_load_explicit(&g_leaf_zns, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_leaf_cns, memory_order_relaxed),
            (atomic_load_explicit(&g_leaf_zns, memory_order_relaxed)
             + atomic_load_explicit(&g_leaf_cns, memory_order_relaxed) > 0)
              ? 100.0 * (double)atomic_load_explicit(&g_leaf_cns, memory_order_relaxed)
                / (double)(atomic_load_explicit(&g_leaf_zns, memory_order_relaxed)
                           + atomic_load_explicit(&g_leaf_cns, memory_order_relaxed))
              : 0.0,
            (fl_samp > 0) ? (double)fl_sum / (double)fl_samp / 1000.0 : 0.0);

    uint64_t nlt_wait  = atomic_load_explicit(&t->nlt.prof_wait_ns_sum,   memory_order_relaxed);
    uint64_t nlt_hold  = atomic_load_explicit(&t->nlt.prof_hold_ns_sum,   memory_order_relaxed);
    uint64_t nlt_cnt   = atomic_load_explicit(&t->nlt.prof_acquire_count, memory_order_relaxed);

    uint64_t iz_wait   = atomic_load_explicit(&t->prof_zwl_iz_wait_ns_sum,   memory_order_relaxed);
    uint64_t iz_hold   = atomic_load_explicit(&t->prof_zwl_iz_hold_ns_sum,   memory_order_relaxed);
    uint64_t iz_cnt    = atomic_load_explicit(&t->prof_zwl_iz_acquire_count, memory_order_relaxed);

    uint64_t hot_wait  = atomic_load_explicit(&t->prof_zwl_hot_wait_ns_sum,   memory_order_relaxed);
    uint64_t hot_hold  = atomic_load_explicit(&t->prof_zwl_hot_hold_ns_sum,   memory_order_relaxed);
    uint64_t hot_cnt   = atomic_load_explicit(&t->prof_zwl_hot_acquire_count, memory_order_relaxed);

    uint64_t cold_wait = atomic_load_explicit(&t->prof_zwl_cold_wait_ns_sum,   memory_order_relaxed);
    uint64_t cold_hold = atomic_load_explicit(&t->prof_zwl_cold_hold_ns_sum,   memory_order_relaxed);
    uint64_t cold_cnt  = atomic_load_explicit(&t->prof_zwl_cold_acquire_count, memory_order_relaxed);

    uint64_t zwl_wait  = iz_wait + hot_wait + cold_wait;
    uint64_t zwl_hold  = iz_hold + hot_hold + cold_hold;
    uint64_t zwl_cnt   = iz_cnt  + hot_cnt  + cold_cnt;

    uint64_t nlrd_wait = atomic_load_explicit(&t->prof_nl_rd_wait_ns_sum,   memory_order_relaxed);
    uint64_t nlrd_cnt  = atomic_load_explicit(&t->prof_nl_rd_acquire_count, memory_order_relaxed);

    uint64_t nlwr_wait = atomic_load_explicit(&t->prof_nl_wr_wait_ns_sum,   memory_order_relaxed);
    uint64_t nlwr_cnt  = atomic_load_explicit(&t->prof_nl_wr_acquire_count, memory_order_relaxed);

#define _AVG_US(sum, cnt) ((cnt) > 0 ? (double)(sum) / (double)(cnt) / 1000.0 : 0.0)
#define _MS(ns)           ((double)(ns) / 1.0e6)
#define _PCT(part, total) ((total) > 0 ? 100.0 * (double)(part) / (double)(total) : 0.0)

    fprintf(stderr,
            "\n[ctree_dynamic lock profile]\n"
            "  %-16s %10s %12s %12s %12s\n",
            "", "acquires", "wait", "avg_wait", "avg_hold");
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us %10.2f us\n",
            "NLT alloc lock", (unsigned long long)nlt_cnt,
            _MS(nlt_wait), _AVG_US(nlt_wait, nlt_cnt), _AVG_US(nlt_hold, nlt_cnt));
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us %10.2f us\n",
            "Zone wrlock", (unsigned long long)zwl_cnt,
            _MS(zwl_wait), _AVG_US(zwl_wait, zwl_cnt), _AVG_US(zwl_hold, zwl_cnt));
    fprintf(stderr,
            "    IZ    (%5.1f%%) %10llu %10.1f ms %10.2f us %10.2f us\n",
            _PCT(iz_wait, zwl_wait), (unsigned long long)iz_cnt,
            _MS(iz_wait), _AVG_US(iz_wait, iz_cnt), _AVG_US(iz_hold, iz_cnt));
    fprintf(stderr,
            "    Hot   (%5.1f%%) %10llu %10.1f ms %10.2f us %10.2f us\n",
            _PCT(hot_wait, zwl_wait), (unsigned long long)hot_cnt,
            _MS(hot_wait), _AVG_US(hot_wait, hot_cnt), _AVG_US(hot_hold, hot_cnt));
    fprintf(stderr,
            "    Cold  (%5.1f%%) %10llu %10.1f ms %10.2f us %10.2f us\n",
            _PCT(cold_wait, zwl_wait), (unsigned long long)cold_cnt,
            _MS(cold_wait), _AVG_US(cold_wait, cold_cnt), _AVG_US(cold_hold, cold_cnt));
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us\n",
            "Node rdlock", (unsigned long long)nlrd_cnt,
            _MS(nlrd_wait), _AVG_US(nlrd_wait, nlrd_cnt));
    fprintf(stderr,
            "  %-16s %10llu %10.1f ms %10.2f us\n",
            "Node wrlock", (unsigned long long)nlwr_cnt,
            _MS(nlwr_wait), _AVG_US(nlwr_wait, nlwr_cnt));

#undef _AVG_US
#undef _MS
#undef _PCT

    cache_destroy(t);
    nlt_destroy(&t->nlt);
    zone_alloc_destroy(&t->za);

    if (t->node_latches)
    {
        for (size_t i = 0; i < ZTREE_NODE_LATCH_BUCKETS; i++)
            pthread_rwlock_destroy(&t->node_latches[i]);
        free(t->node_latches);
    }

    if (t->zone_write_locks)
    {
        for (uint32_t z = 0; z < t->info.nr_zones; z++)
            pthread_mutex_destroy(&t->zone_write_locks[z]);
        free(t->zone_write_locks);
    }

    free(t->zones);
    free(t->zone_wp_bytes);
    free(t->zone_full);
    free(t->zone_valid_leaves);

    zbd_close(t->fd);
    if (t->direct_fd >= 0)
        close(t->direct_fd);
    for (int k = 0; k < CTREE_CNS_SHARDS; k++)
        if (t->cns_fd_shard[k] >= 0)
            close(t->cns_fd_shard[k]);
    if (t->trace_fp)
        fclose(t->trace_fp);

    free(t->cns_bitmap);
    free(t->cns_dirty_bitmap);

    pthread_mutex_destroy(&t->sb_lock);

    free(t);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Point lookup
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ORWC read: rd-lock coupling from the root (the paper's readers only ever
 * take cheap shared locks and never fail to acquire them).  The parent latch
 * is held until the child latch is acquired, so no split/CoW window can hide
 * or misroute a key.  Restarts only when the root was replaced under us. */
ztree_record *ztree_find(ztree_t *t, int64_t key)
{
    for (uint32_t attempt = 0;; attempt++)
    {
        if (attempt > 0 && (attempt & 0xFU) == 0)
            sched_yield();
        if (attempt >= 1000000U)
        {
            fprintf(stderr, "ztree_find: excessive restarts (key=%ld)\n", (long)key);
            return NULL;
        }

        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot, height;
        (void)k3_root_snapshot(t, &root_nid, &root_zone, &root_slot, &height);

        if (root_nid == ZTREE_INVALID_NODE_ID)
            return NULL;

        node_rdlock(t, root_nid);
        if (atomic_load_explicit(&t->volatile_sb.root_node_id,
                                 memory_order_acquire) != root_nid)
        {
            node_unlock(t, root_nid);
            continue;
        }

        ztree_node_id_t cur = root_nid;
        uint32_t zone_hint = root_zone;
        ztree_page pg;
        uint32_t z, s;
        if (!load_latest_node(t, zone_hint, cur, &z, &s, &pg))
        {
            node_unlock(t, cur);
            continue;
        }

        int ok = 1;
        while (!pg.is_leaf)
        {
            uint32_t cidx = child_pos_for_key(&pg, key);
            ztree_node_id_t child = (cidx == RIGHTMOST_IDX)
                                        ? pg.ptr_node_id
                                        : pg.internal[cidx].child_node_id;
            uint32_t child_zone = (cidx == RIGHTMOST_IDX)
                                      ? pg.ptr_zone_id
                                      : pg.internal[cidx].child_zone_id;

            node_rdlock(t, child);
            node_unlock(t, cur);
            cur = child;
            if (!load_latest_node(t, child_zone, cur, &z, &s, &pg))
            {
                node_unlock(t, cur);
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        for (uint32_t i = 0; i < pg.num_keys; i++)
        {
            if ((int64_t)pg.leaf[i].key == key)
            {
                ztree_record *r = malloc(sizeof *r);
                node_unlock(t, cur);
                if (!r)
                    return NULL;
                *r = pg.leaf[i].record;
                return r;
            }
        }
        node_unlock(t, cur);
        return NULL;
    }
}
