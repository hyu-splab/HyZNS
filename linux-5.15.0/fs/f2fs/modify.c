/*
 * fs/f2fs/modify.c
 *
 * Copyright (c) 2025 Software Platforms Lab.,
 *             https://splab.hanyang.ac.kr/
 *
 * Online CNS<->ZNS resize: moves the device ABA (ModifyZone) and gates the
 * FS usable area to match. Entry points: f2fs_resize_cns() (ioctl path),
 * f2fs_aux_gate_to_aba() (mount time), f2fs_aux_modify().
 */
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "segment.h"
#include "modify.h"
#include "f2fs_exp.h"

int f2fs_modify_zone(struct f2fs_sb_info *sbi, __u32 new_rzone) {
    struct block_device *bdev = sbi->sb->s_bdev;
    struct bio *bio;
    sector_t zone_sectors;
    int ret;

    bio = bio_alloc(GFP_KERNEL, 0);
    if (!bio)
        return -ENOMEM;

    /* Wire the new ABA as a real LBA (= new first S-zone's zslba),
     * not a zone count. The NVMe driver maps bi_sector directly to
     * SLBA for the ModifyZone action, consistent with the standard
     * ZSA wire encoding. */
    zone_sectors = bdev_zone_sectors(bdev);
    bio_set_dev(bio, bdev);
    bio->bi_opf = REQ_OP_ZONE_MODIFY | REQ_SYNC;
    bio->bi_iter.bi_sector = (sector_t)new_rzone * zone_sectors;

    ret = submit_bio_wait(bio);
    bio_put(bio);

    return ret;
}

int f2fs_update_super_info(struct f2fs_sb_info *sbi, __u32 new_rzone)
{
    struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
    struct block_device *bdev = sbi->sb->s_bdev;
    block_t new_end_blkaddr;
    int ret = 0, i;

    /* Take zone geometry from the device itself, not from segs_per_sec */
    unsigned int zone_sectors = bdev_zone_sectors(bdev);
    unsigned int zone_blocks = SECTOR_TO_BLOCK(zone_sectors);
    unsigned int segs_per_zone = zone_blocks >> sbi->log_blocks_per_seg;
    unsigned int total_zones = bdev_nr_sectors(bdev) / zone_sectors;

    down_write(&sbi->sb_lock);

    /* end_blkaddr of device 0 for the new R-zone count */
    new_end_blkaddr = (block_t)new_rzone * zone_blocks - 1;

    /* TODO: revisit commit vs. in-memory update ordering w.r.t. recovery */

    f2fs_info(sbi, "modify: zone_blocks=%u, segs_per_zone=%u, total_zones=%u\n",
              zone_blocks, segs_per_zone, total_zones);

    /* Update every device entry */
    for (i = 0; i < sbi->s_ndevs; i++) {
        if (i == 0) {
            FDEV(i).total_segments = (new_rzone - 1) * segs_per_zone;
            FDEV(i).start_blk = 0;
            FDEV(i).end_blk = new_end_blkaddr;
        } else {
            FDEV(i).start_blk = FDEV(i-1).end_blk + 1;
            FDEV(i).total_segments = (total_zones - new_rzone) * segs_per_zone;
            FDEV(i).end_blk = FDEV(i).start_blk +
                (FDEV(i).total_segments << sbi->log_blocks_per_seg) - 1;
        }
        raw_super->devs[i].total_segments =
                        cpu_to_le32(FDEV(i).total_segments);
        f2fs_info(sbi, "dev[%d] start_blkaddr = %llu, end_blkaddr = %llu, total_segments = %u\n",
                i, FDEV(i).start_blk, FDEV(i).end_blk, FDEV(i).total_segments);
    }

    /* Commit the superblock */
    ret = f2fs_commit_super(sbi, false);

    printk("JM: f2fs_commit_super() ret = %d\n", ret);

    if (ret) {
        f2fs_err(sbi, "Failed to commit super blocks");
        set_sbi_flag(sbi, SBI_NEED_FSCK);
    } else {
        set_sbi_flag(sbi, SBI_IS_DIRTY);
    }

    up_write(&sbi->sb_lock);
    return ret;
}

/*
 * In-kernel ReportABA (vendor ZRA 0x21 / BLKREPORTABA): returns the
 * device's current R-zone count, or a negative errno. Mirrors the bio in
 * blkdev_report_rzone_ioctl(). dm-hyzns answers this locally from its r_end.
 */
static int f2fs_aux_query_rzone(struct f2fs_sb_info *sbi)
{
    struct block_device *bdev = sbi->sb->s_bdev;
    struct bio *bio;
    __u32 *buffer;
    int ret;

    buffer = kzalloc(sizeof(__u32), GFP_KERNEL);
    if (!buffer)
        return -ENOMEM;

    bio = bio_alloc(GFP_KERNEL, 1);
    if (!bio) {
        kfree(buffer);
        return -ENOMEM;
    }
    if (bio_add_page(bio, virt_to_page(buffer), sizeof(__u32),
                     offset_in_page(buffer)) != sizeof(__u32)) {
        bio_put(bio);
        kfree(buffer);
        return -ENOMEM;
    }
    bio_set_dev(bio, bdev);
    bio->bi_opf = REQ_OP_ZONE_RRZONE | REQ_SYNC;
    bio->bi_iter.bi_sector = 0;

    ret = submit_bio_wait(bio);
    bio_put(bio);
    if (!ret)
        ret = (int)*buffer;     /* current R-zone count */
    kfree(buffer);
    return ret;
}

