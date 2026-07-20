/* hyznsd.c - main: config, monitor loop, cooldown/backoff, logging.
 * See DESIGN.md and hyznsd.h for the contract. */
#define _GNU_SOURCE
#include "hyznsd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

/* ------------------------------------------------------------------------- *
 * util
 * ------------------------------------------------------------------------- */
uint64_t hyd_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_epoch(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec;
}

/* run a shell command, capture stdout (first n bytes). 0 on exit-0. */
int hyd_run_capture(const char *cmd, char *out, size_t n)
{
	FILE *p = popen(cmd, "r");
	if (!p) return -1;
	size_t got = 0;
	if (out && n) {
		out[0] = 0;
		got = fread(out, 1, n - 1, p);
		out[got] = 0;
	}
	(void)got;
	int rc = pclose(p);
	if (rc == -1) return -1;
	return (rc == 0) ? 0 : 1;
}

/* find "key=<int>" in line; return value or -1. Word-boundary on the key
 * start so "free_lines" doesn't match inside another token. */
long hyd_kv_lookup(const char *line, const char *key)
{
	size_t klen = strlen(key);
	const char *p = line;
	while ((p = strstr(p, key)) != NULL) {
		bool lhs_ok = (p == line) || !isalnum((unsigned char)p[-1]);
		if (lhs_ok && p[klen] == '=') {
			char *end;
			long v = strtol(p + klen + 1, &end, 10);
			if (end != p + klen + 1) return v;
		}
		p += klen;
	}
	return -1;
}

/* ------------------------------------------------------------------------- *
 * logging
 * ------------------------------------------------------------------------- */
static FILE *g_log = NULL;

static void logline(const char *fmt, ...)
{
	va_list ap;
	FILE *o = g_log ? g_log : stderr;
	char ts[32];
	time_t t = time(NULL);
	struct tm tm; localtime_r(&t, &tm);
	strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(o, "[%s] ", ts);
	va_start(ap, fmt);
	vfprintf(o, fmt, ap);
	va_end(ap);
	fputc('\n', o);
	fflush(o);
}

/* ------------------------------------------------------------------------- *
 * config
 * ------------------------------------------------------------------------- */
static void cfg_defaults(hyd_cfg *c)
{
	memset(c, 0, sizeof *c);
	strcpy(c->dm, "hyzns0");
	strcpy(c->backing, "/dev/nvme0n1");
	strcpy(c->f2fs_io, "f2fs_io");
	c->fs = FS_F2FS;
	c->rz_source = RZ_FS;       /* paper default: ask the zone-aware FS (ZenFS .hyzns_status); falls back to blkzone report if absent */
	c->poll_ms = 500;
	c->poll_min_ms = 100;    /* fast poll when R_C is under pressure */
	c->ftr_grow_rc = 2;      /* paper §5.6.3: grow iff R_C < 2*S_Z ...        */
	c->ftr_grow_rz = 5;      /*               ... && R_Z >= 5                 */
	c->ftr_shrink_rc = 3;    /*             shrink iff R_C > 3*S_Z ...        */
	c->ftr_shrink_rz = 4;    /*               ... && R_Z < 4                  */
	c->ftr_gcu_resize = 1;   /* paper: gc_urgent while a resize fires */
	c->ftr_gcu_park = 0;     /* park pulse off by default (paper behavior) */
	c->r_min = 4;
	c->r_max = 64;
	c->cooldown_ms = 2000;
	c->backoff_ms = 1000;
	c->backoff_max_ms = 30000;
	c->ack_timeout_ms = 60000;
	c->verbose = 1;
}

