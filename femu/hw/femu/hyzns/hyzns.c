/*
 * Host-managed Hybrid SSD (HYZNS) — FEMU mode 5.
 *
 * Self-contained: no include or call into zns/, bbssd/, or ocssd/.
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

#include "./hyzns.h"

/* Paper latency defaults (ns). */
#define HYZNS_PG_RD_LAT_NS         (80000ULL)
#define HYZNS_PG_WR_LAT_NS         (450000ULL)
#define HYZNS_BLK_ER_LAT_NS        (2000000ULL)
#define HYZNS_CH_XFER_LAT_NS       (25000ULL)

/* R-region (conventional namespace) write latency. Kept as a separate knob
 * so R can be modeled independently (e.g. lowered to model a DRAM write
 * cache), but the ConfZNS-matched configuration uses the same raw-NAND
 * program latency as S (= HYZNS_PG_WR_LAT_NS). */
#define HYZNS_PG_WR_LAT_R_NS       (450000ULL)

#define HYZNS_CACHE_WR_LAT_NS      (1000)
#define HYZNS_CACHE_RD_LAT_NS      (1000)

/* NAND geometry fallbacks — used only if the device props are left at 0.
 * The real values come from `-device femu,hyzns_nchs=..,hyzns_luns_per_ch=..,
 * hyzns_page_bytes=..,hyzns_pgs_per_blk=..,hyzns_zone_size_mb=..` (see
 * femu.c). Default baseline: 8 ch x 4 way x 128 pg/blk x 16 KiB NAND page =>
 * block = 2 MiB, line = 64 MiB (device erase / host GC unit), zone = 1 GiB
 * = 16 lines.
 *
 * Two distinct "pages": the host LBA is 4 KiB (NVMe LBA format, dm-hyzns's
 * L2P granularity), while the device NAND page is 16 KiB — the ch/lun
 * striping + program-latency unit. hyzns_charge_latency folds host LBAs
 * into NAND pages via lbas_per_page = page_bytes / lba_bytes (= 4 here), so
 * a 16 KiB NAND page covers 4 consecutive 4 KiB host LBAs. Keep them in
 * sync with dm-hyzns, which counts the same 64 MiB line in *host-page*
 * units (nchs*luns_per_ch*(block_bytes/4KiB)).
 *
 * The timing model stripes NAND pages round-robin across nchs×luns_per_ch
 * lanes, so changing the geometry just rescales parallelism — no code path
 * assumes a particular lane count or that zone == line. */
#define HYZNS_DEFAULT_NCHS          (8u)
#define HYZNS_DEFAULT_LUNS_PER_CH   (4u)
#define HYZNS_DEFAULT_PAGE_BYTES    (16384u) /* NAND page = 16 KiB = 32 sectors */
#define HYZNS_DEFAULT_PGS_PER_BLK   (128u)   /* 2 MiB erase block @ 16 KiB page */

/* Default zone size (sectors). 1 GiB at 4 KiB sector = 256 Ki sectors. */
#define HYZNS_DEFAULT_ZONE_SIZE_BYTES  (1ULL << 30)
// #define LOG_MODE

// #define WRITE_CACHE_ON
#define DEFAULT_NUM_WRITE_CACHE 4

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static inline HyZNSSSD *to_hyzns(FemuCtrl *n)
{
    return (HyZNSSSD *)n->ext_ops.state;
}

static inline size_t hyzns_l2b(NvmeNamespace *ns, uint64_t lba)
{
    return lba << NVME_ID_NS_LBADS(ns);
}

static inline bool hyzns_lba_in_r_region(HyZNSSSD *h, uint64_t slba)
{
    return slba < h->r_region_end_lba;
}

static inline uint16_t hyzns_check_bounds(NvmeNamespace *ns, uint64_t slba,
                                           uint32_t nlb)
{
    uint64_t nsze = le64_to_cpu(ns->id_ns.nsze);
    if (unlikely(UINT64_MAX - slba < nlb || slba + nlb > nsze)) {
        return NVME_LBA_RANGE | NVME_DNR;
    }
    return NVME_SUCCESS;
}

static uint16_t hyzns_map_dptr(FemuCtrl *n, size_t len, NvmeRequest *req)
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

enum HyZNSNandOp {
    HYZNS_NAND_READ,
    HYZNS_NAND_WRITE,      /* S-region (ZNS) raw-NAND program latency */
    HYZNS_NAND_WRITE_R,    /* R-region (conventional) — DRAM-cached, lower latency */
    HYZNS_NAND_ERASE,
};

/* LBA→(ch, lun) interleaving — page-major (BBSSD-style write pointer order).
 *
 * Mirrors bbssd's `ssd_advance_write_pointer`: consecutive NAND pages round-
 * robin through (ch, lun, pg) with ch the fastest-changing axis. Within a
 * single line N the order is (ch=0..nchs-1, lun=0, pg=0) → (ch=*, lun=1, pg=0)
 * → ... → (ch=*, lun=luns_per_ch-1, pg=0) → (ch=*, lun=*, pg=1) → ...
 *
 * The (ch, lun) distribution per write trace must be identical to BBSSD's
 * allocator output for the geometry comparison to hold. dm-hyzns's line
 * allocator walks the same (ch, lun, pg) order so host and device land on
 * the same page index.
 */
