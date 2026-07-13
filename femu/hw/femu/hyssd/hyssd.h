#ifndef __FEMU_HYZNS_H
#define __FEMU_HYZNS_H

#include "../nvme.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

typedef struct QEMU_PACKED NvmeZonedResult {
    uint64_t slba;
} NvmeZonedResult;

typedef struct NvmeIdCtrlZoned {
    uint8_t     zasl;
    uint8_t     rsvd1[4095];
} NvmeIdCtrlZoned;

enum NvmeZoneAttr {
    NVME_ZA_FINISHED_BY_CTLR         = 1 << 0,
    NVME_ZA_FINISH_RECOMMENDED       = 1 << 1,
    NVME_ZA_RESET_RECOMMENDED        = 1 << 2,
    NVME_ZA_ZD_EXT_VALID             = 1 << 7,
};

typedef struct QEMU_PACKED NvmeZoneReportHeader {
    uint64_t    nr_zones;
    uint8_t     rsvd[56];
} NvmeZoneReportHeader;

// modify zone
struct nvme_rzone_report {
    uint64_t nr_rzones;
};

struct nvme_victim_idx {
    uint32_t old_idx;
    uint32_t new_idx;
};

struct nvme_modify_zone_report {
    uint64_t nr_victims;
    uint8_t victim_type;
    uint8_t rsvd8[7];
    struct nvme_victim_idx  entries[];
};

struct nvme_zns_get_rzone {
	__le64	nr_rzones;
	__u8 	rsvd8[56];
};

enum NvmeZoneReceiveAction {
    NVME_ZONE_REPORT                 = 0x00,
    NVME_ZONE_REPORT_EXTENDED        = 0x01,
    NVME_ZONE_REPORT_VICTIM          = 0x20,
    NVME_ZONE_REPORT_RZONE           = 0x21,
};

enum NvmeZoneReportType {
    NVME_ZONE_REPORT_ALL             = 0,
    NVME_ZONE_REPORT_EMPTY           = 1,
    NVME_ZONE_REPORT_IMPLICITLY_OPEN = 2,
    NVME_ZONE_REPORT_EXPLICITLY_OPEN = 3,
    NVME_ZONE_REPORT_CLOSED          = 4,
    NVME_ZONE_REPORT_FULL            = 5,
    NVME_ZONE_REPORT_READ_ONLY       = 6,
    NVME_ZONE_REPORT_OFFLINE         = 7,
};

enum NvmeZoneType {
    NVME_ZONE_TYPE_RESERVED          = 0x00,
    NVME_ZONE_TYPE_RANDOM_WRITE      = 0x01,
    NVME_ZONE_TYPE_SEQ_WRITE         = 0x02,
};

enum NvmeZoneSendAction {
    NVME_ZONE_ACTION_RSD             = 0x00,
    NVME_ZONE_ACTION_CLOSE           = 0x01,
    NVME_ZONE_ACTION_FINISH          = 0x02,
    NVME_ZONE_ACTION_OPEN            = 0x03,
    NVME_ZONE_ACTION_RESET           = 0x04,
    NVME_ZONE_ACTION_OFFLINE         = 0x05,
    NVME_ZONE_ACTION_SET_ZD_EXT      = 0x10,
    NVME_ZONE_ACTION_MODIFY_ZONE     = 0X20,
};

typedef struct QEMU_PACKED NvmeZoneDescr {
    uint8_t     zt;
    uint8_t     zs;
    uint8_t     za;
    uint8_t     rsvd3[5];
    uint64_t    zcap;
    uint64_t    zslba;
    uint64_t    wp;
    uint8_t     rsvd32[32];
} NvmeZoneDescr;

typedef enum NvmeZoneState {
    NVME_ZONE_STATE_RESERVED         = 0x00,
    NVME_ZONE_STATE_EMPTY            = 0x01,
    NVME_ZONE_STATE_IMPLICITLY_OPEN  = 0x02,
    NVME_ZONE_STATE_EXPLICITLY_OPEN  = 0x03,
    NVME_ZONE_STATE_CLOSED           = 0x04,
    NVME_ZONE_STATE_READ_ONLY        = 0x0D,
    NVME_ZONE_STATE_FULL             = 0x0E,
    NVME_ZONE_STATE_OFFLINE          = 0x0F,
} NvmeZoneState;