/* assign one "key=value" pair (config file or --set). returns 0 if known. */
static int cfg_set(hyd_cfg *c, const char *key, const char *val)
{
	if      (!strcmp(key, "dm"))            snprintf(c->dm, sizeof c->dm, "%s", val);
	else if (!strcmp(key, "backing"))       snprintf(c->backing, sizeof c->backing, "%s", val);
	else if (!strcmp(key, "mnt"))           snprintf(c->mnt, sizeof c->mnt, "%s", val);
	else if (!strcmp(key, "aux"))           snprintf(c->aux, sizeof c->aux, "%s", val);
	else if (!strcmp(key, "f2fs_io"))       snprintf(c->f2fs_io, sizeof c->f2fs_io, "%s", val);
	else if (!strcmp(key, "fs"))            c->fs = strcmp(val, "zenfs") ? FS_F2FS : FS_ZENFS;
	else if (!strcmp(key, "coord"))         c->coord_ctree = !strcmp(val, "ctree");
	else if (!strcmp(key, "rz_source"))     c->rz_source = strcmp(val, "fs") ? RZ_REPORT : RZ_FS;
	else if (!strcmp(key, "poll_ms"))       c->poll_ms = atoi(val);
	else if (!strcmp(key, "poll_min_ms"))   c->poll_min_ms = atoi(val);
	else if (!strcmp(key, "ftr_grow_rc"))   c->ftr_grow_rc = atoi(val);
	else if (!strcmp(key, "ftr_grow_rz"))   c->ftr_grow_rz = atoi(val);
	else if (!strcmp(key, "ftr_shrink_rc")) c->ftr_shrink_rc = atoi(val);
	else if (!strcmp(key, "ftr_shrink_rz")) c->ftr_shrink_rz = atoi(val);
	else if (!strcmp(key, "ftr_gcu_resize")) c->ftr_gcu_resize = atoi(val);
	else if (!strcmp(key, "ftr_gcu_park"))  c->ftr_gcu_park = atoi(val);
	else if (!strcmp(key, "r_min"))         c->r_min = atoi(val);
	else if (!strcmp(key, "r_max"))         c->r_max = atoi(val);
	else if (!strcmp(key, "s_end_zone"))    c->s_end_zone = atoi(val);
	else if (!strcmp(key, "cooldown_ms"))   c->cooldown_ms = atoi(val);
	else if (!strcmp(key, "backoff_ms"))    c->backoff_ms = atoi(val);
	else if (!strcmp(key, "backoff_max_ms"))c->backoff_max_ms = atoi(val);
	else if (!strcmp(key, "ack_timeout_ms"))c->ack_timeout_ms = atoi(val);
	else if (!strcmp(key, "logfile"))       snprintf(c->logfile, sizeof c->logfile, "%s", val);
	else if (!strcmp(key, "snapfile"))      snprintf(c->snapfile, sizeof c->snapfile, "%s", val);
	else if (!strcmp(key, "mock_dm"))       snprintf(c->mock_dm, sizeof c->mock_dm, "%s", val);
	else if (!strcmp(key, "mock_rz"))       snprintf(c->mock_rz, sizeof c->mock_rz, "%s", val);
	else return -1;
	return 0;
}

static int cfg_load_file(hyd_cfg *c, const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *s = line;
		while (*s && isspace((unsigned char)*s)) s++;
		if (*s == '#' || *s == 0 || *s == '\n') continue;
		char *eq = strchr(s, '=');
		if (!eq) continue;
		*eq = 0;
		char *key = s, *val = eq + 1;
		/* strip inline comments: "rz_source=fs   # ..." must yield "fs" */
		char *hash = strchr(val, '#');
		if (hash) *hash = 0;
		/* trim */
		char *ke = key + strlen(key); while (ke > key && isspace((unsigned char)ke[-1])) *--ke = 0;
		while (*val && isspace((unsigned char)*val)) val++;
		char *ve = val + strlen(val); while (ve > val && isspace((unsigned char)ve[-1])) *--ve = 0;
		cfg_set(c, key, val);
	}
	fclose(f);
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
"hyznsd " HYZNSD_VERSION " - HyZNS CNS-area autoscaling daemon\n"
"usage: %s [-c conf] [--set k=v]... [--once] [--dry-run] [-v] [-h]\n"
"  -c <file>        config file (key=value lines)\n"
"  --set k=v        override one config key (repeatable)\n"
"  --once           run a single monitor/decide/act cycle and exit\n"
"  --dry-run        decide + log, do not execute resize\n"
"  -v / -q          more / less verbose\n"
"keys: dm backing mnt aux fs(f2fs|zenfs) rz_source(fs|report) poll_ms\n"
"      ftr_grow_rc ftr_grow_rz ftr_shrink_rc ftr_shrink_rz (paper 2/5/3/4)\n"
"      ftr_gcu_resize ftr_gcu_park r_min r_max s_end_zone\n"
"      cooldown_ms backoff_ms backoff_max_ms ack_timeout_ms logfile\n"
"      mock_dm <file>  mock_rz <file>   (offline policy test)\n", p);
}

/* ------------------------------------------------------------------------- *
 * main loop
 * ------------------------------------------------------------------------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static const char *act_name(hyd_action a)
{
	return a == ACT_GROW ? "GROW" : a == ACT_SHRINK ? "SHRINK" : "NONE";
}

/* Append one fs/dm capacity snapshot line around a resize act. dm numbers are
 * exact (dmsetup status); F2FS valid is debugfs "valid blocks"; the F2FS
 * seg[valid/dirty/prefree/free] counts are against the max-provisioned
 * (fantasy) capacity - compare trends, not absolutes. */
