/*
 * Insert path implements Optimistic Readers-Writer Coupling: rd-lock coupled
 * descent, preemptive splits under w(parent)+w(child), optimistic upgrades
 * with trylock, level-parameterised restarts escalating to classical
 * exclusive coupling.  Node latches are EXACT per-node (chunked array indexed
 * by node_id, writer-preferred) — no hash aliasing, so every wait follows
 * tree order and the protocol is deadlock-free by construction.
 * See the block comment above do_single_insert for the full protocol.

 gcc -O2 -g -Wall -Wextra -std=c11 -pthread \
      ztree/ztree_nlt.c ztree/ztree_zone.c ztree/ztree_main_k3.c \
      ztree/bench_main_ztree.c \
      -o build/ztree_k3 -lzbd -lnvme -lpthread
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
#include <time.h>
#include <unistd.h>

#include "ztree_main.h"

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

#define TRACE_SAMPLE_INTERVAL 10000U

static inline void emit_trace_row(ztree_t *t)
{
    if (!t->trace_fp) return;
    uint64_t total = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    double elapsed = (double)(monotonic_ns() - t->trace_start_ns) / 1e9;
    uint32_t total_nodes = atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
    size_t zns_phys = 0;
    for (uint32_t z = 2; z < t->info.nr_zones; z++) {
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[z], memory_order_relaxed);
        uint64_t start = t->zones[z].start;
        if (wp > start) zns_phys += (size_t)(wp - start);
    }
    /* ztree has no CNS: cns_current / cns_writes / height / cns_phys = 0. */
    fprintf(t->trace_fp, "%.3f,%llu,0,%llu,0,0,0,%zu\n",
            elapsed,
            (unsigned long long)total_nodes,
            (unsigned long long)total,
            zns_phys);
}

static inline void maybe_trace_sample(ztree_t *t)
{
    if (!t->trace_fp) return;
    uint64_t total = atomic_load_explicit(&t->stat_page_appends, memory_order_relaxed);
    if (total % TRACE_SAMPLE_INTERVAL != 0) return;
    emit_trace_row(t);
}

static inline void force_trace_sample(ztree_t *t)
{
    emit_trace_row(t);
    if (t->trace_fp) fflush(t->trace_fp);
}

