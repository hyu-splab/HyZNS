/*
 * Host-managed Hybrid SSD (HYHOSTSSD) — FEMU mode 5.
 *
 * Self-contained: no include or call into zns/, bbssd/, ocssd/, hyssd/.
 * Shared infrastructure (nvme.h, backend/dram.h, NVMe wire structs) is
 * the only allowed cross-cutting dependency.
 *
 * Tier 0 / D4-D5: R-region read/write (random) and S-region read/write
 * + zone management (sequential). Latency uses a bbssd-style channel/LUN
 * timing model: per-page advance on (ch, lun) implied by LBA striping,
 * max across pages becomes req->expire_time.
 *
 * Out of scope (per docs/OCSSD_HYBRID_DESIGN.md §4):
 *   - OOB metadata / crash recovery
 *   - Append, Set ZD Ext, ZRWA
 *   - Active/Open zone resource limits
 *   - GC / wear leveling
 */

#include "./hyhost.h"

/* Paper latency defaults (ns). */
#define HYHOST_PG_RD_LAT_NS         (80000ULL)
#define HYHOST_PG_WR_LAT_NS         (450000ULL)
#define HYHOST_BLK_ER_LAT_NS        (2000000ULL)
#define HYHOST_CH_XFER_LAT_NS       (25000ULL)

/* R-region (conventional namespace) write latency. Kept as a separate knob
 * so R can be modeled independently (e.g. lowered to model a DRAM write
 * cache), but the ConfZNS-matched configuration uses the same raw-NAND
 * program latency as S (= HYHOST_PG_WR_LAT_NS). */
#define HYHOST_PG_WR_LAT_R_NS       (450000ULL)

#define HYHOST_CACHE_WR_LAT_NS      (1000)
#define HYHOST_CACHE_RD_LAT_NS      (1000)

/* NAND geometry fallbacks — used only if the device props are left at 0.
 * The real values come from `-device femu,hyhost_nchs=..,hyhost_luns_per_ch=..,
 * hyhost_page_bytes=..,hyhost_pgs_per_blk=..,hyhost_zone_size_mb=..` (see
 * femu.c). Default baseline: 8 ch x 4 way x 128 pg/blk x 16 KiB NAND page =>
 * block = 2 MiB, line = 64 MiB (device erase / host GC unit), zone = 1 GiB
 * = 16 lines.
 *
 * Two distinct "pages": the host LBA is 4 KiB (NVMe LBA format, dm-hyhost's
 * L2P granularity), while the device NAND page is 16 KiB — the ch/lun
 * striping + program-latency unit. hyhost_charge_latency folds host LBAs
 * into NAND pages via lbas_per_page = page_bytes / lba_bytes (= 4 here), so
 * a 16 KiB NAND page covers 4 consecutive 4 KiB host LBAs. Keep them in
 * sync with dm-hyhost, which counts the same 64 MiB line in *host-page*
 * units (nchs*luns_per_ch*(block_bytes/4KiB)).
 *
 * The timing model stripes NAND pages round-robin across nchs×luns_per_ch
 * lanes, so changing the geometry just rescales parallelism — no code path
 * assumes a particular lane count or that zone == line. */
#define HYHOST_DEFAULT_NCHS          (8u)
#define HYHOST_DEFAULT_LUNS_PER_CH   (4u)
#define HYHOST_DEFAULT_PAGE_BYTES    (16384u) /* NAND page = 16 KiB = 32 sectors */
#define HYHOST_DEFAULT_PGS_PER_BLK   (128u)   /* 2 MiB erase block @ 16 KiB page */

/* Default zone size (sectors). 1 GiB at 4 KiB sector = 256 Ki sectors. */
#define HYHOST_DEFAULT_ZONE_SIZE_BYTES  (1ULL << 30)
// #define LOG_MODE

// #define WRITE_CACHE_ON
#define DEFAULT_NUM_WRITE_CACHE 4

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline HyHostSSD *to_hyhost(FemuCtrl *n)
{
    return (HyHostSSD *)n->ext_ops.state;
}

static inline size_t hyhost_l2b(NvmeNamespace *ns, uint64_t lba)
{
    return lba << NVME_ID_NS_LBADS(ns);
}

static inline bool hyhost_lba_in_r_region(HyHostSSD *h, uint64_t slba)
{
    return slba < h->r_region_end_lba;
}

static inline uint16_t hyhost_check_bounds(NvmeNamespace *ns, uint64_t slba,
                                           uint32_t nlb)
{
    uint64_t nsze = le64_to_cpu(ns->id_ns.nsze);
    if (unlikely(UINT64_MAX - slba < nlb || slba + nlb > nsze)) {
        return NVME_LBA_RANGE | NVME_DNR;
    }
    return NVME_SUCCESS;
}

static uint16_t hyhost_map_dptr(FemuCtrl *n, size_t len, NvmeRequest *req)
{
    uint64_t prp1, prp2;
    switch (req->cmd.psdt) {
    case NVME_PSDT_PRP:
        prp1 = le64_to_cpu(req->cmd.dptr.prp1);
        prp2 = le64_to_cpu(req->cmd.dptr.prp2);
        return nvme_map_prp(&req->qsg, &req->iov, prp1, prp2, len, n);
    default:
        return NVME_INVALID_FIELD;
    }
}

enum HyHostNandOp {
    HYHOST_NAND_READ,
    HYHOST_NAND_WRITE,      /* S-region (ZNS) raw-NAND program latency */
    HYHOST_NAND_WRITE_R,    /* R-region (conventional) — DRAM-cached, lower latency */
    HYHOST_NAND_ERASE,
};

/* LBA→(ch, lun) interleaving — page-major (BBSSD-style write pointer order).
 *
 * Mirrors bbssd's `ssd_advance_write_pointer`: consecutive NAND pages round-
 * robin through (ch, lun, pg) with ch the fastest-changing axis. Within a
 * single line N the order is (ch=0..nchs-1, lun=0, pg=0) → (ch=*, lun=1, pg=0)
 * → ... → (ch=*, lun=luns_per_ch-1, pg=0) → (ch=*, lun=*, pg=1) → ...
 *
 * The (ch, lun) distribution per write trace must be identical to BBSSD's
 * allocator output for the geometry comparison to hold. dm-hyhost's line
 * allocator walks the same (ch, lun, pg) order so host and device land on
 * the same page index.
 */
static inline void hyhost_lba_to_chlun(HyHostSSD *h, uint64_t page_idx,
                                       uint32_t *ch, uint32_t *lun)
{
    *ch  = page_idx % h->nchs;
    *lun = (page_idx / h->nchs) % h->luns_per_ch;
}

/* Advance one NAND op on the given (ch, lun). Mirrors bbssd's
 * ssd_advance_status — read = NAND-busy then channel transfer; write =
 * channel transfer then NAND-busy. Mutex serializes the ch/lun timer
 * updates so concurrent IOs don't race on max(prev, now) reasoning. */