/*
 * Convert an R-zone count to the number of usable MAIN sections for the aux
 * F2FS. usable main = [main_blkaddr, rzone * zone_blocks), floored to whole
 * sections. (ABA is zone-aligned by ModifyZone; main is section-aligned by
 * mkfs. Validate alignment at runtime.)
 */
static __u32 f2fs_aux_rzone_to_main_secs(struct f2fs_sb_info *sbi, __u32 rzone)
{
    struct block_device *bdev = sbi->sb->s_bdev;
    unsigned int zone_blocks = SECTOR_TO_BLOCK(bdev_zone_sectors(bdev));
    block_t main_blkaddr = le32_to_cpu(F2FS_RAW_SUPER(sbi)->main_blkaddr);
    block_t r_blocks = (block_t)rzone * zone_blocks;

    if (r_blocks <= main_blkaddr)
        return 0;
    return (__u32)((r_blocks - main_blkaddr) / BLKS_PER_SEC(sbi));
}

/*
 * f2fs_aux_gate_to_aba - mount-time gate of a max-provisioned aux F2FS down
 * to the current ABA. Called from f2fs_fill_super() after the segment
 * manager is built. No-op unless the volume carries F2FS_FEATURE_AUX_MAXPROV
 * and the device's current R is smaller than the provisioned maximum.
 */
int f2fs_aux_gate_to_aba(struct f2fs_sb_info *sbi)
{
    int rzone;
    __u32 target;

    if (!f2fs_sb_has_aux_maxprov(sbi))
        return 0;

    rzone = f2fs_aux_query_rzone(sbi);
    if (rzone <= 0) {
        f2fs_warn(sbi, "aux maxprov: ReportABA failed (%d); not gating", rzone);
        return 0;
    }
    target = f2fs_aux_rzone_to_main_secs(sbi, rzone);
    if (!target || target >= MAIN_SECS(sbi))
        return 0;       /* already == provisioned max: nothing to gate */

    return f2fs_aux_gate_init(sbi, target);
}

/*
 * f2fs_aux_modify - runtime re-gate of a max-provisioned aux F2FS to a new
 * R-zone count, after the device ABA has been moved (ModifyZone). Called from
 * the modify ioctls instead of the multi-device f2fs_update_super_info().
 */
int f2fs_aux_modify(struct f2fs_sb_info *sbi, __u32 new_rzone)
{
    __u32 target = f2fs_aux_rzone_to_main_secs(sbi, new_rzone);

    if (!target)
        return -EINVAL;
    return f2fs_aux_resize(sbi, target);
}

/*
 * f2fs_resize_cns - direction-aware orchestration of an online
 * CNS<->ZNS area resize for a max-provisioned aux F2FS.
 *
 * The ordering of the FS-side usable-area gate (f2fs_aux_resize) and the
 * device-side ABA change (f2fs_modify_zone) MUST differ by direction:
 *
 *   SHRINK (target < current usable): gate the FS usable area DOWN first.
 *     f2fs_aux_resize() drains the released tail (free_segment_range), lowers
 *     MAIN_SECS so the allocator can no longer hand the tail out, and briefly
 *     freeze_super()s for a consistent checkpoint -- all BEFORE the device
 *     reclassifies the tail R-zones as S-zones. This closes the race where a
 *     concurrent writer could re-dirty the tail between the device ABA change
 *     and the FS gate (the old "gc_force then modify" path left that window).
 *
 *   GROW (target > current usable): move the device ABA UP first so the new
 *     R-zones physically exist, then grow the FS usable area into them.
 *
 * One ioctl performs the whole resize; no separate gc_force is needed.
 * Non-maxprov volumes keep the legacy (device-then-super-update) path.
 */
int f2fs_resize_cns(struct f2fs_sb_info *sbi, __u32 new_rzone)
{
    __u32 target, cur;
    int ret;

    if (!f2fs_sb_has_aux_maxprov(sbi)) {
        /* legacy multi-device path: device ABA, then super info */
        ret = f2fs_modify_zone(sbi, new_rzone);
        if (ret)
            return ret;
        return f2fs_update_super_info(sbi, new_rzone);
    }

    target = f2fs_aux_rzone_to_main_secs(sbi, new_rzone);
    cur = MAIN_SECS(sbi);
    if (!target)
        return -EINVAL;
    if (target == cur)
        return 0;   /* no change */

    if (target < cur) {
        /* SHRINK: FS gate-down (drain + gate + freeze + ckpt) BEFORE device */
        f2fs_rz_begin(true, cur - target);
        ret = f2fs_aux_resize(sbi, target);
        if (ret) {
            f2fs_err(sbi, "modify(shrink): aux gate-down failed (%d)", ret);
            f2fs_rz_end();
            return ret;
        }
        {
            u64 _d = f2fs_rz_now();
            ret = f2fs_modify_zone(sbi, new_rzone);
            f2fs_rz_dev_aba(f2fs_rz_now() - _d);
        }
        if (ret)
            f2fs_err(sbi, "modify(shrink): device ABA change failed (%d)", ret);
        f2fs_rz_end();
        return ret;
    }

    /* GROW: device ABA up first, then FS grows into the new R-zones */
    f2fs_rz_begin(false, target - cur);
    {
        u64 _d = f2fs_rz_now();
        ret = f2fs_modify_zone(sbi, new_rzone);
        f2fs_rz_dev_aba(f2fs_rz_now() - _d);
    }
    if (ret) {
        f2fs_err(sbi, "modify(grow): device ABA change failed (%d)", ret);
        f2fs_rz_end();
        return ret;
    }
    ret = f2fs_aux_resize(sbi, target);
    if (ret)
        f2fs_err(sbi, "modify(grow): aux gate-up failed (%d)", ret);
    f2fs_rz_end();
    return ret;
}
