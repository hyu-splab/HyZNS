#include "../nvme.h"
#include "./ftl.h"

static void bb_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vbb = 0;
    const char *vbbssd_mn = "FEMU BlackBox-SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

/* bb <=> black-box */
static void bb_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    bb_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in Blackbox-SSD mode ...\n");
    ssd_init(n);
}

static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

static uint16_t bb_discard(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    struct ssd *ssd = n->ssd;
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    
    uint16_t nr = (dw10 & 0xff) + 1;
    uint64_t slba;
    uint32_t nlb;
    NvmeDsmRange *range = g_malloc0(sizeof(NvmeDsmRange) * nr);
    
    if (dma_write_prp(n, (uint8_t *)range, sizeof(*range) * nr, prp1, prp2)) {
        printf("Failed to dma_write_prp\n");
        g_free(range);
        return NVME_INVALID_FIELD | NVME_DNR;
    }
    
    req->status = NVME_SUCCESS;
    
    for (int i = 0; i < nr; i++) {
        slba = le64_to_cpu(range[i].slba);
        nlb = le32_to_cpu(range[i].nlb);
        
        if (slba + nlb > le64_to_cpu(ns->id_ns.nsze)) {
            ftl_err("slba + nlb > ns->id_ns.nsze\n");
            req->status = NVME_LBA_RANGE | NVME_DNR;
            break;
        }

        /* Pass nlb (DSM range LB count, up to 32-bit) explicitly. Only set
         * req->slba here; req->nlb is uint16_t and would truncate a large
         * mkfs/fstrim discard range to garbage. */
        req->slba = slba;

        ssd_discard(ssd, req, (uint64_t)nlb);
    }
    
    g_free(range);
    return req->status;
}

static uint16_t bb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_DSM:
        return bb_discard(n, ns, cmd, req);
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return bb_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t bb_id_dev(FemuCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t data_size = (dw10 + 1) << 2;
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    nvme_device_info dev_data;
    uint16_t status;

    memset(&dev_data, 0, sizeof(nvme_device_info));

    // common device info
    dev_data.type = 0;

    // device-type-specific info (matches struct field order)
    dev_data.bbssd.secsz = spp->secsz;                // sector size (bytes)
    dev_data.bbssd.secs_per_pg = spp->secs_per_pg;    // sectors per page
    dev_data.bbssd.pgs_per_blk = spp->pgs_per_blk;    // NAND pages per block
    dev_data.bbssd.blks_per_pl = spp->blks_per_pl;    // blocks per plane
    dev_data.bbssd.pls_per_lun = spp->pls_per_lun;    // planes per LUN
    dev_data.bbssd.luns_per_ch = spp->luns_per_ch;    // LUNs per channel
    dev_data.bbssd.nchs = spp->nchs;                  // channels per SSD
    dev_data.bbssd.secs_per_blk = spp->secs_per_blk;  // sectors per block
    dev_data.bbssd.secs_per_pl = spp->secs_per_pl;    // sectors per plane
    dev_data.bbssd.secs_per_lun = spp->secs_per_lun;  // sectors per LUN
    dev_data.bbssd.secs_per_ch = spp->secs_per_ch;    // sectors per channel
    dev_data.bbssd.tt_secs = spp->tt_secs;            // total sectors
    dev_data.bbssd.pgs_per_pl = spp->pgs_per_pl;      // pages per plane
    dev_data.bbssd.pgs_per_lun = spp->pgs_per_lun;    // pages per LUN
    dev_data.bbssd.pgs_per_ch = spp->pgs_per_ch;      // pages per channel
    dev_data.bbssd.tt_pgs = spp->tt_pgs;              // total pages
    dev_data.bbssd.blks_per_lun = spp->blks_per_lun;  // blocks per LUN
    dev_data.bbssd.blks_per_ch = spp->blks_per_ch;    // blocks per channel
    dev_data.bbssd.tt_blks = spp->tt_blks;            // total blocks
    dev_data.bbssd.secs_per_line = spp->secs_per_line; // sectors per line
    dev_data.bbssd.pgs_per_line = spp->pgs_per_line;  // pages per line
    dev_data.bbssd.blks_per_line = spp->blks_per_line; // blocks per line
    dev_data.bbssd.tt_lines = spp->tt_lines;          // total lines
    dev_data.bbssd.pls_per_ch = spp->pls_per_ch;      // planes per channel
    dev_data.bbssd.tt_pls = spp->tt_pls;              // total planes
    dev_data.bbssd.tt_luns = spp->tt_luns;            // total LUNs

    // transfer to host
    status = dma_read_prp(n, (uint8_t *)&dev_data, data_size, prp1, prp2);
    
    return status;
}

static uint16_t bb_mon(FemuCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t data_size = (dw10 + 1) << 2;
    struct ssd *ssd = n->ssd;
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp = &ssd->wp;
    
    /* Calculate header size and total size */
    size_t header_size = sizeof(bb_write_pointer_info) + 
                         sizeof(uint64_t) * 2 + // gc
                         sizeof(int) * 4; // tt_lines, free_line_cnt, victim_line_cnt, full_line_cnt
    
    size_t line_info_size = lm->tt_lines * sizeof(bb_line_info);
    size_t total_size = header_size + line_info_size;
    
    if (data_size < total_size) {
        fprintf(stderr, "Error: Buffer too small (%u bytes), need %zu bytes\n", 
                data_size, total_size);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    /* Allocate buffer */
    void *buf = g_malloc0(total_size);
    if (!buf) {
        fprintf(stderr, "Error: Failed to allocate memory\n");
        return NVME_INTERNAL_DEV_ERROR;
    }

    /* Flat buffer approach for better compatibility */
    uint8_t *ptr = (uint8_t *)buf;
    
    /* 1. Write pointer info */
    bb_write_pointer_info *wp_info = (bb_write_pointer_info *)ptr;
    wp_info->curline_id = wpp->curline ? wpp->curline->id : -1;
    wp_info->ch = wpp->ch;
    wp_info->lun = wpp->lun;
    wp_info->pg = wpp->pg;
    wp_info->blk = wpp->blk;
    wp_info->pl = wpp->pl;
    ptr += sizeof(bb_write_pointer_info);
    
    uint64_t *gc_count_ptr = (uint64_t *)ptr;
    *gc_count_ptr = ssd->sp.gc_count;
    ptr += sizeof(uint64_t);
    
    uint64_t *gc_pgs_ptr = (uint64_t *)ptr;
    *gc_pgs_ptr = ssd->sp.gc_pgs;
    ptr += sizeof(uint64_t);

    /* 2. Line management info header fields */
    int *tt_lines_ptr = (int *)ptr;
    *tt_lines_ptr = lm->tt_lines;
    ptr += sizeof(int);
    
    int *free_line_cnt_ptr = (int *)ptr;
    *free_line_cnt_ptr = lm->free_line_cnt;
    ptr += sizeof(int);
    
    int *victim_line_cnt_ptr = (int *)ptr;
    *victim_line_cnt_ptr = lm->victim_line_cnt;
    ptr += sizeof(int);
    
    int *full_line_cnt_ptr = (int *)ptr;
    *full_line_cnt_ptr = lm->full_line_cnt;
    ptr += sizeof(int);
    
    /* 3. Line info array */
    bb_line_info *line_infos = (bb_line_info *)ptr;
    for (int i = 0; i < lm->tt_lines; i++) {
        struct line *line = &lm->lines[i];
        line_infos[i].id = line->id;
        line_infos[i].ipc = line->ipc;
        line_infos[i].vpc = line->vpc;
        line_infos[i].pos = line->pos;
    }
    
    /* Send data to host */
    uint16_t status = dma_read_prp(n, (uint8_t *)buf, data_size, prp1, prp2);
    
    g_free(buf);
    return status;
}

static uint16_t bb_format(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct write_pointer *wpp = &ssd->wp;
    int i;

    femu_log("%s: Formatting BlackBox-SSD...\n", n->devname);

    /* 1. Reset all mapping tables */
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }

    /* 2. Reset reverse mapping table */
    for (i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }

    /* 3. Reset all NAND blocks/pages state directly */
    for (int ch = 0; ch < spp->nchs; ch++) {
        struct ssd_channel *channel = &ssd->ch[ch];
        for (int lun = 0; lun < spp->luns_per_ch; lun++) {
            struct nand_lun *nand_lun = &channel->lun[lun];
            for (int pl = 0; pl < spp->pls_per_lun; pl++) {
                struct nand_plane *plane = &nand_lun->pl[pl];
                for (int blk = 0; blk < spp->blks_per_pl; blk++) {
                    struct nand_block *block = &plane->blk[blk];
                    
                    /* Mark all pages in block as FREE */
                    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
                        struct nand_page *page = &block->pg[pg];
                        page->status = PG_FREE;
                        for (int sec = 0; sec < spp->secs_per_pg; sec++) {
                            page->sec[sec] = SEC_FREE;
                        }
                    }
                    
                    /* Reset block status */
                    block->ipc = 0;
                    block->vpc = 0;
                    block->erase_cnt = 0;  /* Factory reset: clear erase count */
                    block->wp = 0;
                }
            }
            
            /* Reset LUN timing */
            nand_lun->next_lun_avail_time = 0;
            nand_lun->busy = false;
            nand_lun->gc_endtime = 0;
        }
        
        /* Reset channel timing */
        channel->next_ch_avail_time = 0;
        channel->busy = 0;
    }

    /* 4. Reinitialize line management structures */
    /* Clear victim and full line lists, mark all lines as free */
    while (!QTAILQ_EMPTY(&lm->full_line_list)) {
        struct line *line = QTAILQ_FIRST(&lm->full_line_list);
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
    }
    lm->full_line_cnt = 0;

    /* Clear victim line priority queue */
    while (pqueue_size(lm->victim_line_pq) > 0) {
        pqueue_pop(lm->victim_line_pq);
    }
    lm->victim_line_cnt = 0;

    /* Rebuild free line list */
    QTAILQ_INIT(&lm->free_line_list);
    lm->free_line_cnt = 0;
    
    for (i = 0; i < lm->tt_lines; i++) {
        struct line *line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;  /* invalid page count */
        line->vpc = 0;  /* valid page count */
        line->pos = 0;
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    /* 5. Reinitialize write pointer */
    struct line *curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;

    /* 6. Reset GC statistics */
    spp->gc_count = 0;
    spp->gc_pgs = 0;
    spp->gc_lat = 0;
    spp->num_erase_blks = 0;
    spp->num_discard = 0;
    
    /* 7. Reset I/O statistics */
    n->nr_tt_ios = 0;
    n->nr_tt_late_ios = 0;

    femu_log("%s: BlackBox-SSD Format completed successfully\n", n->devname);
    
    return NVME_SUCCESS;
}

static uint16_t bb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case DEVICE_TYPE_OPCODE:
        return bb_id_dev(n, cmd);
    case NVME_ADMIN_CMD_BBSSD:
        return bb_mon(n, cmd);
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(n, cmd);
        return NVME_SUCCESS;
    case NVME_ADM_CMD_FORMAT_NVM:
        return bb_format(n, cmd);
    case NVME_ADM_CMD_RESET_DEV:
        return bb_format(n, cmd);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

int nvme_register_bbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = bb_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = bb_admin_cmd,
        .io_cmd           = bb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}