#define NVME_SET_CSI(vec, csi) (vec |= (uint8_t)(1 << (csi)))

typedef struct QEMU_PACKED NvmeLBAFE {
    uint64_t    zsze;
    uint8_t     zdes;
    uint8_t     rsvd9[7];
} NvmeLBAFE;

typedef struct QEMU_PACKED NvmeIdNsZoned {
    uint16_t    zoc;
    uint16_t    ozcs;
    uint32_t    mar;
    uint32_t    mor;
    uint32_t    rrl;
    uint32_t    frl;
    uint8_t     rsvd20[2796];
    NvmeLBAFE   lbafe[16];
    uint8_t     rsvd3072[768];
    uint8_t     vs[256];
} NvmeIdNsZoned;

typedef struct NvmeZone {
    NvmeZoneDescr   d;
    uint64_t        w_ptr;
    uint8_t         dirty; // written or not
    uint8_t         rnd; // random zone or seq zone
    int        line_idx;
    QTAILQ_ENTRY(NvmeZone) entry;
} NvmeZone;

typedef struct {
    uint8_t     zt;
    uint8_t     zs;
    uint8_t     za;
    uint64_t    zcap;
    uint64_t    zslba;
    uint64_t    wp;
} hyssd_zone_descr_info;

typedef struct {
    hyssd_zone_descr_info d;
    uint64_t w_ptr;
    uint8_t dirty; // written or not
    uint8_t rnd; // random zone or seq zone
} hyssd_zone_info;

typedef struct NvmeNamespaceParams {
    uint32_t nsid;
    QemuUUID uuid;

    bool     zoned;
    bool     cross_zone_read;
    uint64_t zone_size_bs;
    uint64_t zone_cap_bs;
    uint32_t max_active_zones;
    uint32_t max_open_zones;
    uint32_t zd_extension_size;
} NvmeNamespaceParams;

static inline uint32_t hy_zns_nsid(NvmeNamespace *ns)
{
    if (ns) {
        return ns->id;
    }

    return -1;
}

static inline NvmeLBAF *hy_zns_ns_lbaf(NvmeNamespace *ns)
{
    NvmeIdNs *id_ns = &ns->id_ns;
    return &id_ns->lbaf[NVME_ID_NS_FLBAS_INDEX(id_ns->flbas)];
}

static inline uint8_t hy_zns_ns_lbads(NvmeNamespace *ns)
{
    /* NvmeLBAF */
    return hy_zns_ns_lbaf(ns)->lbads;
}

/* calculate the number of LBAs that the namespace can accomodate */
static inline uint64_t hy_zns_ns_nlbas(NvmeNamespace *ns)
{
    return ns->size >> hy_zns_ns_lbads(ns);
}

/* convert an LBA to the equivalent in bytes */
static inline size_t hy_zns_l2b(NvmeNamespace *ns, uint64_t lba)
{
    return lba << hy_zns_ns_lbads(ns);
}

static inline NvmeZoneState hy_zns_get_zone_state(NvmeZone *zone)
{
    return zone->d.zs >> 4;
}

static inline void hy_zns_set_zone_state(NvmeZone *zone, NvmeZoneState state)
{
    zone->d.zs = state << 4;
}

static inline uint64_t hy_zns_zone_rd_boundary(NvmeNamespace *ns, NvmeZone *zone)
{
    return zone->d.zslba + ns->ctrl->zone_size;
}

static inline uint64_t hy_zns_zone_wr_boundary(NvmeZone *zone)
{
    return zone->d.zslba + zone->d.zcap;
}

static inline bool hy_zns_wp_is_valid(NvmeZone *zone)
{
    uint8_t st = hy_zns_get_zone_state(zone);

    return st != NVME_ZONE_STATE_FULL &&
           st != NVME_ZONE_STATE_READ_ONLY &&
           st != NVME_ZONE_STATE_OFFLINE;
}

static inline uint8_t *hy_zns_get_zd_extension(NvmeNamespace *ns, uint32_t zone_idx)
{
    return &ns->ctrl->zd_extensions[zone_idx * ns->ctrl->zd_extension_size];
}