static int64_t hyhost_advance_nand(HyHostSSD *h, uint32_t ch, uint32_t lun,
                                   int64_t cmd_stime, enum HyHostNandOp op)
{
    int64_t *lun_next = &h->lun_next_avail_ns[ch * h->luns_per_ch + lun];
    int64_t *ch_next  = &h->ch_next_avail_ns[ch];
    int64_t lat = 0;
    int64_t nand_stime, chnl_stime;

    // pthread_mutex_lock(&h->timing_mutex);

    switch (op) {
    case HYHOST_NAND_READ:
        nand_stime = (*lun_next < cmd_stime) ? cmd_stime : *lun_next;
        *lun_next  = nand_stime + h->pg_rd_lat_ns;
        chnl_stime = (*ch_next < *lun_next) ? *lun_next : *ch_next;
        *ch_next   = chnl_stime + h->ch_xfer_lat_ns;
        lat = *ch_next - cmd_stime;
        break;
    case HYHOST_NAND_WRITE:
        chnl_stime = (*ch_next < cmd_stime) ? cmd_stime : *ch_next;
        *ch_next   = chnl_stime + h->ch_xfer_lat_ns;
        nand_stime = (*lun_next < *ch_next) ? *ch_next : *lun_next;
        *lun_next  = nand_stime + h->pg_wr_lat_ns;
        lat = *lun_next - cmd_stime;
        break;
    case HYHOST_NAND_WRITE_R:
        /* Same channel-then-NAND ordering as a normal write, but the
         * "NAND" stage charges the conventional DRAM-cached latency. */
        chnl_stime = (*ch_next < cmd_stime) ? cmd_stime : *ch_next;
        *ch_next   = chnl_stime + h->ch_xfer_lat_ns;
        nand_stime = (*lun_next < *ch_next) ? *ch_next : *lun_next;
        *lun_next  = nand_stime + h->pg_wr_lat_r_ns;
        lat = *lun_next - cmd_stime;
        break;
    case HYHOST_NAND_ERASE:
        /* Erase is NAND-only — no channel transfer. */
        nand_stime = (*lun_next < cmd_stime) ? cmd_stime : *lun_next;
        *lun_next  = nand_stime + h->blk_er_lat_ns;
        lat = *lun_next - cmd_stime;
        break;
    }

    // pthread_mutex_unlock(&h->timing_mutex);
    return lat;
}

/* Walk the request page-by-page, advance per-(ch,lun) timing for each, and
 * report the max latency through req->expire_time so the poller delays the
 * CQE accordingly. Pages on different LUNs charge in parallel. */
static void hyhost_charge_latency(HyHostSSD *h, NvmeRequest *req,
                                  NvmeNamespace *ns, uint64_t slba,
                                  uint32_t nlb, bool is_write)
{
    uint64_t lba_bytes = NVME_ID_NS_LBADS_BYTES(ns);
    uint64_t lbas_per_page = h->page_bytes / lba_bytes;
    if (lbas_per_page == 0) lbas_per_page = 1;

    uint64_t first_page = slba / lbas_per_page;
    uint64_t last_page  = (slba + nlb - 1) / lbas_per_page;

    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t max_lat = 0;
    for (uint64_t pg = first_page; pg <= last_page; pg++) {
        uint32_t ch, lun;
        int64_t lat;
        enum HyHostNandOp op;

        if (!is_write) {
            op = HYHOST_NAND_READ;
        } else if (pg * lbas_per_page < h->r_region_end_lba) {
            /* R-region (conventional) write — DRAM-cached, lower latency. */
            op = HYHOST_NAND_WRITE_R;
        } else {
            op = HYHOST_NAND_WRITE;
        }

        hyhost_lba_to_chlun(h, pg, &ch, &lun);
        lat = hyhost_advance_nand(h, ch, lun, cmd_stime, op);
        if (lat > max_lat) max_lat = lat;
    }

    req->reqlat      = max_lat;
    /* Absolute set, NOT '+='. Single R/S IO is called once per req (expire_time
     * was just initialized to stime by the SQ poller, so += and = match), but
     * the whole-zone (select_all) RESET/FINISH paths call the charge helpers in
     * a per-zone loop; with '+=' each of the 256 iterations accumulates, pushing
     * expire_time tens of seconds into the future. Such a req never satisfies
     * `now >= expire_time` in the CQ poller, so it is never popped from the
     * pqueue and rots there until its io_req is reused — the source of the
     * stale/garbage req the poller later dereferences (dual crash). */
    req->expire_time = cmd_stime + max_lat;

    /* Diagnostic probe: on R-region writes, track the spread of cmd_stime
     * across consecutive commands and the average per-command gap, to tell
     * poller-side serialization (small regular gaps) apart from guest SQ
     * arrival gaps. Window = 512 R-writes (one full (ch, lun) sweep). */
    if (is_write && slba < h->r_region_end_lba) {
        static int64_t prev_stime;
        static int64_t win_min, win_max, gap_sum;
        static int     win_n;
        if (win_n == 0) { win_min = win_max = cmd_stime; }
        if (cmd_stime < win_min) win_min = cmd_stime;
        if (cmd_stime > win_max) win_max = cmd_stime;
        if (prev_stime) gap_sum += (cmd_stime - prev_stime);
        prev_stime = cmd_stime;
        if (++win_n >= 512) {
            int64_t spread = win_max - win_min;
            femu_log("[hyhost_gc_probe] R-write win_n=%d stime_spread_ns=%" PRId64
                     " avg_gap_ns=%" PRId64 "\n",
                     win_n, spread, win_n > 1 ? gap_sum / (win_n - 1) : 0);
            win_n = 0; gap_sum = 0; prev_stime = 0;
        }
    }
}

static int hyhost_get_wcidx(HyHostSSD *h, int zone_idx)
{
    int wcidx = -1;

    for (int i = 0; i < h->num_wc; i++) {
        if (h->cache[i].zidx == zone_idx) {
            return i;
        }
    }

    return wcidx;
}

