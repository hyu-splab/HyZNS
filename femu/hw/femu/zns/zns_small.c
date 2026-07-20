#include "./zns_small.h"

#define LOGICAL_PAGE_SIZE (4 * KiB)
#define MIN_DISCARD_GRANULARITY     (4 * KiB)
#define NVME_DEFAULT_ZONE_CAP_SIZE      (64 * MiB)
#define NVME_DEFAULT_ZONE_SIZE      (64 * MiB)
#define NVME_DEFAULT_MAX_AZ_SIZE    (16 * KiB)
#define NVME_ZNS_MAX_ACTIVE_ZONES   384
#define NVME_ZNS_MAX_OPEN_ZONES     384

static int base_arr[] = {0, 2, 4, 6, 8, 10, 12, 14};

static void *zns_ftl_thread(void *arg);

static void get_timestamp(char *timestamp, size_t size) {
    int64_t cur_time = qemu_clock_get_ns(QEMU_CLOCK_HOST);  // current system time (ns)

    struct timespec ts;
    ts.tv_sec = cur_time / 1000000000;  // convert ns to seconds
    ts.tv_nsec = cur_time % 1000000000;  // remainder in ns

    // convert to struct tm
    struct tm *tm_info = localtime(&ts.tv_sec);

    // format as human-readable time
    strftime(timestamp, size, "%Y-%m-%d %H:%M:%S", tm_info);

    // append nanoseconds
    snprintf(timestamp + strlen(timestamp), size - strlen(timestamp), ".%03ld", ts.tv_nsec);
}

static inline uint32_t zns_zone_idx(NvmeNamespace *ns, uint64_t slba)
{
    FemuCtrl *n = ns->ctrl;
    return (n->zone_size_log2 > 0 ? slba >> n->zone_size_log2 : slba / n->zone_size);
}

static inline NvmeZone *zns_get_zone_by_slba(NvmeNamespace *ns, uint64_t slba)
{
    FemuCtrl *n = ns->ctrl;
    uint32_t zone_idx = zns_zone_idx(ns, slba);

    assert(zone_idx < n->num_zones);
    return &n->zone_array[zone_idx];
}

static inline struct zns_ch *get_ch(struct zns *zns, struct ppa *ppa)
{
    return &(zns->ch[ppa->g.ch]);
}

