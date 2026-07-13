#ifndef __FEMU_HYHOSTSSD_H
#define __FEMU_HYHOSTSSD_H

#include "../nvme.h"

/*
 * Host-managed Hybrid SSD (HYHOSTSSD) — femu_mode = 5.
 *
 * Self-contained: this directory must NOT include or call into zns/,
 * bbssd/, ocssd/, or hyssd/ (mirroring the convention of hyssd/).
 * Common primitives (NvmeRwCmd, backend_rw, NVME_ID_*, …) come from
 * shared infrastructure under hw/femu/.
 *
 * LBA layout:
 *
 *   slba 0 .. r_region_end_lba - 1   →  R-region (random-write)
 *                                        host FTL owns L2P/GC; the
 *                                        device just moves bytes and
 *                                        charges latency.
 *   slba r_region_end_lba .. nsze-1  →  S-region (sequential-write zones)
 *                                        the device tracks per-zone
 *                                        state and write pointer.
 *
 * The boundary is runtime-modifiable via Zone Management Send action
 * HYHOST_ZA_SET_R_END (0x20). The new r_end LBA arrives in the standard
 * SLBA field (cdw10/cdw11), matching every other ZSA's wire encoding.
 * Issued by dm-hyhost through REQ_OP_ZONE_MODIFY (bi_sector = new ABA LBA)
 * so the request travels the IO queue, which an out-of-tree dm target can
 * reach (the admin queue cannot). An earlier CDW14 vendor encoding was
 * dropped to keep the wire consistent with other zone ops.
 *
 * Wire-level zone-type convention (mirrors hyssd-conf.c:1062):
 *   Every zone descriptor reports d.zt = HYHOST_ZONE_TYPE_SEQ_WRITE (0x02)
 *   regardless of region. The 5.15 NVMe driver's zone revalidation
 *   (drivers/nvme/host/zns.c:158) only accepts SEQ_WRITE_REQ; reporting
 *   CONV (0x01) makes it reject the namespace ("invalid zone type") and
 *   the whole zoned stack unwinds. Instead:
 *     - wire: every zone is SEQ_WRITE_REQ
 *     - internal: HyHostZone::is_random tags R-region zones (slba < r_end).
 *       hyhost_classify_zones flips this on init and on every set_r_end.
 *     - device write check skips wp/state validation when is_random — that
 *       allows in-place overwrite. R-region data integrity stays the host
 *       FTL's job (dm-hyhost L2P + the EMPTY-only invariant on set_r_end).
 *     - host learns R-zone count via Zone Management Receive action
 *       HYHOST_ZRA_REPORT_RZONE (0x21), exposed to userspace through the
 *       BLKREPORTRZONE ioctl (struct blk_rzone_report).
 *
 * MVP scope (Tier 0, see docs/OCSSD_HYBRID_DESIGN.md §4):
 *   - R-region: direct LBA→backend mapping, flat per-page latency
 *   - S-region: minimal zone state machine, sequential write check, reset
 *   - vendor opcode to resize the boundary
 *   - no OOB metadata, no recovery, no GC, no append.
 */

/* -------------------------------------------------------------------------
 * Zone primitives (independent copy; hyssd / zns each declare their own).
 * Layout matches the NVMe ZNS spec so libzbd / kernel zone code work.
 * ------------------------------------------------------------------------- */

enum HyHostZoneType {
    HYHOST_ZONE_TYPE_SEQ_WRITE  = 0x02, /* only wire-level type — see header comment */
};

enum HyHostZoneState {
    HYHOST_ZS_RESERVED          = 0x00,
    HYHOST_ZS_EMPTY             = 0x01,
    HYHOST_ZS_IMPLICITLY_OPEN   = 0x02,
    HYHOST_ZS_EXPLICITLY_OPEN   = 0x03,
    HYHOST_ZS_CLOSED            = 0x04,
    HYHOST_ZS_READ_ONLY         = 0x0D,
    HYHOST_ZS_FULL              = 0x0E,
    HYHOST_ZS_OFFLINE           = 0x0F,
};

enum HyHostZoneSendAction {
    HYHOST_ZA_CLOSE        = 0x01,
    HYHOST_ZA_FINISH       = 0x02,
    HYHOST_ZA_OPEN         = 0x03,
    HYHOST_ZA_RESET        = 0x04,
    HYHOST_ZA_OFFLINE      = 0x05,
    HYHOST_ZA_SET_R_END    = 0x20, /* device-scoped; new r_end LBA in SLBA */
    HYHOST_ZA_R_BLOCK_ERASE = 0x22, /* R-region only; slba = R-block start (debug/legacy) */
    HYHOST_ZA_R_LINE_ERASE  = 0x23, /* R-region only; slba = line start, parallel erase across all (ch, lun) */
};

enum HyHostZoneReceiveAction {
    HYHOST_ZRA_REPORT       = 0x00,
    HYHOST_ZRA_REPORT_RZONE = 0x21, /* vendor: returns u32 R-zone count via PRP */
};