/* Override the weak stub in YCSB-cpp's ztree_db_stubs (if any). */
void cow_phase_mark(cow_tree *t, const char *name)
{
    static FILE *phase_fp = NULL;
    static int tried_open = 0;
    if (!t) return;
    if (!phase_fp && !tried_open) {
        tried_open = 1;
        const char *path = getenv("CTREE_DYNAMIC_PHASE_PATH");
        char fallback[1024];
        if (!path || !*path) {
            const char *tp = getenv("CTREE_DYNAMIC_TRACE_PATH");
            if (tp) { snprintf(fallback, sizeof fallback, "%s.phases", tp); path = fallback; }
        }
        if (path) {
            phase_fp = fopen(path, "w");
            if (phase_fp) fprintf(phase_fp, "time_sec,phase\n");
        }
    }
    if (phase_fp) {
        double sec = (double)(monotonic_ns() - t->trace_start_ns) / 1e9;
        fprintf(phase_fp, "%.3f,%s\n", sec, name);
        fflush(phase_fp);
    }
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

/* Exact (not hashed) per-node latch table: ORWC coupling holds two latches at
 * once, so hashed buckets could alias and deadlock; exact latches keep waits in
 * tree order (parent before child) → acyclic. Writer-preferred, chunked by node_id. */
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

/* Per-zone write mutex (ZWL) profile recorders, broken out by zone group.
 * Routing: ilayer_pool_base..hot_pool_base → IZ, hot..cold → Hot, cold+ → Cold.
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

/* Instrumented node locking: records wait time for contention profiling.
 * No hold-time tracking (rd/wr share node_unlock). */
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

/* Returns 1 if acquired, 0 if would block.  Used by ZNS GC to avoid
 * deadlocking with foreground threads that may hold a leaf wrlock while
 * busy-looping on zone admission. */
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

/* Helper: compute byte offset from zone + slot */
static inline uint64_t zone_slot_to_offset(ztree_t *t,
                                           uint32_t zone_id,
                                           uint32_t slot_id)
{
    return t->zones[zone_id].start + (uint64_t)slot_id * ZTREE_PAGE_SIZE;
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
            /* Cache hit: bump LRU counter */
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

    /* Find an empty slot or the LRU victim */
    int victim = 0;
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

    set->ways[victim].valid = 1;
    set->ways[victim].tag = pn;
    set->ways[victim].lru_counter = clock;
    set->ways[victim].data = *src;

    pthread_mutex_unlock(&set->lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Page I/O (NLT-aware load)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * load_page_by_pn  –  read a page from the global cache or disk.
 * Also populates the NLT lazily: when a page is first read from disk, its
 * embedded (zone_id, slot_id, node_id) fields are registered in the NLT so
 * that subsequent lookups avoid disk I/O.
 */
static void load_page_by_pn(ztree_t *t, ztree_pagenum_t pn, ztree_page *dst)
{
    /* 1. Try the global cache */
    if (cache_lookup(t, pn, dst))
        return;

    /* 2. Cache miss → read from device */
    atomic_fetch_add_explicit(&t->stat_cache_miss, 1, memory_order_relaxed);

    off_t off = (off_t)pn * ZTREE_PAGE_SIZE;
    int rfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;

    if (t->direct_fd >= 0)
    {
        /* O_DIRECT requires aligned buffer */
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

    /* 3. Warm the cache */
    cache_insert(t, pn, dst);
}

/*
 * load_page_by_nlt  –  resolve a node's physical location via the NLT,
 * then delegate to load_page_by_pn.
 * Returns 1 on success, 0 if the node_id is not in the NLT.
 */
static int load_page_by_nlt(ztree_t *t, const nlt_location_t *loc,
                            ztree_page *dst)
{
    if (!loc || loc->zone_id == ZTREE_INVALID_ZONE_ID ||
        loc->node_id == ZTREE_INVALID_NODE_ID)
    {
        return 0;
    }

    ztree_pagenum_t pn = zone_slot_to_pn(t, loc->zone_id, loc->slot_id);
    load_page_by_pn(t, pn, dst);
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Zone write helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * zone_finish_if_full  –  call zbd_finish_zones() on a full zone so the
 * device can reclaim its active-zone slot.
 */
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

/*
 * zone_append_page  –  append one page to zone_id at the current write
 * pointer.  Atomically advances the in-memory WP tracking.
 *
 * On success returns 0 and sets *out_slot_id and *out_pn.
 * Returns -1 if the zone is full after this write.
 */
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
        /* O_DIRECT requires page-aligned buffer; use per-call stack buffer. */
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

    /* Compute slot ID from the offset */
    uint32_t slot = (uint32_t)((cur_wp - t->zones[zone_id].start) / ZTREE_PAGE_SIZE);
    if (out_slot_id)
        *out_slot_id = slot;
    if (out_pn)
        *out_pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);

    /* Mark zone full if write pointer reached capacity */
    uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
    uint64_t zone_end = t->zones[zone_id].start + t->zones[zone_id].capacity;
    if (new_wp >= zone_end)
    {
        atomic_store_explicit(&t->zone_full[zone_id], 1, memory_order_release);
        zone_finish_if_full(t, zone_id);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Meta zone management (RLayer – superblock ping-pong)
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t other_meta_zone(uint32_t z)
{
    return (z == ZTREE_META_ZONE_0) ? ZTREE_META_ZONE_1 : ZTREE_META_ZONE_0;
}

static void activate_meta_zone(ztree_t *t, uint32_t zone_id, uint64_t version)
{
    off_t zstart = (off_t)t->zones[zone_id].start;
    fdatasync(t->fd);
    zbd_finish_zones(t->fd, zstart, (off_t)t->info.zone_size); /* best-effort */
    if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
    {
        perror("activate_meta_zone: zbd_reset_zones");
        exit(EXIT_FAILURE);
    }

    atomic_store_explicit(&t->zone_wp_bytes[zone_id],
                          t->zones[zone_id].start, memory_order_release);
    atomic_store_explicit(&t->zone_full[zone_id], 0, memory_order_release);

    /* Write zone header in slot 0 */
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
    t->meta_wp = 1; /* slot 0 used by zone header */
    t->meta_version = version;
}

static void rotate_meta_zone(ztree_t *t)
{
    activate_meta_zone(t, other_meta_zone(t->active_meta_zone),
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
    uint64_t wp0 = atomic_load_explicit(&t->zone_wp_bytes[ZTREE_META_ZONE_0],
                                        memory_order_acquire);
    uint64_t wp1 = atomic_load_explicit(&t->zone_wp_bytes[ZTREE_META_ZONE_1],
                                        memory_order_acquire);

    int v0 = (wp0 > t->zones[ZTREE_META_ZONE_0].start) &&
             (pread(t->fd, &zh0, ZTREE_PAGE_SIZE, 0) == (ssize_t)ZTREE_PAGE_SIZE) &&
             (zh0.magic == ZTREE_ZH_MAGIC);
    int v1 = (wp1 > t->zones[ZTREE_META_ZONE_1].start) &&
             (pread(t->fd, &zh1, ZTREE_PAGE_SIZE,
                    (off_t)t->zones[ZTREE_META_ZONE_1].start) == (ssize_t)ZTREE_PAGE_SIZE) &&
             (zh1.magic == ZTREE_ZH_MAGIC);

    ztree_superblock_entry sb0, sb1;
    uint64_t sbwp0 = 0, sbwp1 = 0;

    if (v0)
        sbwp0 = scan_meta_zone(t->fd, ZTREE_META_ZONE_0,
                               t->zones[ZTREE_META_ZONE_0].start,
                               t->info.zone_size, &sb0);
    if (v1)
        sbwp1 = scan_meta_zone(t->fd, ZTREE_META_ZONE_1,
                               t->zones[ZTREE_META_ZONE_1].start,
                               t->info.zone_size, &sb1);

    if (sbwp0 == 0 && sbwp1 == 0)
    {
        /* Fresh device – format superblock */
        memset(&t->durable_sb, 0, sizeof t->durable_sb);
        t->durable_sb.root_node_id = ZTREE_INVALID_NODE_ID;
        t->durable_sb.root_zone_id = ZTREE_INVALID_ZONE_ID;
        t->durable_sb.root_slot_id = ZTREE_INVALID_SLOT_ID;
        t->durable_sb.next_node_id = 1; /* IDs start at 1 */
        t->durable_sb.leaf_order = ZTREE_LEAF_ORDER;
        t->durable_sb.internal_order = ZTREE_INTERNAL_ORDER;
        activate_meta_zone(t, ZTREE_META_ZONE_0, 0);
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
                best_zone = ZTREE_META_ZONE_0;
                best_wp = sbwp0;
                best_meta_version = zh0.version;
            }
            else
            {
                best = &sb1;
                best_zone = ZTREE_META_ZONE_1;
                best_wp = sbwp1;
                best_meta_version = zh1.version;
            }
        }
        else if (sbwp0 > 0)
        {
            best = &sb0;
            best_zone = ZTREE_META_ZONE_0;
            best_wp = sbwp0;
            best_meta_version = zh0.version;
        }
        else
        {
            best = &sb1;
            best_zone = ZTREE_META_ZONE_1;
            best_wp = sbwp1;
            best_meta_version = zh1.version;
        }

        t->durable_sb = *best;
        t->active_meta_zone = best_zone;
        t->meta_wp = best_wp + 1;
        t->meta_version = best_meta_version;
    }

    /* Seed the volatile superblock for lock-free readers */
    atomic_store_explicit(&t->volatile_sb.root_node_id,
                          t->durable_sb.root_node_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id,
                          t->durable_sb.root_zone_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id,
                          t->durable_sb.root_slot_id, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.tree_height,
                          t->durable_sb.tree_height, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no,
                          t->durable_sb.seq_no * 2, memory_order_release);

    /* Seed the node ID counter from the last persisted value */
    atomic_store_explicit(&t->next_node_id,
                          t->durable_sb.next_node_id, memory_order_relaxed);

    /* Seed the NLT with the root's location so the first traversal works */
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

    /* Rotate meta zone if it is almost full */
    if (t->meta_wp >= t->zones[t->active_meta_zone].capacity / ZTREE_PAGE_SIZE)
    {
        fprintf(stderr, "[ztree] meta zone full, rotating\n");
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

/*
 * sb_flusher_thread  –  background thread that periodically flushes the
 * durable superblock without sitting on the critical flush path.
 */
static void *sb_flusher_thread(void *arg)
{
    ztree_t *t = (ztree_t *)arg;

    while (!atomic_load_explicit(&t->stop_flusher, memory_order_acquire))
    {
        usleep(ZTREE_FLUSH_INTERVAL_MS * 1000);

        if (!atomic_exchange_explicit(&t->dirty_sb, false, memory_order_acq_rel))
            continue;

        /* Read the volatile superblock with a seqlock-style read */
        ztree_node_id_t root_nid;
        uint32_t root_zone, root_slot, tree_height;
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
            tree_height = atomic_load_explicit(&t->volatile_sb.tree_height,
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
        t->durable_sb.tree_height = tree_height;
        t->durable_sb.seq_no = seq / 2;
        /* Persist the node ID counter so recovery gets the right starting point */
        t->durable_sb.next_node_id =
            atomic_load_explicit(&t->next_node_id, memory_order_relaxed);
        pthread_mutex_unlock(&t->sb_lock);

        write_superblock_sync(t);
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Single-pass CoW insert helpers 
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
 * flush_page_immediate  –  write one CoW page to ZNS.
 * prev_zone: current zone (INVALID for new nodes).
 * avoid_zone: skip this zone during allocation (split sibling rule).
 * Tries sticky append first (same zone → no parent rewrite), falls back
 * to dynamic allocation via round-robin (paper §3.2).
 */
/* ZNS GC scans both ILayer and LLayer zones (paper §3.1.2 / 3.2). */
#define ZNS_GC_STALE_THRESHOLD  0.5

static pthread_t   g_zns_gc_tid;
static _Atomic bool g_zns_gc_running = false;
static _Atomic bool g_zns_gc_stop    = false;
static unsigned     g_zns_gc_interval_ms = 0;

/* Counts live nodes (leaf or internal) per zone for GC victim selection. */
static inline void zone_valid_nodes_move(ztree_t *t,
                                         uint32_t prev_zone,
                                         uint32_t target_zone) {
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && prev_zone >= ZTREE_ILAYER_ZONE_START)
        atomic_fetch_sub_explicit(&t->zone_valid_leaves[prev_zone], 1,
                                  memory_order_relaxed);
    if (target_zone >= ZTREE_ILAYER_ZONE_START)
        atomic_fetch_add_explicit(&t->zone_valid_leaves[target_zone], 1,
                                  memory_order_relaxed);
}

static void *ztree_zns_gc_thread(void *arg);
size_t cow_gc_zns(cow_tree *t);

static void flush_page_immediate(ztree_t *t,
                                 ztree_page *pg,
                                 uint32_t prev_zone,
                                 uint32_t avoid_zone,
                                 int *out_zone_changed,
                                 uint32_t *out_zone,
                                 uint32_t *out_slot)
{
    uint32_t target_zone;
    uint64_t cur_wp;
    bool     sticky_ok = false;

    /* Timestamp of current zone_write_lock acquisition for hold-time profiling. */
    uint64_t zwl_hold_start = 0;

retry_flush:
    sticky_ok = false;

    /* ── Phase 1a: try stickiness ──────────────────────────────────────── */
    if (prev_zone != ZTREE_INVALID_ZONE_ID
        && !atomic_load_explicit(&t->zone_full[prev_zone], memory_order_acquire))
    {
        uint64_t lock_t0 = monotonic_ns();
        pthread_mutex_lock(&t->zone_write_locks[prev_zone]);
        uint64_t lock_t1 = monotonic_ns();
        record_zwl_wait(t, prev_zone, lock_t1 - lock_t0);
        zwl_hold_start = lock_t1;

        uint64_t zone_start = t->zones[prev_zone].start;
        uint64_t zone_end   = zone_start + t->zones[prev_zone].capacity;
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
            pthread_mutex_unlock(&t->zone_write_locks[prev_zone]);
        }
    }

    /* ── Phase 1b: Dynamic_Allocation if sticky path was not taken ─────── */
    if (!sticky_ok)
    {
        for (;;)
        {
            target_zone = pg->is_leaf
                              ? zone_alloc_llayer(&t->za, pg->node_id, avoid_zone)
                              : zone_alloc_ilayer(&t->za, avoid_zone);
            if (target_zone == ZTREE_INVALID_ZONE_ID) {
                usleep(20);   /* per-group cap full; wait for a seal */
                continue;
            }

            uint64_t lock_t0 = monotonic_ns();
            pthread_mutex_lock(&t->zone_write_locks[target_zone]);
            uint64_t lock_t1 = monotonic_ns();
            record_zwl_wait(t, target_zone, lock_t1 - lock_t0);
            zwl_hold_start = lock_t1;

            /* Re-check under lock: zone may have been finished between pick and lock. */
            if (atomic_load_explicit(&t->zone_full[target_zone],
                                     memory_order_acquire))
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                continue;
            }

            uint64_t zone_start = t->zones[target_zone].start;
            uint64_t zone_end   = zone_start + t->zones[target_zone].capacity;
            uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target_zone],
                                               memory_order_relaxed);

            if (wp + ZTREE_PAGE_SIZE > zone_end)
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
                nlt_set_zone_sealed(&t->nlt, target_zone, true);
                zone_seal_and_replace(&t->za, target_zone);
                continue;
            }

            /* First write opens a device-active zone; wait if at cap. */
            if (wp == zone_start
                && !zone_admission_acquire(&t->za, target_zone))
            {
                record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
                pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
                usleep(20);
                continue;
            }

            cur_wp = wp;
            atomic_store_explicit(&t->zone_wp_bytes[target_zone],
                                   wp + ZTREE_PAGE_SIZE, memory_order_relaxed);
            break;
        }
    }

    uint32_t slot_id = (uint32_t)((cur_wp - t->zones[target_zone].start) / ZTREE_PAGE_SIZE);
    ztree_pagenum_t pn = (ztree_pagenum_t)(cur_wp / ZTREE_PAGE_SIZE);

    /* Stamp zone_id and slot_id before the write. */
    pg->zone_id = target_zone;
    pg->slot_id = slot_id;

    int wfd;
    const void *wbuf;
    _Alignas(ZTREE_PAGE_SIZE) char local_bounce[ZTREE_PAGE_SIZE];
    if (t->direct_fd >= 0)
    {
        /* O_DIRECT requires page-aligned buffer; use per-call stack buffer. */
        memcpy(local_bounce, pg, ZTREE_PAGE_SIZE);
        wfd = t->direct_fd;
        wbuf = local_bounce;
    }
    else
    {
        wfd = t->fd;
        wbuf = pg;
    }
    {
        uint64_t zs = t->zones[target_zone].start;
        uint64_t ze = zs + t->zones[target_zone].capacity;
        if (cur_wp < zs || cur_wp + ZTREE_PAGE_SIZE > ze)
        {
            fprintf(stderr,
                    "flush_page_immediate: cur_wp out of range "
                    "target_zone=%u prev_zone=%u avoid=%u is_leaf=%u "
                    "node_id=%llu cur_wp=0x%llx "
                    "zone_start=0x%llx zone_end=0x%llx sticky=%d\n",
                    target_zone, prev_zone, avoid_zone, pg->is_leaf,
                    (unsigned long long)pg->node_id,
                    (unsigned long long)cur_wp,
                    (unsigned long long)zs,
                    (unsigned long long)ze,
                    sticky_ok ? 1 : 0);
            exit(EXIT_FAILURE);
        }
    }
    ssize_t pwr = pwrite(wfd, wbuf, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE)
    {
        int e = errno;
        if (e == EOVERFLOW)
        {
            /* Re-sync WP from device; if FULL, seal + release the slot. */
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
                    atomic_store_explicit(&t->zone_full[target_zone], 1,
                                          memory_order_release);
                    zone_admission_release_zone(&t->za, target_zone);
                }
            }
            else
            {
                atomic_fetch_sub_explicit(&t->zone_wp_bytes[target_zone],
                                          ZTREE_PAGE_SIZE, memory_order_relaxed);
            }
            record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
            pthread_mutex_unlock(&t->zone_write_locks[target_zone]);
            usleep(500);
            goto retry_flush;
        }
        fprintf(stderr,
                "flush_page_immediate: pwrite at 0x%llx ret=%zd errno=%d (%s) "
                "target_zone=%u prev_zone=%u avoid=%u is_leaf=%u sticky=%d\n",
                (unsigned long long)cur_wp, pwr, e, strerror(e),
                target_zone, prev_zone, avoid_zone, pg->is_leaf,
                sticky_ok ? 1 : 0);
        exit(EXIT_FAILURE);
    }

    /* Successful pwrite — record full hold time spanning the pwrite. */
    record_zwl_hold(t, target_zone, monotonic_ns() - zwl_hold_start);
    pthread_mutex_unlock(&t->zone_write_locks[target_zone]);

    /* Zone-full detection + seal threshold. */
    uint64_t new_wp = cur_wp + ZTREE_PAGE_SIZE;
    uint64_t zone_end = t->zones[target_zone].start + t->zones[target_zone].capacity;
    if (new_wp >= zone_end)
    {
        atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
        zone_seal_and_replace(&t->za, target_zone);
    }

    cache_insert(t, pn, pg);
    nlt_location_t loc = {
        .zone_id = target_zone,
        .node_id = pg->node_id,
        .slot_id = slot_id,
    };
    /* Atomic insert-new + remove-stale-from-prev so each node has exactly
     * one bucket entry (paper §3.1.2 "latest valid" invariant). */
    nlt_update_migrate(&t->nlt, &loc, prev_zone);
    zone_valid_nodes_move(t, prev_zone, target_zone);

    uint64_t zone_bytes_used = new_wp - t->zones[target_zone].start;
    uint64_t seal_threshold = (t->zones[target_zone].capacity * 95ULL) / 100ULL;
    if (zone_bytes_used >= seal_threshold)
    {
        atomic_store_explicit(&t->zone_full[target_zone], 1, memory_order_release);
        nlt_set_zone_sealed(&t->nlt, target_zone, true);
        zone_seal_and_replace(&t->za, target_zone);
    }

    if (pg->is_leaf)
    {
        /* Reset heat on zone relocation (paper §3.1.1). */
        if (prev_zone != ZTREE_INVALID_ZONE_ID && prev_zone != target_zone)
            zone_heat_reset(&t->za, pg->node_id);
        zone_heat_record_write(&t->za, pg->node_id);
    }

    atomic_fetch_add_explicit(&t->stat_page_appends, 1, memory_order_relaxed);
    maybe_trace_sample(t);

    if (prev_zone != ZTREE_INVALID_ZONE_ID && prev_zone == target_zone)
    {
        if (out_zone_changed)
            *out_zone_changed = 0;
        atomic_fetch_add_explicit(&t->stat_nlt_only_updates, 1, memory_order_relaxed);
    }
    else
    {
        if (out_zone_changed)
            *out_zone_changed = 1;
        atomic_fetch_add_explicit(&t->stat_zone_changes, 1, memory_order_relaxed);
    }

    if (out_zone)
        *out_zone = target_zone;
    if (out_slot)
        *out_slot = slot_id;
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

/* Finds child link position by node ID (used after reloading parent). */
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

/*
 * try_publish_root_if_unchanged
 *
 * Optimistic publish: commit this insert only if root seq_no is unchanged
 * from the snapshot used to build the overlay.
 */
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
    {
        return 0;
    }

    atomic_store_explicit(&t->volatile_sb.root_node_id, root_nid, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_zone_id, root_zone, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.root_slot_id, root_slot, memory_order_release);
    atomic_store_explicit(&t->volatile_sb.seq_no, expected_seq_even + 2, memory_order_release);
    atomic_store_explicit(&t->dirty_sb, true, memory_order_release);
    (void)atomic_fetch_add_explicit(&t->txg_next, 1, memory_order_acq_rel);
    return 1;
}

/* ORWC insert path — El-Shaikh et al., "Lightweight Latches for B-Trees to Cope
 * with High Contention" (DEXA 2024, Sect. 3), adapted to ZTree's CoW/NLT design.
 * Descend with rd-lock coupling (parent held until child latched), leaf always
 * exclusive; each level is known before locking from the seqlock-published root
 * id/height. Preemptive splits (a full child split under w(parent)+w(child)) keep
 * an insert from propagating upward. Optimistic upgrade is child-first — the held
 * parent forbids a completed child split, so the trywrlock is decisive — then the
 * parent; a failed upgrade restarts exclusively from that level, escalating to root
 * exclusive coupling after K3_ESCALATE_AFTER tries. Two-stage tracking defers the
 * parent zone-hint refresh until all latches drop; hints are advisory (nlt_lookup
 * scans on miss), so a missed refresh is correctness-neutral.
 * Deadlock-free: exact per-node latches, strictly top-down blocking acquisitions,
 * trylock-only upgrades, zone/NLT locks only after node latches. */

#define K3_ESCALATE_AFTER 3U   /* optimistic restarts before exclusive coupling */

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
 * against GC's optimistic try_publish_root_if_unchanged. */
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
        atomic_store_explicit(&t->volatile_sb.tree_height, height, memory_order_release);
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
        *height = atomic_load_explicit(&t->volatile_sb.tree_height, memory_order_acquire);
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
            return;
        }

        uint32_t cidx = child_pos_for_id(&pf.page, cid);
        if (cidx == UINT32_MAX)
        {
            node_unlock(t, pid);
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

        int is_root = (atomic_load_explicit(&t->volatile_sb.root_node_id,
                                            memory_order_acquire) == pid);
        if (is_root)
            publish_root_locked(t, pid, pf.zone_id, pf.slot_id,
                                atomic_load_explicit(&t->volatile_sb.tree_height,
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
            if (restart > K3_ESCALATE_AFTER && x_level != INT_MAX)
            {
                x_level = INT_MAX;   /* classical exclusive coupling from root */
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
                atomic_fetch_add_explicit(&t->stat_descent_ns_sum,
                                          monotonic_ns() - apply_t0, memory_order_relaxed);
                atomic_fetch_add_explicit(&t->stat_descent_ns_samples, 1, memory_order_relaxed);

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
 * cow_open / cow_insert / cow_close
 * ═══════════════════════════════════════════════════════════════════════════ */

cow_tree *cow_open(const char *path)
{
    fprintf(stderr, "[ztree] opening %s  cache_sets=%d ways=%d\n",
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

    /* Allocate per-zone arrays */
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
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    for (uint32_t z = 0; z < t->info.nr_zones; z++)
    {
        atomic_store_explicit(&t->zone_wp_bytes[z], t->zones[z].wp, memory_order_relaxed);
        atomic_store_explicit(&t->zone_full[z],
                              (t->zones[z].cond == ZBD_ZONE_COND_FULL) ? 1 : 0,
                              memory_order_relaxed);
    }

    /* Initialise subsystems */
    cache_init(t);

    /* Per-zone tracker pre-sized to ~4× zone page capacity so we can
     * absorb tombstone accumulation without ever resizing.  zones[]
     * capacity covers every device zone (with headroom). */
    uint64_t zone_pages = (t->info.nr_zones > 0)
                              ? (t->zones[0].capacity / ZTREE_PAGE_SIZE)
                              : 65536ULL;
    size_t tracker_cap = (size_t)zone_pages * 4ULL;
    if (tracker_cap < 4096) tracker_cap = 4096;
    size_t zones_cap = (size_t)t->info.nr_zones * 2ULL;
    if (zones_cap < 256) zones_cap = 256;
    nlt_init(&t->nlt, zones_cap, tracker_cap);

    /* k3: node latches live in the process-global exact per-node chunk
     * table (g_latch_chunk); t->node_latches stays NULL. */

    /* Per-zone write mutexes: serialise pwrite calls within each zone for ZNS ordering. */
    t->zone_write_locks = calloc(t->info.nr_zones, sizeof(*t->zone_write_locks));
    if (!t->zone_write_locks)
    {
        perror("cow_open zone_write_locks");
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
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
            cache_destroy(t);
            nlt_destroy(&t->nlt);
            free(t->zones);
            free(t->zone_wp_bytes);
            free(t->zone_full);
            if (t->direct_fd >= 0)
                close(t->direct_fd);
            zbd_close(t->fd);
            free(t);
            return NULL;
        }
    }

    /* Zone layout: 0-1 RLayer, 2-17 ILayer, 18+ LLayer (80/20 hot/cold). */
    uint32_t ilayer_pool_base  = ZTREE_ILAYER_ZONE_START;   /* 2  */
    uint32_t ilayer_pool_size  = ZTREE_ILAYER_POOL_SIZE;    /* 16 */
    uint32_t ilayer_init       = ZTREE_ILAYER_INIT_COUNT;   /* 4  */

    uint32_t llayer_pool_base  = ZTREE_LLAYER_ZONE_START;   /* 18 */
    uint32_t llayer_pool_total = (t->info.nr_zones > llayer_pool_base)
                                     ? (t->info.nr_zones - llayer_pool_base) : 1;

    /* 80 / 20 hot-cold split of the LLayer pool */
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

    /* Active-zone admission (finish-then-release); leaf blocks at cap. */
    t->za.admission_enabled = 1;
    t->za.active_cap = 13;
    atomic_store_explicit(&t->za.active_zones, 0, memory_order_relaxed);

    fprintf(stderr,
            "[ztree] ILayer pool [%u, %u)  init_group=%u\n",
            ilayer_pool_base, ilayer_pool_base + ilayer_pool_size, ilayer_init);
    fprintf(stderr,
            "[ztree] LLayer hot-pool [%u, %u)  init_group=%u"
            "  cold-pool [%u, %u)  init_group=%u\n",
            hot_pool_base,  hot_pool_base  + hot_pool_size,  ZTREE_LZGROUP_HOT_INIT,
            cold_pool_base, cold_pool_base + cold_pool_size, ZTREE_LZGROUP_COLD_INIT);

    load_superblock(t);

    /* Initialise atomic flags */
    atomic_store_explicit(&t->dirty_sb, false, memory_order_relaxed);
    atomic_store_explicit(&t->stop_flusher, false, memory_order_relaxed);
    atomic_store_explicit(&t->txg_next, 0, memory_order_relaxed);
    atomic_store_explicit(&t->txg_synced, 0, memory_order_relaxed);

    {
        const char *trace_path = getenv("CTREE_DYNAMIC_TRACE_PATH");
        if (!trace_path || !*trace_path)
            trace_path = "/tmp/ztree_trace.csv";
        t->trace_fp = fopen(trace_path, "w");
        if (t->trace_fp)
            fprintf(stderr, "[ztree] trace -> %s\n", trace_path);
    }
    t->trace_start_ns = monotonic_ns();
    if (t->trace_fp)
        fprintf(t->trace_fp,
                "time_sec,zns_current,cns_current,appends,cns_writes,height,cns_phys_bytes,zns_phys_bytes\n");


    /* Start background superblock flusher thread. */
    if (pthread_create(&t->flusher_tid, NULL, sb_flusher_thread, t) != 0)
    {
        perror("pthread_create sb_flusher_thread");
        cache_destroy(t);
        nlt_destroy(&t->nlt);
        zone_alloc_destroy(&t->za);
        free(t->zones);
        free(t->zone_wp_bytes);
        free(t->zone_full);
        zbd_close(t->fd);
        free(t);
        return NULL;
    }

    {
        /* k3 default: ZNS GC ON, 5 s interval (interval 0 disables). */
        const char *env = getenv("CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS");
        long ms = env ? atol(env) : 5000;
        if (ms < 0) ms = 0;
        g_zns_gc_interval_ms = (unsigned)ms;
    }
    if (g_zns_gc_interval_ms > 0) {
        atomic_store_explicit(&g_zns_gc_stop, false, memory_order_relaxed);
        atomic_store_explicit(&g_zns_gc_running, true, memory_order_release);
        if (pthread_create(&g_zns_gc_tid, NULL, ztree_zns_gc_thread, t) != 0) {
            perror("pthread_create ztree_zns_gc_thread");
            atomic_store_explicit(&g_zns_gc_running, false, memory_order_release);
        } else {
            fprintf(stderr,
                    "[ztree] ZNS GC thread enabled: interval=%ums "
                    "(disable: CTREE_DYNAMIC_ZNS_GC=0 or _INTERVAL_MS=0)\n",
                    g_zns_gc_interval_ms);
        }
    }

    return t;
}

void cow_insert(cow_tree *t, int64_t key, const char *value)
{
    do_single_insert(t, key, value);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * ZNS GC (ILayer + LLayer) — paper-aligned cross-zone migration:
 *   1. Per-node latched migrate (trywrlock to avoid foreground deadlock).
 *   2. After NLT migrate, descend from root to find the parent of the moved
 *      node, then take parent wrlock and rewrite the parent's child_zone_id
 *      entry to converge to the NLT-reported current zone (matches paper
 *      Algorithm 2 line 12).  Parent's own flush cascades further up via
 *      the same insert-style propagation loop.
 *   3. If migrated node is root, publish (zone, slot) to the superblock.
 * ═══════════════════════════════════════════════════════════════════════════ */

static int gc_maint_threads(void) {
    const char *e = getenv("CTREE_DYNAMIC_MAINT_THREADS");
    int n = e ? atoi(e) : 8;
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return n;
}

/* Read root snapshot via seqlock.  Returns even seq_no on success. */
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

/* Paper Algorithm 2 line 12: after a node moves zone, rewrite the parent's
 * child_zone_id entry.  Descend from root using `key` (which lies in the
 * moved node's key range) until the next-hop child equals target_nid; that
 * node is the parent.  Take parent trywrlock, update the child entry to
 * NLT-resolved current zone, and flush.  If parent itself moves zone, the
 * inner cascade loop propagates further up — mirroring the insert path's
 * `if (prop.left_zone_changed)` loop. */
static int gc_cascade_parent(ztree_t *t,
                             ztree_node_id_t target_nid,
                             int64_t key) {
    ztree_node_id_t root_nid;
    uint32_t root_zone, root_slot;
    uint64_t seq_snapshot = gc_root_snapshot(t, &root_nid, &root_zone, &root_slot);
    if (root_nid == ZTREE_INVALID_NODE_ID) return 0;

    /* Resolve current location of the migrated node via NLT.  We sync
     * parent to whatever NLT currently says — handles the case where
     * foreground further migrated the node between our migrate and
     * cascade. */
    nlt_location_t want_q = { .zone_id = ZTREE_INVALID_ZONE_ID,
                              .node_id = target_nid,
                              .slot_id = ZTREE_INVALID_SLOT_ID };
    nlt_location_t want;
    if (!nlt_lookup(&t->nlt, &want_q, &want)) return 0;

    /* If target is the root itself: publish (zone, slot) to superblock. */
    if (root_nid == target_nid) {
        if (root_zone == want.zone_id && root_slot == want.slot_id) return 1;
        return try_publish_root_if_unchanged(t, seq_snapshot,
                                             target_nid, want.zone_id, want.slot_id);
    }

    /* Descent: read-crab from root, building path until next-hop child
     * equals target_nid.  At that point, path[depth-1] is the parent. */
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
        if (f->page.is_leaf) return 0;  /* target not reachable via key */
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
        if (child_nid == target_nid) break;  /* f is the parent */
        cur_id   = child_nid;
        cur_zone = child_zone;
    }

    /* path[depth-1] is the immediate parent.  Take wrlock, reload, find
     * entry by id (concurrent splits may have shifted positions), update
     * child_zone, flush. */
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
    prop.left_id = pf->page.node_id;
    node_unlock(t, pf->node_id);

    /* Cascade further up if parent itself moved zone. */
    for (int level = depth - 2; level >= 0 && prop.left_zone_changed; level--) {
        insert_path_frame *gp = &path[level];
        if (!node_trywrlock(t, gp->node_id)) return 1;
        if (!load_latest_node(t, gp->zone_id, gp->node_id,
                              &gp->zone_id, &gp->slot_id, &gp->page)) {
            node_unlock(t, gp->node_id); return 1;
        }
        uint32_t gidx = child_pos_for_id(&gp->page, prop.left_id);
        if (gidx == UINT32_MAX) {
            node_unlock(t, gp->node_id); return 1;
        }
        if (gidx == RIGHTMOST_IDX)
            gp->page.ptr_zone_id = prop.left_zone;
        else
            gp->page.internal[gidx].child_zone_id = prop.left_zone;
        flush_page_immediate(t, &gp->page, gp->zone_id, ZTREE_INVALID_ZONE_ID,
                             &prop.left_zone_changed, &prop.left_zone, &prop.left_slot);
        prop.left_id = gp->page.node_id;
        if (prop.left_zone_changed)
            atomic_fetch_add_explicit(&t->stat_parent_rewrites, 1,
                                      memory_order_relaxed);
        node_unlock(t, gp->node_id);
    }

    /* If cascade reached and rewrote the root, publish new (zone, slot). */
    if (prop.left_zone_changed && prop.left_id == root_nid) {
        (void)try_publish_root_if_unchanged(t, seq_snapshot,
                                            root_nid, prop.left_zone, prop.left_slot);
    }
    return 1;
}

static int zns_gc_migrate_node(cow_tree *t, uint32_t victim,
                               ztree_node_id_t nid, uint32_t slot) {
    if (!node_trywrlock(t, nid)) return 0;  /* skip contended; 2nd-pass retries */
    nlt_location_t query  = { .zone_id = victim, .node_id = nid,
                              .slot_id = ZTREE_INVALID_SLOT_ID };
    nlt_location_t actual;
    if (!nlt_lookup(&t->nlt, &query, &actual)
        || actual.zone_id != victim || actual.slot_id != slot) {
        node_unlock(t, nid); return 0;
    }
    ztree_page p;
    ztree_pagenum_t pn = zone_slot_to_pn(t, victim, slot);
    load_page_by_pn(t, pn, &p);

    /* Key that routes through this node from root (cascade parent descent). */
    int64_t cascade_key;
    if (p.is_leaf) {
        if (p.num_keys == 0) { node_unlock(t, nid); return 0; }
        cascade_key = (int64_t)p.leaf[0].key;
    } else {
        if (p.num_keys == 0) { node_unlock(t, nid); return 0; }
        cascade_key = (int64_t)p.internal[0].key;
    }

    /* Prefer an already-open zone (no admission slot); open new only if none
     * has space. */
    uint32_t target = p.is_leaf
                          ? zone_alloc_llayer_existing(&t->za, nid, victim)
                          : zone_alloc_ilayer_existing(&t->za, victim);
    if (target == ZTREE_INVALID_ZONE_ID)
        target = p.is_leaf
                     ? zone_alloc_llayer(&t->za, nid, victim)
                     : zone_alloc_ilayer(&t->za, victim);
    if (target == ZTREE_INVALID_ZONE_ID) { node_unlock(t, nid); return 0; }
    pthread_mutex_lock(&t->zone_write_locks[target]);
    if (atomic_load_explicit(&t->zone_full[target], memory_order_acquire)) {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid); return 0;
    }
    uint64_t zone_end = t->zones[target].start + t->zones[target].capacity;
    uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[target], memory_order_relaxed);
    if (wp + ZTREE_PAGE_SIZE > zone_end) {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        atomic_store_explicit(&t->zone_full[target], 1, memory_order_release);
        zone_seal_and_replace(&t->za, target);
        node_unlock(t, nid); return 0;
    }
    if (wp == t->zones[target].start
        && !zone_admission_acquire(&t->za, target)) {
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid); return 0;
    }
    atomic_store_explicit(&t->zone_wp_bytes[target], wp + ZTREE_PAGE_SIZE,
                          memory_order_relaxed);
    uint64_t cur_wp = wp;
    uint32_t new_slot = (uint32_t)((cur_wp - t->zones[target].start) / ZTREE_PAGE_SIZE);
    p.zone_id = target; p.slot_id = new_slot;

    _Alignas(ZTREE_PAGE_SIZE) char bounce[ZTREE_PAGE_SIZE];
    memcpy(bounce, &p, ZTREE_PAGE_SIZE);
    int wfd = (t->direct_fd >= 0) ? t->direct_fd : t->fd;
    ssize_t pwr = pwrite(wfd, bounce, ZTREE_PAGE_SIZE, (off_t)cur_wp);
    if (pwr != (ssize_t)ZTREE_PAGE_SIZE) {
        atomic_fetch_sub_explicit(&t->zone_wp_bytes[target], ZTREE_PAGE_SIZE,
                                  memory_order_relaxed);
        pthread_mutex_unlock(&t->zone_write_locks[target]);
        node_unlock(t, nid); return 0;
    }
    pthread_mutex_unlock(&t->zone_write_locks[target]);

    nlt_location_t newloc = { .zone_id = target, .node_id = nid, .slot_id = new_slot };
    nlt_update_migrate(&t->nlt, &newloc, victim);
    zone_valid_nodes_move(t, victim, target);
    node_unlock(t, nid);

    /* Paper-aligned: rewrite parent's child_zone_id entry. */
    (void)gc_cascade_parent(t, nid, cascade_key);
    return 1;
}

struct zns_gc_ctx { cow_tree *t; uint32_t victim; size_t seen, migrated; };
static void zns_gc_migrate_cb(ztree_node_id_t nid, uint32_t slot, void *vp) {
    struct zns_gc_ctx *c = vp;
    c->seen++;
    c->migrated += (size_t)zns_gc_migrate_node(c->t, c->victim, nid, slot);
}

struct zns_victim_result { uint32_t zone, counter_before; size_t seen, migrated; int done; };
static void zns_gc_migrate_victim(cow_tree *t, struct zns_victim_result *r) {
    r->counter_before = atomic_load_explicit(&t->zone_valid_leaves[r->zone],
                                             memory_order_relaxed);
    struct zns_gc_ctx ctx = { .t = t, .victim = r->zone, .seen = 0, .migrated = 0 };
    nlt_zone_for_each(&t->nlt, r->zone, zns_gc_migrate_cb, &ctx);
    r->seen = ctx.seen; r->migrated = ctx.migrated;
}

struct zns_gc_arg { cow_tree *t; struct zns_victim_result *results; int v_start, v_end; };
static void *zns_gc_worker(void *p) {
    struct zns_gc_arg *a = p;
    for (int v = a->v_start; v < a->v_end; v++)
        zns_gc_migrate_victim(a->t, &a->results[v]);
    return NULL;
}

static inline size_t ztree_zns_phys_bytes(ztree_t *t) {
    size_t total = 0;
    for (uint32_t z = 2; z < t->info.nr_zones; z++) {
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[z], memory_order_relaxed);
        uint64_t start = t->zones[z].start;
        if (wp > start) total += (size_t)(wp - start);
    }
    return total;
}