static inline struct zns_lun *get_lun(struct zns *zns, struct ppa *ppa)
{
    struct zns_ch *ch = get_ch(zns, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static int zns_init_zone_geometry(NvmeNamespace *ns, Error **errp)
{
    FemuCtrl *n = ns->ctrl;
    uint64_t zone_size, zone_cap;
    uint32_t lbasz = 1 << zns_ns_lbads(ns);

    if (n->zone_size_bs) {
        zone_size = n->zone_size_bs;
    } else {
        zone_size = NVME_DEFAULT_ZONE_SIZE;
    }

    if (n->zone_cap_bs) {
        zone_cap = n->zone_cap_bs;
    } else {
        zone_cap = zone_size;
    }

    if (zone_cap > zone_size) {
        femu_err("zone capacity %luB > zone size %luB", zone_cap, zone_size);
        return -1;
    }
    if (zone_size < lbasz) {
        femu_err("zone size %luB too small, must >= %uB", zone_size, lbasz);
        return -1;
    }
    if (zone_cap < lbasz) {
        femu_err("zone capacity %luB too small, must >= %uB", zone_cap, lbasz);
        return -1;
    }

    n->zone_size = zone_size / lbasz; // 256MB / 512B = 524288
    n->zone_capacity = zone_cap / lbasz; // 256MB / 512B = 524288
    n->num_zones = ns->size / lbasz / n->zone_size; // 64GB / 512 / 524288 = 256

    if (n->max_open_zones > n->num_zones) {
        femu_err("max_open_zones value %u exceeds the number of zones %u",
                 n->max_open_zones, n->num_zones);
        return -1;
    }
    if (n->max_active_zones > n->num_zones) {
        femu_err("max_active_zones value %u exceeds the number of zones %u",
                 n->max_active_zones, n->num_zones);
        return -1;
    }

    if (n->zd_extension_size) {
        if (n->zd_extension_size & 0x3f) {
            femu_err("zone descriptor extension size must be multiples of 64B");
            return -1;
        }
        if ((n->zd_extension_size >> 6) > 0xff) {
            femu_err("zone descriptor extension size is too large");
            return -1;
        }
    }

    return 0;
}

static void zns_init_zoned_state(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    uint64_t start = 0, zone_size = n->zone_size;
    uint64_t capacity = n->num_zones * zone_size;
    NvmeZone *zone;
    int i;

    n->zone_array = g_new0(NvmeZone, n->num_zones);
    if (n->zd_extension_size) {
        n->zd_extensions = g_malloc0(n->zd_extension_size * n->num_zones);
    }

    QTAILQ_INIT(&n->exp_open_zones);
    QTAILQ_INIT(&n->imp_open_zones);
    QTAILQ_INIT(&n->closed_zones);
    QTAILQ_INIT(&n->full_zones);

    zone = n->zone_array;
    for (i = 0; i < n->num_zones; i++, zone++) {
        if (start + zone_size > capacity) {
            zone_size = capacity - start;
        }
        zone->d.zt = NVME_ZONE_TYPE_SEQ_WRITE;
        zns_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
        zone->d.za = 0;
        zone->d.zcap = n->zone_capacity;
        zone->d.zslba = start;
        zone->d.wp = start;
        zone->w_ptr = start;
        start += zone_size;
    }

    n->zone_size_log2 = 0;
    if (is_power_of_2(n->zone_size)) {
        n->zone_size_log2 = 63 - clz64(n->zone_size);
    }
}

static void zns_init_zone_identify(FemuCtrl *n, NvmeNamespace *ns, int lba_index)
{
    NvmeIdNsZoned *id_ns_z;

    zns_init_zoned_state(ns);

    id_ns_z = g_malloc0(sizeof(NvmeIdNsZoned));

    /* MAR/MOR are zeroes-based, 0xffffffff means no limit */
    id_ns_z->mar = cpu_to_le32(n->max_active_zones - 1);
    id_ns_z->mor = cpu_to_le32(n->max_open_zones - 1);
    id_ns_z->zoc = 0;
    id_ns_z->ozcs = n->cross_zone_read ? 0x01 : 0x00;

    id_ns_z->lbafe[lba_index].zsze = cpu_to_le64(n->zone_size);
    id_ns_z->lbafe[lba_index].zdes = n->zd_extension_size >> 6; /* Units of 64B */

    n->csi = NVME_CSI_ZONED;
    ns->id_ns.nsze = cpu_to_le64(n->num_zones * n->zone_size);
    ns->id_ns.ncap = ns->id_ns.nsze;
    ns->id_ns.nuse = ns->id_ns.ncap;

    /* NvmeIdNs */
    /*
     * The device uses the BDRV_BLOCK_ZERO flag to determine the "deallocated"
     * status of logical blocks. Since the spec defines that logical blocks
     * SHALL be deallocated when then zone is in the Empty or Offline states,
     * we can only support DULBE if the zone size is a multiple of the
     * calculated NPDG.
     */
    if (n->zone_size % (ns->id_ns.npdg + 1)) {
        femu_err("the zone size (%"PRIu64" blocks) is not a multiple of the"
                 "calculated deallocation granularity (%"PRIu16" blocks); DULBE"
                 "support disabled", n->zone_size, ns->id_ns.npdg + 1);
        ns->id_ns.nsfeat &= ~0x4;
    }

    n->id_ns_zoned = id_ns_z;
}

// static void zns_clear_zone(NvmeNamespace *ns, NvmeZone *zone)
// {
//     FemuCtrl *n = ns->ctrl;
//     uint8_t state;

//     zone->w_ptr = zone->d.wp;
//     state = zns_get_zone_state(zone);
//     if (zone->d.wp != zone->d.zslba || (zone->d.za & NVME_ZA_ZD_EXT_VALID)) {
//         if (state != NVME_ZONE_STATE_CLOSED) {
//             zns_set_zone_state(zone, NVME_ZONE_STATE_CLOSED);
//         }
//         zns_aor_inc_active(ns);
//         QTAILQ_INSERT_HEAD(&n->closed_zones, zone, entry);
//     } else {
//         zns_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
//     }
// }

// static void zns_zoned_ns_shutdown(NvmeNamespace *ns)
// {
//     FemuCtrl *n = ns->ctrl;
//     NvmeZone *zone, *next;

//     QTAILQ_FOREACH_SAFE(zone, &n->closed_zones, entry, next) {
//         QTAILQ_REMOVE(&n->closed_zones, zone, entry);
//         zns_aor_dec_active(ns);
//         zns_clear_zone(ns, zone);
//     }
//     QTAILQ_FOREACH_SAFE(zone, &n->imp_open_zones, entry, next) {
//         QTAILQ_REMOVE(&n->imp_open_zones, zone, entry);
//         zns_aor_dec_open(ns);
//         zns_aor_dec_active(ns);
//         zns_clear_zone(ns, zone);
//     }
//     QTAILQ_FOREACH_SAFE(zone, &n->exp_open_zones, entry, next) {
//         QTAILQ_REMOVE(&n->exp_open_zones, zone, entry);
//         zns_aor_dec_open(ns);
//         zns_aor_dec_active(ns);
//         zns_clear_zone(ns, zone);
//     }

//     assert(n->nr_open_zones == 0);
// }

// void zns_ns_shutdown(NvmeNamespace *ns)
// {
//     FemuCtrl *n = ns->ctrl;
//     if (n->zoned) {
//         zns_zoned_ns_shutdown(ns);
//     }
// }

// void zns_ns_cleanup(NvmeNamespace *ns)
// {
//     FemuCtrl *n = ns->ctrl;
//     if (n->zoned) {
//         g_free(n->id_ns_zoned);
//         g_free(n->zone_array);
//         g_free(n->zd_extensions);
//     }
// }

static void zns_assign_zone_state(NvmeNamespace *ns, NvmeZone *zone, NvmeZoneState state)
{
    FemuCtrl *n = ns->ctrl;

    if (QTAILQ_IN_USE(zone, entry)) {
        switch (zns_get_zone_state(zone)) {
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            QTAILQ_REMOVE(&n->exp_open_zones, zone, entry);
            break;
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
            QTAILQ_REMOVE(&n->imp_open_zones, zone, entry);
            break;
        case NVME_ZONE_STATE_CLOSED:
            QTAILQ_REMOVE(&n->closed_zones, zone, entry);
            break;
        case NVME_ZONE_STATE_FULL:
            QTAILQ_REMOVE(&n->full_zones, zone, entry);
        default:
            ;
        }
    }

    zns_set_zone_state(zone, state);

    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        QTAILQ_INSERT_TAIL(&n->exp_open_zones, zone, entry);
        break;
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        QTAILQ_INSERT_TAIL(&n->imp_open_zones, zone, entry);
        break;
    case NVME_ZONE_STATE_CLOSED:
        QTAILQ_INSERT_TAIL(&n->closed_zones, zone, entry);
        break;
    case NVME_ZONE_STATE_FULL:
        QTAILQ_INSERT_TAIL(&n->full_zones, zone, entry);
    case NVME_ZONE_STATE_READ_ONLY:
        break;
    default:
        zone->d.za = 0;
    }
}

/*
 * Check if we can open a zone without exceeding open/active limits.
 * AOR stands for "Active and Open Resources" (see TP 4053 section 2.5).
 */
static int zns_aor_check(NvmeNamespace *ns, uint32_t act, uint32_t opn)
{
    FemuCtrl *n = ns->ctrl;
    if (n->max_active_zones != 0 &&
        n->nr_active_zones + act > n->max_active_zones) {
        return NVME_ZONE_TOO_MANY_ACTIVE | NVME_DNR;
    }
    if (n->max_open_zones != 0 &&
        n->nr_open_zones + opn > n->max_open_zones) {
        return NVME_ZONE_TOO_MANY_OPEN | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t zns_check_zone_state_for_write(NvmeZone *zone)
{
    uint16_t status;

    switch (zns_get_zone_state(zone)) {
    case NVME_ZONE_STATE_EMPTY:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_CLOSED:
        status = NVME_SUCCESS;
        break;
    case NVME_ZONE_STATE_FULL:
        status = NVME_ZONE_FULL;
        break;
    case NVME_ZONE_STATE_OFFLINE:
        status = NVME_ZONE_OFFLINE;
        break;
    case NVME_ZONE_STATE_READ_ONLY:
        status = NVME_ZONE_READ_ONLY;
        break;
    default:
        assert(false);
    }

    return status;
}

static uint16_t zns_check_zone_write(FemuCtrl *n, NvmeNamespace *ns,
                                     NvmeZone *zone, uint64_t slba,
                                     uint32_t nlb, bool append)
{
    uint16_t status;

    if (unlikely((slba + nlb) > zns_zone_wr_boundary(zone))) {
        status = NVME_ZONE_BOUNDARY_ERROR;
    } else {
        status = zns_check_zone_state_for_write(zone);
    }

    if (status != NVME_SUCCESS) {
    } else {
        assert(zns_wp_is_valid(zone));
        if (append) {
            if (unlikely(slba != zone->d.zslba)) {
                status = NVME_INVALID_FIELD;
            }
            if (zns_l2b(ns, nlb) > (n->page_size << n->zasl)) {
                status = NVME_INVALID_FIELD;
            }
        } else if (unlikely(slba != zone->w_ptr)) {
            status = NVME_ZONE_INVALID_WRITE;
        }
    }

    return status;
}

static uint16_t zns_check_zone_state_for_read(NvmeZone *zone)
{
    uint16_t status;

    switch (zns_get_zone_state(zone)) {
    case NVME_ZONE_STATE_EMPTY:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_FULL:
    case NVME_ZONE_STATE_CLOSED:
    case NVME_ZONE_STATE_READ_ONLY:
        status = NVME_SUCCESS;
        break;
    case NVME_ZONE_STATE_OFFLINE:
        status = NVME_ZONE_OFFLINE;
        break;
    default:
        assert(false);
    }

    return status;
}

static uint16_t zns_check_zone_read(NvmeNamespace *ns, uint64_t slba, uint32_t nlb)
{
    FemuCtrl *n = ns->ctrl;
    NvmeZone *zone = zns_get_zone_by_slba(ns, slba);
    uint64_t bndry = zns_zone_rd_boundary(ns, zone);
    uint64_t end = slba + nlb;
    uint16_t status;

    status = zns_check_zone_state_for_read(zone);
    if (status != NVME_SUCCESS) {
        ;
    } else if (unlikely(end > bndry)) {
        if (!n->cross_zone_read) {
            status = NVME_ZONE_BOUNDARY_ERROR;
        } else {
            /*
             * Read across zone boundary - check that all subsequent
             * zones that are being read have an appropriate state.
             */
            do {
                zone++;
                status = zns_check_zone_state_for_read(zone);
                if (status != NVME_SUCCESS) {
                    break;
                }
            } while (end > zns_zone_rd_boundary(ns, zone));
        }
    }

    return status;
}

static void zns_auto_transition_zone(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    NvmeZone *zone;

    if (n->max_open_zones &&
        n->nr_open_zones == n->max_open_zones) {
        zone = QTAILQ_FIRST(&n->imp_open_zones);
        if (zone) {
             /* Automatically close this implicitly open zone */
            QTAILQ_REMOVE(&n->imp_open_zones, zone, entry);
            zns_aor_dec_open(ns);
            zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_CLOSED);
        }
    }
}

static uint16_t zns_auto_open_zone(NvmeNamespace *ns, NvmeZone *zone)
{
    uint16_t status = NVME_SUCCESS;
    uint8_t zs = zns_get_zone_state(zone);

    if (zs == NVME_ZONE_STATE_EMPTY) {
        zns_auto_transition_zone(ns);
        status = zns_aor_check(ns, 1, 1);
    } else if (zs == NVME_ZONE_STATE_CLOSED) {
        zns_auto_transition_zone(ns);
        status = zns_aor_check(ns, 0, 1);
    }

    return status;
}

static void zns_finalize_zoned_write(NvmeNamespace *ns, NvmeRequest *req, bool failed)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeZone *zone;
    NvmeZonedResult *res = (NvmeZonedResult *)&req->cqe;
    uint64_t slba;
    uint32_t nlb;

    slba = le64_to_cpu(rw->slba);
    nlb = le16_to_cpu(rw->nlb) + 1;
    zone = zns_get_zone_by_slba(ns, slba);

    zone->d.wp += nlb;

    if (failed) {
        res->slba = 0;
    }

    if (zone->d.wp == zns_zone_wr_boundary(zone)) {
        switch (zns_get_zone_state(zone)) {
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            zns_aor_dec_open(ns);
            /* fall through */
        case NVME_ZONE_STATE_CLOSED:
            zns_aor_dec_active(ns);
            /* fall through */
        case NVME_ZONE_STATE_EMPTY:
            zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_FULL);
            /* fall through */
        case NVME_ZONE_STATE_FULL:
            break;
        default:
            assert(false);
        }
    }
}