typedef struct QEMU_PACKED HyHostZoneDescr {
    uint8_t  zt;
    uint8_t  zs;            /* state in upper nibble */
    uint8_t  za;
    uint8_t  rsvd3[5];
    uint64_t zcap;
    uint64_t zslba;
    uint64_t wp;
    uint8_t  rsvd32[32];
} HyHostZoneDescr;

typedef struct QEMU_PACKED HyHostZoneReportHeader {
    uint64_t nr_zones;
    uint8_t  rsvd[56];
} HyHostZoneReportHeader;

/* Zoned-namespace identify (CSI=ZONED, CNS=0x05). Layout copies the NVMe
 * ZNS spec; we keep a private copy so this directory stays self-contained
 * (same convention as hyssd/). The struct names match the forward
 * declarations in nvme.h so FemuCtrl::id_ns_zoned can be assigned directly
 * — nvme-admin.c returns the pointer from NVME_ID_CNS_CS_NS when
 * n->csi == NVME_CSI_ZONED. */
typedef struct QEMU_PACKED NvmeLBAFE {
    uint64_t zsze;
    uint8_t  zdes;
    uint8_t  rsvd9[7];
} NvmeLBAFE;

typedef struct QEMU_PACKED NvmeIdNsZoned {
    uint16_t  zoc;
    uint16_t  ozcs;
    uint32_t  mar;
    uint32_t  mor;
    uint32_t  rrl;
    uint32_t  frl;
    uint8_t   rsvd20[2796];
    NvmeLBAFE lbafe[16];
    uint8_t   rsvd3072[768];
    uint8_t   vs[256];
} NvmeIdNsZoned;

typedef struct HyHostZone {
    HyHostZoneDescr d;
    uint64_t        w_ptr;
    bool            is_random; /* R-region zone (slba < r_end). Wire still
                                * reports SEQ_WRITE; this flag drives the
                                * device-internal in-place-overwrite path. */
} HyHostZone;

typedef struct ZNSWriteCache {
    uint64_t zidx;
    uint64_t used;
    uint64_t cap;
    uint64_t *lpns;
} ZNSWriteCache;

typedef struct HyHostSSD {
    /* LBA boundary. */
    uint64_t r_region_end_lba;

    /* S-region zone descriptors. The array covers the whole namespace
     * (zone 0 starts at slba 0). Zones whose slba < r_region_end_lba are
     * "dormant": IO routes through the R-region path and the zone
     * descriptor is unused. This keeps the zone-id arithmetic trivial
     * regardless of where the boundary sits. */
    HyHostZone *zone_array;
    uint32_t    num_zones;
    uint64_t    zone_size;       /* sectors per zone (and == zone_capacity for MVP) */
    uint32_t    zone_size_log2;  /* ilog2(zone_size) if it is a power of 2, else 0 */

    /* R-region "line": one NAND block per (ch, lun) in parallel = the device
     * erase unit and the host FTL (dm-hyhost) GC unit. line_size (in LBAs) =
     * nchs * luns_per_ch * pgs_per_blk * (page_bytes / lba_bytes). A zone is
     * an integer number of lines (zone_size % line_size == 0). Used to
     * validate HYHOST_ZA_R_LINE_ERASE alignment — dm issues one erase per
     * line, which may be smaller than a zone. */
    uint64_t    line_size;       /* sectors per R-region line */

    /* NAND geometry + per-(channel,LUN) timing model.
     *
     * IO latency is computed by walking the request page-by-page and calling
     * advance_nand on the (ch, lun) implied by the page index — bbssd-style.
     * Pages on different LUNs charge in parallel (max wins); pages on the
     * same LUN serialize through that LUN's busy timer. The channel timer
     * accounts for shared bus contention during data transfer. */
    uint32_t nchs;
    uint32_t luns_per_ch;
    uint32_t page_bytes;        /* user page size for ch/lun striping (e.g. 4 KiB) */
    uint32_t pgs_per_blk;       /* NAND erase-block size in pages (e.g. 256 = 1 MiB) */
    int64_t  pg_rd_lat_ns;
    int64_t  pg_wr_lat_ns;      /* S-region (ZNS) raw-NAND program latency */
    int64_t  pg_wr_lat_r_ns;    /* R-region (conventional) DRAM-cached write latency */
    int64_t  blk_er_lat_ns;
    int64_t  ch_xfer_lat_ns;

    int64_t *ch_next_avail_ns;  /* len = nchs */
    int64_t *lun_next_avail_ns; /* len = nchs * luns_per_ch */
    pthread_mutex_t timing_mutex;

    // write cache
    ZNSWriteCache *cache;
    int num_wc;
    int used_wc;
} HyHostSSD;



/* Set the R/S boundary. Returns NVME_SUCCESS or an NVMe error code. */
uint16_t hyhost_set_r_end(FemuCtrl *n, uint64_t new_r_end_lba);

#endif /* __FEMU_HYHOSTSSD_H */
