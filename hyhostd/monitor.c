/* monitor.c - read live R_C (dm-hyhost) and R_Z (FS | report-zones). */
#define _GNU_SOURCE
#include "hyhostd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * R_C: dm-hyhost via `dmsetup status <dm>`
 *
 *   0 <len> hyhost ... r_end=<sec> line_pblocks=<p> zone_pblocks=<z> ... free_pages=<n> ... nospc=<n>
 *
 * A zone (1 GiB) is 16 lines (64 MiB), so zone_sectors = zone_pblocks*8
 * (NOT line_pblocks*8), and cur_r = r_end/zone_sectors. R_C = free CNS capacity
 * in zones = free_pages/zone_pblocks (free_pages = real free blocks; unlike
 * free_lines it is not pinned by fragmentation). Older dm builds omit
 * zone_pblocks (line == zone there), so fall back to line_pblocks.
 * With --mock-dm the line is read from a file instead (offline policy test).
 * ------------------------------------------------------------------------- */
int hyd_read_dm(const hyd_cfg *cfg, hyd_state *st)
{
	char line[4096] = {0};

	if (cfg->mock_dm[0]) {
		FILE *f = fopen(cfg->mock_dm, "r");
		if (!f) return -1;
		if (!fgets(line, sizeof line, f)) { fclose(f); return -1; }
		fclose(f);
	} else {
		char cmd[256];
		snprintf(cmd, sizeof cmd, "dmsetup status %s 2>/dev/null", cfg->dm);
		if (hyd_run_capture(cmd, line, sizeof line) != 0 || !line[0])
			return -1;
	}

	long r_end       = hyd_kv_lookup(line, DM_KEY_R_END);
	long line_pblk   = hyd_kv_lookup(line, DM_KEY_LINE_PBLOCKS);
	long zone_pblk   = hyd_kv_lookup(line, DM_KEY_ZONE_PBLOCKS);
	long free_lines  = hyd_kv_lookup(line, DM_KEY_FREE_LINES);
	long valid_pages = hyd_kv_lookup(line, DM_KEY_VALID_PAGES);
	long bv_vpc      = hyd_kv_lookup(line, DM_KEY_BEST_VICTIM_VPC);
	long free_pages  = hyd_kv_lookup(line, DM_KEY_FREE_PAGES);
	long gc_blocked  = hyd_kv_lookup(line, DM_KEY_GC_BLOCKED);
	long nospc       = hyd_kv_lookup(line, DM_KEY_NOSPC);

	if (r_end < 0 || line_pblk <= 0 || free_lines < 0)
		return -1;
	/* Older dm builds omit zone_pblocks; there line == zone == 1 GiB. */
	if (zone_pblk <= 0)
		zone_pblk = line_pblk;

	/* dm target length (2nd token: "0 <len> hyhost ..."): the S-region ends
	 * here, NOT at the physical device end, so R_Z must not count zones past it. */
	unsigned long long dev_sectors = 0;
	sscanf(line, "%*llu %llu", &dev_sectors);

	uint32_t zone_sectors = (uint32_t)zone_pblk * HYHOST_PAGE_SECTORS;
	st->dev_sectors  = dev_sectors;
	st->zone_sectors = zone_sectors;
	st->zone_pblocks = (uint32_t)zone_pblk;
	st->line_pblocks = (uint32_t)line_pblk;
	st->cur_r        = (uint32_t)((uint64_t)r_end / zone_sectors);   /* true zones */
	st->free_lines   = (uint32_t)free_lines;                          /* raw (display) */
	/* R_C = free CNS capacity in ZONES. Prefer free_pages (real free blocks);
	 * fall back to free_lines converted to zones if the dm predates it. */
	if (free_pages >= 0)
		st->r_c  = (uint32_t)((uint64_t)free_pages / (uint32_t)zone_pblk);
	else
		st->r_c  = (uint32_t)((uint64_t)free_lines * (uint32_t)line_pblk
		                      / (uint32_t)zone_pblk);
	st->valid_pages  = valid_pages < 0 ? 0 : (uint64_t)valid_pages;
	st->free_pages   = free_pages < 0 ? 0 : (uint64_t)free_pages;
	st->gc_blocked   = gc_blocked > 0;
	st->best_victim_vpc = bv_vpc < 0 ? 0 : (uint32_t)bv_vpc;
	st->nospc        = nospc < 0 ? 0 : (uint64_t)nospc;
	st->dm_ok        = true;
	return 0;
}

/* ------------------------------------------------------------------------- *
 * R_Z (fs): ZenFS publishes <aux>/.hyzns_status with `free_zones=<n>`.
 *   F2FS has no zone-granular free query exposed; fs-source falls back to
 *   report (logged by the caller). Returns -1 if the status file is absent.
 * ------------------------------------------------------------------------- */