// Add some function
// --------------------------------
// static inline struct zns_blk *get_blk(struct zns_ssd *zns, struct ppa *ppa)
// {
//     struct zns_fc *fc = get_fc(zns, ppa);
//     return &(fc->blk[ppa->g.blk]);
// }

static inline uint64_t zone_slba(FemuCtrl *n, uint32_t zone_idx)
{
    return (zone_idx) * n->zone_size;
}

static inline void check_addr(int a, int max)
{
   assert(a >= 0 && a < max);
}

// static struct ppa lba_to_ppa(NvmeNamespace *ns, uint64_t lba)
// {

// }

static struct ppa lpn_to_ppa(NvmeNamespace *ns, uint64_t lpn)
{
    FemuCtrl *n = ns->ctrl;
    struct zns *zns = n->zns;
    uint64_t nchs = zns->sp.nchs;
    uint64_t nluns = zns->sp.luns_per_ch;

    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = lpn % nchs;
    ppa.g.lun = (lpn / nchs) % nluns;
    
    return ppa;
}

static uint64_t zns_advance_status2(struct zns *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct znsparams *spp = &ssd->sp;
    struct zns_ch *ch = get_ch(ssd, ppa);
    struct zns_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;
    uint64_t chnl_stime = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
  
        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;
        lat = ch->next_ch_avail_time - cmd_stime;
        break;
    case NAND_WRITE:
        /* write: transfer data through channel first */
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                    ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
        break;
    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
        break;
    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

static uint64_t zns_advance_zone_wp(NvmeNamespace *ns, NvmeZone *zone, uint32_t nlb)
{
    uint64_t result = zone->w_ptr;
    uint8_t zs;

    zone->w_ptr += nlb;

    if (zone->w_ptr < zns_zone_wr_boundary(zone)) {
        zs = zns_get_zone_state(zone);
        switch (zs) {
        case NVME_ZONE_STATE_EMPTY:
            zns_aor_inc_active(ns);
            /* fall through */
        case NVME_ZONE_STATE_CLOSED:
            zns_aor_inc_open(ns);
            zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_IMPLICITLY_OPEN);
        }
    }

    return result;
}

struct zns_zone_reset_ctx {
    NvmeRequest *req;
    NvmeZone    *zone;
};

static void zns_aio_zone_reset_cb(NvmeRequest *req, NvmeZone *zone)
{
    NvmeNamespace *ns = req->ns;

    /* FIXME, We always assume reset SUCCESS */
    switch (zns_get_zone_state(zone)) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        zns_aor_dec_open(ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        zns_aor_dec_active(ns);
        /* fall through */
    case NVME_ZONE_STATE_FULL:
        zone->w_ptr = zone->d.zslba;
        zone->d.wp = zone->w_ptr;
        zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_EMPTY);
    default:
        break;
    }
}

typedef uint16_t (*op_handler_t)(NvmeNamespace *, NvmeZone *, NvmeZoneState,
                                 NvmeRequest *);