size_t cow_gc_zns(cow_tree *t) {
    if (!t) return 0;
    /* k3 default-on; opt out with CTREE_DYNAMIC_ZNS_GC=0. */
    const char *e = getenv("CTREE_DYNAMIC_ZNS_GC");
    if (e && e[0] == '0') return 0;
    force_trace_sample(t);
    size_t before = ztree_zns_phys_bytes(t);

    uint32_t *victims = malloc(sizeof(uint32_t) * t->info.nr_zones);
    if (!victims) return 0;
    int nvictims = 0;
    for (uint32_t z = ZTREE_ILAYER_ZONE_START; z < t->info.nr_zones; z++) {
        if (!atomic_load_explicit(&t->zone_full[z], memory_order_acquire)) continue;
        uint64_t start = t->zones[z].start;
        uint64_t wp = atomic_load_explicit(&t->zone_wp_bytes[z], memory_order_relaxed);
        if (wp <= start) continue;
        uint64_t used = wp - start;
        uint64_t valid = (uint64_t)atomic_load_explicit(&t->zone_valid_leaves[z],
                                                       memory_order_relaxed)
                         * ZTREE_PAGE_SIZE;
        double sr = (used > valid) ? (1.0 - (double)valid / (double)used) : 0.0;
        if (sr > ZNS_GC_STALE_THRESHOLD) victims[nvictims++] = z;
    }
    if (nvictims == 0) { free(victims); return 0; }

    fprintf(stderr, "[ztree] cow_gc_zns: %d victim(s)\n", nvictims);
    struct zns_victim_result *results = malloc(sizeof(*results) * (size_t)nvictims);
    if (!results) { free(victims); return 0; }
    for (int v = 0; v < nvictims; v++)
        results[v] = (struct zns_victim_result){ .zone = victims[v] };

    int N = gc_maint_threads();
    if (N > nvictims) N = nvictims;
    if (N < 1) N = 1;
    pthread_t tids[32]; struct zns_gc_arg args[32];
    int per = (nvictims + N - 1) / N;
    for (int i = 0; i < N; i++) {
        int s = i * per, ee = ((i + 1) * per > nvictims) ? nvictims : (i + 1) * per;
        args[i] = (struct zns_gc_arg){ .t = t, .results = results, .v_start = s, .v_end = ee };
        pthread_create(&tids[i], NULL, zns_gc_worker, &args[i]);
    }
    for (int i = 0; i < N; i++) pthread_join(tids[i], NULL);

    size_t total_migrated = 0, zones_reset = 0;
    for (int v = 0; v < nvictims; v++)
        total_migrated += results[v].migrated;

    /* Reset drained victims; re-migrate stragglers (transient trylock holds)
     * over a few short passes — foreground releases latches / drains its hot
     * nodes in between, so the remaining few become migratable. */
    for (int pass = 0; pass <= 3; pass++) {
        int pending = 0;
        for (int v = 0; v < nvictims; v++) {
            struct zns_victim_result *r = &results[v];
            if (r->done) continue;
            uint32_t remaining = atomic_load_explicit(&t->zone_valid_leaves[r->zone],
                                                      memory_order_acquire);
            if (remaining == 0) {
                off_t zstart = (off_t)t->zones[r->zone].start;
                if (zbd_reset_zones(t->fd, zstart, (off_t)t->info.zone_size) != 0)
                    continue;
                atomic_store_explicit(&t->zone_wp_bytes[r->zone],
                                      t->zones[r->zone].start, memory_order_release);
                zone_admission_release_zone(&t->za, r->zone);
                atomic_store_explicit(&t->zone_full[r->zone], 0, memory_order_release);
                nlt_set_zone_sealed(&t->nlt, r->zone, false);
                r->done = 1; zones_reset++;
            } else {
                pending = 1;
            }
        }
        if (!pending || pass == 3) break;
        usleep(20000);
        for (int v = 0; v < nvictims; v++) {
            struct zns_victim_result *r = &results[v];
            if (r->done) continue;
            zns_gc_migrate_victim(t, r);
            total_migrated += r->migrated;
        }
    }
    free(results); free(victims);
    size_t after = ztree_zns_phys_bytes(t);
    force_trace_sample(t);
    fprintf(stderr,
            "[ztree] cow_gc_zns: migrated=%zu  reset=%zu  "
            "zns_phys: %zu → %zu KB  (freed %zu KB)\n",
            total_migrated, zones_reset, before/1024, after/1024,
            (before > after) ? (before - after)/1024 : 0);
    return (before > after) ? (before - after) : 0;
}