static inline void hyzns_lba_to_chlun(HyZNSSSD *h, uint64_t page_idx,
                                       uint32_t *ch, uint32_t *lun)
{
    *ch  = page_idx % h->nchs;
    *lun = (page_idx / h->nchs) % h->luns_per_ch;
}

/* Advance one NAND op on the given (ch, lun). Mirrors bbssd's
 * ssd_advance_status — read = NAND-busy then channel transfer; write =
 * channel transfer then NAND-busy. Mutex serializes the ch/lun timer
 * updates so concurrent IOs don't race on max(prev, now) reasoning. */
static int64_t hyzns_advance_nand(HyZNSSSD *h, uint32_t ch, uint32_t lun,
                                   int64_t cmd_stime, enum HyZNSNandOp op)
{
    int64_t *lun_next = &h->lun_next_avail_ns[ch * h->luns_per_ch + lun];
    int64_t *ch_next  = &h->ch_next_avail_ns[ch];
    int64_t lat = 0;
    int64_t nand_stime, chnl_stime;

    // pthread_mutex_lock(&h->timing_mutex);

    switch (op) {
    case HYZNS_NAND_READ:
        nand_stime = (*lun_next < cmd_stime) ? cmd_stime : *lun_next;
        *lun_next  = nand_stime + h->pg_rd_lat_ns;
        chnl_stime = (*ch_next < *lun_next) ? *lun_next : *ch_next;
        *ch_next   = chnl_stime + h->ch_xfer_lat_ns;
        lat = *ch_next - cmd_stime;
        break;
    case HYZNS_NAND_WRITE:
        chnl_stime = (*ch_next < cmd_stime) ? cmd_stime : *ch_next;
        *ch_next   = chnl_stime + h->ch_xfer_lat_ns;
        nand_stime = (*lun_next < *ch_next) ? *ch_next : *lun_next;
        *lun_next  = nand_stime + h->pg_wr_lat_ns;
        lat = *lun_next - cmd_stime;
        break;
    case HYZNS_NAND_WRITE_R:
        /* Same channel-then-NAND ordering as a normal write, but the
         * "NAND" stage charges the conventional DRAM-cached latency. */
        chnl_stime = (*ch_next < cmd_stime) ? cmd_stime : *ch_next;
        *ch_next   = chnl_stime + h->ch_xfer_lat_ns;
        nand_stime = (*lun_next < *ch_next) ? *ch_next : *lun_next;
        *lun_next  = nand_stime + h->pg_wr_lat_r_ns;
        lat = *lun_next - cmd_stime;
        break;
    case HYZNS_NAND_ERASE:
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
static void hyzns_charge_latency(HyZNSSSD *h, NvmeRequest *req,
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
        enum HyZNSNandOp op;

        if (!is_write) {
            op = HYZNS_NAND_READ;
        } else if (pg * lbas_per_page < h->r_region_end_lba) {
            /* R-region (conventional) write — DRAM-cached, lower latency. */
            op = HYZNS_NAND_WRITE_R;
        } else {
            op = HYZNS_NAND_WRITE;
        }

        hyzns_lba_to_chlun(h, pg, &ch, &lun);
        lat = hyzns_advance_nand(h, ch, lun, cmd_stime, op);
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
            femu_log("[hyzns_gc_probe] R-write win_n=%d stime_spread_ns=%" PRId64
                     " avg_gap_ns=%" PRId64 "\n",
                     win_n, spread, win_n > 1 ? gap_sum / (win_n - 1) : 0);
            win_n = 0; gap_sum = 0; prev_stime = 0;
        }
    }
}

static int hyzns_get_wcidx(HyZNSSSD *h, int zone_idx)
{
    int wcidx = -1;

    for (int i = 0; i < h->num_wc; i++) {
        if (h->cache[i].zidx == zone_idx) {
            return i;
        }
    }

    return wcidx;
}