static int read_rz_fs(const hyd_cfg *cfg, hyd_state *st)
{
	if (cfg->fs != FS_ZENFS)
		return -1;                       /* only ZenFS has a precise publisher */

	char path[512];
	snprintf(path, sizeof path, "%s/%s", cfg->aux, ZENFS_STATUS_FILE);
	FILE *f = fopen(path, "r");
	if (!f) return -1;

	char buf[4096];
	long free_zones = -1;
	while (fgets(buf, sizeof buf, f)) {
		long v = hyd_kv_lookup(buf, "free_zones");
		if (v >= 0) { free_zones = v; break; }
	}
	fclose(f);
	if (free_zones < 0) return -1;

	st->r_z  = (uint32_t)free_zones;
	st->rz_ok = true;
	return 0;
}

/* one zone descriptor line -> (slba, is_empty). Handles two formats:
 *   blkzone:  "  start: 0x.., len .., ... zcond: 1(em) [type: ..]"  (preferred:
 *             returns ALL zones with no count arg; clean text condition)
 *   nvme-cli: "SLBA: 0x..  ..  State: EMPTY|0x10  Type: .."        (state may be
 *             text or the raw ZNS condition byte; nvme needs an exact -d count,
 *             so blkzone is the default, but nvme is still parsed for --mock-rz)
 * returns true if the line was a zone descriptor. */
static bool parse_zone_line(const char *line, uint64_t *slba, bool *empty)
{
	char *p;
	if ((p = strstr(line, "start:")) != NULL) {           /* blkzone */
		*slba = strtoull(p + 6, NULL, 0);
		char *z = strstr(line, "zcond:");
		long cond = z ? strtol(z + 6, NULL, 0) : -1;       /* 1 = ZSE (empty) */
		*empty = (cond == 1) || (z && strstr(z, "(em)"));
		return true;
	}
	if ((p = strstr(line, "SLBA:")) != NULL) {            /* nvme-cli */
		char *s = strstr(line, "State:");
		if (!s) return false;
		*slba = strtoull(p + 5, NULL, 0);
		char state[32] = {0};
		sscanf(s + 6, "%31s", state);
		*empty = (strncmp(state, "EMPTY", 5) == 0);
		if (!*empty) {
			char *e; long sv = strtol(state, &e, 0);
			if (e != state && ((sv >> 4) == 1 || sv == 1)) *empty = true;
		}
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- *
 * R_Z (report): count free (EMPTY) S-region zones (zslba >= ABA) from
 *   `blkzone report <backing>` (all zones, no count arg). --mock-rz reads the
 *   report text from a file (offline test; accepts blkzone or nvme format).
 * ------------------------------------------------------------------------- */
static int read_rz_report(const hyd_cfg *cfg, hyd_state *st)
{
	if (!st->dm_ok || st->zone_sectors == 0)
		return -1;                       /* need ABA from the dm read first */

	uint64_t aba_lba = (uint64_t)st->cur_r * st->zone_sectors;

	FILE *f;
	if (cfg->mock_rz[0]) {
		f = fopen(cfg->mock_rz, "r");
	} else {
		char cmd[256];
		snprintf(cmd, sizeof cmd,
		         "blkzone report %s 2>/dev/null", cfg->backing);
		f = popen(cmd, "r");
	}
	if (!f) return -1;

	char buf[4096];
	uint32_t free_z = 0;
	bool any = false;
	/* This instance's S-region is [ABA, s_end_zone) when set (a shared
	 * device must not count the other instance's zones), else [ABA, dev_sectors). */
	uint64_t s_end = st->dev_sectors ? st->dev_sectors : (uint64_t)-1;
	if (cfg->s_end_zone && st->zone_sectors)
		s_end = (uint64_t)cfg->s_end_zone * st->zone_sectors;
	while (fgets(buf, sizeof buf, f)) {
		uint64_t slba; bool empty;
		if (!parse_zone_line(buf, &slba, &empty)) continue;
		any = true;
		if (slba >= aba_lba && slba < s_end && empty)
			free_z++;
	}
	if (cfg->mock_rz[0]) fclose(f); else pclose(f);

	if (!any) return -1;
	st->r_z   = free_z;
	st->rz_ok = true;
	return 0;
}

int hyd_read_rz(const hyd_cfg *cfg, hyd_state *st)
{
	if (cfg->rz_source == RZ_FS) {
		if (read_rz_fs(cfg, st) == 0)
			return 0;
		/* fs source unavailable (F2FS, or status file missing) -> report */
	}
	return read_rz_report(cfg, st);
}