static void *ztree_zns_gc_thread(void *arg) {
    cow_tree *t = (cow_tree *)arg;
    while (!atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire)) {
        unsigned slept = 0;
        while (slept < g_zns_gc_interval_ms
            && !atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire)) {
            unsigned step = (g_zns_gc_interval_ms - slept > 50)
                                ? 50 : (g_zns_gc_interval_ms - slept);
            struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)step * 1000000L };
            nanosleep(&ts, NULL);
            slept += step;
        }
        if (atomic_load_explicit(&g_zns_gc_stop, memory_order_acquire)) break;
        cow_phase_mark(t, "begin:zns_gc_periodic");
        cow_gc_zns(t);
        cow_phase_mark(t, "end:zns_gc_periodic");
    }
    return NULL;
}

void cow_close(cow_tree *t)
{
    if (!t)
        return;

    if (atomic_load_explicit(&g_zns_gc_running, memory_order_acquire)) {
        atomic_store_explicit(&g_zns_gc_stop, true, memory_order_release);
        pthread_join(g_zns_gc_tid, NULL);
        atomic_store_explicit(&g_zns_gc_running, false, memory_order_release);
    }

    /* Stop the superblock flusher */
    atomic_store_explicit(&t->stop_flusher, true, memory_order_release);
    pthread_join(t->flusher_tid, NULL);

    /* Print profile summary */
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
    uint64_t de_sum  = atomic_load_explicit(&t->stat_descent_ns_sum, memory_order_relaxed);
    uint64_t de_samp = atomic_load_explicit(&t->stat_descent_ns_samples, memory_order_relaxed);

    fprintf(stderr,
            "\n[ztree profile]\n"
            "  inserts        = %llu\n"
            "  deletes        = %llu  (merges=%llu cascades=%llu root_collapses=%llu)\n"
            "  cache_hit      = %llu  miss = %llu  hit_rate = %.1f%%\n"
            "  page_appends   = %llu\n"
            "  two-stage tracking:\n"
            "    nlt_only_updates  = %llu  (parent skipped, same zone)\n"
            "    zone_changes      = %llu  (node moved to new zone)\n"
            "    parent_rewrites   = %llu\n"
            "  avg_descent_us = %.1f  (root->leaf traversal)\n"
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
            (unsigned long long)nlt_only,
            (unsigned long long)zone_chg,
            (unsigned long long)par_rew,
            (de_samp > 0) ? (double)de_sum / (double)de_samp / 1000.0 : 0.0,
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
            "\n[ztree lock profile]\n"
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

    /* Destroy subsystems */
    cache_destroy(t);
    nlt_destroy(&t->nlt);
    zone_alloc_destroy(&t->za);

    /* k3: exact per-node latches live in the process-global chunk table and
     * are reused across in-process runs; nothing to destroy here. */

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

    if (t->trace_fp)
        fclose(t->trace_fp);

    pthread_mutex_destroy(&t->sb_lock);

    free(t);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Point lookup (optional, not used by insert-only benchmark)
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
