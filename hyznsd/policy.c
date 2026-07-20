/* policy.c - decide grow/shrink (paper §5.6.3 FTR) and execute it.
 *
 * FTR (File System-triggered CNS Area Resizing) is the only policy. Per poll,
 * with R_C = remaining CNS capacity in BLOCKS (dm free_pages), R_Z =
 * remaining ZNS zones, S_Z = zone size:
 *   grow   +1 zone  iff  R_C < ftr_grow_rc*S_Z    and  R_Z >= ftr_grow_rz
 *   shrink -1 zone  iff  R_C > ftr_shrink_rc*S_Z  and  R_Z < ftr_shrink_rz
 * Paper defaults 2/5/3/4, administrator-configurable via hyznsd.conf
 * (ftr_grow_rc/ftr_grow_rz/ftr_shrink_rc/ftr_shrink_rz). Targets clamp to
 * [r_min, r_max]. Cooldown/backoff are enforced by the main loop around
 * hyd_act().
 */
#define _GNU_SOURCE
#include "hyznsd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

hyd_decision hyd_decide(const hyd_cfg *cfg, const hyd_state *st)
{
	hyd_decision d = { ACT_NONE, st->cur_r, "steady (within hysteresis band)", -1 };

	if (!st->dm_ok || !st->rz_ok) {
		d.reason = "state unavailable (dm/rz read failed)";
		goto out;
	}

	/* Paper FTR (§5.6.3). gc_urgent is enabled only when a resize fires
	 * (delayed-discard mitigation before ResizeCNS); ResizeCNS itself
	 * checkpoints so the GC'd segments' discards reach dm. */
	uint64_t zone = st->zone_pblocks ? st->zone_pblocks : 1;  /* S_Z = pages/ZONE (1 GiB) */
	static char why[96];   /* daemon is single-threaded */
	if (st->free_pages < (uint64_t)cfg->ftr_grow_rc * zone &&
	    st->r_z >= cfg->ftr_grow_rz) {
		if (cfg->r_max && st->cur_r + 1 > cfg->r_max) {
			d.reason = "FTR grow wanted but blocked (r_max)";
		} else {
			d.action = ACT_GROW;   d.target_r = st->cur_r + 1;
			snprintf(why, sizeof why, "FTR grow(+1): R_C<%u*S_Z && R_Z>=%u",
			         cfg->ftr_grow_rc, cfg->ftr_grow_rz);
			d.reason = why;
		}
	} else if (st->free_pages > (uint64_t)cfg->ftr_shrink_rc * zone &&
	           st->r_z < cfg->ftr_shrink_rz) {
		if (st->cur_r <= cfg->r_min) {
			d.reason = "FTR shrink wanted but blocked (r_min)";
		} else {
			d.action = ACT_SHRINK; d.target_r = st->cur_r - 1;
			snprintf(why, sizeof why, "FTR shrink(-1): R_C>%u*S_Z && R_Z<%u",
			         cfg->ftr_shrink_rc, cfg->ftr_shrink_rz);
			d.reason = why;
		}
	}
	/* gc_urgent: two independently-switchable sources (hyznsd.conf).
	 *  - ftr_gcu_resize: while a resize fires (flush delayed discards
	 *    before ResizeCNS; its checkpoint completes them).
	 *  - ftr_gcu_park: while dm GC is parked (victims ~fully valid),
	 *    force F2FS GC+discard so dm gains invalid blocks and the park
	 *    auto-releases. */
	d.gc_urgent = ((cfg->ftr_gcu_resize && d.action != ACT_NONE) ||
	               (cfg->ftr_gcu_park && st->gc_blocked)) ? 1 : 0;
	if (d.action == ACT_NONE && d.gc_urgent)
		d.reason = "steady, but dm GC parked -> gc_urgent (park pulse)";

out:
	return d;
}

/* ------------------------------------------------------------------------- *
 * Executors. Return 0 on success, -1 on failure; resmsg gets a short note.
 * ------------------------------------------------------------------------- */
static int act_f2fs(const hyd_cfg *cfg, uint32_t target, char *resmsg, size_t n)
{
	/* the ioctl needs an O_RDWR fd on the FS; keep a tiny probe file. */
	char probe[512];
	snprintf(probe, sizeof probe, "%s/.hyznsd_probe", cfg->mnt);
	FILE *pf = fopen(probe, "a");
	if (pf) fclose(pf);

	/* The RESIZE_CNS ioctl is spelled `resize_cns` in current f2fs-tools but
	 * `modify_zone` in older builds; both drive F2FS_IOC_RESIZE_CNS. Try the
	 * current name, and on "Unknown command" fall back so the daemon works
	 * regardless of which f2fs_io is installed. */
	const char *bin = cfg->f2fs_io[0] ? cfg->f2fs_io : "f2fs_io";
	const char *verbs[] = { "resize_cns", "modify_zone" };
	char cmd[1024], out[2048] = {0};
	int rc = -1;
	for (size_t vi = 0; vi < sizeof verbs / sizeof verbs[0]; vi++) {
		out[0] = 0;
		snprintf(cmd, sizeof cmd, "%s %s %u %s 2>&1", bin, verbs[vi], target, probe);
		rc = hyd_run_capture(cmd, out, sizeof out);
		out[strcspn(out, "\n")] = 0;                 /* strip trailing newline */
		if (rc == 0 && strstr(out, "Success")) {
			snprintf(resmsg, n, "f2fs_io %s OK (R=%u)", verbs[vi], target);
			return 0;
		}
		if (!strstr(out, "Unknown command"))         /* real failure, not a name miss */
			break;
	}
	snprintf(resmsg, n, "f2fs_io FAIL rc=%d: %s", rc, out[0] ? out : "(no output)");
	return -1;
}