static uint64_t hyzns_wc_flush(HyZNSSSD *h, int wcidx, NvmeRequest *req)
{
    uint64_t lpn;
    uint64_t sublat = 0, maxlat = 0;
    uint32_t ch, lun;
    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    for (int i = 0; i < h->cache[wcidx].used; i++) {
        lpn = h->cache[wcidx].lpns[i];
        hyzns_lba_to_chlun(h, lpn, &ch, &lun);
        sublat = hyzns_advance_nand(h, ch, lun, cmd_stime, HYZNS_NAND_WRITE);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    h->cache[wcidx].zidx = -1;
    h->cache[wcidx].used = 0;
    h->used_wc -= 1;

    // femu_log("[hyzns_wc_flush] wcidx : %d\n", wcidx);
    return maxlat;
}

static uint64_t hyzns_cache_latency(HyZNSSSD *h, NvmeRequest *req,
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
            sublat = hyzns_wc_flush(h, wcidx, req);
            maxlat = (sublat > maxlat) ? sublat : maxlat;
            sublat = 0;
        }
        h->cache[wcidx].lpns[h->cache[wcidx].used++] = lpn;
        sublat += HYZNS_CACHE_WR_LAT_NS;
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static void hyzns_invalidate_cache(HyZNSSSD *h, uint32_t zone_idx)
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

static inline uint8_t hyzns_zone_state(const HyZNSZone *z)
{
    return z->d.zs >> 4;
}

static inline void hyzns_zone_set_state(HyZNSZone *z, uint8_t state)
{
    z->d.zs = state << 4;
}

static inline uint32_t hyzns_zone_idx(HyZNSSSD *h, uint64_t slba)
{
    return h->zone_size_log2 ? (uint32_t)(slba >> h->zone_size_log2)
                             : (uint32_t)(slba / h->zone_size);
}

static inline HyZNSZone *hyzns_zone_by_slba(HyZNSSSD *h, uint64_t slba)
{
    uint32_t idx = hyzns_zone_idx(h, slba);
    if (idx >= h->num_zones) return NULL;
    return &h->zone_array[idx];
}

/* -------------------------------------------------------------------------
 * R-region IO
 * ------------------------------------------------------------------------- */

static uint16_t hyzns_r_write(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                               NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyzns_l2b(ns, nlb);
    uint64_t data_offset;
    uint16_t status;

    req->is_write = true;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyzns_check_bounds(ns, slba, nlb))) return status;
    if (slba + nlb > h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
    if ((status = hyzns_map_dptr(n, data_size, req))) return status;

    /* Random write: no write-pointer check. The host FTL must not overwrite
     * a still-valid physical page; from the device POV LBA→byte is direct. */
    data_offset = hyzns_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    // femu_log("hyzns_r_write\n");

    hyzns_charge_latency(h, req, ns, slba, nlb, /*is_write=*/true);
    return NVME_SUCCESS;
}

static uint16_t hyzns_r_read(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                              NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyzns_l2b(ns, nlb);
    uint64_t data_offset;
    uint16_t status;

    req->is_write = false;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyzns_check_bounds(ns, slba, nlb))) return status;
    if (slba + nlb > h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
    if ((status = hyzns_map_dptr(n, data_size, req))) return status;

    data_offset = hyzns_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    // femu_log("hyzns_r_read\n");

    hyzns_charge_latency(h, req, ns, slba, nlb, /*is_write=*/false);
    return NVME_SUCCESS;
}

/* -------------------------------------------------------------------------
 * S-region IO
 * ------------------------------------------------------------------------- */

static uint16_t hyzns_s_check_zone_write(HyZNSSSD *h, HyZNSZone *z,
                                          uint64_t slba, uint32_t nlb)
{
    uint8_t state = hyzns_zone_state(z);

    /* R-region zones report SEQ on the wire but accept random/in-place
     * writes — only bounds check, no wp/state enforcement. */
    if (z->is_random) {
        if (slba + nlb > z->d.zslba + z->d.zcap) {
            return NVME_ZONE_BOUNDARY_ERROR | NVME_DNR;
        }
        return NVME_SUCCESS;
    }

    if (state == HYZNS_ZS_FULL || state == HYZNS_ZS_READ_ONLY ||
        state == HYZNS_ZS_OFFLINE) {
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

static void hyzns_s_advance_wp(HyZNSZone *z, uint32_t nlb)
{
    uint8_t state = hyzns_zone_state(z);

    if (state == HYZNS_ZS_EMPTY) {
        hyzns_zone_set_state(z, HYZNS_ZS_IMPLICITLY_OPEN);
    }
    z->w_ptr += nlb;
    z->d.wp = z->w_ptr;
    if (z->w_ptr >= z->d.zslba + z->d.zcap) {
        hyzns_zone_set_state(z, HYZNS_ZS_FULL);
    }
}

static uint16_t hyzns_s_write(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                               NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyzns_l2b(ns, nlb);
    uint64_t data_offset;
    HyZNSZone *z;
    uint16_t status;
    int wcidx = 0;
    uint64_t sublat = 0, maxlat = 0;
    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    req->is_write = true;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyzns_check_bounds(ns, slba, nlb))) return status;

    z = hyzns_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    if ((status = hyzns_s_check_zone_write(h, z, slba, nlb))) return status;
    if ((status = hyzns_map_dptr(n, data_size, req))) return status;

    data_offset = hyzns_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    hyzns_s_advance_wp(z, nlb);

#ifdef WRITE_CACHE_ON
    wcidx = hyzns_get_wcidx(h, hyzns_zone_idx(h, slba));
    // femu_log("[get_wcidx] wcidx : %ld, zone_idx : %lu used : %lu num_wc : %lu\n",
    //          wcidx, hyzns_zone_idx(h, slba), h->used_wc, h->num_wc);

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
                sublat = hyzns_wc_flush(h, wcidx, req);
                // femu_log("[Flush cache] %ld %lu\n", wcidx, hyzns_zone_idx(h, slba));
                h->used_wc++;
            }

            h->cache[wcidx].zidx = hyzns_zone_idx(h, slba);
        } else {
            // find empty cache slot
            for (int i = 0; i < h->num_wc; i++) {
                if (h->cache[i].zidx == -1) {
                    h->cache[i].zidx = hyzns_zone_idx(h, slba);
                    wcidx = i;
                    h->used_wc++;
                    // femu_log("[Empty cache] cache# : %lu zone# : %lu\n", i, hyzns_zone_idx(h, slba));
                    break;
                }
            }
        }
    }

    // femu_log("[Alloc cache] cache# : %ld zone# : %lu\n", wcidx, hyzns_zone_idx(h, slba));
    assert(wcidx != -1);

    maxlat = hyzns_cache_latency(h, req, ns, slba, nlb, true, wcidx);
    maxlat = (sublat > maxlat) ? sublat : maxlat;

    req->reqlat = maxlat;
    req->expire_time = cmd_stime + maxlat;