enum NvmeZoneProcessingMask {
    NVME_PROC_CURRENT_ZONE    = 0,
    NVME_PROC_OPENED_ZONES    = 1 << 0,
    NVME_PROC_CLOSED_ZONES    = 1 << 1,
    NVME_PROC_READ_ONLY_ZONES = 1 << 2,
    NVME_PROC_FULL_ZONES      = 1 << 3,
};

static uint16_t zns_open_zone(NvmeNamespace *ns, NvmeZone *zone,
                              NvmeZoneState state, NvmeRequest *req)
{
    uint16_t status;

    switch (state) {
    case NVME_ZONE_STATE_EMPTY:
        status = zns_aor_check(ns, 1, 0);
        if (status != NVME_SUCCESS) {
            return status;
        }
        zns_aor_inc_active(ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        status = zns_aor_check(ns, 0, 1);
        if (status != NVME_SUCCESS) {
            if (state == NVME_ZONE_STATE_EMPTY) {
                zns_aor_dec_active(ns);
            }
            return status;
        }
        zns_aor_inc_open(ns);
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_EXPLICITLY_OPEN);
        /* fall through */
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t zns_close_zone(NvmeNamespace *ns, NvmeZone *zone,
                               NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        zns_aor_dec_open(ns);
        zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_CLOSED);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t zns_finish_zone(NvmeNamespace *ns, NvmeZone *zone,
                                NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        zns_aor_dec_open(ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        zns_aor_dec_active(ns);
        /* fall through */
    case NVME_ZONE_STATE_EMPTY:
        zone->w_ptr = zns_zone_wr_boundary(zone);
        zone->d.wp = zone->w_ptr;
        zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_FULL);
        /* fall through */
    case NVME_ZONE_STATE_FULL:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t zns_reset_zone(NvmeNamespace *ns, NvmeZone *zone,
                               NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_EMPTY:
        return NVME_SUCCESS;
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
    case NVME_ZONE_STATE_CLOSED:
    case NVME_ZONE_STATE_FULL:
        break;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }

    zns_aio_zone_reset_cb(req, zone);

    return NVME_SUCCESS;
}

static uint16_t zns_offline_zone(NvmeNamespace *ns, NvmeZone *zone,
                                 NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_READ_ONLY:
        zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_OFFLINE);
        /* fall through */
    case NVME_ZONE_STATE_OFFLINE:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t zns_set_zd_ext(NvmeNamespace *ns, NvmeZone *zone)
{
    uint16_t status;
    uint8_t state = zns_get_zone_state(zone);

    if (state == NVME_ZONE_STATE_EMPTY) {
        status = zns_aor_check(ns, 1, 0);
        if (status != NVME_SUCCESS) {
            return status;
        }
        zns_aor_inc_active(ns);
        zone->d.za |= NVME_ZA_ZD_EXT_VALID;
        zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_CLOSED);
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static uint16_t zns_bulk_proc_zone(NvmeNamespace *ns, NvmeZone *zone,
                                   enum NvmeZoneProcessingMask proc_mask,
                                   op_handler_t op_hndlr, NvmeRequest *req)
{
    uint16_t status = NVME_SUCCESS;
    NvmeZoneState zs = zns_get_zone_state(zone);
    bool proc_zone;

    switch (zs) {
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        proc_zone = proc_mask & NVME_PROC_OPENED_ZONES;
        break;
    case NVME_ZONE_STATE_CLOSED:
        proc_zone = proc_mask & NVME_PROC_CLOSED_ZONES;
        break;
    case NVME_ZONE_STATE_READ_ONLY:
        proc_zone = proc_mask & NVME_PROC_READ_ONLY_ZONES;
        break;
    case NVME_ZONE_STATE_FULL:
        proc_zone = proc_mask & NVME_PROC_FULL_ZONES;
        break;
    default:
        proc_zone = false;
    }

    if (proc_zone) {
        status = op_hndlr(ns, zone, zs, req);
    }

    return status;
}

static uint16_t zns_do_zone_op(NvmeNamespace *ns, NvmeZone *zone,
                               enum NvmeZoneProcessingMask proc_mask,
                               op_handler_t op_hndlr, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeZone *next;
    uint16_t status = NVME_SUCCESS;
    int i;

    if (!proc_mask) {
        status = op_hndlr(ns, zone, zns_get_zone_state(zone), req);
    } else {
        if (proc_mask & NVME_PROC_CLOSED_ZONES) {
            QTAILQ_FOREACH_SAFE(zone, &n->closed_zones, entry, next) {
                status = zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr, req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }
        if (proc_mask & NVME_PROC_OPENED_ZONES) {
            QTAILQ_FOREACH_SAFE(zone, &n->imp_open_zones, entry, next) {
                status = zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }

            QTAILQ_FOREACH_SAFE(zone, &n->exp_open_zones, entry, next) {
                status = zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }
        if (proc_mask & NVME_PROC_FULL_ZONES) {
            QTAILQ_FOREACH_SAFE(zone, &n->full_zones, entry, next) {
                status = zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr, req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }

        if (proc_mask & NVME_PROC_READ_ONLY_ZONES) {
            for (i = 0; i < n->num_zones; i++, zone++) {
                status = zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }
    }

out:
    return status;
}

static uint16_t zns_get_mgmt_zone_slba_idx(FemuCtrl *n, NvmeCmd *c,
                                           uint64_t *slba, uint32_t *zone_idx)
{
    NvmeNamespace *ns = &n->namespaces[0];
    uint32_t dw10 = le32_to_cpu(c->cdw10);
    uint32_t dw11 = le32_to_cpu(c->cdw11);

    if (!n->zoned) {
        return NVME_INVALID_OPCODE | NVME_DNR;
    }

    *slba = ((uint64_t)dw11) << 32 | dw10;
    if (unlikely(*slba >= ns->id_ns.nsze)) {
        *slba = 0;
        return NVME_LBA_RANGE | NVME_DNR;
    }

    *zone_idx = zns_zone_idx(ns, *slba);
    assert(*zone_idx < n->num_zones);

    return NVME_SUCCESS;
}

static uint16_t zns_zone_mgmt_send(FemuCtrl *n, NvmeRequest *req)
{
    // fprintf(stdout, "zns_zone_mgmt_send\n");
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    NvmeZone *zone;
    uintptr_t *resets;
    uint8_t *zd_ext;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint64_t slba = 0;
    uint32_t zone_idx = 0;
    uint16_t status;
    uint8_t action;
    bool all;
    enum NvmeZoneProcessingMask proc_mask = NVME_PROC_CURRENT_ZONE;
    
    uint64_t curlat = 0, maxlat = 0;
    uint32_t lpn = 0, slpn = 0, elpn = 0;
    struct zns *zns = n->zns;
    uint32_t nchs = zns->sp.nchs;
    uint32_t nluns = zns->sp.luns_per_ch;
    struct ppa ppa;
    struct nand_cmd swr;
    uint32_t lpns_per_zone = n->zone_size / 8;
    int tag = 0;

    action = dw13 & 0xff;
    all = dw13 & 0x100;
    
    req->status = NVME_SUCCESS;

    if (!all) {
        status = zns_get_mgmt_zone_slba_idx(n, cmd, &slba, &zone_idx);
        if (status) {
            return status;
        }
    }

    tag = zone_idx % 8;

    zone = &n->zone_array[zone_idx];
    if (slba != zone->d.zslba) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    switch (action) {
    case NVME_ZONE_ACTION_OPEN:
        if (all) {
            proc_mask = NVME_PROC_CLOSED_ZONES;
        }
        status = zns_do_zone_op(ns, zone, proc_mask, zns_open_zone, req);
        break;
    case NVME_ZONE_ACTION_CLOSE:
        if (all) {
            proc_mask = NVME_PROC_OPENED_ZONES;
        }
        status = zns_do_zone_op(ns, zone, proc_mask, zns_close_zone, req);
        break;
    case NVME_ZONE_ACTION_FINISH:
        if (all) {
            proc_mask = NVME_PROC_OPENED_ZONES | NVME_PROC_CLOSED_ZONES;
        }

        zns_get_mgmt_zone_slba_idx(n, cmd, &slba, &zone_idx);
        zone = &n->zone_array[zone_idx];
        uint64_t lbas_to_fill = n->zone_capacity - (zone->w_ptr - zone->d.zslba);

        // empty or full state
        if (lbas_to_fill == 0 || lbas_to_fill == n->zone_capacity) {
            break;
        }

        slba = zone->w_ptr;
        slpn = slba / zns->sp.secs_per_pg;
        elpn = (slba + lbas_to_fill - 1) / zns->sp.secs_per_pg;
        
        for (lpn = slpn; lpn <= elpn; lpn++) {
            // ppa.g.ch = zone_idx % nchs;
            // ppa.g.lun = (zone_idx / nchs) % nluns;

            // 1ch/8way
            // ppa.g.ch = zone_idx % nchs;
            // ppa.g.lun = (lpn % lpns_per_zone) % nluns;

            // 16ch/1way
            // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
            // ppa.g.lun = zone_idx % nluns;

            // 16ch/2way
            // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
            // ppa.g.lun = base + ((lpn / nchs) % 2);
            
            // 2ch/8way
            ppa.g.ch = base_arr[tag] + (lpn % 2);
            ppa.g.lun = (lpn / 2) % nluns;

            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;

            curlat = zns_advance_status2(zns, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }

        req->reqlat = maxlat;
        req->expire_time += maxlat;

        status = zns_do_zone_op(ns, zone, proc_mask, zns_finish_zone, req);
        break;
    case NVME_ZONE_ACTION_RESET:
        resets = (uintptr_t *)&req->opaque;

        if (all) {
            proc_mask = NVME_PROC_OPENED_ZONES | NVME_PROC_CLOSED_ZONES |
                NVME_PROC_FULL_ZONES;
        }
        *resets = 1;

        uint32_t blks_per_zone = NVME_DEFAULT_ZONE_SIZE / (2 * 1024 * 1024);
        uint64_t curlat = 0;
        uint64_t lpn = 0;
        // slpn = slba / zns->sp.secs_per_pg;
        // elpn = (slba + n->zone_size - 1) / zns->sp.secs_per_pg;

        for (lpn = 0; lpn < blks_per_zone; lpn++) {
            // ppa.g.ch = zone_idx % nchs;
            // ppa.g.lun = (zone_idx / nchs) % nluns;
            
            // 1ch/8way
            // ppa.g.ch = zone_idx % nchs;
            // ppa.g.lun = (lpn % lpns_per_zone) % nluns;

            // 16ch/1way
            // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
            // ppa.g.lun = zone_idx % nluns;

            // 16ch/2way
            // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
            // ppa.g.lun = base + ((lpn / nchs) % 2);

            // 2ch/8way
            ppa.g.ch = base_arr[tag] + (lpn % 2);
            ppa.g.lun = (lpn / 2) % nluns;

            swr.type = USER_IO;
            swr.cmd = NAND_ERASE;
            swr.stime = req->stime;

            curlat = zns_advance_status2(n->zns, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }

        req->reqlat = maxlat;
        req->expire_time += maxlat;
        status = zns_do_zone_op(ns, zone, proc_mask, zns_reset_zone, req);
        
        // zone reset counts
        n->zns->sp.zr_count++;

        (*resets)--;
        fprintf(stdout, "ZNS RESET SUCCESS [slba : %ld]\n", slba);
        return NVME_SUCCESS;
    case NVME_ZONE_ACTION_OFFLINE:
        if (all) {
            proc_mask = NVME_PROC_READ_ONLY_ZONES;
        }
        status = zns_do_zone_op(ns, zone, proc_mask, zns_offline_zone, req);
        break;
    case NVME_ZONE_ACTION_SET_ZD_EXT:
        if (all || !n->zd_extension_size) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        zd_ext = zns_get_zd_extension(ns, zone_idx);
        status = dma_write_prp(n, (uint8_t *)zd_ext, n->zd_extension_size, prp1,
                               prp2);
        if (status) {
            return status;
        }
        status = zns_set_zd_ext(ns, zone);
        if (status == NVME_SUCCESS) {
            return status;
        }
        break;
    default:
        status = NVME_INVALID_FIELD;
    }

    if (status) {
        status |= NVME_DNR;
    }

    return status;
}

static bool zns_zone_matches_filter(uint32_t zafs, NvmeZone *zl)
{
    NvmeZoneState zs = zns_get_zone_state(zl);

    switch (zafs) {
    case NVME_ZONE_REPORT_ALL:
        return true;
    case NVME_ZONE_REPORT_EMPTY:
        return zs == NVME_ZONE_STATE_EMPTY;
    case NVME_ZONE_REPORT_IMPLICITLY_OPEN:
        return zs == NVME_ZONE_STATE_IMPLICITLY_OPEN;
    case NVME_ZONE_REPORT_EXPLICITLY_OPEN:
        return zs == NVME_ZONE_STATE_EXPLICITLY_OPEN;
    case NVME_ZONE_REPORT_CLOSED:
        return zs == NVME_ZONE_STATE_CLOSED;
    case NVME_ZONE_REPORT_FULL:
        return zs == NVME_ZONE_STATE_FULL;
    case NVME_ZONE_REPORT_READ_ONLY:
        return zs == NVME_ZONE_STATE_READ_ONLY;
    case NVME_ZONE_REPORT_OFFLINE:
        return zs == NVME_ZONE_STATE_OFFLINE;
    default:
        return false;
    }
}

static uint16_t zns_zone_mgmt_recv(FemuCtrl *n, NvmeRequest *req)
{
    // fprintf(stdout, "zns_zone_mgmt_recv\n");
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    /* cdw12 is zero-based number of dwords to return. Convert to bytes */
    uint32_t data_size = (le32_to_cpu(cmd->cdw12) + 1) << 2;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint32_t zone_idx, zra, zrasf, partial;
    uint64_t max_zones, nr_zones = 0;
    uint16_t status;
    uint64_t slba, capacity = zns_ns_nlbas(ns);
    NvmeZoneDescr *z;
    NvmeZone *zone;
    NvmeZoneReportHeader *header;
    void *buf, *buf_p;
    size_t zone_entry_sz;

    req->status = NVME_SUCCESS;

    status = zns_get_mgmt_zone_slba_idx(n, cmd, &slba, &zone_idx);
    if (status) {
        return status;
    }

    zra = dw13 & 0xff;
    if (zra != NVME_ZONE_REPORT && zra != NVME_ZONE_REPORT_EXTENDED) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    if (zra == NVME_ZONE_REPORT_EXTENDED && !n->zd_extension_size) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    zrasf = (dw13 >> 8) & 0xff;
    if (zrasf > NVME_ZONE_REPORT_OFFLINE) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    if (data_size < sizeof(NvmeZoneReportHeader)) {
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    status = nvme_check_mdts(n, data_size);
    if (status) {
        return status;
    }

    partial = (dw13 >> 16) & 0x01;

    zone_entry_sz = sizeof(NvmeZoneDescr);
    if (zra == NVME_ZONE_REPORT_EXTENDED) {
        zone_entry_sz += n->zd_extension_size;
    }

    max_zones = (data_size - sizeof(NvmeZoneReportHeader)) / zone_entry_sz;
    buf = g_malloc0(data_size);

    zone = &n->zone_array[zone_idx];
    for (; slba < capacity; slba += n->zone_size) {
        if (partial && nr_zones >= max_zones) {
            break;
        }
        if (zns_zone_matches_filter(zrasf, zone++)) {
            nr_zones++;
        }
    }
    header = (NvmeZoneReportHeader *)buf;
    header->nr_zones = cpu_to_le64(nr_zones);

    buf_p = buf + sizeof(NvmeZoneReportHeader);
    for (; zone_idx < n->num_zones && max_zones > 0; zone_idx++) {
        zone = &n->zone_array[zone_idx];
        if (zns_zone_matches_filter(zrasf, zone)) {
            z = (NvmeZoneDescr *)buf_p;
            buf_p += sizeof(NvmeZoneDescr);

            z->zt = zone->d.zt;
            z->zs = zone->d.zs;
            z->zcap = cpu_to_le64(zone->d.zcap);
            z->zslba = cpu_to_le64(zone->d.zslba);
            z->za = zone->d.za;

            if (zns_wp_is_valid(zone)) {
                z->wp = cpu_to_le64(zone->d.wp);
            } else {
                z->wp = cpu_to_le64(~0ULL);
            }

            if (zra == NVME_ZONE_REPORT_EXTENDED) {
                if (zone->d.za & NVME_ZA_ZD_EXT_VALID) {
                    memcpy(buf_p, zns_get_zd_extension(ns, zone_idx),
                           n->zd_extension_size);
                }
                buf_p += n->zd_extension_size;
            }

            max_zones--;
        }
    }

    status = dma_read_prp(n, (uint8_t *)buf, data_size, prp1, prp2);

    g_free(buf);

    return status;
}

static inline bool nvme_csi_has_nvm_support(NvmeNamespace *ns)
{
    switch (ns->ctrl->csi) {
    case NVME_CSI_NVM:
    case NVME_CSI_ZONED:
        return true;
    }

    return false;
}

static inline uint16_t zns_check_bounds(NvmeNamespace *ns, uint64_t slba,
                                        uint32_t nlb)
{
    uint64_t nsze = le64_to_cpu(ns->id_ns.nsze);

    if (unlikely(UINT64_MAX - slba < nlb || slba + nlb > nsze)) {
        return NVME_LBA_RANGE | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t zns_map_dptr(FemuCtrl *n, size_t len, NvmeRequest *req)
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

static uint16_t zns_do_write(FemuCtrl *n, NvmeRequest *req, bool append,
                             bool wrz)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = zns_l2b(ns, nlb);
    uint64_t data_offset;
    NvmeZone *zone;
    NvmeZonedResult *res = (NvmeZonedResult *)&req->cqe;
    uint16_t status;

    assert(n->zoned);
    req->is_write = true;

    if (!wrz) {
        status = nvme_check_mdts(n, data_size);
        if (status) {
            goto err;
        }
    }

    status = zns_check_bounds(ns, slba, nlb);
    if (status) {
        goto err;
    }

    zone = zns_get_zone_by_slba(ns, slba);

    status = zns_check_zone_write(n, ns, zone, slba, nlb, append);
    if (status) {
        goto err;
    }

    status = zns_auto_open_zone(ns, zone);
    if (status) {
        goto err;
    }

    if (append) {
        slba = zone->w_ptr;
    }

    res->slba = zns_advance_zone_wp(ns, zone, nlb);

    data_offset = zns_l2b(ns, slba);

    if (!wrz) {
        status = zns_map_dptr(n, data_size, req);
        if (status) {
            goto err;
        }

        backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);
    }

    zns_finalize_zoned_write(ns, req, false);
    return NVME_SUCCESS;

err:
    fprintf(stderr, "****************Append Failed***************\n");
    return status | NVME_DNR;
}

static uint16_t zns_id_dev(FemuCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);

    nvme_device_info dev_data;
    uint16_t status;
    memset(&dev_data, 0, sizeof(nvme_device_info));

    dev_data.type = 1;

    dev_data.znssd.num_ch = n->zns->sp.nchs;
    dev_data.znssd.num_lun = n->zns->sp.luns_per_ch;
    dev_data.znssd.nr_zones = n->num_zones;
    return dma_read_prp(n, (uint8_t *)&dev_data, sizeof(nvme_device_info), prp1, prp2);
}

static uint16_t zns_mon(FemuCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t data_size = (dw10 + 1) << 2;
    
    // compute the total buffer size needed
    size_t required_size = sizeof(uint64_t) * 2 + n->num_zones * sizeof(zns_zone_info);
    if (data_size < required_size) {
        femu_log("zns_mon: WARNING - Requested data size (%u) is less than required (%lu)\n", 
                 data_size, required_size);
    }

    void *buf = g_malloc(data_size);
    if (!buf) {
        femu_log("zns_mon: ERROR - Failed to allocate memory for buffer\n");
        return NVME_INTERNAL_DEV_ERROR;
    }
    
    // zero the buffer
    memset(buf, 0, data_size);
    
    // store zone count
    uint8_t *ptr = (uint8_t *)buf;
    *(uint64_t *)ptr = n->num_zones;
    ptr += sizeof(uint64_t);
    
    // store zr_count
    *(uint64_t *)ptr = n->zns->sp.zr_count;
    ptr += sizeof(uint64_t);
        
    zns_zone_info *zone_infos = (zns_zone_info *)ptr;
    for (int i = 0; i < n->num_zones; i++) {
        NvmeZone *zone = &n->zone_array[i];
        zone_infos[i].d.zslba = zone->d.zslba;
        zone_infos[i].d.wp = zone->d.wp;
        zone_infos[i].d.zcap = zone->d.zcap;
        zone_infos[i].d.zt = zone->d.zt;
        zone_infos[i].d.zs = zone->d.zs;
        zone_infos[i].d.za = zone->d.za;
        
        zone_infos[i].w_ptr = zone->w_ptr;
    }

    uint16_t status = dma_read_prp(n, (uint8_t *)buf, data_size, prp1, prp2);
    g_free(buf);
    return status;
}

static uint16_t zns_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case DEVICE_TYPE_OPCODE:
        return zns_id_dev(n, cmd);
    case NVME_ADMIN_CMD_ZNSSD:
        return zns_mon(n, cmd);
    case NVME_ADM_CMD_FORMAT_NVM:
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static inline uint16_t zns_zone_append(FemuCtrl *n, NvmeRequest *req)
{
    fprintf(stdout, "zns_zone_append\n");
    return 0;
    // return zns_do_write(n, req, true, false);
}

static uint16_t zns_check_dulbe(NvmeNamespace *ns, uint64_t slba, uint32_t nlb)
{
    return NVME_SUCCESS;
}

static uint16_t zns_read(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                         NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = zns_l2b(ns, nlb);
    uint64_t data_offset;
    uint16_t status;

    uint32_t nchs = n->zns_params.num_ch;
    uint32_t nluns = n->zns_params.num_lun;
    uint32_t zone_idx = zns_zone_idx(ns, slba);

    assert(n->zoned);
    req->is_write = false;

    status = nvme_check_mdts(n, data_size);
    if (status) {
        goto err;
    }

    status = zns_check_bounds(ns, slba, nlb);
    if (status) {
        goto err;
    }

    status = zns_check_zone_read(ns, slba, nlb);
    if (status) {
        goto err;
    }

    status = zns_map_dptr(n, data_size, req);
    if (status) {
        goto err;
    }

    if (NVME_ERR_REC_DULBE(n->features.err_rec)) {
        status = zns_check_dulbe(ns, slba, nlb);
        if (status) {
            goto err;
        }
    }

    data_offset = zns_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    struct ppa ppa;
    struct nand_cmd swr;
    uint64_t maxlat = 0;
    uint64_t curlat = 0;
    uint32_t lpn = 0, slpn = 0, elpn = 0;
    uint32_t lpns_per_zone = n->zone_size / n->zns->sp.secs_per_pg;

    slpn = slba / n->zns->sp.secs_per_pg;
    elpn = (slba + nlb - 1) / n->zns->sp.secs_per_pg;

    int tag = zone_idx % 8;

    for (lpn = slpn; lpn <= elpn; lpn++) {
        // ppa.g.ch = zone_idx % nchs;
        // ppa.g.lun = (zone_idx / nchs) % nluns;
        
        // 1ch/8way
        // ppa.g.ch = zone_idx % nchs;
        // ppa.g.lun = (lpn % lpns_per_zone) % nluns;

        // 16ch/1way
        // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
        // ppa.g.lun = zone_idx % nluns;

        // 16ch/2way
        // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
        // ppa.g.lun = base + ((lpn / nchs) % 2);

        // 2ch/8way
        ppa.g.ch = base_arr[tag] + (lpn % 2);
        ppa.g.lun = (lpn / 2) % nluns;

        swr.type = USER_IO;
        swr.cmd = NAND_READ;
        swr.stime = req->stime;

        curlat = zns_advance_status2(n->zns, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    req->reqlat = maxlat;
    req->expire_time += maxlat;

    n->zns_read_cnt++;
    n->zns_read_lat += maxlat; 

    // fprintf(stdout, "ZNS READ SUCCESS [slba : %ld, nlb : %d lat : %lu, acclat : %lu]\n", 
    //                     slba, nlb, maxlat, n->zns_read_lat / n->zns_read_cnt);
    return NVME_SUCCESS;
err:
    fprintf(stdout, "ZSN READ FAILED [slba : %ld, nlb : %d]\n", slba, nlb);
    return status | NVME_DNR;
}

static uint16_t zns_write(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = zns_l2b(ns, nlb);
    uint64_t data_offset;
    NvmeZone *zone;
    NvmeZonedResult *res = (NvmeZonedResult *)&req->cqe;
    uint16_t status;

    uint32_t nchs = n->zns_params.num_ch;
    uint32_t nluns = n->zns_params.num_lun;

    uint32_t ch_idx = 0;
    uint32_t lun_idx = 0;
    uint32_t zone_idx = zns_zone_idx(ns, slba);

    assert(n->zoned);
    req->is_write = true;

    status = nvme_check_mdts(n, data_size);
    if (status) {
        goto err;
    }

    status = zns_check_bounds(ns, slba, nlb);
    if (status) {
        goto err;
    }

    zone = zns_get_zone_by_slba(ns, slba);

    status = zns_check_zone_write(n, ns, zone, slba, nlb, false);
    if (status) {
        goto err;
    }

    status = zns_auto_open_zone(ns, zone);
    if (status) {
        goto err;
    }


    data_offset = zns_l2b(ns, slba);
    status = zns_map_dptr(n, data_size, req);
    if (status) {
        goto err;
    }
    
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);
    
    struct ppa ppa;
    struct nand_cmd swr;
    uint64_t maxlat = 0;
    uint64_t curlat = 0;
    uint32_t lpn = 0, slpn = 0, elpn = 0;
    uint32_t lpns_per_zone = n->zone_size / n->zns->sp.secs_per_pg;

    int tag = zone_idx % 8;

    slpn = slba / n->zns->sp.secs_per_pg;
    elpn = (slba + nlb - 1) / n->zns->sp.secs_per_pg;

    for (lpn = slpn; lpn <= elpn; lpn++) {
        // ppa.g.ch = zone_idx % nchs;
        // ppa.g.lun = (zone_idx / nchs) % nluns;

        // 1ch/8way
        // ppa.g.ch = zone_idx % nchs;
        // ppa.g.lun = (lpn % lpns_per_zone) % nluns;

        // 16ch/1way
        // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
        // ppa.g.lun = zone_idx % nluns;

        // 16ch/2way
        // ppa.g.ch = (lpn % lpns_per_zone) % nchs;
        // ppa.g.lun = base + ((lpn / nchs) % 2);

        // 2ch/8way
        ppa.g.ch = base_arr[tag] + (lpn % 2);
        ppa.g.lun = (lpn / 2) % nluns;

        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;

        curlat = zns_advance_status2(n->zns, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    req->reqlat = maxlat;
    req->expire_time += maxlat;

    n->zns_write_cnt++;
    n->zns_write_lat += maxlat; 

    res->slba = zns_advance_zone_wp(ns, zone, nlb);
    zns_finalize_zoned_write(ns, req, false);

    // fprintf(stdout, "ZNS WRITE SUCCESS [slba : %ld, nlb : %d lat : %lu, acclat : %lu]\n", 
    //                 slba, nlb, maxlat, n->zns_write_lat / n->zns_write_cnt);
    return NVME_SUCCESS;
err:
    femu_err("*********ZONE WRITE FAILED*********\n");
    return status | NVME_DNR;
}

static uint16_t zns_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
        return zns_read(n, ns, cmd, req);
    case NVME_CMD_WRITE:
        return zns_write(n, ns, cmd, req);
    case NVME_CMD_ZONE_MGMT_SEND:
        return zns_zone_mgmt_send(n, req);
    case NVME_CMD_ZONE_MGMT_RECV:
        return zns_zone_mgmt_recv(n, req);
    case NVME_CMD_ZONE_APPEND:
        return zns_zone_append(n, req);
    }

    return NVME_INVALID_OPCODE | NVME_DNR;
}

static void zns_set_ctrl_str(FemuCtrl *n)
{
    static int fsid_zns = 0;
    const char *zns_mn = "FEMU ZNS-SSD Controller";
    const char *zns_sn = "vZNSSD";

    nvme_set_ctrl_name(n, zns_mn, zns_sn, &fsid_zns);
}

static void zns_set_ctrl(FemuCtrl *n)
{
    uint8_t *pci_conf = n->parent_obj.config;

    zns_set_ctrl_str(n);
    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_INTEL);
    pci_config_set_device_id(pci_conf, 0x5845);
}

// Add zns init ch, zns init flash and zns init block
// ----------------------------
static void zns_init_blk(struct zns_blk *blk)
{
    blk->next_blk_avail_time = 0;
}

static void zns_init_lun(struct zns_lun *lun)
{
    lun->blk = g_malloc0(sizeof(struct zns_blk) * 32);
    for (int i = 0; i < 32; i++) {
        zns_init_blk(&lun->blk[i]);
    }
    lun->next_lun_avail_time = 0;
}

static void zns_init_ch(struct zns_ch *ch, uint8_t num_lun)
{
    ch->lun = g_malloc0(sizeof(struct zns_lun) * num_lun);
    for (int i = 0; i < num_lun; i++) {
        zns_init_lun(&ch->lun[i]);
    }
    ch->next_ch_avail_time = 0;
}

static void zns_init_params(znsparams *spp, FemuCtrl *n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; // 512
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 512(# zones) 128 MiB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    spp->zr_count = 0;
    spp->num_erase_blks = 0;
}

static int zns_init_zone_cap(FemuCtrl *n)
{
    n->zoned = true;
    n->zasl_bs = NVME_DEFAULT_MAX_AZ_SIZE;
    n->zone_size_bs = NVME_DEFAULT_ZONE_SIZE;
    n->zone_cap_bs = NVME_DEFAULT_ZONE_CAP_SIZE;
    n->cross_zone_read = false;
    n->max_active_zones = NVME_ZNS_MAX_ACTIVE_ZONES;
    n->max_open_zones = NVME_ZNS_MAX_OPEN_ZONES;
    n->zd_extension_size = 0;

    return 0;
}

static int zns_start_ctrl(FemuCtrl *n)
{
    /* Coperd: let's fail early before anything crazy happens */
    assert(n->page_size == 4096);

    if (!n->zasl_bs) {
        n->zasl = n->mdts;
    } else {
        if (n->zasl_bs < n->page_size) {
            femu_err("ZASL too small (%dB), must >= 1 page (4K)\n", n->zasl_bs);
            return -1;
        }
        n->zasl = 31 - clz32(n->zasl_bs / n->page_size);
    }

    return 0;
}

static void zns_init(FemuCtrl *n, Error **errp)
{
    NvmeNamespace *ns = &n->namespaces[0];

    struct zns *zns = g_malloc0(sizeof(struct zns));
    struct znsparams *spp = &zns->sp;
    n->zns = zns;

    zns_set_ctrl(n);

    zns->dataplane_started_ptr = &n->dataplane_started;

    zns_init_zone_cap(n);

    if (zns_init_zone_geometry(ns, errp) != 0) {
        return;
    }

    zns_init_zone_identify(n, ns, 0);
    zns_init_params(spp, n);
    
    zns->ch = g_malloc0(sizeof(struct zns_ch) * zns->sp.nchs);
    for (int i =0; i < zns->sp.nchs; i++) {
        zns_init_ch(&zns->ch[i], zns->sp.luns_per_ch);
    }
             
    fprintf(stdout, "***Structure***\n");
    fprintf(stdout, "nchs : %u\n", n->zns->sp.nchs);
    fprintf(stdout, "luns_per_ch : %u\n", n->zns->sp.luns_per_ch);
    fprintf(stdout, "pls_per_lun : %u\n", n->zns->sp.pls_per_lun);
    fprintf(stdout, "blks_per_pl : %u\n", n->zns->sp.blks_per_pl);
    fprintf(stdout, "pgs_per_blk : %u\n", n->zns->sp.pgs_per_blk);
    fprintf(stdout, "secs_per_pg : %u\n", n->zns->sp.secs_per_pg);

    fprintf(stdout, "***Latency***\n");
    fprintf(stdout, "pg_rd_lat : %lu\n", n->zns->sp.pg_rd_lat);
    fprintf(stdout, "pg_wr_lat : %lu\n", n->zns->sp.pg_wr_lat);
    fprintf(stdout, "blk_er_lat : %lu\n", n->zns->sp.blk_er_lat);
    fprintf(stdout, "ch_xfer_lat : %lu\n", n->zns->sp.ch_xfer_lat);
}

static void zns_exit(FemuCtrl *n)
{
    /*
     * Release any extra resource (zones) allocated for ZNS mode
     */
}

// int nvme_register_znssd(FemuCtrl *n)
// {
//     n->ext_ops = (FemuExtCtrlOps) {
//         .state            = NULL,
//         .init             = zns_init,
//         .exit             = zns_exit,
//         .rw_check_req     = NULL,
//         .start_ctrl       = zns_start_ctrl,
//         .admin_cmd        = zns_admin_cmd,
//         .io_cmd           = zns_io_cmd,
//         .get_log          = NULL,
//     };

//     return 0;
// }