static uint64_t hyhost_wc_flush(HyHostSSD *h, int wcidx, NvmeRequest *req)
{
    uint64_t lpn;
    uint64_t sublat = 0, maxlat = 0;
    uint32_t ch, lun;
    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    for (int i = 0; i < h->cache[wcidx].used; i++) {
        lpn = h->cache[wcidx].lpns[i];
        hyhost_lba_to_chlun(h, lpn, &ch, &lun);
        sublat = hyhost_advance_nand(h, ch, lun, cmd_stime, HYHOST_NAND_WRITE);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    h->cache[wcidx].zidx = -1;
    h->cache[wcidx].used = 0;
    h->used_wc -= 1;

    // femu_log("[hyhost_wc_flush] wcidx : %d\n", wcidx);
    return maxlat;
}

static uint64_t hyhost_cache_latency(HyHostSSD *h, NvmeRequest *req,
                                     NvmeNamespace *ns, uint64_t slba,
                                     uint32_t nlb, bool is_write, int wcidx)
{
    uint64_t lba_bytes = NVME_ID_NS_LBADS_BYTES(ns);
    uint64_t lbas_per_page = h->page_bytes / lba_bytes;
    uint64_t sublat = 0, maxlat = 0;
    uint64_t first_page = slba / lbas_per_page;
    uint64_t last_page = (slba + nlb + 1) / lbas_per_page;

    if (lbas_per_page == 0) lbas_per_page = 1;

    for (uint64_t lpn = first_page; lpn <= last_page; lpn++) {
        if (h->cache[wcidx].used == h->cache[wcidx].cap) {
            sublat = hyhost_wc_flush(h, wcidx, req);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            sublat = 0;
        }
        h->cache[wcidx].lpns[h->cache[wcidx].used++] = lpn;
        sublat += HYHOST_CACHE_WR_LAT_NS;
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static void hyhost_invalidate_cache(HyHostSSD *h, uint32_t zone_idx)
{
    for (int i = 0; i < h->num_wc; i++) {
        if (h->cache[i].zidx == zone_idx) {
            h->cache[i].zidx = -1;
            h->cache[i].used = 0;
            h->used_wc -= 1;
            break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Zone helpers (S-region)
 * ------------------------------------------------------------------------- */

static inline uint8_t hyhost_zone_state(const HyHostZone *z)
{
    return z->d.zs >> 4;
}

static inline void hyhost_zone_set_state(HyHostZone *z, uint8_t state)
{
    z->d.zs = state << 4;
}

static inline uint32_t hyhost_zone_idx(HyHostSSD *h, uint64_t slba)
{
    return h->zone_size_log2 ? (uint32_t)(slba >> h->zone_size_log2)
                             : (uint32_t)(slba / h->zone_size);
}

static inline HyHostZone *hyhost_zone_by_slba(HyHostSSD *h, uint64_t slba)
{
    uint32_t idx = hyhost_zone_idx(h, slba);
    if (idx >= h->num_zones) return NULL;
    return &h->zone_array[idx];
}

/* -------------------------------------------------------------------------
 * R-region IO
 * ------------------------------------------------------------------------- */

static uint16_t hyhost_r_write(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                               NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyhost_l2b(ns, nlb);
    uint64_t data_offset;
    uint16_t status;

    req->is_write = true;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyhost_check_bounds(ns, slba, nlb))) return status;
    if (slba + nlb > h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
    if ((status = hyhost_map_dptr(n, data_size, req))) return status;

    /* Random write: no write-pointer check. The host FTL must not overwrite
     * a still-valid physical page; from the device POV LBA→byte is direct. */
    data_offset = hyhost_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    // femu_log("hyhost_r_write\n");

    hyhost_charge_latency(h, req, ns, slba, nlb, /*is_write=*/true);
    return NVME_SUCCESS;
}

static uint16_t hyhost_r_read(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                              NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyhost_l2b(ns, nlb);
    uint64_t data_offset;
    uint16_t status;

    req->is_write = false;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyhost_check_bounds(ns, slba, nlb))) return status;
    if (slba + nlb > h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
    if ((status = hyhost_map_dptr(n, data_size, req))) return status;

    data_offset = hyhost_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    // femu_log("hyhost_r_read\n");

    hyhost_charge_latency(h, req, ns, slba, nlb, /*is_write=*/false);
    return NVME_SUCCESS;
}

/* -------------------------------------------------------------------------
 * S-region IO
 * ------------------------------------------------------------------------- */

static uint16_t hyhost_s_check_zone_write(HyHostSSD *h, HyHostZone *z,
                                          uint64_t slba, uint32_t nlb)
{
    uint8_t state = hyhost_zone_state(z);

    /* R-region zones report SEQ on the wire but accept random/in-place
     * writes — only bounds check, no wp/state enforcement. */
    if (z->is_random) {
        if (slba + nlb > z->d.zslba + z->d.zcap) {
            return NVME_ZONE_BOUNDARY_ERROR | NVME_DNR;
        }
        return NVME_SUCCESS;
    }

    if (state == HYHOST_ZS_FULL || state == HYHOST_ZS_READ_ONLY ||
        state == HYHOST_ZS_OFFLINE) {
        return NVME_ZONE_INVALID_WRITE | NVME_DNR;
    }

    /* Sequential write check: slba must equal the zone's write pointer,
     * and the request must not exceed the zone capacity. */
    if (slba != z->w_ptr) {
        return NVME_ZONE_INVALID_WRITE | NVME_DNR;
    }
    if (slba + nlb > z->d.zslba + z->d.zcap) {
        return NVME_ZONE_BOUNDARY_ERROR | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static void hyhost_s_advance_wp(HyHostZone *z, uint32_t nlb)
{
    uint8_t state = hyhost_zone_state(z);

    if (state == HYHOST_ZS_EMPTY) {
        hyhost_zone_set_state(z, HYHOST_ZS_IMPLICITLY_OPEN);
    }
    z->w_ptr += nlb;
    z->d.wp = z->w_ptr;
    if (z->w_ptr >= z->d.zslba + z->d.zcap) {
        hyhost_zone_set_state(z, HYHOST_ZS_FULL);
    }
}

static uint16_t hyhost_s_write(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                               NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyhost_l2b(ns, nlb);
    uint64_t data_offset;
    HyHostZone *z;
    uint16_t status;
    int wcidx = 0;
    uint64_t sublat = 0, maxlat = 0;
    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    req->is_write = true;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyhost_check_bounds(ns, slba, nlb))) return status;

    z = hyhost_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    if ((status = hyhost_s_check_zone_write(h, z, slba, nlb))) return status;
    if ((status = hyhost_map_dptr(n, data_size, req))) return status;

    data_offset = hyhost_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    hyhost_s_advance_wp(z, nlb);

#ifdef WRITE_CACHE_ON
    wcidx = hyhost_get_wcidx(h, hyhost_zone_idx(h, slba));
    // femu_log("[get_wcidx] wcidx : %ld, zone_idx : %lu used : %lu num_wc : %lu\n",
    //          wcidx, hyhost_zone_idx(h, slba), h->used_wc, h->num_wc);

    if (wcidx == -1) { // no matching wc
        if (h->used_wc == h->num_wc) { // write cache full
            wcidx = 0;
            uint64_t t_used = h->cache[0].used;

            for (int i = 1; i < h->num_wc; i++) {
                if (h->cache[i].used == 0) {
                    t_used = 0;
                    wcidx = i;
                    break;
                }

                if (h->cache[i].used < t_used) {
                    t_used = h->cache[i].used;
                    wcidx = i;
                }
            }

            if (t_used) {
                sublat = hyhost_wc_flush(h, wcidx, req);
                // femu_log("[Flush cache] %ld %lu\n", wcidx, hyhost_zone_idx(h, slba));
                h->used_wc++;
            }

            h->cache[wcidx].zidx = hyhost_zone_idx(h, slba);
        } else {
            // find empty cache slot
            for (int i = 0; i < h->num_wc; i++) {
                if (h->cache[i].zidx == -1) {
                    h->cache[i].zidx = hyhost_zone_idx(h, slba);
                    wcidx = i;
                    h->used_wc++;
                    // femu_log("[Empty cache] cache# : %lu zone# : %lu\n", i, hyhost_zone_idx(h, slba));
                    break;
                }
            }
        }
    }

    // femu_log("[Alloc cache] cache# : %ld zone# : %lu\n", wcidx, hyhost_zone_idx(h, slba));
    assert(wcidx != -1);

    maxlat = hyhost_cache_latency(h, req, ns, slba, nlb, true, wcidx);
    maxlat = (sublat > maxlat) ? sublat : maxlat;

    req->reqlat = maxlat;
    req->expire_time = cmd_stime + maxlat;
#else
    hyhost_charge_latency(h, req, ns, slba, nlb, /*is_write=*/true);
#endif
    // femu_log("hyhost_s_write\n");

    return NVME_SUCCESS;
}

static uint16_t hyhost_s_read(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                              NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyhost_l2b(ns, nlb);
    uint64_t data_offset;
    HyHostZone *z;
    uint16_t status;
    req->is_write = false;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyhost_check_bounds(ns, slba, nlb))) return status;

    z = hyhost_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    /* Reads past the zone's write pointer return zeros — matches real ZNS
     * devices and lets udev/blkid/ZenFS bootstrap probe unwritten zone-end
     * areas without faults. The DRAM backend keeps post-reset stale bytes
     * (reset doesn't wipe memory), so explicitly zero the unwritten span
     * before the DMA copy. */
    if (slba + nlb > z->w_ptr) {
        uint64_t uw_lba_start = (slba > z->w_ptr) ? slba : z->w_ptr;
        uint64_t uw_lba_count = slba + nlb - uw_lba_start;
        uint64_t uw_byte_off  = hyhost_l2b(ns, uw_lba_start);
        uint64_t uw_bytes     = hyhost_l2b(ns, uw_lba_count);
        memset((char *)n->mbe->logical_space + uw_byte_off, 0, uw_bytes);
    }
    if ((status = hyhost_map_dptr(n, data_size, req))) return status;

    data_offset = hyhost_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);
// #ifdef WRITE_CACHE_ON
    // wcidx = hyhost_get_wcidx(h, hyhost_zone_idx(h, slba));
    // if (wcidx == -1) {
    //     hyhost_charge_latency(h, req, ns, slba, nlb, /*is_write=*/false);
    //     return NVME_SUCCESS;
    // } else {
        // maxlat = hyhost_cache_latency(h, req, ns, slba, nlb, true, wcidx);
        // maxlat = (sublat > maxlat) ? sublat : maxlat;

        // maxlat = HYHOST_CACHE_RD_LAT_NS;
        // req->reqlat = maxlat;
        // req->expire_time += maxlat;
        // return NVME_SUCCESS;
        // for (int i = 0; i < h->cache[wcidx].used; i++) {
        //     if (h->cache[wcidx].lpns[i] == )
        // }
    // }
// #else
    hyhost_charge_latency(h, req, ns, slba, nlb, /*is_write=*/false);
// #endif
    // femu_log("hyhost_s_read\n");

    return NVME_SUCCESS;
}

/* Zone Append (NVMe opcode 0x7d). Host targets a zone via slba (zone start),
 * device picks the LBA = current wp, writes nlb sectors there, advances wp,
 * and returns the assigned slba via NvmeCqe::res64. ZenFS leans on this to
 * avoid wp contention from concurrent writers. R-region zones (is_random)
 * have no wp semantics on the device side, so append is rejected — dm-hyhost
 * manages R-region addressing through its L2P and shouldn't issue device
 * append into that range.
 */
static uint16_t hyhost_zone_append(FemuCtrl *n, NvmeNamespace *ns,
                                   NvmeCmd *cmd, NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyhost_l2b(ns, nlb);
    uint64_t data_offset;
    uint64_t assigned;
    HyHostZone *z;
    uint16_t status;

    req->is_write = true;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyhost_check_bounds(ns, slba, nlb))) return status;

    z = hyhost_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    /* Append must target the zone's start LBA — that's how the host names
     * the zone. The actual write offset is the zone's wp. */
    if (slba != z->d.zslba) return NVME_INVALID_FIELD | NVME_DNR;
    if (z->is_random)        return NVME_INVALID_FIELD | NVME_DNR;

    /* Reuse the sequential-write check at the wp; this also enforces zone
     * state and capacity. */
    if ((status = hyhost_s_check_zone_write(h, z, z->w_ptr, nlb))) return status;
    if ((status = hyhost_map_dptr(n, data_size, req))) return status;

    assigned    = z->w_ptr;                     /* snapshot before advance */
    data_offset = hyhost_l2b(ns, assigned);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    hyhost_s_advance_wp(z, nlb);
    hyhost_charge_latency(h, req, ns, assigned, nlb, /*is_write=*/true);

    req->cqe.res64 = cpu_to_le64(assigned);
    return NVME_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Zone management (Send / Recv)
 *
 * Send actions implemented (MVP): RESET, OPEN, CLOSE, FINISH.
 * Recv: REPORT (action 0). Other actions return INVALID_FIELD.
 * ------------------------------------------------------------------------- */