static int act_zenfs(const hyd_cfg *cfg, uint32_t target, char *resmsg, size_t n)
{
	char req[512], tmp[512], ack[512];
	snprintf(req, sizeof req, "%s/%s", cfg->aux, ZENFS_RESIZE_REQ);
	snprintf(tmp, sizeof tmp, "%s/%s.tmp", cfg->aux, ZENFS_RESIZE_REQ);
	snprintf(ack, sizeof ack, "%s/%s", cfg->aux, ZENFS_RESIZE_ACK);

	unlink(ack);                                     /* clear any stale ack */

	FILE *f = fopen(tmp, "w");
	if (!f) { snprintf(resmsg, n, "zenfs req open fail"); return -1; }
	fprintf(f, "%u\n", target);
	fflush(f); fclose(f);
	if (rename(tmp, req) != 0) {                      /* atomic publish */
		snprintf(resmsg, n, "zenfs req rename fail");
		return -1;
	}

	/* poll for the executor's ack */
	uint64_t deadline = hyd_now_ms() + cfg->ack_timeout_ms;
	for (;;) {
		FILE *af = fopen(ack, "r");
		if (af) {
			unsigned got = 0; char verdict[16] = {0};
			int k = fscanf(af, "%u %15s", &got, verdict);
			fclose(af);
			unlink(ack);
			if (k >= 2 && strncmp(verdict, "OK", 2) == 0) {
				snprintf(resmsg, n, "zenfs OK (R=%u)", got);
				return 0;
			}
			snprintf(resmsg, n, "zenfs %s (R=%u)",
			         k >= 2 ? verdict : "BADACK", got);
			return -1;
		}
		if (hyd_now_ms() >= deadline) {
			snprintf(resmsg, n, "zenfs ack timeout (%ums)", cfg->ack_timeout_ms);
			return -1;
		}
		usleep(20 * 1000);
	}
}

/* fs=f2fs + coord=ctree: 2-phase boundary handshake around the F2FS resize.
 * ctree owns the ZNS S-zones and writes to them raw, so we must have ctree free
 * the boundary zones BEFORE we move the ABA (f2fs_io), then confirm.  Control
 * files under cfg->aux:
 *   (1) publish .hyzns_prepare = target (atomic temp+rename)
 *   (2) wait .hyzns_prepare.ack = "READY <K>" | "BUSY"
 *   (3) on READY: run the F2FS resize ourselves (act_f2fs)
 *   (4) write .hyzns_commit = "COMMIT" | "ABORT" so ctree finalizes/rolls back */
static int act_f2fs_coord(const hyd_cfg *cfg, uint32_t target, char *resmsg, size_t n)
{
	char prep[512], ptmp[512], pack[512], commit[512], ctmp[512];
	snprintf(prep,   sizeof prep,   "%s/.hyzns_prepare", cfg->aux);
	snprintf(ptmp,   sizeof ptmp,   "%s/.hyzns_prepare.tmp", cfg->aux);
	snprintf(pack,   sizeof pack,   "%s/.hyzns_prepare.ack", cfg->aux);
	snprintf(commit, sizeof commit, "%s/.hyzns_commit", cfg->aux);
	snprintf(ctmp,   sizeof ctmp,   "%s/.hyzns_commit.tmp", cfg->aux);

	unlink(pack);                                    /* clear any stale ack */

	/* (1) publish prepare */
	FILE *f = fopen(ptmp, "w");
	if (!f) { snprintf(resmsg, n, "ctree prepare open fail"); return -1; }
	fprintf(f, "%u\n", target);
	fflush(f); fclose(f);
	if (rename(ptmp, prep) != 0) {
		snprintf(resmsg, n, "ctree prepare rename fail"); return -1;
	}

	/* (2) wait READY | BUSY.  BUSY = ctree froze nothing → just back off. */
	uint64_t deadline = hyd_now_ms() + cfg->ack_timeout_ms;
	char verdict[16] = {0}; unsigned k_frozen = 0;
	for (;;) {
		FILE *af = fopen(pack, "r");
		if (af) {
			int k = fscanf(af, "%15s %u", verdict, &k_frozen);
			fclose(af); unlink(pack);
			if (k >= 1 && strncmp(verdict, "READY", 5) == 0) break;
			snprintf(resmsg, n, "ctree prepare %s", k >= 1 ? verdict : "BADACK");
			return -1;
		}
		if (hyd_now_ms() >= deadline) {
			snprintf(resmsg, n, "ctree prepare ack timeout (%ums)", cfg->ack_timeout_ms);
			return -1;
		}
		usleep(20 * 1000);
	}

	/* (3) WE move the ABA + grow F2FS. */
	int grow_ok = (act_f2fs(cfg, target, resmsg, n) == 0);

	/* (4) tell ctree COMMIT | ABORT (atomic temp+rename) */
	FILE *cf = fopen(ctmp, "w");
	if (cf) {
		fprintf(cf, "%s\n", grow_ok ? "COMMIT" : "ABORT");
		fflush(cf); fclose(cf);
		rename(ctmp, commit);
	}
	if (grow_ok)
		snprintf(resmsg, n, "ctree OK (R=%u, froze %u)", target, k_frozen);
	return grow_ok ? 0 : -1;
}

int hyd_act(const hyd_cfg *cfg, const hyd_decision *d, char *resmsg, size_t n)
{
	if (d->action == ACT_NONE) {
		snprintf(resmsg, n, "noop");
		return 0;
	}
	if (cfg->fs == FS_ZENFS)
		return act_zenfs(cfg, d->target_r, resmsg, n);
	if (cfg->coord_ctree)
		return act_f2fs_coord(cfg, d->target_r, resmsg, n);
	return act_f2fs(cfg, d->target_r, resmsg, n);
}