static void snap_resize(const hyd_cfg *cfg, const char *event,
                        const char *phase, uint32_t target)
{
	if (!cfg->snapfile[0]) return;
	hyd_state s2; memset(&s2, 0, sizeof s2);
	if (hyd_read_dm(cfg, &s2) != 0 || s2.zone_pblocks == 0) return;
	uint64_t rb  = (uint64_t)s2.cur_r * s2.zone_pblocks;   /* R capacity in pages */
	uint64_t inv = rb > s2.valid_pages + s2.free_pages ?
	               rb - s2.valid_pages - s2.free_pages : 0;
	char out[128] = {0};
	hyd_run_capture(
	  "awk '/partition info\\(dm-0/{f=1} "
	  "f&&/Utilization:/{if(match($0,/\\(([0-9]+) valid/,m))vb=m[1]} "
	  "f&&/^  - Valid:/{vs=$3} f&&/^  - Dirty:/{ds=$3} "
	  "f&&/^  - Prefree:/{ps=$3} "
	  "f&&/^  - Free:/{fr=$3; printf \"%d %d %d %d %d\",vb,vs,ds,ps,fr; exit}' "
	  "/sys/kernel/debug/f2fs/status 2>/dev/null", out, sizeof out);
	long vb = 0, vs = 0, ds = 0, ps = 0, fr = 0;
	sscanf(out, "%ld %ld %ld %ld %ld", &vb, &vs, &ds, &ps, &fr);
	FILE *f = fopen(cfg->snapfile, "a");
	if (!f) return;
	fseek(f, 0, SEEK_END);
	if (ftell(f) == 0)
		fprintf(f, "ts,event,phase,R,target,dm_valid_mib,dm_invalid_mib,"
		           "dm_free_mib,f2fs_valid_mib,f2fs_seg_valid,f2fs_seg_dirty,"
		           "f2fs_seg_prefree,f2fs_seg_free\n");
	fprintf(f, "%lu,%s,%s,%u,%u,%llu,%llu,%llu,%ld,%ld,%ld,%ld,%ld\n",
	        (unsigned long)now_epoch(), event, phase, s2.cur_r, target,
	        (unsigned long long)(s2.valid_pages / 256),
	        (unsigned long long)(inv / 256),
	        (unsigned long long)(s2.free_pages / 256),
	        vb / 256, vs, ds, ps, fr);
	fclose(f);
}