static uint16_t hyhost_zone_reset(HyHostZone *z)
{
    uint8_t state = hyhost_zone_state(z);
    if (state == HYHOST_ZS_OFFLINE || state == HYHOST_ZS_READ_ONLY) {
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
    z->w_ptr = z->d.zslba;
    z->d.wp  = z->w_ptr;
    hyhost_zone_set_state(z, HYHOST_ZS_EMPTY);
    return NVME_SUCCESS;
}

static uint16_t hyhost_zone_open(HyHostZone *z)
{
    uint8_t state = hyhost_zone_state(z);
    switch (state) {
    case HYHOST_ZS_EMPTY:
    case HYHOST_ZS_IMPLICITLY_OPEN:
    case HYHOST_ZS_CLOSED:
        hyhost_zone_set_state(z, HYHOST_ZS_EXPLICITLY_OPEN);
        return NVME_SUCCESS;
    case HYHOST_ZS_EXPLICITLY_OPEN:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
}

static uint16_t hyhost_zone_close(HyHostZone *z)
{
    uint8_t state = hyhost_zone_state(z);
    switch (state) {
    case HYHOST_ZS_IMPLICITLY_OPEN:
    case HYHOST_ZS_EXPLICITLY_OPEN:
        hyhost_zone_set_state(z, HYHOST_ZS_CLOSED);
        return NVME_SUCCESS;
    case HYHOST_ZS_CLOSED:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
}

static uint16_t hyhost_zone_finish(HyHostZone *z)
{
    uint8_t state = hyhost_zone_state(z);
    if (state == HYHOST_ZS_OFFLINE || state == HYHOST_ZS_READ_ONLY) {
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
    z->w_ptr = z->d.zslba + z->d.zcap;
    z->d.wp  = z->w_ptr;
    hyhost_zone_set_state(z, HYHOST_ZS_FULL);
    return NVME_SUCCESS;
}

/* Charge zone-reset latency: simulate one NAND erase per (ch, lun) in
 * parallel. With 8x8 striping every zone touches every (ch, lun), so a
 * single zone reset busies one block on each LUN — max latency = one
 * erase op (~2 ms). Mirrors hyssd-conf.c's pattern. select_all reset
 * undercharges (a single erase round vs M*round for M zones) but matches
 * HYSSD; refining requires modeling pages_per_block which isn't part of
 * the current geometry.
 */
static void hyhost_charge_zone_reset(HyHostSSD *h, NvmeRequest *req)
{
    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t max_lat = 0;
    for (uint32_t ch = 0; ch < h->nchs; ch++) {
        for (uint32_t lun = 0; lun < h->luns_per_ch; lun++) {
            int64_t lat = hyhost_advance_nand(h, ch, lun, cmd_stime,
                                              HYHOST_NAND_ERASE);
            if (lat > max_lat) max_lat = lat;
        }
    }
    req->reqlat      = max_lat;
    req->expire_time = cmd_stime + max_lat;
}

/* Charge zone-finish latency: pad the unwritten portion [wp, zone_end)
 * with NAND writes. Reuses hyhost_charge_latency, which iterates pages,
 * advances per-(ch,lun) timing, and sets req->expire_time = stime + max.
 * No-op if zone is already empty or full.
 */
static void hyhost_charge_zone_finish(HyHostSSD *h, NvmeRequest *req,
                                      NvmeNamespace *ns, HyHostZone *z)
{
    uint64_t zone_end  = z->d.zslba + z->d.zcap;
    uint64_t pad_start = z->w_ptr;
    if (pad_start >= zone_end) return;     /* already full or invalid */
    uint64_t pad_lbas  = zone_end - pad_start;
    if (pad_lbas == z->d.zcap) return;     /* empty zone — no real pad needed */
    hyhost_charge_latency(h, req, ns, pad_start, pad_lbas, /*is_write=*/true);
}

static uint16_t hyhost_zone_mgmt_send(FemuCtrl *n, NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeNamespace *ns = req->ns;
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint8_t  action = dw13 & 0xff;

#ifdef LOG_MODE
    femu_log("hyhost_zone_mgmt_send ");
    if (action == HYHOST_ZA_SET_R_END) {
        femu_log(": HYHOST_ZA_SET_R_END\n");
    } else if (action == HYHOST_ZA_R_BLOCK_ERASE) {
        femu_log(": HYHOST_ZA_R_BLOCK_ERASE\n");
    } else if (action == HYHOST_ZA_RESET) {
        femu_log(": HYHOST_ZA_RESET\n");
    } else if (action == HYHOST_ZA_OPEN) {
        femu_log(": HYHOST_ZA_OPEN\n");
    } else if (action == HYHOST_ZA_CLOSE) {
        femu_log(": HYHOST_ZA_CLOSE\n");
    } else if (action == HYHOST_ZA_FINISH) {
        femu_log(": HYHOST_ZA_FINISH\n");
    }
#endif

    /* slba decoded up front so SET_R_END / R_*_ERASE branches share it.
     * Wire convention: every ZSA carries its target LBA in the standard
     * SLBA field, including SET_R_END (= new r_end LBA). */
    uint64_t slba = ((uint64_t)le32_to_cpu(cmd->cdw11) << 32)
                     | le32_to_cpu(cmd->cdw10);

    if (action == HYHOST_ZA_SET_R_END) {
        return hyhost_set_r_end(n, slba);
    }

    /* R-region line erase (the hot GC path). dm-hyhost issues
     * one bio per GC'd line; we charge a NAND erase on every (ch, lun)
     * in parallel — each LUN holds exactly one block at this line's
     * block-offset, so the cost is one erase round (~2 ms). Reuses the
     * existing zone-reset latency helper, which already does the
     * all-(ch, lun) max-fold. Idempotent w.r.t. backend bytes (not
     * zeroed; see R_BLOCK_ERASE rationale).
     *
     * Aligned on line_size (64 MiB), NOT zone_size (1 GiB): a line is the
     * GC/erase unit and is a sub-multiple of a zone, so dm may erase any of
     * the 16 lines that make up a zone independently. */
    if (action == HYHOST_ZA_R_LINE_ERASE) {
        if (slba >= h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
        if (h->line_size == 0 || (slba % h->line_size) != 0) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        hyhost_charge_zone_reset(h, req);
        return NVME_SUCCESS;
    }

    /* R-region erase-block trigger (debug / legacy path; the hot GC path
     * is R_LINE_ERASE above). dm-hyhost no longer emits this in normal
     * operation. The device just charges timing —
     * no state to update because R-region has no zone/wp tracking.
     * Backend bytes are intentionally NOT zeroed: R-region accepts
     * in-place overwrite, so a stale read after erase is not meaningful
     * (host FTL invalidates the L2P entry). Skipping memset keeps the
     * simulation latency-only and matches the wp-less semantics
     * documented in hyhost.h. */
    if (action == HYHOST_ZA_R_BLOCK_ERASE) {
        uint64_t lba_bytes = NVME_ID_NS_LBADS_BYTES(ns);
        uint64_t lbas_per_page = (h->page_bytes && lba_bytes)
                               ? (uint64_t)(h->page_bytes / lba_bytes) : 1;
        uint64_t lbas_per_block = lbas_per_page * h->pgs_per_blk;
        if (slba >= h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
        if (lbas_per_block == 0 || (slba % lbas_per_block) != 0) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        uint64_t page_idx = slba / lbas_per_page;
        uint32_t ch, lun;
        hyhost_lba_to_chlun(h, page_idx, &ch, &lun);
        int64_t cmd_stime = req->stime ? req->stime
                                       : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        int64_t lat = hyhost_advance_nand(h, ch, lun, cmd_stime,
                                          HYHOST_NAND_ERASE);
        req->reqlat      = lat;
        req->expire_time = cmd_stime + lat;
        return NVME_SUCCESS;
    }

    bool select_all = (dw13 >> 8) & 0x1;
    HyHostZone *z;
    int wcidx = 0;

    if (select_all) {
        /* Apply action to all zones; bail on first error. RESET / FINISH
         * charge per-zone latency so the cumulative timer reflects
         * sequential rounds of erases or pad-writes across (ch, lun). */
        for (uint32_t i = 0; i < h->num_zones; i++) {
            HyHostZone *zi = &h->zone_array[i];
            uint16_t st = NVME_SUCCESS;
            switch (action) {
            case HYHOST_ZA_RESET:
                hyhost_charge_zone_reset(h, req);
                st = hyhost_zone_reset(zi);
                break;
            case HYHOST_ZA_OPEN:   st = hyhost_zone_open(zi);  break;
            case HYHOST_ZA_CLOSE:  st = hyhost_zone_close(zi); break;
            case HYHOST_ZA_FINISH:
                hyhost_charge_zone_finish(h, req, ns, zi);
                st = hyhost_zone_finish(zi);
                break;
            default: return NVME_INVALID_FIELD | NVME_DNR;
            }
            if (st != NVME_SUCCESS) return st;
        }
        return NVME_SUCCESS;
    }

    z = hyhost_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    switch (action) {
    case HYHOST_ZA_RESET:
#ifdef WRITE_CACHE_ON
        wcidx = hyhost_get_wcidx(h, hyhost_zone_idx(h, slba));
        if (wcidx == -1) {
            hyhost_charge_zone_reset(h, req);
        } else {
            hyhost_invalidate_cache(h, hyhost_zone_idx(h, slba));
        }
#else
    hyhost_charge_zone_reset(h, req);
#endif
        return hyhost_zone_reset(z);
    case HYHOST_ZA_OPEN:   return hyhost_zone_open(z);
    case HYHOST_ZA_CLOSE:  return hyhost_zone_close(z);
    case HYHOST_ZA_FINISH:
        hyhost_charge_zone_finish(h, req, ns, z);
        return hyhost_zone_finish(z);
    default:               return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static uint16_t hyhost_zone_mgmt_recv(FemuCtrl *n, NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    uint64_t slba = ((uint64_t)le32_to_cpu(cmd->cdw11) << 32)
                     | le32_to_cpu(cmd->cdw10);
    uint32_t numd = le32_to_cpu(cmd->cdw12);
    uint32_t buf_len = (numd + 1) << 2;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint8_t  action = dw13 & 0xff;
    bool partial = (dw13 >> 16) & 0x1;
    uint32_t start_idx;
    uint32_t max_zones;
    HyHostZoneReportHeader hdr;
    uint64_t prp1, prp2;
    uint8_t *buf;
    uint32_t hdr_size = sizeof(hdr);
    uint32_t descr_size = sizeof(HyHostZoneDescr);
    uint32_t i;
    uint32_t copied = 0;

    (void)partial;

#ifdef LOG_MODE
    femu_log("hyhost_zone_mgmt_recv");
    if (action == HYHOST_ZRA_REPORT_RZONE) {
        femu_log(": HYHOST_ZRA_REPORT_RZONE\n");
    } else {
        femu_log(": HYHOST_ZRA_REPORT\n");
    }
#endif
    /* Vendor R-zone count report. Kernel BLKREPORTRZONE -> NVMe Zone Mgmt
     * Recv with action 0x21 + 4-byte payload (struct blk_rzone_report). */
    if (action == HYHOST_ZRA_REPORT_RZONE) {
        uint32_t rzone_count;
        uint32_t le_count;
        if (buf_len < sizeof(uint32_t)) return NVME_INVALID_FIELD | NVME_DNR;
        rzone_count = (h->zone_size > 0)
                    ? (uint32_t)(h->r_region_end_lba / h->zone_size)
                    : 0u;
        le_count = cpu_to_le32(rzone_count);
        prp1 = le64_to_cpu(cmd->dptr.prp1);
        prp2 = le64_to_cpu(cmd->dptr.prp2);
        return dma_read_prp(n, (uint8_t *)&le_count, sizeof(le_count),
                            prp1, prp2);
    }

    if (action != HYHOST_ZRA_REPORT) return NVME_INVALID_FIELD | NVME_DNR;
    if (buf_len < hdr_size) return NVME_INVALID_FIELD | NVME_DNR;

    start_idx = hyhost_zone_idx(h, slba);
    if (start_idx >= h->num_zones) return NVME_LBA_RANGE | NVME_DNR;

    max_zones = (buf_len - hdr_size) / descr_size;
    if (max_zones > h->num_zones - start_idx) {
        max_zones = h->num_zones - start_idx;
    }

    buf = g_malloc0(buf_len);

    memset(&hdr, 0, sizeof(hdr));
    hdr.nr_zones = cpu_to_le64(max_zones);
    memcpy(buf, &hdr, hdr_size);
    copied = hdr_size;

    for (i = 0; i < max_zones; i++) {
        memcpy(buf + copied, &h->zone_array[start_idx + i].d, descr_size);
        copied += descr_size;
    }

    prp1 = le64_to_cpu(cmd->dptr.prp1);
    prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint16_t status = dma_read_prp(n, buf, copied, prp1, prp2);
    g_free(buf);
    return status;
}

/* -------------------------------------------------------------------------
 * Top-level dispatch
 * ------------------------------------------------------------------------- */

static uint16_t hyhost_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                              NvmeRequest *req)
{
    HyHostSSD *h = to_hyhost(n);

    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE: {
        NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
        uint64_t slba = le64_to_cpu(rw->slba);
        bool is_write = (cmd->opcode == NVME_CMD_WRITE);
        if (hyhost_lba_in_r_region(h, slba)) {
            return is_write ? hyhost_r_write(n, ns, cmd, req)
                            : hyhost_r_read(n, ns, cmd, req);
        }
        return is_write ? hyhost_s_write(n, ns, cmd, req)
                        : hyhost_s_read(n, ns, cmd, req);
    }
    case NVME_CMD_ZONE_MGMT_SEND:
        return hyhost_zone_mgmt_send(n, req);
    case NVME_CMD_ZONE_MGMT_RECV:
        return hyhost_zone_mgmt_recv(n, req);
    case NVME_CMD_ZONE_APPEND:
        return hyhost_zone_append(n, ns, cmd, req);
    case NVME_CMD_DSM:
        /* Dataset Management (discard / TRIM hints). Host FTL owns the
         * invalidation bookkeeping; on the device side it's a no-op. We
         * still ack success so the kernel doesn't see ENOTSUPP and
         * disable discard support on the namespace. */
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

/* Forward decl: hyhost_set_r_end calls this after updating r_region_end_lba.
 * Definition lives with the init helpers further down so init can reuse it. */
static void hyhost_classify_zones(HyHostSSD *h);

/* True if [lo, hi) covers any zone whose state isn't EMPTY. */
static bool hyhost_range_has_nonempty_zone(HyHostSSD *h, uint64_t lo, uint64_t hi)
{
    if (lo >= hi) return false;
    uint32_t first = hyhost_zone_idx(h, lo);
    /* hi is exclusive; the last zone we care about ends at hi - 1. */
    uint32_t last  = hyhost_zone_idx(h, hi - 1);
    if (last >= h->num_zones) last = h->num_zones - 1;
    for (uint32_t i = first; i <= last; i++) {
        if (hyhost_zone_state(&h->zone_array[i]) != HYHOST_ZS_EMPTY) {
            return true;
        }
    }
    return false;
}

uint16_t hyhost_set_r_end(FemuCtrl *n, uint64_t new_r_end_lba)
{
    HyHostSSD *h = to_hyhost(n);

    if (n->num_namespaces == 0) return NVME_INVALID_FIELD | NVME_DNR;
    NvmeNamespace *ns = &n->namespaces[0];
    if (new_r_end_lba > ns->id_ns.nsze) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* Boundary must land on a zone edge so no zone is split between
     * random- and sequential-write semantics. */
    if (h->zone_size == 0 || new_r_end_lba % h->zone_size != 0) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    uint64_t old_r_end = h->r_region_end_lba;
    if (new_r_end_lba == old_r_end) return NVME_SUCCESS;

    /* Affected range = the LBAs whose region is changing.
     *   shrink (R→S): [new, old)  — become sequential, must be EMPTY now
     *   grow   (S→R): [old, new)  — become random,     must be EMPTY now
     * Both directions require the zones in the range to currently hold no
     * data; the host FTL is expected to have drained / GC'd them already.
     */
    uint64_t lo = (new_r_end_lba < old_r_end) ? new_r_end_lba : old_r_end;
    uint64_t hi = (new_r_end_lba < old_r_end) ? old_r_end     : new_r_end_lba;

    if (hyhost_range_has_nonempty_zone(h, lo, hi)) {
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }

    h->r_region_end_lba = new_r_end_lba;
    hyhost_classify_zones(h);
    femu_log("hyhostssd: r_region_end_lba %lu -> %lu (zone %u -> %u)\n",
             old_r_end, new_r_end_lba,
             (uint32_t)(old_r_end / h->zone_size),
             (uint32_t)(new_r_end_lba / h->zone_size));
    return NVME_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Init / exit
 * ------------------------------------------------------------------------- */

/* Requires hyhost_init_timing() to have run first (uses h->nchs / luns_per_ch
 * / pgs_per_blk / page_bytes to derive line_size). */
static void hyhost_init_zones(FemuCtrl *n, HyHostSSD *h, NvmeNamespace *ns)
{
    uint64_t nsze     = le64_to_cpu(ns->id_ns.nsze);
    uint64_t lba_sz   = NVME_ID_NS_LBADS_BYTES(ns);   /* bytes per LBA */
    int      zsize_mb = n->hyhost_params.zone_size_mb;
    uint64_t zone_bytes;

    /* zone_size is expressed in LBAs, not bytes. Prop is in MiB; 0 => 1 GiB. */
    zone_bytes = zsize_mb > 0 ? (uint64_t)zsize_mb << 20
                              : HYHOST_DEFAULT_ZONE_SIZE_BYTES;
    h->zone_size      = zone_bytes / lba_sz;
    h->zone_size_log2 = is_power_of_2(h->zone_size) ? ctz64(h->zone_size) : 0;
    h->num_zones      = nsze / h->zone_size;

    /* Line = one NAND block per (ch, lun) in parallel = the erase / GC unit.
     * line_size (LBAs) = nchs * luns_per_ch * pgs_per_blk * (page_bytes /
     * lba_sz). A zone must be an integer number of lines. */
    h->line_size = (uint64_t)h->nchs * h->luns_per_ch * h->pgs_per_blk
                 * (h->page_bytes / lba_sz);
    if (h->line_size == 0) h->line_size = h->zone_size;
    if (h->zone_size % h->line_size != 0) {
        femu_log("hyhostssd: WARNING zone_size %lu not a multiple of line_size "
                 "%lu — R_LINE_ERASE alignment will be zone-granular\n",
                 h->zone_size, h->line_size);
        h->line_size = h->zone_size;
    }

    if (h->num_zones == 0) {
        /* Namespace smaller than one zone: still allocate one zone covering it. */
        h->num_zones = 1;
        h->zone_size = nsze;
        h->zone_size_log2 = is_power_of_2(h->zone_size) ? ctz64(h->zone_size) : 0;
        if (h->line_size > h->zone_size) h->line_size = h->zone_size;
    }

    h->zone_array = g_malloc0(sizeof(HyHostZone) * h->num_zones);
    for (uint32_t i = 0; i < h->num_zones; i++) {
        HyHostZone *z = &h->zone_array[i];
        z->d.zcap  = h->zone_size;        /* MVP: capacity == size */
        z->d.zslba = (uint64_t)i * h->zone_size;
        z->d.wp    = z->d.zslba;
        z->w_ptr   = z->d.zslba;
        hyhost_zone_set_state(z, HYHOST_ZS_EMPTY);
    }
}

/* Tag each zone as random or sequential by region (internal flag only —
 * see header comment for the wire-level SEQ-only convention).
 *
 * Idempotent: called once at init and again from hyhost_set_r_end after the
 * boundary moves. The set_r_end EMPTY-only invariant means zones whose tag
 * is changing are guaranteed empty (wp == zslba, state == EMPTY), so no
 * write-pointer or state fixup is needed here.
 */
static void hyhost_classify_zones(HyHostSSD *h)
{
    for (uint32_t i = 0; i < h->num_zones; i++) {
        HyHostZone *z = &h->zone_array[i];
        /* All zones report SEQ_WRITE on the wire — the 5.15 NVMe driver
         * rejects any other type (zns.c:158) and tears down the namespace.
         * R-region status is tracked by the device-internal is_random flag
         * and surfaced to userspace through Zone Mgmt Recv action 0x21. */
        z->d.zt      = HYHOST_ZONE_TYPE_SEQ_WRITE;
        z->is_random = (z->d.zslba < h->r_region_end_lba);
    }
}

/* Populate the CSI=ZONED identify response so the kernel sees /dev/nvme0n1
 * as a zoned namespace. After this point, the standard zoned-block layer
 * (blkzone, REQ_OP_ZONE_*, ZenFS, ...) accepts the device. Mirrors
 * zns_init_zone_identify() but reads geometry from HyHostSSD instead of the
 * ZNS-mode globals. lba_index is the LBA format index, conventionally 0.
 */
static void hyhost_init_zone_identify(FemuCtrl *n, NvmeNamespace *ns,
                                      int lba_index)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeIdNsZoned *id_ns_z = g_malloc0(sizeof(NvmeIdNsZoned));

    /* MAR/MOR are zeroes-based (0xFFFFFFFF == "no limit"). When
     * max_active_zones / max_open_zones are unset (the MVP case), the
     * arithmetic 0u - 1 wraps to 0xFFFFFFFF, which is exactly the spec's
     * "unlimited" sentinel — so unconfigured == unlimited, as desired. */
    id_ns_z->mar = cpu_to_le32(n->max_active_zones - 1);
    id_ns_z->mor = cpu_to_le32(n->max_open_zones - 1);
    id_ns_z->zoc = 0;
    id_ns_z->ozcs = n->cross_zone_read ? 0x01 : 0x00;

    id_ns_z->lbafe[lba_index].zsze = cpu_to_le64(h->zone_size);
    id_ns_z->lbafe[lba_index].zdes = 0;     /* no zone descriptor extension */

    n->csi          = NVME_CSI_ZONED;
    n->id_ns_zoned  = id_ns_z;

    /* Align ns capacity with what we actually expose as zones. integer
     * division in hyhost_init_zones may have rounded down; expose the
     * post-rounding value to the host so all reported zones are addressable. */
    ns->id_ns.nsze = cpu_to_le64((uint64_t)h->num_zones * h->zone_size);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.ncap;
}

/* Initialize the channel/LUN timing model. NAND geometry comes from the
 * device props (hyhost_nchs / hyhost_luns_per_ch / hyhost_page_bytes /
 * hyhost_pgs_per_blk); each falls back to its compile-time default when the
 * prop is left at 0. Latencies still use compile-time defaults. Must run
 * before hyhost_init_zones so the geometry is available to compute
 * line_size. */
static void hyhost_init_timing(FemuCtrl *n, HyHostSSD *h)
{
    HyHostCtrlParams *p = &n->hyhost_params;

    h->nchs        = p->nchs        > 0 ? (uint32_t)p->nchs        : HYHOST_DEFAULT_NCHS;
    h->luns_per_ch = p->luns_per_ch > 0 ? (uint32_t)p->luns_per_ch : HYHOST_DEFAULT_LUNS_PER_CH;
    h->page_bytes  = p->page_bytes  > 0 ? (uint32_t)p->page_bytes  : HYHOST_DEFAULT_PAGE_BYTES;
    h->pgs_per_blk = p->pgs_per_blk > 0 ? (uint32_t)p->pgs_per_blk : HYHOST_DEFAULT_PGS_PER_BLK;
    h->pg_rd_lat_ns   = HYHOST_PG_RD_LAT_NS;
    h->pg_wr_lat_ns   = HYHOST_PG_WR_LAT_NS;
    h->pg_wr_lat_r_ns = HYHOST_PG_WR_LAT_R_NS;
    h->blk_er_lat_ns  = HYHOST_BLK_ER_LAT_NS;
    h->ch_xfer_lat_ns = HYHOST_CH_XFER_LAT_NS;

    h->ch_next_avail_ns  = g_malloc0(sizeof(int64_t) * h->nchs);
    h->lun_next_avail_ns = g_malloc0(sizeof(int64_t) * h->nchs * h->luns_per_ch);
    pthread_mutex_init(&h->timing_mutex, NULL);
}

static void hyhost_exit_timing(HyHostSSD *h)
{
    g_free(h->ch_next_avail_ns);
    h->ch_next_avail_ns = NULL;
    g_free(h->lun_next_avail_ns);
    h->lun_next_avail_ns = NULL;
    pthread_mutex_destroy(&h->timing_mutex);
}

static void hyhost_init(FemuCtrl *n, Error **errp)
{
    HyHostSSD *h = to_hyhost(n);
    NvmeNamespace *ns;
    uint64_t nsze;

    if (n->num_namespaces == 0) {
        h->r_region_end_lba = 0;
        return;
    }
    ns = &n->namespaces[0];
    nsze = le64_to_cpu(ns->id_ns.nsze);
    h->r_region_end_lba = nsze;     /* default: entire namespace is R */
    // h->r_region_end_lba = 0;        /* entire namespace is S */
    // h->r_region_end_lba = 8388608;        /* entire namespace is S */

    /* Timing first: init_zones derives line_size from the NAND geometry. */
    hyhost_init_timing(n, h);
    hyhost_init_zones(n, h, ns);
    hyhost_classify_zones(h);
    hyhost_init_zone_identify(n, ns, 0);

    // write cache initialization
    h->num_wc = DEFAULT_NUM_WRITE_CACHE;
    h->used_wc = 0;
    h->cache = g_malloc0(sizeof(struct ZNSWriteCache) * h->num_wc);
    for (int i = 0; i < h->num_wc; i++) {
        h->cache[i].zidx = -1;
        h->cache[i].used = 0;
        h->cache[i].cap = h->nchs * h->luns_per_ch * 128;
        h->cache[i].lpns = g_malloc(sizeof(uint64_t) * h->cache[i].cap);
    }

    femu_log("hyhostssd: init nsze=%lu lba=%uB r_end=%lu zones=%u zone_size=%lu "
             "line_size=%lu lines_per_zone=%lu ch=%u lun_per_ch=%u page=%uB "
             "pgs_per_blk=%u rd=%ldns wr=%ldns wr_r=%ldns er=%ldns\n",
             nsze, (unsigned)NVME_ID_NS_LBADS_BYTES(ns),
             h->r_region_end_lba, h->num_zones, h->zone_size,
             h->line_size, h->line_size ? h->zone_size / h->line_size : 0,
             h->nchs, h->luns_per_ch, h->page_bytes, h->pgs_per_blk,
             h->pg_rd_lat_ns, h->pg_wr_lat_ns, h->pg_wr_lat_r_ns,
             h->blk_er_lat_ns);
    (void)errp;
}

static void hyhost_exit(FemuCtrl *n)
{
    HyHostSSD *h = to_hyhost(n);
    hyhost_exit_timing(h);
    g_free(h->zone_array);
    g_free(n->id_ns_zoned);
    n->id_ns_zoned = NULL;
    g_free(h);
    n->ext_ops.state = NULL;
}

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

int nvme_register_hyhostssd(FemuCtrl *n)
{
    HyHostSSD *h = g_malloc0(sizeof(*h));

    n->ext_ops = (FemuExtCtrlOps) {
        .state            = h,
        .init             = hyhost_init,
        .exit             = hyhost_exit,
        .rw_check_req     = NULL,
        .start_ctrl       = NULL,
        .admin_cmd        = NULL,
        .io_cmd           = hyhost_io_cmd,
        .get_log          = NULL,
    };
    return 0;
}