#else
    hyzns_charge_latency(h, req, ns, slba, nlb, /*is_write=*/true);
#endif
    // femu_log("hyzns_s_write\n");

    return NVME_SUCCESS;
}

static uint16_t hyzns_s_read(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                              NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyzns_l2b(ns, nlb);
    uint64_t data_offset;
    HyZNSZone *z;
    uint16_t status;
    req->is_write = false;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyzns_check_bounds(ns, slba, nlb))) return status;

    z = hyzns_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    /* Reads past the zone's write pointer return zeros — matches real ZNS
     * devices and lets udev/blkid/ZenFS bootstrap probe unwritten zone-end
     * areas without faults. The DRAM backend keeps post-reset stale bytes
     * (reset doesn't wipe memory), so explicitly zero the unwritten span
     * before the DMA copy. */
    if (slba + nlb > z->w_ptr) {
        uint64_t uw_lba_start = (slba > z->w_ptr) ? slba : z->w_ptr;
        uint64_t uw_lba_count = slba + nlb - uw_lba_start;
        uint64_t uw_byte_off  = hyzns_l2b(ns, uw_lba_start);
        uint64_t uw_bytes     = hyzns_l2b(ns, uw_lba_count);
        memset((char *)n->mbe->logical_space + uw_byte_off, 0, uw_bytes);
    }
    if ((status = hyzns_map_dptr(n, data_size, req))) return status;

    data_offset = hyzns_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);
// #ifdef WRITE_CACHE_ON
    // wcidx = hyzns_get_wcidx(h, hyzns_zone_idx(h, slba));
    // if (wcidx == -1) {
    //     hyzns_charge_latency(h, req, ns, slba, nlb, /*is_write=*/false);
    //     return NVME_SUCCESS;
    // } else {
        // maxlat = hyzns_cache_latency(h, req, ns, slba, nlb, true, wcidx);
        // maxlat = (sublat > maxlat) ? sublat : maxlat;

        // maxlat = HYZNS_CACHE_RD_LAT_NS;
        // req->reqlat = maxlat;
        // req->expire_time += maxlat;
        // return NVME_SUCCESS;
        // for (int i = 0; i < h->cache[wcidx].used; i++) {
        //     if (h->cache[wcidx].lpns[i] == )
        // }
    // }
// #else
    hyzns_charge_latency(h, req, ns, slba, nlb, /*is_write=*/false);
// #endif
    // femu_log("hyzns_s_read\n");

    return NVME_SUCCESS;
}

/* Zone Append (NVMe opcode 0x7d). Host targets a zone via slba (zone start),
 * device picks the LBA = current wp, writes nlb sectors there, advances wp,
 * and returns the assigned slba via NvmeCqe::res64. ZenFS leans on this to
 * avoid wp contention from concurrent writers. R-region zones (is_random)
 * have no wp semantics on the device side, so append is rejected — dm-hyzns
 * manages R-region addressing through its L2P and shouldn't issue device
 * append into that range.
 */