static inline void hy_zns_aor_inc_open(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    assert(n->nr_open_zones >= 0);
    if (n->max_open_zones) {
        n->nr_open_zones++;
        assert(n->nr_open_zones <= n->max_open_zones);
    }
}

static inline void hy_zns_aor_dec_open(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    if (n->max_open_zones) {
        assert(n->nr_open_zones > 0);
        n->nr_open_zones--;
    }
    assert(n->nr_open_zones >= 0);
}

static inline void hy_zns_aor_inc_active(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    assert(n->nr_active_zones >= 0);
    if (n->max_active_zones) {
        n->nr_active_zones++;
        assert(n->nr_active_zones <= n->max_active_zones);
    }
}

static inline void hy_zns_aor_dec_active(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    if (n->max_active_zones) {
        assert(n->nr_active_zones > 0);
        n->nr_active_zones--;
        assert(n->nr_active_zones >= n->nr_open_zones);
    }
    assert(n->nr_active_zones >= 0);
}
// blockbox
enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t sec : SEC_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t blk : BLK_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct hyssd_pg {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct hyssd_blk {
    struct hyssd_pg *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct hyssd_pl {
    struct hyssd_blk *blk;
    int nblks;
};

struct hyssd_lun {
    struct hyssd_pl *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct hyssd_ch {
    struct hyssd_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
    int is_rnd; // whether this line is used by an rzone or an szone
    int zone_idx;
    // int ridx; // index within the rzone when the line is rzone-owned
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

// HYSSD Monitor Protocol Structures

/* Write pointer structure for communication with the host */
typedef struct {
    int curline_id;          /* Current write line ID */
    int ch;                  /* Channel */
    int lun;                 /* LUN */
    int pg;                  /* Page */
    int blk;                 /* Block */
    int pl;                  /* Plane */
} hyssd_write_pointer_info;

/* Line info structure for communication with the host */
typedef struct {
    int id;                  /* Line ID (same as block ID) */
    int ipc;                 /* Invalid page count in this line */
    int vpc;                 /* Valid page count in this line */
    unsigned long long pos;  /* Position in victim line priority queue */
    int is_rnd;              /* Is random zone (1) or sequential zone (0) */
    int ridx;                /* If random zone line, which index */
} hyssd_line_info;

/* Line management info structure for communication with the host */
typedef struct {
    int tt_lines;            /* Total number of lines */
    int free_line_cnt;       /* Number of free lines */
    int victim_line_cnt;     /* Number of victim lines */
    int full_line_cnt;       /* Number of full lines */
    /* Line info array follows as flexible array member */
    hyssd_line_info lines[]; /* Changed to C99 flexible array member syntax */
} hyssd_line_mgmt_info;

typedef struct {
    hyssd_write_pointer_info wp_info;
    uint64_t gc_count;      /* Number of GC operations performed */
    uint64_t gc_pgs;        /* # of pages moved in GC */
    hyssd_line_mgmt_info lm_info;
    uint32_t num_zones;
    uint64_t zr_count;
    hyssd_zone_info zones[];
} hyssd_monitor_data;

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

typedef struct hyparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    int set_time_model;

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;

    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;
    
    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
    
    int rnd_lines;
    int num_rzones;
    int num_op_line;
    int zr_count;

    uint64_t gc_count;      /* Number of GC operations performed */
    uint64_t gc_pgs;        /* # of pages moved in GC */
    uint64_t gc_lat;
    uint64_t num_erase_blks;

    uint64_t num_discard;

    int lbas_per_zone;
    int pgs_per_zone; /* # of pages per zone */
    int tt_rpgs;      /* total # of random zone pages */
} hyparams;

struct hyssd {
    char *ssdname;
    hyparams sp;
    struct hyssd_ch *ch;

    FemuCtrl *n;
    
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
    QemuThread gc_thread;
};

#define FEMU_DEBUG_FTL

#ifdef FEMU_DEBUG_FTL
// void get_timestamp(char *timestamp, size_t size);

#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)

#define DE_err(fmt, ...) \
    do { \
        char timestamp[32]; \
        get_timestamp(timestamp, sizeof(timestamp)); \
        fprintf(stderr, "[DE-Err] [ %s ]: " fmt, timestamp, ## __VA_ARGS__); \
    } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif