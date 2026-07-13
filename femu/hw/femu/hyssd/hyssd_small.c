#include "hyssd_small.h"

#define MIN_DISCARD_GRANULARITY     (4 * KiB)
#define NVME_DEFAULT_ZONE_CAP_SIZE  (64 * MiB)
#define NVME_DEFAULT_ZONE_SIZE      (64 * MiB)
#define NVME_DEFAULT_MAX_AZ_SIZE    (16 * KiB)
#define NVME_ZNS_MAX_ACTIVE_ZONES   (384)
#define NVME_ZNS_MAX_OPEN_ZONES     (384)

#define DEVICE_SIZE (128 * GiB)
#define CHS_PER_ZONE (2)
#define WAYS_PER_ZONE (8)

// 16 channels / 2ch per zone
static int base_arr[] = {0, 2, 4, 6, 8, 10, 12, 14};

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

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct hyssd_ch *get_ch(struct hyssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct hyssd_lun *get_lun(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyssd_ch *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct hyssd_pl *get_pl(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyssd_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct hyssd_blk *get_blk(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyssd_pl *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct hyssd_pg *get_pg(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyssd_blk *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static inline uint32_t hy_zns_zone_idx(NvmeNamespace *ns, uint64_t slba)
{
    FemuCtrl *n = ns->ctrl;

    return (n->zone_size_log2 > 0 ? slba >> n->zone_size_log2 : slba /
            n->zone_size);
}

static inline bool hy_should_gc(struct hyssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool hy_should_gc_high(struct hyssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa hy_get_maptbl_ent(struct hyssd *ssd, uint64_t lpn)
{   
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct hyssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_rpgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct hyssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    int line_idx = ((ppa->g.blk / 2) * 8) + (ppa->g.ch / 2);
    struct line *line = &lm->lines[line_idx];
    struct hyparams *spp = &ssd->sp;
    uint32_t pgs_per_line = spp->pgs_per_line;
    uint64_t pgidx;

    // check whether the line belongs to a random zone
    if (line->is_rnd != 1) {
        // error case; log it
        ftl_err("Attempting to access id on non-random line (is_rnd=%d, id=%d)\n", 
                line->is_rnd, line->id);
        return UINT64_MAX; // error value
    }
    
    // fixes the out-of-bounds problem of the old scheme
    // pgidx = line->id * pgs_per_line + \
    //         ppa->g.ch * (spp->luns_per_ch * spp->pgs_per_blk) + \
    //         ppa->g.lun * spp->pgs_per_blk + \
    //         ppa->g.pg;
    // int ch_base = (victim_line->id * CHS_PER_ZONE) % ssd->sp.nchs;
    // int blk_base = (victim_line->id / 8) * 2;
    pgidx = line->id * pgs_per_line + \
            (ppa->g.blk % 2) * (CHS_PER_ZONE * WAYS_PER_ZONE * spp->pgs_per_blk) + \
            (ppa->g.ch % CHS_PER_ZONE) * (WAYS_PER_ZONE * spp->pgs_per_blk) + \
            ppa->g.lun * spp->pgs_per_blk + \
            ppa->g.pg;
            // (ppa->g.blk % 2) * ppa->g.lun * spp->pgs_per_blk + \

    // bounds check - must not exceed the rzone's total page count
    ftl_assert(pgidx < spp->tt_rpgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct hyssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    return ssd->rmap[pgidx];
}

// TODO: derive pgidx from ppa differently (old scheme can go out of bounds)
/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct hyssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);
    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

// line == super block(physical zone)
static void hy_ssd_init_lines(FemuCtrl *n, struct hyssd *ssd)
{
    struct hyparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;
    int num_rzone = spp->num_rzones;

    // # lines == #zones
    // lm->tt_lines = spp->blks_per_pl;
    lm->tt_lines = spp->tt_lines;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        if (i < num_rzone) {
            line->is_rnd = 1; // line used by an rzone
            // line->ridx = i; // index of this line among the rzone-owned lines
            /* initialize all the lines as free lines */
            QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry); // free_line holds only rzone lines
            lm->free_line_cnt++;
        } else {
            line->is_rnd = 0;
            // line->ridx = -1;
        }
    }

    ftl_assert(lm->free_line_cnt == num_rzone);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
    
    fprintf(stdout, "[hy_ssd_init_lines] free : %d victim : %d full : %d\n", 
                lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt);
}

static void hy_ssd_init_write_pointer(struct hyssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch_base = (curline->id * CHS_PER_ZONE) % ssd->sp.nchs;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pl = 0;
    wpp->blk_base = 0;
    wpp->blk = 0;
    wpp->pg = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *hy_get_next_free_line(struct hyssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void hy_ssd_advance_write_pointer(struct hyssd *ssd)
{
    struct hyparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == CHS_PER_ZONE) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == WAYS_PER_ZONE) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                wpp->blk++;
                if (wpp->blk == 2) {
                    wpp->blk = 0;
                    /* move current line to {victim,full} line list */
                    if (wpp->curline->vpc == spp->pgs_per_line) {
                        /* all pgs are still valid, move to full line list */
                        ftl_assert(wpp->curline->ipc == 0);
                        QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                        lm->full_line_cnt++;
                        fprintf(stdout, "full line\n");
                    } else {
                        ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                        /* there must be some invalid pages in this line */
                        ftl_assert(wpp->curline->ipc > 0);
                        pqueue_insert(lm->victim_line_pq, wpp->curline);
                        lm->victim_line_cnt++;
                        fprintf(stdout, "victim line\n");
                    }
                    /* current line is used up, pick another empty line */
                    check_addr(wpp->blk, spp->blks_per_pl);
                    wpp->curline = NULL;
                    wpp->curline = hy_get_next_free_line(ssd);
                    if (!wpp->curline) {
                        /* TODO */
                        ftl_log("no free lines\n");
                        abort();
                    }
                    wpp->ch_base = (wpp->curline->id * CHS_PER_ZONE) % ssd->sp.nchs;
                    wpp->blk_base = (wpp->curline->id / 8) * 2; //+ (wpp->curline->id % 8);
                    fprintf(stdout, "new line(ch_base : %lu, blk_base : %lu)\n", wpp->ch_base, wpp->blk_base);
                    check_addr(wpp->ch_base, spp->nchs);
                    /* make sure we are starting from page 0 in the super block */
                    ftl_assert(wpp->pg == 0);
                    ftl_assert(wpp->lun == 0);
                    ftl_assert(wpp->ch == 0);
                    /* TODO: assume # of pl_per_lun is 1, fix later */
                    ftl_assert(wpp->pl == 0);
                }
            }
        }
    }
}

static struct ppa hy_get_new_page(struct hyssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch_base + wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pl = wpp->pl;
    ppa.g.blk = wpp->blk_base + wpp->blk;
    ppa.g.pg = wpp->pg;
    ftl_assert(ppa.g.pl == 0);
    
    return ppa;
}

static void hy_check_params(struct hyparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void hy_ssd_init_params(struct hyparams *spp, FemuCtrl *n)
{
    /* per zone */
    spp->secsz = n->bb_params.secsz;
    spp->secs_per_pg = n->bb_params.secs_per_pg;
    spp->pgs_per_blk = n->bb_params.pgs_per_blk;
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 128 MiB */
    spp->pls_per_lun = n->bb_params.pls_per_lun;
    spp->luns_per_ch = n->bb_params.luns_per_ch;
    spp->nchs = n->bb_params.nchs;

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    spp->num_rzones = n->sp.num_rzones;
    spp->num_op_line = n->sp.num_op_line;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs; // total ssd pages
    
    // random zone pages
    int blks_per_zone = NVME_DEFAULT_ZONE_SIZE / (2 * 1024 * 1024);
    spp->tt_rpgs = spp->pgs_per_blk * blks_per_zone * spp->num_rzones;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;
    
    /* line is special, put it at the end */
    spp->blks_per_line = CHS_PER_ZONE * WAYS_PER_ZONE * 2;
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = DEVICE_SIZE / NVME_DEFAULT_ZONE_SIZE; /* TODO: to fix under multiplanes */

    fprintf(stdout, "%lu %lu %lu\n", spp->tt_rpgs, spp->blks_per_line, spp->tt_lines);
    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->num_rzones);

    if (spp->gc_thres_lines == 0) {
        spp->gc_thres_lines = 1;
    }

    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->num_rzones);

    if (spp->gc_thres_lines_high == 0) {
        spp->gc_thres_lines_high = 1;
    }

    spp->enable_gc_delay = true;

    spp->zr_count = 0;
    spp->gc_count = 0;
    spp->gc_pgs = 0;

    hy_check_params(spp);
}

static void hy_ssd_init_nand_page(struct hyssd_pg *pg, struct hyparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void hy_ssd_init_nand_blk(struct hyssd_blk *blk, struct hyparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct hyssd_pg) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        hy_ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void hy_ssd_init_nand_plane(struct hyssd_pl *pl, struct hyparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct hyssd_blk) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        hy_ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void hy_ssd_init_nand_lun(struct hyssd_lun *lun, struct hyparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct hyssd_pl) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        hy_ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void hy_ssd_init_ch(struct hyssd_ch *ch, struct hyparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct hyssd_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        hy_ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void hy_ssd_init_maptbl(struct hyssd *ssd)
{
    struct hyparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_rpgs);
    for (int i = 0; i < spp->tt_rpgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void hy_ssd_init_rmap(struct hyssd *ssd)
{
    struct hyparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_rpgs);
    for (int i = 0; i < spp->tt_rpgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

static void hy_ssd_init_conv(FemuCtrl *n)
{
    struct hyssd *hyssd = n->hyssd;
    hyparams *spp = &hyssd->sp;
    hy_ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    hyssd->ch = g_malloc0(sizeof(struct hyssd_ch) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        hy_ssd_init_ch(&hyssd->ch[i], spp);
    }

    /* initialize maptbl */
    hy_ssd_init_maptbl(hyssd);

    /* initialize rmap */
    hy_ssd_init_rmap(hyssd);

    /* initialize all the lines */
    hy_ssd_init_lines(n, hyssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    hy_ssd_init_write_pointer(hyssd);
}

static inline bool valid_lpn(struct hyssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline struct line *get_line(struct hyssd *ssd, struct ppa *ppa)
{
    // 8 : lines per superblock
    int line_idx = ((ppa->g.blk / 2) * 8) + (ppa->g.ch / 2);
    return &(ssd->lm.lines[line_idx]);
}

static inline bool valid_ppa(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static uint64_t hy_ssd_advance_status(struct hyssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct hyparams *spp = &ssd->sp;
    struct hyssd_ch *ch = get_ch(ssd, ppa);
    struct hyssd_lun *lun = get_lun(ssd, ppa);
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

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void hy_mark_page_invalid(struct hyssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct hyparams *spp = &ssd->sp;
    struct hyssd_blk *blk = NULL;
    struct hyssd_pg *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }
    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void hy_mark_page_valid(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyssd_blk *blk = NULL;
    struct hyssd_pg *pg = NULL;
    struct line *line;
    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void hy_mark_block_free(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyparams *spp = &ssd->sp;
    struct hyssd_blk *blk = get_blk(ssd, ppa);
    struct hyssd_pg *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void hy_gc_read_page(struct hyssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        hy_ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t hy_gc_write_page(struct hyssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct hyssd_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    if (!valid_lpn(ssd, lpn)) {
        fprintf(stdout, "lpn : %lu\n", lpn);
        ftl_assert(valid_lpn(ssd, lpn));
    }
    new_ppa = hy_get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    hy_mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    hy_ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        hy_ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;
    return 0;
}

static struct line *hy_select_victim_line(struct hyssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static void hy_clean_one_block(struct hyssd *ssd, struct ppa *ppa)
{
    struct hyparams *spp = &ssd->sp;
    struct hyssd_pg *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            hy_gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            hy_gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ssd->sp.gc_pgs += cnt;
    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void hy_mark_line_free(struct hyssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int hy_do_gc(struct hyssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct hyparams *spp = &ssd->sp;
    struct hyssd_lun *lunp;
    struct ppa ppa;
    int ch, lun, blk;
    uint64_t sublat, maxlat = 0;
    int ch_base = 0;
    int blk_base = 0;

    victim_line = hy_select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ssd->sp.gc_count++;

    ftl_log("GC-ing line:%d,vpc=%d, ipc=%d,victim=%d,full=%d,free=%d\n", victim_line->id,
              victim_line->vpc, victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    ch_base = (victim_line->id * CHS_PER_ZONE) % ssd->sp.nchs;
    blk_base = (victim_line->id / 8) * 2;

    /* copy back valid data */
    for (ch = 0; ch < CHS_PER_ZONE; ch++) {
        for (lun = 0; lun < WAYS_PER_ZONE; lun++) {
            for (blk = 0; blk < 2; blk++) {
                ppa.g.ch = ch_base + ch;
                ppa.g.lun = lun;
                ppa.g.pl = 0;
                ppa.g.blk = blk_base + blk;
                lunp = get_lun(ssd, &ppa);
                hy_clean_one_block(ssd, &ppa);
                hy_mark_block_free(ssd, &ppa);

                if (spp->enable_gc_delay) {
                    struct nand_cmd gce;
                    gce.type = GC_IO;
                    gce.cmd = NAND_ERASE;
                    gce.stime = 0;
                    sublat = hy_ssd_advance_status(ssd, &ppa, &gce);
                    maxlat = (sublat > maxlat) ? sublat : maxlat;
                }

                lunp->gc_endtime = lunp->next_lun_avail_time;
            }
        }
    }

    /* update line status */
    hy_mark_line_free(ssd, &ppa);
    ftl_log("Rzone GC success cnt : %d\n", ssd->sp.gc_count);
    return 0;
}

#if 0
static inline struct ssd_channel *hy_zns_get_ch_by_slba(struct ssd *ssd, uint64_t slba, struct ssdparams *spp)
{
    int C = spp->nchs;          //channel
    int L = spp->luns_per_ch;   //luns per channel
    int P = spp->pgs_per_blk;   //pgs per blk
    int S = spp->secs_per_pg;   //sectors per pg

    int ch = ( slba % (P*S*C*L) ) / (P*S*L);
    printf("**[LOG ch id] %d\n", ch);
    return &(ssd->ch[ch]);
}

static inline struct nand_lun *hy_zns_get_lun_by_slba(struct ssd *ssd, uint64_t slba, struct ssdparams *spp)
{
    int C = spp->nchs;          //channel
    int L = spp->luns_per_ch;   //luns per channel
    int P = spp->pgs_per_blk;   //pages per block
    int S = spp->secs_per_pg;   //sectors per page

    int lun = ( slba % (P*S*L) ) / (P*S);

    struct ssd_channel *ch = hy_zns_get_ch_by_slba(ssd, slba, spp);

    printf("**[LOG lun id] %d\n", lun);

    return &(ch->lun[lun]);
}
#endif

static inline NvmeZone *hy_zns_get_zone_by_slba(NvmeNamespace *ns, uint64_t slba)
{
    FemuCtrl *n = ns->ctrl;
    uint32_t zone_idx = hy_zns_zone_idx(ns, slba);

    assert(zone_idx < n->num_zones);
    return &n->zone_array[zone_idx];
}

static int hy_ssd_init_zone_geometry(NvmeNamespace *ns, Error **errp)
{
    fprintf(stdout, "hy_ssd_init_zone_geometry\n");
    FemuCtrl *n = ns->ctrl;
    uint64_t zone_size, zone_cap;
    uint32_t lbasz = 1 << hy_zns_ns_lbads(ns);
    struct hyparams *spp = &n->hyssd->sp;

    // zone size = #ch * #way * #pgs * sector size
    if (n->zone_size_bs) {
        zone_size = n->zone_size_bs;
    } else {
        zone_size = NVME_DEFAULT_ZONE_SIZE;
    }
    
    // 8 * 4 * 1024 * 128 * 512
    // zone_size = spp->nchs * spp->luns_per_ch * spp->pgs_per_blk * spp->secs_per_pg * spp->secsz;
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

    n->zone_size = zone_size / lbasz; // # of LBAs per zone
    n->zone_capacity = zone_cap / lbasz;
    n->num_zones = ns->size / lbasz / n->zone_size;
    n->num_rzone = n->hyssd->sp.num_rzones;
    n->hyssd->sp.pgs_per_zone = n->zone_size / n->hyssd->sp.secs_per_pg;
    
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

static void hy_ssd_init_zoned_state(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    uint64_t start = 0, zone_size = n->zone_size;
    uint64_t capacity = n->num_zones * zone_size;
    NvmeZone *zone;
    uint64_t num_rzone = n->num_rzone;
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

        if (i < num_rzone)
            zone->rnd = 1;
        else {
            zone->rnd = 0;
            zone->line_idx = i;
        }

        zone->dirty = 0;
        if (start + zone_size > capacity) {
            zone_size = capacity - start;
        }
        zone->d.zt = NVME_ZONE_TYPE_SEQ_WRITE;
        // zone->d.zt = NVME_ZONE_TYPE_RANDOM_WRITE;
        hy_zns_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
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

static void hy_ssd_init_zone_identify(FemuCtrl *n, NvmeNamespace *ns, int lba_index)
{
    NvmeIdNsZoned *id_ns_z;

    hy_ssd_init_zoned_state(ns);

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

// static void hy_zns_clear_zone(NvmeNamespace *ns, NvmeZone *zone)
// {
//     FemuCtrl *n = ns->ctrl;
//     uint8_t state;

//     zone->w_ptr = zone->d.wp;
//     state = hy_zns_get_zone_state(zone);
//     if (zone->d.wp != zone->d.zslba ||
//         (zone->d.za & NVME_ZA_ZD_EXT_VALID)) {
//         if (state != NVME_ZONE_STATE_CLOSED) {
//             hy_zns_set_zone_state(zone, NVME_ZONE_STATE_CLOSED);
//         }
//         hy_zns_aor_inc_active(ns);
//         QTAILQ_INSERT_HEAD(&n->closed_zones, zone, entry);
//     } else {
//         hy_zns_set_zone_state(zone, NVME_ZONE_STATE_EMPTY);
//     }
// }

// static void hy_zns_zoned_ns_shutdown(NvmeNamespace *ns)
// {
//     FemuCtrl *n = ns->ctrl;
//     NvmeZone *zone, *next;

//     QTAILQ_FOREACH_SAFE(zone, &n->closed_zones, entry, next) {
//         QTAILQ_REMOVE(&n->closed_zones, zone, entry);
//         hy_zns_aor_dec_active(ns);
//         hy_zns_clear_zone(ns, zone);
//     }
//     QTAILQ_FOREACH_SAFE(zone, &n->imp_open_zones, entry, next) {
//         QTAILQ_REMOVE(&n->imp_open_zones, zone, entry);
//         hy_zns_aor_dec_open(ns);
//         hy_zns_aor_dec_active(ns);
//         hy_zns_clear_zone(ns, zone);
//     }
//     QTAILQ_FOREACH_SAFE(zone, &n->exp_open_zones, entry, next) {
//         QTAILQ_REMOVE(&n->exp_open_zones, zone, entry);
//         hy_zns_aor_dec_open(ns);
//         hy_zns_aor_dec_active(ns);
//         hy_zns_clear_zone(ns, zone);
//     }

//     assert(n->nr_open_zones == 0);
// }

// static void hy_zns_ns_shutdown(NvmeNamespace *ns)
// {
//     FemuCtrl *n = ns->ctrl;
//     if (n->zoned) {
//         hy_zns_zoned_ns_shutdown(ns);
//     }
// }

// static void hy_zns_ns_cleanup(NvmeNamespace *ns)
// {
//     FemuCtrl *n = ns->ctrl;
//     if (n->zoned) {
//         g_free(n->id_ns_zoned);
//         g_free(n->zone_array);
//         g_free(n->zd_extensions);
//     }
// }

static void hy_zns_assign_zone_state(NvmeNamespace *ns, NvmeZone *zone,
                                  NvmeZoneState state)
{
    FemuCtrl *n = ns->ctrl;

    if (QTAILQ_IN_USE(zone, entry)) {
        switch (hy_zns_get_zone_state(zone)) {
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

    hy_zns_set_zone_state(zone, state);

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
static int hy_zns_aor_check(NvmeNamespace *ns, uint32_t act, uint32_t opn)
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

static uint16_t hy_zns_check_zone_state_for_write(NvmeZone *zone)
{
    uint16_t status;

    switch (hy_zns_get_zone_state(zone)) {
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

static uint16_t hy_zns_check_zone_write(FemuCtrl *n, NvmeNamespace *ns,
                                      NvmeZone *zone, uint64_t slba,
                                      uint32_t nlb, bool append)
{
    uint16_t status;

    if (unlikely((slba + nlb) > hy_zns_zone_wr_boundary(zone))) {
        status = NVME_ZONE_BOUNDARY_ERROR;
    } else {
        status = hy_zns_check_zone_state_for_write(zone);
    }

    if (status != NVME_SUCCESS) {
    } else {
        assert(hy_zns_wp_is_valid(zone));
        if (append) {
            if (unlikely(slba != zone->d.zslba)) {
                status = NVME_INVALID_FIELD;
            }
            if (hy_zns_l2b(ns, nlb) > (n->page_size << n->zasl)) {
                status = NVME_INVALID_FIELD;
            }
        } else if (unlikely(slba != zone->w_ptr)) {
            status = NVME_ZONE_INVALID_WRITE;
        }
    }

    return status;
}

static uint16_t hy_zns_check_zone_state_for_read(NvmeZone *zone)
{
    uint16_t status;

    switch (hy_zns_get_zone_state(zone)) {
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

static uint16_t hy_zns_check_zone_read(NvmeNamespace *ns, uint64_t slba,
                                    uint32_t nlb)
{
    FemuCtrl *n = ns->ctrl;
    NvmeZone *zone = hy_zns_get_zone_by_slba(ns, slba);
    uint64_t bndry = hy_zns_zone_rd_boundary(ns, zone);
    uint64_t end = slba + nlb;
    uint16_t status;

    status = hy_zns_check_zone_state_for_read(zone);
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
                status = hy_zns_check_zone_state_for_read(zone);
                if (status != NVME_SUCCESS) {
                    break;
                }
            } while (end > hy_zns_zone_rd_boundary(ns, zone));
        }
    }

    return status;
}

static void hy_zns_auto_transition_zone(NvmeNamespace *ns)
{
    FemuCtrl *n = ns->ctrl;
    NvmeZone *zone;

    if (n->max_open_zones &&
        n->nr_open_zones == n->max_open_zones) {
        zone = QTAILQ_FIRST(&n->imp_open_zones);
        if (zone) {
             /* Automatically close this implicitly open zone */
            QTAILQ_REMOVE(&n->imp_open_zones, zone, entry);
            hy_zns_aor_dec_open(ns);
            hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_CLOSED);
        }
    }
}

static uint16_t hy_zns_auto_open_zone(NvmeNamespace *ns, NvmeZone *zone)
{
    uint16_t status = NVME_SUCCESS;
    uint8_t zs = hy_zns_get_zone_state(zone);

    if (zs == NVME_ZONE_STATE_EMPTY) {
        hy_zns_auto_transition_zone(ns);
        status = hy_zns_aor_check(ns, 1, 1);
    } else if (zs == NVME_ZONE_STATE_CLOSED) {
        hy_zns_auto_transition_zone(ns);
        status = hy_zns_aor_check(ns, 0, 1);
    }

    return status;
}

static void hy_zns_finalize_zoned_write(NvmeNamespace *ns, NvmeRequest *req,
                                     bool failed)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeZone *zone;
    NvmeZonedResult *res = (NvmeZonedResult *)&req->cqe;
    uint64_t slba;
    uint32_t nlb;

    slba = le64_to_cpu(rw->slba);
    nlb = le16_to_cpu(rw->nlb) + 1;
    zone = hy_zns_get_zone_by_slba(ns, slba);

    zone->d.wp += nlb;

    if (failed) {
        res->slba = 0;
    }

    if (zone->d.wp == hy_zns_zone_wr_boundary(zone)) {
        switch (hy_zns_get_zone_state(zone)) {
        case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        case NVME_ZONE_STATE_EXPLICITLY_OPEN:
            hy_zns_aor_dec_open(ns);
            /* fall through */
        case NVME_ZONE_STATE_CLOSED:
            hy_zns_aor_dec_active(ns);
            /* fall through */
        case NVME_ZONE_STATE_EMPTY:
            hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_FULL);
            /* fall through */
        case NVME_ZONE_STATE_FULL:
            break;
        default:
            assert(false);
        }
    }
}

static uint64_t hy_zns_advance_zone_wp(NvmeNamespace *ns, NvmeZone *zone,
                                    uint32_t nlb)
{
    uint64_t result = zone->w_ptr;
    uint8_t zs;

    zone->w_ptr += nlb;

    if (zone->w_ptr < hy_zns_zone_wr_boundary(zone)) {
        zs = hy_zns_get_zone_state(zone);
        switch (zs) {
        case NVME_ZONE_STATE_EMPTY:
            hy_zns_aor_inc_active(ns);
            /* fall through */
        case NVME_ZONE_STATE_CLOSED:
            hy_zns_aor_inc_open(ns);
            hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_IMPLICITLY_OPEN);
        }
    }

    return result;
}

struct zns_zone_reset_ctx {
    NvmeRequest *req;
    NvmeZone    *zone;
};

static void hy_zns_aio_zone_reset_cb(NvmeRequest *req, NvmeZone *zone)
{
    NvmeNamespace *ns = req->ns;

    /* FIXME, We always assume reset SUCCESS */
    switch (hy_zns_get_zone_state(zone)) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        hy_zns_aor_dec_open(ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        hy_zns_aor_dec_active(ns);
        /* fall through */
    case NVME_ZONE_STATE_FULL:
        zone->w_ptr = zone->d.zslba;
        zone->d.wp = zone->w_ptr;
        zone->dirty = 0;
        hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_EMPTY);
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

static uint16_t hy_zns_open_zone(NvmeNamespace *ns, NvmeZone *zone,
                              NvmeZoneState state, NvmeRequest *req)
{
    uint16_t status;

    switch (state) {
    case NVME_ZONE_STATE_EMPTY:
        status = hy_zns_aor_check(ns, 1, 0);
        if (status != NVME_SUCCESS) {
            return status;
        }
        hy_zns_aor_inc_active(ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        status = hy_zns_aor_check(ns, 0, 1);
        if (status != NVME_SUCCESS) {
            if (state == NVME_ZONE_STATE_EMPTY) {
                hy_zns_aor_dec_active(ns);
            }
            return status;
        }
        hy_zns_aor_inc_open(ns);
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_EXPLICITLY_OPEN);
        /* fall through */
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t hy_zns_close_zone(NvmeNamespace *ns, NvmeZone *zone,
                               NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        hy_zns_aor_dec_open(ns);
        hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_CLOSED);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t hy_zns_finish_zone(NvmeNamespace *ns, NvmeZone *zone,
                                NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_EXPLICITLY_OPEN:
        /* fall through */
    case NVME_ZONE_STATE_IMPLICITLY_OPEN:
        hy_zns_aor_dec_open(ns);
        /* fall through */
    case NVME_ZONE_STATE_CLOSED:
        hy_zns_aor_dec_active(ns);
        /* fall through */
    case NVME_ZONE_STATE_EMPTY:
        zone->w_ptr = hy_zns_zone_wr_boundary(zone);
        zone->d.wp = zone->w_ptr;
        hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_FULL);
        /* fall through */
    case NVME_ZONE_STATE_FULL:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t hy_zns_reset_zone(NvmeNamespace *ns, NvmeZone *zone,
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

    hy_zns_aio_zone_reset_cb(req, zone);

    return NVME_SUCCESS;
}

static uint16_t hy_zns_offline_zone(NvmeNamespace *ns, NvmeZone *zone,
                                 NvmeZoneState state, NvmeRequest *req)
{
    switch (state) {
    case NVME_ZONE_STATE_READ_ONLY:
        hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_OFFLINE);
        /* fall through */
    case NVME_ZONE_STATE_OFFLINE:
        return NVME_SUCCESS;
    default:
        return NVME_ZONE_INVAL_TRANSITION;
    }
}

static uint16_t hy_zns_set_zd_ext(NvmeNamespace *ns, NvmeZone *zone)
{
    uint16_t status;
    uint8_t state = hy_zns_get_zone_state(zone);

    if (state == NVME_ZONE_STATE_EMPTY) {
        status = hy_zns_aor_check(ns, 1, 0);
        if (status != NVME_SUCCESS) {
            return status;
        }
        hy_zns_aor_inc_active(ns);
        zone->d.za |= NVME_ZA_ZD_EXT_VALID;
        hy_zns_assign_zone_state(ns, zone, NVME_ZONE_STATE_CLOSED);
        return NVME_SUCCESS;
    }

    return NVME_ZONE_INVAL_TRANSITION;
}

static uint16_t hy_zns_bulk_proc_zone(NvmeNamespace *ns, NvmeZone *zone,
                                   enum NvmeZoneProcessingMask proc_mask,
                                   op_handler_t op_hndlr, NvmeRequest *req)
{
    uint16_t status = NVME_SUCCESS;
    NvmeZoneState zs = hy_zns_get_zone_state(zone);
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

static uint16_t hy_zns_do_zone_op(NvmeNamespace *ns, NvmeZone *zone,
                               enum NvmeZoneProcessingMask proc_mask,
                               op_handler_t op_hndlr, NvmeRequest *req)
{
    FemuCtrl *n = ns->ctrl;
    NvmeZone *next;
    uint16_t status = NVME_SUCCESS;
    int i;

    if (!proc_mask) {
        status = op_hndlr(ns, zone, hy_zns_get_zone_state(zone), req);
    } else {
        if (proc_mask & NVME_PROC_CLOSED_ZONES) {
            QTAILQ_FOREACH_SAFE(zone, &n->closed_zones, entry, next) {
                status = hy_zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }
        if (proc_mask & NVME_PROC_OPENED_ZONES) {
            QTAILQ_FOREACH_SAFE(zone, &n->imp_open_zones, entry, next) {
                status = hy_zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }

            QTAILQ_FOREACH_SAFE(zone, &n->exp_open_zones, entry, next) {
                status = hy_zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }
        if (proc_mask & NVME_PROC_FULL_ZONES) {
            QTAILQ_FOREACH_SAFE(zone, &n->full_zones, entry, next) {
                status = hy_zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
                                             req);
                if (status && status != NVME_NO_COMPLETE) {
                    goto out;
                }
            }
        }

        if (proc_mask & NVME_PROC_READ_ONLY_ZONES) {
            for (i = 0; i < n->num_zones; i++, zone++) {
                status = hy_zns_bulk_proc_zone(ns, zone, proc_mask, op_hndlr,
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

static uint16_t hy_zns_get_mgmt_zone_slba_idx(FemuCtrl *n, NvmeCmd *c,
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

    *zone_idx = hy_zns_zone_idx(ns, *slba);
    assert(*zone_idx < n->num_zones);

    return NVME_SUCCESS;
}

// if new_rzone > old_rzone (grow), return the victim zone count
// if new_rzone < old_rzone (shrink), return the victim page count
static uint32_t hy_zns_get_num_victim(FemuCtrl *n, uint32_t old_rzone, uint32_t new_rzone)
{
    int start, end = 0;
    int result = 0;

    if (new_rzone > old_rzone) {
        start = old_rzone;
        end = new_rzone;

        while (start < end) {
            if (n->zone_array[start].dirty == 1)
                result++;
            start++;
        }
        return result;
    } else {
        start = new_rzone;
        end = old_rzone;
        uint32_t ppz = n->hyssd->sp.pgs_per_zone; // pages per zone
        uint32_t victim_slpn = start * ppz;
        uint32_t victim_elpn = end * ppz;

        while (victim_slpn < victim_elpn) {
            if (n->hyssd->maptbl[victim_slpn].ppa != UNMAPPED_PPA)
                result++;
            victim_slpn++;
        }
        return result;
    }
}

/* Handle zone state changes when the random region grows */
static uint16_t handle_zone_increase(FemuCtrl *n, uint32_t old_rzone, uint32_t new_rzone, 
                              struct nvme_victim_idx **vidx)
{
    int victim_report_idx = 0;
    NvmeZone *zones = n->zone_array;
    uint32_t victim_zone = 0;
    
    // find an available sequential zone and convert it to random
    for (victim_zone = old_rzone; victim_zone < new_rzone; ++victim_zone) {
        if (zones[victim_zone].dirty == 1) {
            femu_err("s-zone must be clean\n");
            return NVME_ZONE_INVAL_TRANSITION;
        }
        
        // switch the zone to random type
        zones[victim_zone].rnd = 1;
        zones[victim_zone].d.wp = zones[victim_zone].d.zslba;
        zones[victim_zone].w_ptr = zones[victim_zone].d.zslba;
        hy_zns_set_zone_state(&zones[victim_zone], NVME_ZONE_STATE_EMPTY);

        femu_log("zone #%2d is now r-zone\n", victim_zone);
    }
    return 0;
}

/* Handle zone state changes when the random region shrinks */
static int handle_zone_decrease(FemuCtrl *n, uint32_t new_rzone, uint32_t old_rzone)
{
    NvmeZone *zones = n->zone_array;
    struct hyparams *spp = &n->hyssd->sp;
    uint32_t victim_zone;
    uint32_t ppz = spp->pgs_per_zone;
    uint32_t victim_start_lpn, victim_end_lpn;
    struct ppa ppa;

    femu_log("ppz: %u\n", ppz);

    for (victim_zone = new_rzone; victim_zone < old_rzone; victim_zone++) {
        // check for valid pages in the zone before converting
        victim_start_lpn = victim_zone * ppz;
        victim_end_lpn = (victim_zone + 1) * ppz;
        
        femu_log("victim_zone: %u, victim_start_lpn: %u, victim_end_lpn: %u\n",
                victim_zone, victim_start_lpn, victim_end_lpn);
        for (uint32_t lpn = victim_start_lpn; lpn < victim_end_lpn; lpn++) {
            ppa = n->hyssd->maptbl[lpn]; // JM: use the existing helper instead.
            if (ppa.ppa != UNMAPPED_PPA) { // JM: verify it should be UNMAPPED here (interacts with DSM).
                fprintf(stdout, "Zone %u still has valid pages, cannot convert to sequential (lpn:%u)\n", victim_zone, lpn);
                return NVME_ZONE_INVAL_TRANSITION;
    }
        }
        
        // convert the random zone back to sequential
        zones[victim_zone].rnd = 0;
        zones[victim_zone].dirty = 0;
        zones[victim_zone].d.wp = zones[victim_zone].d.zslba;
        zones[victim_zone].w_ptr = zones[victim_zone].d.zslba;
        hy_zns_set_zone_state(&zones[victim_zone], NVME_ZONE_STATE_EMPTY);
        
        femu_log("zone #%2d is now s-zone\n", victim_zone);
    }

    return 0;
}

/* Line management */
static uint16_t handle_line_management(FemuCtrl *n, uint32_t old_rzone, uint32_t new_rzone, bool is_increase)
{
    struct line_mgmt *lm = &n->hyssd->lm;
    struct line *line = NULL;
    NvmeZone *zones = n->zone_array;
    uint32_t victim_zone;

    if (is_increase) {
        // JM: later size this by the max possible rzone pages instead of tt_lines --> saves memory
        for (victim_zone = old_rzone; victim_zone < new_rzone; victim_zone++) {
            line = &lm->lines[zones[victim_zone].line_idx];
            
            line->is_rnd = 1;
            line->pos = 0;
            line->ipc = 0;
            line->vpc = 0;

            QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
            lm->free_line_cnt++;
        }
    } else {
        for (victim_zone = new_rzone; victim_zone < old_rzone; victim_zone++) {
            line = QTAILQ_FIRST(&lm->free_line_list);
            if (!line) {
                femu_err("No free line available\n");
                return NVME_ZONE_INVAL_TRANSITION;
            }

            QTAILQ_REMOVE(&lm->free_line_list, line, entry);
            lm->free_line_cnt--;

            line->is_rnd = 0;
            line->pos = 0;
            line->ipc = 0;
            line->vpc = 0;
            zones[victim_zone].line_idx = line->id;
        }
    }

    return 0;
}

/*
 * Validate the mapping table after an rzone change.
 * Checks every rzone mapping and logs any mismatch.
 */
static int verify_mapping_tables(FemuCtrl *n)
{
    struct hyssd *ssd = n->hyssd;
    struct hyparams *spp = &ssd->sp;
    uint32_t num_rzone = n->num_rzone;
    uint32_t ppz = spp->pgs_per_zone;
    uint32_t max_lpn = num_rzone * ppz;
    uint64_t pgidx;
    int errors = 0;
    
    fprintf(stdout, "Verifying mapping tables for %u rzones (%u pages)\n",
            num_rzone, max_lpn);
    
    // validate the mapping table
    for (uint32_t lpn = 0; lpn < max_lpn; lpn++) {
        struct ppa ppa = ssd->maptbl[lpn];
        
        // skip unmapped entries
        if (ppa.ppa == UNMAPPED_PPA) {
            continue;
        }
        
        // compute pgidx
        pgidx = ppa2pgidx(ssd, &ppa);
        if (pgidx == UINT64_MAX) {
            fprintf(stdout, "Error: Invalid pgidx for lpn %u\n", lpn);
            errors++;
            continue;
        }
        
        // verify the reverse mapping
        // uint64_t mapped_lpn = ssd->rmap[pgidx];
        // if (mapped_lpn != lpn) {
        //     fprintf(stdout, "Error: Mapping inconsistency - lpn %u maps to pgidx %lu, "
        //             "but rmap[%lu] = %lu\n", lpn, pgidx, pgidx, mapped_lpn);
        //     errors++;
        // }
    }
    
    // validate the reverse mapping table
    // for (uint32_t pgidx = 0; pgidx < spp->tt_rpgs; pgidx++) {
    //     uint64_t lpn = ssd->rmap[pgidx];
        
    //     // skip unmapped entries
    //     if (lpn == INVALID_LPN) {
    //         continue;
    //     }
        
    //     // check lpn is in range
    //     if (lpn >= max_lpn) {
    //         fprintf(stdout, "Error: rmap[%u] points to invalid lpn %lu (max %u)\n",
    //                 pgidx, lpn, max_lpn - 1);
    //         errors++;
    //         continue;
    //     }
        
    //     // verify the forward mapping
    //     struct ppa ppa = ssd->maptbl[lpn];
    //     uint64_t calc_pgidx = ppa2pgidx(ssd, &ppa);
        
    //     if (calc_pgidx != pgidx) {
    //         fprintf(stdout, "Error: Reverse mapping inconsistency - "
    //                 "rmap[%u] = %lu, but ppa2pgidx(maptbl[%lu]) = %lu\n",
    //                 pgidx, lpn, lpn, calc_pgidx);
    //         errors++;
    //     }
    // }
    
    fprintf(stdout, "Mapping verification complete: %d errors found\n", errors);
    return errors;
}

static uint16_t hy_zns_modify_rzone(FemuCtrl *n, uint32_t old_rzone, uint32_t new_rzone, 
                              struct nvme_victim_idx **vidx) 
{
    uint16_t ret = 0;
    // struct line_mgmt *lm = &n->hyssd->lm;
    uint32_t ppz = n->hyssd->sp.pgs_per_zone;
    int r;
    
    // compute basic parameters
    // uint32_t new_rpgs = new_rzone * ppz;
    // uint32_t old_rpgs = old_rzone * ppz;

    if (new_rzone == old_rzone) {
        return NVME_SUCCESS;
    }

    if (new_rzone > old_rzone) {
        // random region grow - check there are enough clean sequential zones
        uint32_t available_zones = 0;
        for (int i = old_rzone; i < n->num_zones; i++) {
            if (n->zone_array[i].dirty == 0) {
                available_zones++;
            }
        }

        if (available_zones < (new_rzone - old_rzone)) {
            fprintf(stdout, "Not enough clean sequential zones available\n");
            return NVME_CAP_EXCEEDED;
        }
    } else if (new_rzone < old_rzone) {
        // random region shrink - run GC to migrate all data out
        while (hy_should_gc_high(n->hyssd)) {
            fprintf(stdout, "Running GC before reducing rzone count\n");
            r = hy_do_gc(n->hyssd, true);
            if (r == -1) {
                fprintf(stdout, "GC failed, cannot reduce rzone count\n");
                return NVME_CAP_EXCEEDED;
            }
        }

        // // check for valid pages in the rzone being converted
        // for (uint32_t lpn = new_rpgs; lpn < old_rpgs; lpn++) {
        //     if (n->ssd->maptbl[lpn].ppa != UNMAPPED_PPA) {
        //         fprintf(stdout, "Some pages in the zones to be converted are still valid\n");
        //         return NVME_ZONE_INVAL_TRANSITION;
        //     }
        // }
    }

    // update zone states
    if (new_rzone > old_rzone) {
        fprintf(stdout, "Increasing rzone count from %u to %u\n", old_rzone, new_rzone);
        ret = handle_zone_increase(n, old_rzone, new_rzone, vidx);
        if (ret) {
            fprintf(stdout, "Failed to increase rzone count: %u\n", ret);
            return ret;
        }
    } else if (new_rzone < old_rzone) {
        fprintf(stdout, "Decreasing rzone count from %u to %u\n", old_rzone, new_rzone);
        ret = handle_zone_decrease(n, new_rzone, old_rzone);
        if (ret) {
            fprintf(stdout, "Failed to decrease rzone count: %u\n", ret);
            return ret;
        }
    }

    // update line management
    ret = handle_line_management(n, old_rzone, new_rzone, new_rzone > old_rzone);
    if (ret) {
        fprintf(stdout, "Failed to update line management: %u\n", ret);
        return ret;
    }

    // update the total random page count
    n->hyssd->sp.tt_rpgs = new_rzone * ppz;

    if (verify_mapping_tables(n) > 0) {
        fprintf(stdout, "Warning: Mapping inconsistencies found after rzone modification\n");
    }

    return NVME_SUCCESS;
}

static uint16_t hy_zns_zone_mgmt_send(FemuCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t data_size = (le32_to_cpu(cmd->cdw12) + 1) << 2;
    uint32_t new_rzone = le32_to_cpu(cmd->cdw14); // # of rzones
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
    uint64_t lat;

    uint64_t curlat = 0, maxlat = 0;
    uint32_t lpn = 0, slpn = 0, elpn = 0;
    struct hyssd *hyssd = n->hyssd;
    uint32_t nchs = hyssd->sp.nchs;
    uint32_t nluns = hyssd->sp.luns_per_ch;
    struct ppa ppa;

    uint32_t num_victim = 0;
    struct nvme_victim_idx *vidx;
    void *buf;

    int tag = 0;

    action = dw13 & 0xff;
    all = dw13 & 0x100;

    req->status = NVME_SUCCESS;
    if (!all) {
        status = hy_zns_get_mgmt_zone_slba_idx(n, cmd, &slba, &zone_idx);
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
    case NVME_ZONE_ACTION_MODIFY_ZONE:
        status = hy_zns_modify_rzone(n, n->num_rzone, new_rzone, &vidx);
        if (status) {
            femu_err("hy_zns_zone_mgmt_send, status=0x%x\n", status);
            return NVME_SUCCESS;
        }
        n->num_rzone = new_rzone;
        break;

        num_victim = hy_zns_get_num_victim(n, n->num_rzone, new_rzone);
        buf = g_malloc0(data_size);

        ((struct nvme_modify_zone_report*)buf)->nr_victims = num_victim;

        if (new_rzone > n->num_rzone)
            ((struct nvme_modify_zone_report*)buf)->victim_type = 0;
        else
            ((struct nvme_modify_zone_report*)buf)->victim_type = 1;

        vidx = buf + sizeof(struct nvme_modify_zone_report);
        status = hy_zns_modify_rzone(n, n->num_rzone, new_rzone, &vidx);
        if (status) {
            femu_err("NVME_ZONE_ACTION_MODIFY_ZONE, status=0x%x\n", status);

            return NVME_SUCCESS;
        }
        n->num_rzone = new_rzone;

        status = dma_read_prp(n, (uint8_t *)buf, data_size, prp1, prp2);

        break;
    case NVME_ZONE_ACTION_OPEN:
        if (all) {
            proc_mask = NVME_PROC_CLOSED_ZONES;
        }
        status = hy_zns_do_zone_op(ns, zone, proc_mask, hy_zns_open_zone, req);
        break;
    case NVME_ZONE_ACTION_CLOSE:
        if (all) {
            proc_mask = NVME_PROC_OPENED_ZONES;
        }
        status = hy_zns_do_zone_op(ns, zone, proc_mask, hy_zns_close_zone, req);
        break;
    case NVME_ZONE_ACTION_FINISH:
        if (all) {
            proc_mask = NVME_PROC_OPENED_ZONES | NVME_PROC_CLOSED_ZONES;
        }

        hy_zns_get_mgmt_zone_slba_idx(n, cmd, &slba, &zone_idx);
        zone = &n->zone_array[zone_idx];
        uint64_t lbas_to_fill = n->zone_capacity - (zone->w_ptr - zone->d.zslba);

        // empty or full state
        if (lbas_to_fill == 0 || lbas_to_fill == n->zone_capacity) {
            break;
        }

        slba = zone->w_ptr;
        slpn = slba / hyssd->sp.secs_per_pg;
        elpn = (slba + lbas_to_fill - 1) / hyssd->sp.secs_per_pg;

        for (lpn = slpn; lpn <= elpn; lpn++) {
            // 2ch/8way
            ppa.g.ch = base_arr[tag] + (lpn % 2);
            ppa.g.lun = (lpn / 2) % nluns;

            struct nand_cmd swr;
            swr.type = USER_IO;
            swr.cmd = NAND_WRITE;
            swr.stime = req->stime;

            curlat = hy_ssd_advance_status(hyssd, &ppa, &swr);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }

        req->reqlat = maxlat;
        req->expire_time += maxlat;

        status = hy_zns_do_zone_op(ns, zone, proc_mask, hy_zns_finish_zone, req);
        break;
    case NVME_ZONE_ACTION_RESET:
        resets = (uintptr_t *)&req->opaque;

        // on zone reset
        // clear the zone's dirty flag
        // zone = hy_zns_get_zone_by_slba(ns, slba);
        // zone->dirty = 0;

        if (all) {
            proc_mask = NVME_PROC_OPENED_ZONES | NVME_PROC_CLOSED_ZONES |
                NVME_PROC_FULL_ZONES;
        }
        *resets = 1;

        uint32_t blks_per_zone = NVME_DEFAULT_ZONE_SIZE / (2 * 1024 * 1024);
        uint64_t curlat = 0;
        uint64_t lpn = 0;

        for (lpn = 0; lpn < blks_per_zone; lpn++) {
            // 2ch/8way
            ppa.g.ch = base_arr[tag] + (lpn % 2);
            ppa.g.lun = (lpn / 2) % nluns;

            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_ERASE;
            srd.stime = req->stime;

            curlat = hy_ssd_advance_status(hyssd, &ppa, &srd);
            maxlat = (curlat > maxlat) ? curlat : maxlat;
        }

        req->reqlat = maxlat;
        req->expire_time += maxlat;
        hyssd->sp.zr_count++;
        status = hy_zns_do_zone_op(ns, zone, proc_mask, hy_zns_reset_zone, req);
        (*resets)--;
        fprintf(stdout, "Szone RESET SUCCESS [slba : %ld]\n", slba);
        return NVME_SUCCESS;
    case NVME_ZONE_ACTION_OFFLINE:
        if (all) {
            proc_mask = NVME_PROC_READ_ONLY_ZONES;
        }
        status = hy_zns_do_zone_op(ns, zone, proc_mask, hy_zns_offline_zone, req);
        break;
    case NVME_ZONE_ACTION_SET_ZD_EXT:
        if (all || !n->zd_extension_size) {
            return NVME_INVALID_FIELD | NVME_DNR;
        }
        zd_ext = hy_zns_get_zd_extension(ns, zone_idx);
        status = dma_write_prp(n, (uint8_t *)zd_ext, n->zd_extension_size, prp1,
                               prp2);
        if (status) {
            return status;
        }
        status = hy_zns_set_zd_ext(ns, zone);
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

static bool hy_zns_zone_matches_filter(uint32_t zafs, NvmeZone *zl)
{
    NvmeZoneState zs = hy_zns_get_zone_state(zl);

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

static uint16_t hy_zns_zone_mgmt_recv(FemuCtrl *n, NvmeRequest *req)
{
    NvmeCmd *cmd = (NvmeCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    /* cdw12 is zero-based number of dwords to return. Convert to bytes */
    uint32_t data_size = (le32_to_cpu(cmd->cdw12) + 1) << 2;
    uint32_t dw13 = le32_to_cpu(cmd->cdw13);
    uint32_t new_rzone = le32_to_cpu(cmd->cdw14); // # of rzones

    uint32_t zone_idx, zra, zrasf, partial;
    uint64_t max_zones, nr_zones = 0;
    uint16_t status;
    uint64_t slba, capacity = hy_zns_ns_nlbas(ns);
    NvmeZoneDescr *z;
    NvmeZone *zone;
    NvmeZoneReportHeader *header;
    void *buf, *buf_p;
    size_t zone_entry_sz;

    req->status = NVME_SUCCESS;

    status = hy_zns_get_mgmt_zone_slba_idx(n, cmd, &slba, &zone_idx);
    if (status) {
        return status;
    }

    zra = dw13 & 0xff;
    if (zra == NVME_ZONE_REPORT_RZONE) {
        for (int i = 0; i < n->num_zones; ++i) {
            zone = &n->zone_array[i];
            // fprintf(stdout, "[%3d]w_ptr:%8x, dirty:%2d, rnd:%2d\n", i, zone->w_ptr, zone->dirty, zone->rnd);
            // fprintf(stdout, "     zt:%2d, zs:%2d, za:%2d, zcap:%08llx, zslba:%08llx, wp:%8llx\n", zone->d.zt, zone->d.zs, zone->d.za, zone->d.zcap, zone->d.zslba, zone->d.wp);
        }

        for (int i = 0; i < n->hyssd->lm.tt_lines; ++i) {
            struct line *line = &n->hyssd->lm.lines[i];
            // fprintf(stdout, "[%3d]is_rnd:%2d, pos:%2d, ipc:%2d, vpc:%2d\n", i, line->is_rnd, line->pos, line->ipc, line->vpc);
        }

        for (int lpn = 0; lpn < n->hyssd->sp.tt_rpgs; lpn += 32768) {
            // fprintf(stdout, "#%3d [%6d]ppa:%8x\n", lpn/32768, lpn, n->hyssd->maptbl[lpn].ppa);
        }

        // fprintf(stdout, "total rzone pages=%d\n", n->hyssd->sp.tt_rpgs);
        // fprintf(stdout, "total pages=%d\n", n->hyssd->sp.tt_pgs);
        // fprintf(stdout, "total lines=%d\n", n->hyssd->lm.tt_lines);

        // fprintf(stdout, "[J] rzones: %d\n", n->num_rzone);

        void *buf = NULL;

        buf = g_malloc0(data_size);
        ((struct nvme_rzone_report*)buf)->nr_rzones = n->num_rzone;

        status = dma_read_prp(n, (uint8_t*)buf, data_size, prp1, prp2);

        return NVME_SUCCESS;
    } else if (zra == NVME_ZONE_REPORT_VICTIM) {
        fprintf(stdout, "[J] report victim %d\n", new_rzone);
        uint32_t num_victim = 0;
        num_victim = hy_zns_get_num_victim(n, n->num_rzone, new_rzone);
        
        buf = g_malloc0(data_size);
        struct nvme_modify_zone_report *tmp_header = NULL;
        tmp_header = (struct nvme_modify_zone_report*)buf;
        tmp_header->nr_victims = num_victim;
        // printf("num victim : %lu\n", tmp_header->nr_victims);

        if (new_rzone > n->num_rzone) // rzone grow
            tmp_header->victim_type = 0;
        else // rzone shrink
            tmp_header->victim_type = 1;

        status = dma_read_prp(n, (uint8_t *)buf, data_size, prp1, prp2);

        return NVME_SUCCESS;
        // return hy_zns_report_victim();
    }

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
        if (hy_zns_zone_matches_filter(zrasf, zone++)) {
            nr_zones++;
        }
    }
    header = (NvmeZoneReportHeader *)buf;
    header->nr_zones = cpu_to_le64(nr_zones);

    buf_p = buf + sizeof(NvmeZoneReportHeader);
    for (; zone_idx < n->num_zones && max_zones > 0; zone_idx++) {
        zone = &n->zone_array[zone_idx];
        if (hy_zns_zone_matches_filter(zrasf, zone)) {
            z = (NvmeZoneDescr *)buf_p;
            buf_p += sizeof(NvmeZoneDescr);

            z->zt = zone->d.zt;
            z->zs = zone->d.zs;
            z->zcap = cpu_to_le64(zone->d.zcap);
            z->zslba = cpu_to_le64(zone->d.zslba);
            z->za = zone->d.za;

            if (hy_zns_wp_is_valid(zone)) {
                z->wp = cpu_to_le64(zone->d.wp);
            } else {
                z->wp = cpu_to_le64(~0ULL);
            }

            if (zra == NVME_ZONE_REPORT_EXTENDED) {
                if (zone->d.za & NVME_ZA_ZD_EXT_VALID) {
                    memcpy(buf_p, hy_zns_get_zd_extension(ns, zone_idx),
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

static inline uint16_t hy_zns_check_bounds(NvmeNamespace *ns, uint64_t slba,
                                        uint32_t nlb)
{
    uint64_t nsze = le64_to_cpu(ns->id_ns.nsze);

    if (unlikely(UINT64_MAX - slba < nlb || slba + nlb > nsze)) {
        return NVME_LBA_RANGE | NVME_DNR;
    }

    return NVME_SUCCESS;
}

static uint16_t hy_zns_map_dptr(FemuCtrl *n, size_t len, NvmeRequest *req)
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

static uint16_t hy_zns_do_write(FemuCtrl *n, NvmeRequest *req, bool append,
                             bool wrz)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    NvmeNamespace *ns = req->ns;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hy_zns_l2b(ns, nlb);
    uint64_t data_offset;
    NvmeZone *zone;
    NvmeZonedResult *res = (NvmeZonedResult *)&req->cqe;
    uint16_t status;

    if (!wrz) {
        status = nvme_check_mdts(n, data_size);
        if (status) {
            goto err;
        }
    }

    status = hy_zns_check_bounds(ns, slba, nlb);
    if (status) {
        goto err;
    }

    zone = hy_zns_get_zone_by_slba(ns, slba);

    status = hy_zns_check_zone_write(n, ns, zone, slba, nlb, append);
    if (status) {
        goto err;
    }

    status = hy_zns_auto_open_zone(ns, zone);
    if (status) {
        goto err;
    }

    if (append) {
        slba = zone->w_ptr;
    }

    res->slba = hy_zns_advance_zone_wp(ns, zone, nlb);

    data_offset = hy_zns_l2b(ns, slba);

    if (!wrz) {
        status = hy_zns_map_dptr(n, data_size, req);
        if (status) {
            goto err;
        }

        backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);
    }

    hy_zns_finalize_zoned_write(ns, req, false);
    return NVME_SUCCESS;

err:
    fprintf(stderr, "****************Append Failed***************\n");
    return status | NVME_DNR;
}

static uint16_t hyssd_id_dev(FemuCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    struct hyssd *hyssd = n->hyssd;
    struct hyparams *spp = &hyssd->sp;
    nvme_device_info dev_data;
    uint16_t status;

    memset(&dev_data, 0, sizeof(nvme_device_info));

    // common device info
    dev_data.type = 2;

    // device-type-specific info (matches struct field order)
    dev_data.hyssd.secsz = spp->secsz;                // sector size (bytes)
    dev_data.hyssd.secs_per_pg = spp->secs_per_pg;    // sectors per page
    dev_data.hyssd.pgs_per_blk = spp->pgs_per_blk;    // NAND pages per block
    dev_data.hyssd.blks_per_pl = spp->blks_per_pl;    // blocks per plane
    dev_data.hyssd.pls_per_lun = spp->pls_per_lun;    // planes per LUN
    dev_data.hyssd.luns_per_ch = spp->luns_per_ch;    // LUNs per channel
    dev_data.hyssd.nchs = spp->nchs;                  // channels per SSD
    dev_data.hyssd.secs_per_blk = spp->secs_per_blk;  // sectors per block
    dev_data.hyssd.secs_per_pl = spp->secs_per_pl;    // sectors per plane
    dev_data.hyssd.secs_per_lun = spp->secs_per_lun;  // sectors per LUN
    dev_data.hyssd.secs_per_ch = spp->secs_per_ch;    // sectors per channel
    dev_data.hyssd.tt_secs = spp->tt_secs;            // total sectors
    dev_data.hyssd.pgs_per_pl = spp->pgs_per_pl;      // pages per plane
    dev_data.hyssd.pgs_per_lun = spp->pgs_per_lun;    // pages per LUN
    dev_data.hyssd.pgs_per_ch = spp->pgs_per_ch;      // pages per channel
    dev_data.hyssd.tt_pgs = spp->tt_pgs;              // total pages
    dev_data.hyssd.blks_per_lun = spp->blks_per_lun;  // blocks per LUN
    dev_data.hyssd.blks_per_ch = spp->blks_per_ch;    // blocks per channel
    dev_data.hyssd.tt_blks = spp->tt_blks;            // total blocks
    dev_data.hyssd.secs_per_line = spp->secs_per_line; // sectors per line
    dev_data.hyssd.pgs_per_line = spp->pgs_per_line;  // pages per line
    dev_data.hyssd.blks_per_line = spp->blks_per_line; // blocks per line
    dev_data.hyssd.tt_lines = spp->tt_lines;          // total lines
    dev_data.hyssd.pls_per_ch = spp->pls_per_ch;      // planes per channel
    dev_data.hyssd.tt_pls = spp->tt_pls;              // total planes
    dev_data.hyssd.tt_luns = spp->tt_luns;            // total LUNs
    dev_data.hyssd.lbas_per_zone = n->zone_size;      // LBAs per zone
    dev_data.hyssd.pgs_per_zone = spp->pgs_per_zone;  // pages per zone
    dev_data.hyssd.tt_rpgs = spp->tt_rpgs;            // total random-zone pages

    // transfer to host
    status = dma_read_prp(n, (uint8_t *)&dev_data, sizeof(nvme_device_info), prp1, prp2);
    
    return status;
}

static uint16_t hy_zns_mon_hyssd(FemuCtrl *n, NvmeCmd *cmd)
{
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t data_size = (dw10 + 1) << 2;
    struct hyssd *hyssd = n->hyssd;
    struct line_mgmt *lm = &hyssd->lm;
    struct write_pointer *wpp = &hyssd->wp;
    uint32_t num_zones = n->num_zones;
    NvmeZone *zones = n->zone_array;
    uint32_t num_rzones = n->num_rzone;
    
    /* Calculate header size and total size */
    size_t header_size = sizeof(hyssd_write_pointer_info) + 
                         sizeof(uint64_t) + sizeof(uint64_t) + /* GC */
                         sizeof(uint32_t) + /* tt_lines field */
                         sizeof(uint32_t) + /* free_line_cnt field */
                         sizeof(uint32_t) + /* victim_line_cnt field */
                         sizeof(uint32_t) + /* full_line_cnt field */
                         sizeof(uint32_t) + /* num_zones field */
                         sizeof(uint64_t);  /* zr_count field */
    
    size_t line_info_size = lm->tt_lines * sizeof(hyssd_line_info);
    size_t zone_info_size = num_zones * sizeof(hyssd_zone_info);
    size_t total_size = header_size + line_info_size + zone_info_size;
    
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
    hyssd_write_pointer_info *wp_info = (hyssd_write_pointer_info *)ptr;
    wp_info->curline_id = wpp->curline ? wpp->curline->id : -1;
    wp_info->ch = wpp->ch;
    wp_info->lun = wpp->lun;
    wp_info->pg = wpp->pg;
    wp_info->blk = wpp->blk;
    wp_info->pl = wpp->pl;
    ptr += sizeof(hyssd_write_pointer_info);
    
    uint64_t *gc_count_ptr = (uint64_t *)ptr;
    *gc_count_ptr = hyssd->sp.gc_count;
    ptr += sizeof(uint64_t);
    
    uint64_t *gc_pgs_ptr = (uint64_t *)ptr;
    *gc_pgs_ptr = hyssd->sp.gc_pgs;
    ptr += sizeof(uint64_t);

    /* 2. Line management info (just the header fields) */
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
    
    /* 3. Number of zones */
    uint32_t *num_zones_ptr = (uint32_t *)ptr;
    *num_zones_ptr = num_zones;
    ptr += sizeof(uint32_t);

    uint64_t *zr_count_ptr = (uint64_t *)ptr;
    *zr_count_ptr = hyssd->sp.zr_count;
    ptr += sizeof(uint64_t);
    
    /* 4. Line info array */
    hyssd_line_info *line_infos = (hyssd_line_info *)ptr;
    for (int i = 0; i < lm->tt_lines; i++) {
        struct line *line = &lm->lines[i];
        line_infos[i].id = line->id;
        line_infos[i].ipc = line->ipc;
        line_infos[i].vpc = line->vpc;
        line_infos[i].pos = (unsigned long long)line->pos;
        line_infos[i].is_rnd = line->is_rnd;
        line_infos[i].ridx = 0; // JM: remove this
    }
    ptr += line_info_size;
    
    /* 5. Zone info array */
    hyssd_zone_info *zone_infos = (hyssd_zone_info *)ptr;
    for (int i = 0; i < num_zones; i++) {
        NvmeZone *zone = &zones[i];
        
        /* Basic zone descriptor info */
        zone_infos[i].d.zslba = zone->d.zslba;
        zone_infos[i].d.wp = zone->d.wp;
        zone_infos[i].d.zcap = zone->d.zcap;
        zone_infos[i].d.zt = zone->d.zt;
        zone_infos[i].d.zs = zone->d.zs;
        zone_infos[i].d.za = zone->d.za;
        
        /* Additional zone info */
        zone_infos[i].w_ptr = zone->w_ptr;
        zone_infos[i].dirty = zone->dirty;
        zone_infos[i].rnd = (i < num_rzones) ? 1 : 0;
    }
    
    /* Send data to host */
    uint16_t status = dma_read_prp(n, (uint8_t *)buf, data_size, prp1, prp2);
    
    g_free(buf);
    return status;
}

static uint16_t hy_zns_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case DEVICE_TYPE_OPCODE:
        return hyssd_id_dev(n, cmd);
    case NVME_ADM_CMD_MON_HYSSD:
        return hy_zns_mon_hyssd(n, cmd);
case NVME_ADM_CMD_FORMAT_NVM:
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t hy_zns_zone_append(FemuCtrl *n, NvmeRequest *req)
{
    // if(true){
    //   return hy_zns_do_write(n, req, true, false);
    // }
    return 1;
}

static uint16_t hy_zns_check_dulbe(NvmeNamespace *ns, uint64_t slba, uint32_t nlb)
{
    return NVME_SUCCESS;
}

static uint16_t hy_ssd_read(struct hyssd *ssd, NvmeNamespace *ns, NvmeRequest *req)     
{
    NvmeRwCmd *rw = (NvmeRwCmd*)&(req->cmd);
    struct hyparams *spp = &ssd->sp;
    uint64_t slba = le64_to_cpu(rw->slba);
    int nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;

    struct ppa ppa;
    uint64_t start_lpn = slba / spp->secs_per_pg;
    uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t curlat = 0;
    uint64_t maxlat = 0;

    fprintf(stdout, "hy_ssd_read()\n");

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = hy_get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            // printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            // printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            // ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }
        
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        curlat = hy_ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }
    
    req->reqlat = maxlat;
    req->expire_time += maxlat;

    ssd->n->cns_read_cnt++;
    ssd->n->cns_read_lat += maxlat;

    nvme_rw(ssd->n, ns, rw, req);
    
    fprintf(stdout, "Rzone READ SUCCESS [slba : %ld, nlb : %d lat : %lu, acclat : %lu]\n", 
                        slba, nlb, maxlat, ssd->n->cns_read_lat / ssd->n->cns_read_cnt);
    return NVME_SUCCESS;
}

static uint16_t hy_ssd_write(struct hyssd *ssd, NvmeNamespace *ns, NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd*) &(req->cmd);
    struct hyparams *spp = &ssd->sp;
    uint64_t slba = le64_to_cpu(rw->slba);
    int nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t start_lpn = slba / spp->secs_per_pg;
    uint64_t end_lpn = (slba + nlb - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    fprintf(stdout, "hy_ssd_write()\n");
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("Rzone start_lpn=%"PRIu64",tt_rpgs=%d\n", start_lpn, ssd->sp.tt_rpgs);
    }

    while (hy_should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = hy_do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = hy_get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            hy_mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }
        
        /* new write */
        ppa = hy_get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);
        
        hy_mark_page_valid(ssd, &ppa);
        
        /* need to advance the write pointer here */
        hy_ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        curlat = hy_ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    req->reqlat = maxlat;
    req->expire_time += maxlat;

    ssd->n->cns_write_cnt++;
    ssd->n->cns_write_lat += maxlat;

    nvme_rw(ssd->n, ns, rw, req);

    fprintf(stdout, "Rzone WRITE SUCCESS [slba : %ld, nlb : %d, lat : %lu, acclat : %lu]\n", 
                    slba, nlb, maxlat, ssd->n->cns_write_lat / ssd->n->cns_write_cnt);
    return NVME_SUCCESS;
}

static uint16_t hy_zns_read(FemuCtrl *n, NvmeNamespace *ns, NvmeRwCmd *cmd,
                            NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hy_zns_l2b(ns, nlb);
    uint64_t data_offset;
    uint16_t status;
    uint64_t lat;
    uint64_t curlat = 0;
    uint64_t maxlat = 0;
    uint32_t nluns = n->hyssd->sp.luns_per_ch;
    uint32_t zone_idx = hy_zns_zone_idx(ns, slba);

    assert(n->zoned);
    req->is_write = false;

    status = nvme_check_mdts(n, data_size);
    if (status) {
        goto err;
    }

    status = hy_zns_check_bounds(ns, slba, nlb);
    if (status) {
        goto err;
    }

    status = hy_zns_check_zone_read(ns, slba, nlb);
    if (status) {
        goto err;
    }

    status = hy_zns_map_dptr(n, data_size, req);
    if (status) {
        goto err;
    }

    if (NVME_ERR_REC_DULBE(n->features.err_rec)) {
        status = hy_zns_check_dulbe(ns, slba, nlb);
        if (status) {
            goto err;
        }
    }

    data_offset = hy_zns_l2b(ns, slba);
    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    struct ppa ppa;
    uint32_t lpn = 0, slpn = 0, elpn = 0;
    struct hyssd *hyssd = n->hyssd;

    slpn = slba / hyssd->sp.secs_per_pg;
    elpn = (slba + nlb - 1) / hyssd->sp.secs_per_pg;
    int tag = zone_idx % 8;

    for (lpn = slpn; lpn <= elpn; lpn++) {
        // 2ch/8way
        ppa.g.ch = base_arr[tag] + (lpn % 2);
        ppa.g.lun = (lpn / 2) % nluns;

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_READ;
        swr.stime = req->stime;

        curlat = hy_ssd_advance_status(hyssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    req->reqlat = maxlat;
    req->expire_time += maxlat;

    n->zns_read_cnt++;
    n->zns_read_lat += maxlat; 

    // fprintf(stdout, "Szone READ SUCCESS [slba : %ld, nlb : %d lat : %lu, acclat : %lu]\n", 
    //                     slba, nlb, maxlat, n->zns_read_lat / n->zns_read_cnt);
    return NVME_SUCCESS;
err:
    femu_err("SZONE READ FAILED [slba : %ld, nlb : %d]n", slba, nlb);
    return status | NVME_DNR;
}

static uint16_t hy_zns_write(FemuCtrl *n, NvmeNamespace *ns, NvmeRwCmd *cmd,
                             NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd *)cmd;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t data_size = hy_zns_l2b(ns, nlb);
    uint64_t data_offset;
    NvmeZone *zone;
    NvmeZonedResult *res = (NvmeZonedResult *)&req->cqe;
    uint16_t status;
    uint64_t maxlat = 0;
    uint64_t curlat = 0;
    uint32_t err_idx = 0;
    struct hyssd *hyssd = n->hyssd;
    uint32_t nluns = hyssd->sp.luns_per_ch;

    assert(n->zoned);
    req->is_write = true;

    uint32_t zone_idx = hy_zns_zone_idx(ns, slba);

    status = nvme_check_mdts(n, data_size);
    if (status) {
        err_idx = 0;
        goto err;
    }

    status = hy_zns_check_bounds(ns, slba, nlb);
    if (status) {
        err_idx = 1;
        goto err;
    }

    zone = hy_zns_get_zone_by_slba(ns, slba);

    status = hy_zns_check_zone_write(n, ns, zone, slba, nlb, false);
    if (status) {
        err_idx = 2;
        goto err;
    }

    status = hy_zns_auto_open_zone(ns, zone);
    if (status) {
        err_idx = 3;
        goto err;
    }
    
    data_offset = hy_zns_l2b(ns, slba);
    status = hy_zns_map_dptr(n, data_size, req);
    if (status) {
        err_idx = 4;
        goto err;
    }

    backend_rw(n->mbe, &req->qsg, &data_offset, req->is_write);

    if (zone->dirty == 0) {
        zone->dirty = 1;
    }

    struct ppa ppa;
    uint32_t lpn = 0, slpn = 0, elpn = 0;
    int tag = zone_idx % 8;

    slpn = slba / hyssd->sp.secs_per_pg;
    elpn = (slba + nlb - 1) / hyssd->sp.secs_per_pg;

    for (lpn = slpn; lpn <= elpn; lpn++) {
        // 2ch/8way
        ppa.g.ch = base_arr[tag] + (lpn % 2);
        ppa.g.lun = (lpn / 2) % nluns;

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;

        curlat = hy_ssd_advance_status(hyssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    req->reqlat = maxlat;
    req->expire_time += maxlat;
    
    n->zns_write_cnt++;
    n->zns_write_lat += maxlat; 

    res->slba = hy_zns_advance_zone_wp(ns, zone, nlb);
    hy_zns_finalize_zoned_write(ns, req, false);

    // fprintf(stdout, "Szone WRITE SUCCESS [slba : %ld, nlb : %d lat : %lu, acclat : %lu]\n", 
    //                 slba, nlb, maxlat, n->zns_write_lat / n->zns_write_cnt);
    return NVME_SUCCESS;

err:
    femu_err("*********ZONE WRITE FAILED [ZIDX : %d SLBA : %lu NLB : %u WP : %lu] ER IDX : %d *********\n", hy_zns_zone_idx(ns, slba), slba, nlb, zone->w_ptr, err_idx);
    return status | NVME_DNR;
}

static uint16_t hy_zns_discard(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
    NvmeRequest *req)
{
    uint32_t dw10 = le32_to_cpu(cmd->cdw10);
    uint32_t dw11 = le32_to_cpu(cmd->cdw11);
    uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
    uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
    int i;
    struct hyssd *ssd = n->hyssd;
    struct hyparams *spp = &ssd->sp;

    if (!(dw11 & NVME_DSMGMT_AD)) {
        return NVME_SUCCESS;
    }

    uint16_t nr = (dw10 & 0xff) + 1;
    uint64_t slba;
    uint32_t nlb;
    NvmeDsmRange *range = g_malloc0(sizeof(NvmeDsmRange) * nr);

    uint64_t lpn;
    if (dma_write_prp(n, (uint8_t *)range, sizeof(NvmeDsmRange) * nr, prp1, prp2)) {
        femu_err("failed to dma_write_prp in discard\n");
        g_free(range);
        return NVME_INVALID_FIELD | NVME_DNR;
    }

    uint32_t total_invalidated = 0;
    uint32_t total_skipped = 0;

    for (i = 0; i < nr; ++i) {
        slba = le64_to_cpu(range[i].slba);
        nlb = le32_to_cpu(range[i].nlb);
        if (slba + nlb > le64_to_cpu(ns->id_ns.nsze)) {
            femu_err("DISCARD: range[%d] out of bounds, slba=%"PRIu64", nlb=%u, nsze=%"PRIu64"\n",
                    i, slba, nlb, le64_to_cpu(ns->id_ns.nsze));
            g_free(range);
            return NVME_LBA_RANGE | NVME_DNR;
        }

        struct ppa ppa;
        uint32_t invalidated = 0;
        uint32_t skipped = 0;
        uint64_t lpn_start = slba / spp->secs_per_pg;
        uint64_t lpn_end = (slba + nlb) / spp->secs_per_pg;

        for (lpn = lpn_start; lpn < lpn_end; lpn++) {
            /* lpn outside the rzone range is not in maptbl; skip */
            if (lpn >= spp->tt_rpgs) {
                skipped += (lpn_end - lpn);
                break;
            }
            ppa = hy_get_maptbl_ent(ssd, lpn);
            if (mapped_ppa(&ppa)) {
                hy_mark_page_invalid(ssd, &ppa);
                set_rmap_ent(ssd, INVALID_LPN, &ppa);
                ssd->maptbl[lpn].ppa = UNMAPPED_PPA;
                invalidated++;
            }
        }
        femu_log("DISCARD[%d/%d]: slba=%"PRIu64", nlb=%u, zone=%"PRIu64
        ", lpn=[%"PRIu64",%"PRIu64"), invalidated=%u, skipped=%u\n",
        i + 1, nr, slba, nlb, slba / n->zone_size,
        lpn_start, lpn_end, invalidated, skipped);


        total_invalidated += invalidated;
        total_skipped += skipped;
    }
    g_free(range);

    if (nr > 1) {
        femu_log("DISCARD DONE: nr=%u, total_invalidated=%u, total_skipped=%u\n",
                nr, total_invalidated, total_skipped);
    }

    return NVME_SUCCESS;
}

// static uint16_t hy_zns_discard(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
//     NvmeRequest *req)
// {
//     uint32_t dw10 = le32_to_cpu(cmd->cdw10);
//     uint32_t dw11 = le32_to_cpu(cmd->cdw11);
//     uint64_t prp1 = le64_to_cpu(cmd->dptr.prp1);
//     uint64_t prp2 = le64_to_cpu(cmd->dptr.prp2);
//     int i;
//     struct hyssd *ssd = n->hyssd;
//     struct hyparams *spp = &ssd->sp;

//     uint16_t nr = (dw10 & 0xff) + 1;
//     uint64_t slba;
//     uint32_t nlb;
//     NvmeDsmRange *range = g_malloc0(sizeof(NvmeDsmRange) * nr);

//     uint64_t lpn;
//     if (dma_write_prp(n, (uint8_t *)range, sizeof(*range), prp1, prp2)) {
//         printf("failed to dma_write_prp\n");
//         // g_free(range);
//     }

//     req->status = NVME_SUCCESS;

//     for (i = 0; i < nr; ++i) {
//         slba = le64_to_cpu(range[i].slba);
//         nlb = le32_to_cpu(range[i].nlb);
//         // printf("JM: #DSM\nslba: %lu, nlb: %u, zidx: %u\n", slba, nlb, hy_zns_zone_idx(ns, slba));
//         // printf("JM: ns->id_ns.nsze: %lu\n", le64_to_cpu(ns->id_ns.nsze));
//         if (slba + nlb > le64_to_cpu(ns->id_ns.nsze)) {
//             femu_err("slba + nlb > ns->id_ns.nsze\n");
//             req->status = NVME_LBA_RANGE | NVME_DNR;
//             break;
//         }

//         struct ppa ppa;

//         // double-check slba + nlb.
//         // switch to a shift later.
//         // fprintf(stdout, "dicard from %lu to %lu\n", slba / spp->secs_per_pg, (slba + nlb) / spp->secs_per_pg);

//         for (lpn = slba/spp->secs_per_pg; lpn < (slba + nlb)/spp->secs_per_pg; lpn++) {
//             ppa = hy_get_maptbl_ent(ssd, lpn);
//             if (mapped_ppa(&ppa)) {
//                 /* update old page information first */
//                 hy_mark_page_invalid(ssd, &ppa);
//                 set_rmap_ent(ssd, INVALID_LPN, &ppa);
//                 ssd->maptbl[lpn].ppa = UNMAPPED_PPA;
//             }
//         }
//     }
//     g_free(range);
    
//     // fprintf(stdout, "DISCARD SUCCESS\n");
//     return NVME_SUCCESS;
// }

static uint16_t hy_zns_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    NvmeRwCmd *rw = (NvmeRwCmd*)cmd; 
    // NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    // uint64_t data_offset;
    uint64_t zone_size = n->zone_size;
    uint64_t slba = le64_to_cpu(rw->slba);
    uint32_t nlb = (uint32_t)le16_to_cpu(rw->nlb) + 1;
    uint64_t rzone_limit = n->hyssd->sp.num_rzones * zone_size;
    uint16_t res = NVME_SUCCESS;

    switch (cmd->opcode) {
    case NVME_CMD_READ:
        if(slba + nlb <= rzone_limit)
            res = hy_ssd_read(n->hyssd, ns, req);
        else
            res = hy_zns_read(n, ns, cmd, req);
        break;
    case NVME_CMD_WRITE:
        if(slba + nlb <= rzone_limit)
            res = hy_ssd_write(n->hyssd, ns, req);
        else
            res = hy_zns_write(n, ns, cmd, req);
        break;
    case NVME_CMD_ZONE_MGMT_SEND:
        res = hy_zns_zone_mgmt_send(n, req);
        break;
    case NVME_CMD_ZONE_MGMT_RECV:
        res = hy_zns_zone_mgmt_recv(n, req);
        break;
    case NVME_CMD_ZONE_APPEND:
        res = hy_zns_zone_append(n, req);
        break;
    case NVME_CMD_DSM:
        res = hy_zns_discard(n, ns, cmd, req);
    }

    /* clean one line if needed (in the background) */
    if (hy_should_gc_high(n->hyssd)) {
        hy_do_gc(n->hyssd, false);
    }

    return res;
}

static void hy_ssd_set_ctrl_str(FemuCtrl *n)
{
    static int fsid_zns = 0;
    const char *zns_mn = "FEMU HYSSD(BB+ZNS.ver) Controller";

    const char *zns_sn = "vHYSSD";

    nvme_set_ctrl_name(n, zns_mn, zns_sn, &fsid_zns);
}

static void hy_ssd_set_ctrl(FemuCtrl *n)
{
    uint8_t *pci_conf = n->parent_obj.config;

    n->hyssd->dataplane_started_ptr = &n->dataplane_started;

    hy_ssd_set_ctrl_str(n);
    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_INTEL);
    pci_config_set_device_id(pci_conf, 0x5845);
}

static int hy_ssd_init_zone_cap(FemuCtrl *n)
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

static int hy_zns_start_ctrl(FemuCtrl *n)
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

static void hy_ssd_init(FemuCtrl *n, Error **errp)
{
    NvmeNamespace *ns = &n->namespaces[0];

    struct hyssd *hyssd = g_malloc0(sizeof(struct hyssd));
    n->hyssd = hyssd;
    hyssd->n = n;

    hy_ssd_set_ctrl(n);

    hy_ssd_init_conv(n);
    
    hy_ssd_init_zone_cap(n);
    
    if (hy_ssd_init_zone_geometry(ns, errp) != 0) {
        return;
    }
    
    hy_ssd_init_zone_identify(n, ns, 0);

    n->zns_read_cnt = 0;
    n->zns_read_lat = 0;
    n->zns_write_cnt = 0;
    n->zns_write_lat = 0;

    n->cns_read_cnt = 0;
    n->cns_read_lat = 0;
    n->cns_write_cnt = 0;
    n->cns_write_lat = 0;

    fprintf(stdout, "***SSD Structure***\n");
    fprintf(stdout, "nchs : %u\n", n->hyssd->sp.nchs);
    fprintf(stdout, "luns_per_ch : %u\n", n->hyssd->sp.luns_per_ch);
    fprintf(stdout, "pls_per_lun : %u\n", n->hyssd->sp.pls_per_lun);
    fprintf(stdout, "blks_per_pl : %u\n", n->hyssd->sp.blks_per_pl);
    fprintf(stdout, "pgs_per_blk : %u\n", n->hyssd->sp.pgs_per_blk);
    fprintf(stdout, "secs_per_pg : %u\n", n->hyssd->sp.secs_per_pg);

    fprintf(stdout, "***SSD Latency***\n");
    fprintf(stdout, "pg_rd_lat : %d\n", n->hyssd->sp.pg_rd_lat);
    fprintf(stdout, "pg_wr_lat : %d\n", n->hyssd->sp.pg_wr_lat);
    fprintf(stdout, "blk_er_lat : %d\n", n->hyssd->sp.blk_er_lat);
    fprintf(stdout, "ch_xfer_lat : %d\n", n->hyssd->sp.ch_xfer_lat);
}

static void hy_ssd_exit(FemuCtrl *n)
{
    /*
     * Release any extra resource (zones) allocated for ZNS mode
     */
}

// int nvme_register_hyssd(FemuCtrl *n)
// {
//     n->ext_ops = (FemuExtCtrlOps) {
//         .state            = NULL,
//         .init             = hy_ssd_init,
//         .exit             = hy_ssd_exit,
//         .rw_check_req     = NULL,
//         .start_ctrl       = hy_zns_start_ctrl,
//         .admin_cmd        = hy_zns_admin_cmd,
//         .io_cmd           = hy_zns_io_cmd,
//         .get_log          = NULL,
//     };

//     return 0;
// }