static uint16_t hyzns_zone_append(FemuCtrl *n, NvmeNamespace *ns,
                                   NvmeCmd *cmd, NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hyzns_l2b(ns, nlb);
    uint64_t data_offset;
    uint64_t assigned;
    HyZNSZone *z;
    uint16_t status;

    req->is_write = true;

    if ((status = nvme_check_mdts(n, data_size))) return status;
    if ((status = hyzns_check_bounds(ns, slba, nlb))) return status;

    z = hyzns_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    /* Append must target the zone's start LBA — that's how the host names
     * the zone. The actual write offset is the zone's wp. */
    if (slba != z->d.zslba) return NVME_INVALID_FIELD | NVME_DNR;
    if (z->is_random)        return NVME_INVALID_FIELD | NVME_DNR;

    /* Reuse the sequential-write check at the wp; this also enforces zone
     * state and capacity. */
    if ((status = hyzns_s_check_zone_write(h, z, z->w_ptr, nlb))) return status;
    if ((status = hyzns_map_dptr(n, data_size, req))) return status;

    assigned    = z->w_ptr;                     /* snapshot before advance */
    data_offset = hyzns_l2b(ns, assigned);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    hyzns_s_advance_wp(z, nlb);
    hyzns_charge_latency(h, req, ns, assigned, nlb, /*is_write=*/true);

    req->cqe.res64 = cpu_to_le64(assigned);
    return NVME_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Zone management (Send / Recv)
 *
 * Send actions implemented (MVP): RESET, OPEN, CLOSE, FINISH.
 * Recv: REPORT (action 0). Other actions return INVALID_FIELD.
 * ------------------------------------------------------------------------- */

static uint16_t hyzns_zone_reset(HyZNSZone *z)
{
    uint8_t state = hyzns_zone_state(z);
    if (state == HYZNS_ZS_OFFLINE || state == HYZNS_ZS_READ_ONLY) {
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
    z->w_ptr = z->d.zslba;
    z->d.wp  = z->w_ptr;
    hyzns_zone_set_state(z, HYZNS_ZS_EMPTY);
    return NVME_SUCCESS;
}

static uint16_t hyzns_zone_open(HyZNSZone *z)
{
    uint8_t state = hyzns_zone_state(z);
    switch (state) {
    case HYZNS_ZS_EMPTY:
    case HYZNS_ZS_IMPLICITLY_OPEN:
    case HYZNS_ZS_CLOSED:
        hyzns_zone_set_state(z, HYZNS_ZS_EXPLICITLY_OPEN);
        return NVME_SUCCESS;
    case HYZNS_ZS_EXPLICITLY_OPEN:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
}

static uint16_t hyzns_zone_close(HyZNSZone *z)
{
    uint8_t state = hyzns_zone_state(z);
    switch (state) {
    case HYZNS_ZS_IMPLICITLY_OPEN:
    case HYZNS_ZS_EXPLICITLY_OPEN:
        hyzns_zone_set_state(z, HYZNS_ZS_CLOSED);
        return NVME_SUCCESS;
    case HYZNS_ZS_CLOSED:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
}

static uint16_t hyzns_zone_finish(HyZNSZone *z)
{
    uint8_t state = hyzns_zone_state(z);
    if (state == HYZNS_ZS_OFFLINE || state == HYZNS_ZS_READ_ONLY) {
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }
    z->w_ptr = z->d.zslba + z->d.zcap;
    z->d.wp  = z->w_ptr;
    hyzns_zone_set_state(z, HYZNS_ZS_FULL);
    return NVME_SUCCESS;
}

/* Charge zone-reset latency: simulate one NAND erase per (ch, lun) in
 * parallel. With 8x8 striping every zone touches every (ch, lun), so a
 * single zone reset busies one block on each LUN — max latency = one
 * erase op (~2 ms). select_all reset
 * undercharges (a single erase round vs M*round for M zones) but matches
 * the legacy behavior; refining requires modeling pages_per_block which isn't part of
 * the current geometry.
 */
static void hyzns_charge_zone_reset(HyZNSSSD *h, NvmeRequest *req)
{
    int64_t cmd_stime = req->stime ? req->stime
                                   : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t max_lat = 0;
    for (uint32_t ch = 0; ch < h->nchs; ch++) {
        for (uint32_t lun = 0; lun < h->luns_per_ch; lun++) {
            int64_t lat = hyzns_advance_nand(h, ch, lun, cmd_stime,
                                              HYZNS_NAND_ERASE);
            if (lat > max_lat) max_lat = lat;
        }
    }
    req->reqlat      = max_lat;
    req->expire_time = cmd_stime + max_lat;
}

/* Charge zone-finish latency: pad the unwritten portion [wp, zone_end)
 * with NAND writes. Reuses hyzns_charge_latency, which iterates pages,
 * advances per-(ch,lun) timing, and sets req->expire_time = stime + max.
 * No-op if zone is already empty or full.
 */
static void hyzns_charge_zone_finish(HyZNSSSD *h, NvmeRequest *req,
                                      NvmeNamespace *ns, HyZNSZone *z)
{
    uint64_t zone_end  = z->d.zslba + z->d.zcap;
    uint64_t pad_start = z->w_ptr;
    if (pad_start >= zone_end) return;     /* already full or invalid */
    uint64_t pad_lbas  = zone_end - pad_start;
    if (pad_lbas == z->d.zcap) return;     /* empty zone — no real pad needed */
    hyzns_charge_latency(h, req, ns, pad_start, pad_lbas, /*is_write=*/true);
}

static uint16_t hyzns_zone_mgmt_send(FemuCtrl *n, NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
    NvmeNamespace *ns = req->ns;
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint8_t  action = dw13 & 0xff;

#ifdef LOG_MODE
    femu_log("hyzns_zone_mgmt_send ");
    if (action == HYZNS_ZA_SET_R_END) {
        femu_log(": HYZNS_ZA_SET_R_END\n");
    } else if (action == HYZNS_ZA_R_BLOCK_ERASE) {
        femu_log(": HYZNS_ZA_R_BLOCK_ERASE\n");
    } else if (action == HYZNS_ZA_RESET) {
        femu_log(": HYZNS_ZA_RESET\n");
    } else if (action == HYZNS_ZA_OPEN) {
        femu_log(": HYZNS_ZA_OPEN\n");
    } else if (action == HYZNS_ZA_CLOSE) {
        femu_log(": HYZNS_ZA_CLOSE\n");
    } else if (action == HYZNS_ZA_FINISH) {
        femu_log(": HYZNS_ZA_FINISH\n");
    }
#endif

    /* slba decoded up front so SET_R_END / R_*_ERASE branches share it.
     * Wire convention: every ZSA carries its target LBA in the standard
     * SLBA field, including SET_R_END (= new r_end LBA). */
    uint64_t slba = ((uint64_t)le32_to_cpu(cmd->cdw11) << 32)
                     | le32_to_cpu(cmd->cdw10);

    if (action == HYZNS_ZA_SET_R_END) {
        return hyzns_set_r_end(n, slba);
    }

    /* R-region line erase (the hot GC path). dm-hyzns issues
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
    if (action == HYZNS_ZA_R_LINE_ERASE) {
        if (slba >= h->r_region_end_lba) return NVME_LBA_RANGE | NVME_DNR;
        if (h->line_size == 0 || (slba % h->line_size) != 0) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        hyzns_charge_zone_reset(h, req);
        return NVME_SUCCESS;
    }

    /* R-region erase-block trigger (debug / legacy path; the hot GC path
     * is R_LINE_ERASE above). dm-hyzns no longer emits this in normal
     * operation. The device just charges timing —
     * no state to update because R-region has no zone/wp tracking.
     * Backend bytes are intentionally NOT zeroed: R-region accepts
     * in-place overwrite, so a stale read after erase is not meaningful
     * (host FTL invalidates the L2P entry). Skipping memset keeps the
     * simulation latency-only and matches the wp-less semantics
     * documented in hyzns.h. */
    if (action == HYZNS_ZA_R_BLOCK_ERASE) {
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
        hyzns_lba_to_chlun(h, page_idx, &ch, &lun);
        int64_t cmd_stime = req->stime ? req->stime
                                       : qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        int64_t lat = hyzns_advance_nand(h, ch, lun, cmd_stime,
                                          HYZNS_NAND_ERASE);
        req->reqlat      = lat;
        req->expire_time = cmd_stime + lat;
        return NVME_SUCCESS;
    }

    bool select_all = (dw13 >> 8) & 0x1;
    HyZNSZone *z;
    int wcidx = 0;

    if (select_all) {
        /* Apply action to all zones; bail on first error. RESET / FINISH
         * charge per-zone latency so the cumulative timer reflects
         * sequential rounds of erases or pad-writes across (ch, lun). */
        for (uint32_t i = 0; i < h->num_zones; i++) {
            HyZNSZone *zi = &h->zone_array[i];
            uint16_t st = NVME_SUCCESS;
            switch (action) {
            case HYZNS_ZA_RESET:
                hyzns_charge_zone_reset(h, req);
                st = hyzns_zone_reset(zi);
                break;
            case HYZNS_ZA_OPEN:   st = hyzns_zone_open(zi);  break;
            case HYZNS_ZA_CLOSE:  st = hyzns_zone_close(zi); break;
            case HYZNS_ZA_FINISH:
                hyzns_charge_zone_finish(h, req, ns, zi);
                st = hyzns_zone_finish(zi);
                break;
            default: return NVME_INVALID_FIELD | NVME_DNR;
            }
            if (st != NVME_SUCCESS) return st;
        }
        return NVME_SUCCESS;
    }

    z = hyzns_zone_by_slba(h, slba);
    if (!z) return NVME_LBA_RANGE | NVME_DNR;

    switch (action) {
    case HYZNS_ZA_RESET:
#ifdef WRITE_CACHE_ON
        wcidx = hyzns_get_wcidx(h, hyzns_zone_idx(h, slba));
        if (wcidx == -1) {
            hyzns_charge_zone_reset(h, req);
        } else {
            hyzns_invalidate_cache(h, hyzns_zone_idx(h, slba));
        }
#else
    hyzns_charge_zone_reset(h, req);
#endif
        return hyzns_zone_reset(z);
    case HYZNS_ZA_OPEN:   return hyzns_zone_open(z);
    case HYZNS_ZA_CLOSE:  return hyzns_zone_close(z);
    case HYZNS_ZA_FINISH:
        hyzns_charge_zone_finish(h, req, ns, z);
        return hyzns_zone_finish(z);
    default:               return NVME_INVALID_FIELD | NVME_DNR;
    }
}

static uint16_t hyzns_zone_mgmt_recv(FemuCtrl *n, NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);
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
    HyZNSZoneReportHeader hdr;
    uint64_t prp1, prp2;
    uint8_t *buf;
    uint32_t hdr_size = sizeof(hdr);
    uint32_t descr_size = sizeof(HyZNSZoneDescr);
    uint32_t i;
    uint32_t copied = 0;

    (void)partial;

#ifdef LOG_MODE
    femu_log("hyzns_zone_mgmt_recv");
    if (action == HYZNS_ZRA_REPORT_RZONE) {
        femu_log(": HYZNS_ZRA_REPORT_RZONE\n");
    } else {
        femu_log(": HYZNS_ZRA_REPORT\n");
    }
#endif
    /* Vendor R-zone count report. Kernel BLKREPORTRZONE -> NVMe Zone Mgmt
     * Recv with action 0x21 + 4-byte payload (struct blk_rzone_report). */
    if (action == HYZNS_ZRA_REPORT_RZONE) {
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

    if (action != HYZNS_ZRA_REPORT) return NVME_INVALID_FIELD | NVME_DNR;
    if (buf_len < hdr_size) return NVME_INVALID_FIELD | NVME_DNR;

    start_idx = hyzns_zone_idx(h, slba);
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

static uint16_t hyzns_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                              NvmeRequest *req)
{
    HyZNSSSD *h = to_hyzns(n);

    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE: {
        NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
        uint64_t slba = le64_to_cpu(rw->slba);
        bool is_write = (cmd->opcode == NVME_CMD_WRITE);
        if (hyzns_lba_in_r_region(h, slba)) {
            return is_write ? hyzns_r_write(n, ns, cmd, req)
                            : hyzns_r_read(n, ns, cmd, req);
        }
        return is_write ? hyzns_s_write(n, ns, cmd, req)
                        : hyzns_s_read(n, ns, cmd, req);
    }
    case NVME_CMD_ZONE_MGMT_SEND:
        return hyzns_zone_mgmt_send(n, req);
    case NVME_CMD_ZONE_MGMT_RECV:
        return hyzns_zone_mgmt_recv(n, req);
    case NVME_CMD_ZONE_APPEND:
        return hyzns_zone_append(n, ns, cmd, req);
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

/* Forward decl: hyzns_set_r_end calls this after updating r_region_end_lba.
 * Definition lives with the init helpers further down so init can reuse it. */
static void hyzns_classify_zones(HyZNSSSD *h);

/* True if [lo, hi) covers any zone whose state isn't EMPTY. */
static bool hyzns_range_has_nonempty_zone(HyZNSSSD *h, uint64_t lo, uint64_t hi)
{
    if (lo >= hi) return false;
    uint32_t first = hyzns_zone_idx(h, lo);
    /* hi is exclusive; the last zone we care about ends at hi - 1. */
    uint32_t last  = hyzns_zone_idx(h, hi - 1);
    if (last >= h->num_zones) last = h->num_zones - 1;
    for (uint32_t i = first; i <= last; i++) {
        if (hyzns_zone_state(&h->zone_array[i]) != HYZNS_ZS_EMPTY) {
            return true;
        }
    }
    return false;
}

uint16_t hyzns_set_r_end(FemuCtrl *n, uint64_t new_r_end_lba)
{
    HyZNSSSD *h = to_hyzns(n);

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

    if (hyzns_range_has_nonempty_zone(h, lo, hi)) {
        return NVME_ZONE_INVAL_TRANSITION | NVME_DNR;
    }

    h->r_region_end_lba = new_r_end_lba;
    hyzns_classify_zones(h);
    femu_log("hyzns: r_region_end_lba %lu -> %lu (zone %u -> %u)\n",
             old_r_end, new_r_end_lba,
             (uint32_t)(old_r_end / h->zone_size),
             (uint32_t)(new_r_end_lba / h->zone_size));
    return NVME_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Init / exit
 * ------------------------------------------------------------------------- */

/* Requires hyzns_init_timing() to have run first (uses h->nchs / luns_per_ch
 * / pgs_per_blk / page_bytes to derive line_size). */
static void hyzns_init_zones(FemuCtrl *n, HyZNSSSD *h, NvmeNamespace *ns)
{
    uint64_t nsze     = le64_to_cpu(ns->id_ns.nsze);
    uint64_t lba_sz   = NVME_ID_NS_LBADS_BYTES(ns);   /* bytes per LBA */
    int      zsize_mb = n->hyzns_params.zone_size_mb;
    uint64_t zone_bytes;

    /* zone_size is expressed in LBAs, not bytes. Prop is in MiB; 0 => 1 GiB. */
    zone_bytes = zsize_mb > 0 ? (uint64_t)zsize_mb << 20
                              : HYZNS_DEFAULT_ZONE_SIZE_BYTES;
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
        femu_log("hyzns: WARNING zone_size %lu not a multiple of line_size "
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

    h->zone_array = g_malloc0(sizeof(HyZNSZone) * h->num_zones);
    for (uint32_t i = 0; i < h->num_zones; i++) {
        HyZNSZone *z = &h->zone_array[i];
        z->d.zcap  = h->zone_size;        /* MVP: capacity == size */
        z->d.zslba = (uint64_t)i * h->zone_size;
        z->d.wp    = z->d.zslba;
        z->w_ptr   = z->d.zslba;
        hyzns_zone_set_state(z, HYZNS_ZS_EMPTY);
    }
}

/* Tag each zone as random or sequential by region (internal flag only —
 * see header comment for the wire-level SEQ-only convention).
 *
 * Idempotent: called once at init and again from hyzns_set_r_end after the
 * boundary moves. The set_r_end EMPTY-only invariant means zones whose tag
 * is changing are guaranteed empty (wp == zslba, state == EMPTY), so no
 * write-pointer or state fixup is needed here.
 */
static void hyzns_classify_zones(HyZNSSSD *h)
{
    for (uint32_t i = 0; i < h->num_zones; i++) {
        HyZNSZone *z = &h->zone_array[i];
        /* All zones report SEQ_WRITE on the wire — the 5.15 NVMe driver
         * rejects any other type (zns.c:158) and tears down the namespace.
         * R-region status is tracked by the device-internal is_random flag
         * and surfaced to userspace through Zone Mgmt Recv action 0x21. */
        z->d.zt      = HYZNS_ZONE_TYPE_SEQ_WRITE;
        z->is_random = (z->d.zslba < h->r_region_end_lba);
    }
}

/* Populate the CSI=ZONED identify response so the kernel sees /dev/nvme0n1
 * as a zoned namespace. After this point, the standard zoned-block layer
 * (blkzone, REQ_OP_ZONE_*, ZenFS, ...) accepts the device. Mirrors
 * zns_init_zone_identify() but reads geometry from HyZNSSSD instead of the
 * ZNS-mode globals. lba_index is the LBA format index, conventionally 0.
 */
static void hyzns_init_zone_identify(FemuCtrl *n, NvmeNamespace *ns,
                                      int lba_index)
{
    HyZNSSSD *h = to_hyzns(n);
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
     * division in hyzns_init_zones may have rounded down; expose the
     * post-rounding value to the host so all reported zones are addressable. */
    ns->id_ns.nsze = cpu_to_le64((uint64_t)h->num_zones * h->zone_size);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.ncap;
}

/* Initialize the channel/LUN timing model. NAND geometry comes from the
 * device props (hyzns_nchs / hyzns_luns_per_ch / hyzns_page_bytes /
 * hyzns_pgs_per_blk); each falls back to its compile-time default when the
 * prop is left at 0. Latencies still use compile-time defaults. Must run
 * before hyzns_init_zones so the geometry is available to compute
 * line_size. */
static void hyzns_init_timing(FemuCtrl *n, HyZNSSSD *h)
{
    HyZNSCtrlParams *p = &n->hyzns_params;

    h->nchs        = p->nchs        > 0 ? (uint32_t)p->nchs        : HYZNS_DEFAULT_NCHS;
    h->luns_per_ch = p->luns_per_ch > 0 ? (uint32_t)p->luns_per_ch : HYZNS_DEFAULT_LUNS_PER_CH;
    h->page_bytes  = p->page_bytes  > 0 ? (uint32_t)p->page_bytes  : HYZNS_DEFAULT_PAGE_BYTES;
    h->pgs_per_blk = p->pgs_per_blk > 0 ? (uint32_t)p->pgs_per_blk : HYZNS_DEFAULT_PGS_PER_BLK;
    h->pg_rd_lat_ns   = HYZNS_PG_RD_LAT_NS;
    h->pg_wr_lat_ns   = HYZNS_PG_WR_LAT_NS;
    h->pg_wr_lat_r_ns = HYZNS_PG_WR_LAT_R_NS;
    h->blk_er_lat_ns  = HYZNS_BLK_ER_LAT_NS;
    h->ch_xfer_lat_ns = HYZNS_CH_XFER_LAT_NS;

    h->ch_next_avail_ns  = g_malloc0(sizeof(int64_t) * h->nchs);
    h->lun_next_avail_ns = g_malloc0(sizeof(int64_t) * h->nchs * h->luns_per_ch);
    pthread_mutex_init(&h->timing_mutex, NULL);
}

static void hyzns_exit_timing(HyZNSSSD *h)
{
    g_free(h->ch_next_avail_ns);
    h->ch_next_avail_ns = NULL;
    g_free(h->lun_next_avail_ns);
    h->lun_next_avail_ns = NULL;
    pthread_mutex_destroy(&h->timing_mutex);
}

static void hyzns_init(FemuCtrl *n, Error **errp)
{
    HyZNSSSD *h = to_hyzns(n);
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
    hyzns_init_timing(n, h);
    hyzns_init_zones(n, h, ns);
    hyzns_classify_zones(h);
    hyzns_init_zone_identify(n, ns, 0);

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

    femu_log("hyzns: init nsze=%lu lba=%uB r_end=%lu zones=%u zone_size=%lu "
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

static void hyzns_exit(FemuCtrl *n)
{
    HyZNSSSD *h = to_hyzns(n);
    hyzns_exit_timing(h);
    g_free(h->zone_array);
    g_free(n->id_ns_zoned);
    n->id_ns_zoned = NULL;
    g_free(h);
    n->ext_ops.state = NULL;
}

/* -------------------------------------------------------------------------
 * Registration
 * ------------------------------------------------------------------------- */

int nvme_register_hyzns(FemuCtrl *n)
{
    HyZNSSSD *h = g_malloc0(sizeof(*h));

    n->ext_ops = (FemuExtCtrlOps) {
        .state            = h,
        .init             = hyzns_init,
        .exit             = hyzns_exit,
        .rw_check_req     = NULL,
        .start_ctrl       = NULL,
        .admin_cmd        = NULL,
        .io_cmd           = hyzns_io_cmd,
        .get_log          = NULL,
    };
    return 0;
}