int main(int argc, char **argv)
{
	hyd_cfg cfg; cfg_defaults(&cfg);

	/* pre-scan for -c so file loads before --set overrides */
	for (int i = 1; i < argc - 1; i++)
		if (!strcmp(argv[i], "-c")) cfg_load_file(&cfg, argv[i + 1]);

	for (int i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-c") && i + 1 < argc) i++;
		else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
			char kv[256]; snprintf(kv, sizeof kv, "%s", argv[++i]);
			char *eq = strchr(kv, '='); if (!eq) continue; *eq = 0;
			if (cfg_set(&cfg, kv, eq + 1) != 0)
				fprintf(stderr, "warn: unknown key '%s'\n", kv);
		}
		else if (!strcmp(argv[i], "--once"))    cfg.once = true;
		else if (!strcmp(argv[i], "--dry-run")) cfg.dry_run = true;
		else if (!strcmp(argv[i], "-v"))        cfg.verbose++;
		else if (!strcmp(argv[i], "-q"))        cfg.verbose = 0;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
		else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
	}

	if (cfg.logfile[0]) {
		g_log = fopen(cfg.logfile, "a");
		if (!g_log) { fprintf(stderr, "cannot open logfile %s: %s\n", cfg.logfile, strerror(errno)); return 1; }
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	logline("hyznsd %s start: dm=%s fs=%s rz=%s mnt=%s aux=%s poll=%ums%s%s",
	        HYZNSD_VERSION, cfg.dm, cfg.fs == FS_ZENFS ? "zenfs" : "f2fs",
	        cfg.rz_source == RZ_FS ? "fs" : "report", cfg.mnt, cfg.aux, cfg.poll_ms,
	        cfg.dry_run ? " [DRY-RUN]" : "", cfg.once ? " [ONCE]" : "");
	logline("policy: FTR grow(R_C<%u*S_Z & R_Z>=%u) shrink(R_C>%u*S_Z & R_Z<%u) clamp[%u,%u] cooldown=%ums",
	        cfg.ftr_grow_rc, cfg.ftr_grow_rz, cfg.ftr_shrink_rc, cfg.ftr_shrink_rz,
	        cfg.r_min, cfg.r_max, cfg.cooldown_ms);
	uint64_t last_resize_ms = 0;
	uint32_t cur_backoff_ms = 0;   /* 0 = not backing off */
	uint64_t backoff_until_ms = 0;

	int rc_exit = 0;
	do {
		hyd_state st; memset(&st, 0, sizeof st);
		int dm_rc = hyd_read_dm(&cfg, &st);
		int rz_rc = (dm_rc == 0) ? hyd_read_rz(&cfg, &st) : -1;

		if (dm_rc != 0) {
			logline("WARN dm read failed (device down?) - pausing");
		} else if (rz_rc != 0) {
			logline("WARN R_Z read failed (rz_source=%s) - pausing",
			        cfg.rz_source == RZ_FS ? "fs" : "report");
		} else {
			hyd_decision d = hyd_decide(&cfg, &st);
			uint64_t now = hyd_now_ms();

			/* apply the gc_urgent toggle (F2FS sysfs). The f2fs sysfs dir is
			 * the mounted block device name (dm-hyzns == dm-0 in our setup;
			 * override via env HYD_F2FS_SYSFS). -1 = leave as-is. */
			if (d.gc_urgent >= 0) {
				const char *base = getenv("HYD_F2FS_SYSFS");
				char gp[256];
				snprintf(gp, sizeof gp, "%s/gc_urgent",
				         base && base[0] ? base : "/sys/fs/f2fs/dm-0");
				FILE *gf = fopen(gp, "w");
				if (gf) { fprintf(gf, "%d\n", d.gc_urgent); fclose(gf); }
			}

			if (cfg.verbose > 1 || d.action != ACT_NONE || d.gc_urgent == 1) {
				uint64_t capp = (uint64_t)st.cur_r * st.zone_pblocks;
				unsigned vpct = capp ? (unsigned)(st.valid_pages * 100 / capp) : 0;
				/* R_C = what the FTR decision actually used: dm free_pages
				 * (real free blocks), also shown in zones so it reads
				 * directly against the 2*S_Z / 3*S_Z thresholds.
				 * freeL = whole free lines (allocator view, 64 MiB each). */
				double rcz = st.zone_pblocks ?
					(double)st.free_pages / st.zone_pblocks : 0.0;
				logline("epoch=%lu R_C=%llupg(%.2fz) R_Z=%u freeL=%u curR=%u valid=%lupg(%u%%) gcurg=%d nospc=%lu -> %s target=%u (%s)",
				        now_epoch(), (unsigned long long)st.free_pages, rcz,
				        st.r_z, st.free_lines, st.cur_r, st.valid_pages, vpct,
				        d.gc_urgent, st.nospc,
				        act_name(d.action), d.target_r, d.reason);
			}

			if (d.action != ACT_NONE) {
				bool in_cd = (last_resize_ms && now - last_resize_ms < cfg.cooldown_ms);
				bool in_bo = (backoff_until_ms && now < backoff_until_ms);
				if (in_cd) {
					if (cfg.verbose > 1) logline("  deferred: cooldown (%lums left)",
					        cfg.cooldown_ms - (now - last_resize_ms));
				} else if (in_bo) {
					if (cfg.verbose > 1) logline("  deferred: backoff (%lums left)",
					        backoff_until_ms - now);
				} else if (cfg.dry_run) {
					logline("  [DRY-RUN] would %s to R=%u", act_name(d.action), d.target_r);
				} else {
					char res[256] = {0};
					snap_resize(&cfg, act_name(d.action), "before", d.target_r);
					int ar = hyd_act(&cfg, &d, res, sizeof res);
					snap_resize(&cfg, act_name(d.action),
					            ar == 0 ? "after_ok" : "after_fail", d.target_r);
					logline("  %s R=%u -> %s", act_name(d.action), d.target_r, res);
					if (ar == 0) {
						last_resize_ms = hyd_now_ms();
						cur_backoff_ms = 0;
						backoff_until_ms = 0;
					} else {
						/* exponential backoff on failure */
						cur_backoff_ms = cur_backoff_ms ? cur_backoff_ms * 2 : cfg.backoff_ms;
						if (cur_backoff_ms > cfg.backoff_max_ms) cur_backoff_ms = cfg.backoff_max_ms;
						backoff_until_ms = hyd_now_ms() + cur_backoff_ms;
						logline("  resize failed - backoff %ums", cur_backoff_ms);
					}
				}
			}
		}

		if (cfg.once) break;
		/* Pressure-aware poll: near the grow trigger a burst can exhaust free
		 * space within one idle interval, so poll fast under pressure and
		 * lazily when idle. Not while backing off. */
		uint32_t period = cfg.poll_ms;
		bool backing_off = (backoff_until_ms && hyd_now_ms() < backoff_until_ms);
		if (st.dm_ok && st.rz_ok && !backing_off &&
		    st.r_c <= cfg.ftr_grow_rc * 2 && cfg.poll_min_ms)
			period = cfg.poll_min_ms;
		uint32_t tick = period < 50 ? period : 50;
		for (uint32_t slept = 0; slept < period && !g_stop; slept += tick)
			usleep((period - slept < tick ? period - slept : tick) * 1000);
	} while (!g_stop);

	logline("hyznsd stop");
	if (g_log) fclose(g_log);
	return rc_exit;
}
