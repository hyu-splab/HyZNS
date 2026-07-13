#!/usr/bin/env python3
# fig11_evidence.py <fig10-rundir> — the all-layer evidence pack behind paper
# fig11 (FR/OW under WAL placement, background-jobs sweep on 8x4).
#
# Answers, per (config, jobs, bench), FROM LOGS ONLY:
#   1) is the device at its ceiling?   <tag>_mon.csv (1s diskstats: write MB/s,
#      %busy) vs the measured effective ceiling of this 8x4 device
#   2) who stalls, how much, and why?  RocksDB info LOG events (stall/stop by
#      cause, L0 file count) + db_bench "Interval stall" percent
#   3) what does the foreground pay?   ZenFS wal-write-elapsed (us per WAL
#      flush) per bench
# Produces per-instance 3-panel timelines (throughput+stalls / L0 files /
# flush+compaction lanes) via the resize_ops tools, per-cell device-BW
# timelines, and analysis11/{fig11_summary.csv, REPORT.md}.
#
# Abbreviations: FR=fillrandom, OW=overwrite, j<N>=--max_background_jobs=N,
# WAL=write-ahead log, BW=bandwidth, fg/bg=foreground(client)/background
# (flush+compaction), L0=LSM level-0.
import csv, os, re, subprocess, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

RESIZE = "/data/HYSSD/hyhostd/scripts/resize_ops"
C_ZNS, C_CNS = "#2a78d6", "#1baf7a"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e5e4e0"
# effective write ceilings of THIS 8x4 device (dev_bw.sh + fig10_mon findings):
# model peak 1165 MB/s; kernel max_segments=127 splits >508KiB requests ->
# ~583 MB/s for a 1MiB stream; ~480 MB/s observed under mixed write+read.
CEIL_STREAM, CEIL_MIXED = 583.0, 480.0

LOG_RE = re.compile(r"(WAL_[A-Z]+)_n\d+_j(\d+)_i(\d)\.log$")
REP_RE = re.compile(r"^([\d.]+) ===REPORT (-?\d+) (-?\d+) (-?\d+) (\d+) (\d+)")
PHASE_RE = re.compile(r"^([\d.]+) ===PHASE_START (\S+)")
SUM_RE = re.compile(r"^(fillrandom|overwrite)\s*:\s*[\d.]+ micros/op (\d+) ops/sec")
STALLP_RE = re.compile(r"^Interval stall: [\d:.]+ H:M:S, ([\d.]+) percent")
WAL_RE = re.compile(r"^wal write elapsed : (\d+)")
BENCHES = ("fillrandom", "overwrite")


def parse_dblog(path):
    reps, phases, d = [], [], {}
    stall_seq, wal_seq = [], []
    with open(path, errors="replace") as f:
        for line in f:
            m = REP_RE.match(line)
            if m:
                reps.append((float(m.group(1)), int(m.group(2)), int(m.group(3)),
                             int(m.group(4))))
                continue
            m = PHASE_RE.match(line)
            if m:
                phases.append((float(m.group(1)), m.group(2)))
                continue
            m = SUM_RE.match(line)
            if m:
                d[f"{m.group(1)}_ops"] = int(m.group(2))
                continue
            m = STALLP_RE.match(line)
            if m:
                stall_seq.append(float(m.group(1)))
                continue
            m = WAL_RE.match(line)
            if m:
                wal_seq.append(int(m.group(1)))
                continue
    # dump order after each bench: FR first, OW second (interval percent)
    for i, b in enumerate(BENCHES):
        if i < len(stall_seq):
            d[f"{b}_stall_pct"] = stall_seq[i]
        if i < len(wal_seq):
            d[f"{b}_wal_us"] = wal_seq[i]
    return reps, phases, d


def mon_series(path):
    rows = []
    try:
        with open(path) as f:
            for r in csv.DictReader(f):
                try:
                    rows.append((float(r["ts"]), int(r["wr_sectors"]),
                                 int(r["rd_sectors"]), int(r["io_ticks_ms"])))
                except (KeyError, ValueError):
                    pass
    except OSError:
        return []
    out = []
    for (t0, w0, r0, k0), (t1, w1, r1, k1) in zip(rows, rows[1:]):
        dt = t1 - t0
        if dt <= 0:
            continue
        out.append((t1, (w1 - w0) * 512 / dt / 1e6, (r1 - r0) * 512 / dt / 1e6,
                    (k1 - k0) / (dt * 1000) * 100))
    return out          # (ts, wMB/s, rMB/s, busy%)


def win_stats(series, t0, t1, idx):
    vs = [s[idx] for s in series if t0 <= s[0] <= t1]
    if not vs:
        return float("nan"), float("nan")
    vs.sort()
    return sum(vs) / len(vs), vs[int(len(vs) * 0.95)]


def stall_counts(rdb_csv, t0, t1):
    cnt = defaultdict(int)
    try:
        with open(rdb_csv) as f:
            for r in csv.DictReader(f):
                k = r["kind"]
                if k.startswith(("stall_", "stop_")):
                    try:
                        ts = float(r["ts_epoch"])
                    except ValueError:
                        continue
                    if t0 <= ts <= t1:
                        cnt[k] += 1
    except OSError:
        pass
    return dict(cnt)


def main():
    rundir = sys.argv[1].rstrip("/")
    out = os.path.join(rundir, "analysis11")
    os.makedirs(out, exist_ok=True)

    cells = defaultdict(dict)
    for fn in sorted(os.listdir(rundir)):
        m = LOG_RE.search(fn)
        if m and not fn.startswith("zmkfs"):
            cells[(m.group(1), int(m.group(2)))][int(m.group(3))] = fn
    if not cells:
        sys.exit(f"no WAL_* logs in {rundir}")

    rows = []
    for (cfg, j), insts in sorted(cells.items()):
        tag = f"{cfg}_n10000000_j{j}"
        mon = mon_series(os.path.join(rundir, f"{tag}_mon.csv"))
        per_inst = {}
        for i, fn in sorted(insts.items()):
            reps, phases, d = parse_dblog(os.path.join(rundir, fn))
            per_inst[i] = (reps, phases, d)
            if not reps:
                continue
            # ops.csv (fig9 format) + phases.csv + 3-panel via resize_ops tools
            opscsv = os.path.join(out, f"{tag}_i{i}_ops.csv")
            with open(opscsv, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["ts_epoch", "total_ops_per_100ms",
                            "read_ops_per_100ms", "write_ops_per_100ms"])
                for ts, tot, rd, wr in reps:
                    w.writerow([f"{ts:.6f}", max(tot, 0), max(rd, 0), max(wr, 0)])
            phcsv = os.path.join(out, f"{tag}_i{i}_phases.csv")
            with open(phcsv, "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["ts_epoch", "bench"])
                for ts, b in phases:
                    w.writerow([f"{ts:.6f}", b])
            rlog = os.path.join(rundir, f"{tag}_rocksdb_LOG_i{i}")
            rdbcsv = os.path.join(out, f"{tag}_i{i}_rdb.csv")
            if os.path.exists(rlog) and not os.path.exists(rdbcsv):
                subprocess.run(["python3", f"{RESIZE}/parse_rocksdb_events.py",
                                rlog, rdbcsv], capture_output=True)
            args = ["python3", f"{RESIZE}/plot_ops_timeline.py", opscsv,
                    os.path.join(out, f"{tag}_i{i}"), "--phases", phcsv,
                    "--title", f"{tag} i{i}"]
            if os.path.exists(rdbcsv):
                args += ["--rdb", rdbcsv]
            subprocess.run(args, capture_output=True)

        # per-bench summary rows (windows from i1's phase markers)
        reps1, phases1, _ = per_inst.get(1, ([], [], {}))
        pmap = dict((b, ts) for ts, b in phases1)
        end_ts = reps1[-1][0] if reps1 else 0
        for bi, b in enumerate(BENCHES):
            t0 = pmap.get(b)
            if t0 is None:
                continue
            t1 = pmap.get(BENCHES[bi + 1]) if bi + 1 < len(BENCHES) else end_ts
            t1 = t1 or end_ts
            wmb, wmb95 = win_stats(mon, t0, t1, 1)
            busy, _ = win_stats(mon, t0, t1, 3)
            opsv = [d.get(f"{b}_ops", 0) for _, _, d in per_inst.values()]
            stallv = [d.get(f"{b}_stall_pct") for _, _, d in per_inst.values()
                      if d.get(f"{b}_stall_pct") is not None]
            walv = [d.get(f"{b}_wal_us") for _, _, d in per_inst.values()
                    if d.get(f"{b}_wal_us") is not None]
            causes = defaultdict(int)
            for i in per_inst:
                for k, v in stall_counts(
                        os.path.join(out, f"{tag}_i{i}_rdb.csv"), t0, t1).items():
                    causes[k] += v
            rows.append({
                "cfg": cfg, "jobs": j, "bench": b,
                "ops_avg": sum(opsv) / len(opsv) if opsv else 0,
                "dev_wMBps": wmb, "dev_wMBps_p95": wmb95, "dev_busy_pct": busy,
                "stall_pct": sum(stallv) / len(stallv) if stallv else float("nan"),
                "wal_us": sum(walv) / len(walv) if walv else float("nan"),
                "stall_events": "; ".join(f"{k}={v}" for k, v in sorted(causes.items())),
            })

    with open(os.path.join(out, "fig11_summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)

    # device-BW-vs-ceiling figure: one panel per bench, x=jobs, mean write MB/s
    by = {(r["cfg"], r["jobs"], r["bench"]): r for r in rows}
    jobs_list = sorted({r["jobs"] for r in rows})
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4), sharey=True)
    for ax, b in zip(axes, BENCHES):
        for cfg, col in (("WAL_ZNS", C_ZNS), ("WAL_CNS", C_CNS)):
            vals = [by.get((cfg, j, b), {}).get("dev_wMBps", float("nan"))
                    for j in jobs_list]
            ax.plot(range(len(jobs_list)), vals, "o-", color=col, lw=2, ms=7,
                    label=cfg)
        ax.axhline(CEIL_MIXED, color=INK2, ls="--", lw=1)
        ax.axhline(CEIL_STREAM, color=INK2, ls=":", lw=1)
        ax.annotate("mixed ceiling 480", (0, CEIL_MIXED), fontsize=8,
                    color=INK2, va="bottom")
        ax.annotate("stream ceiling 583", (0, CEIL_STREAM), fontsize=8,
                    color=INK2, va="bottom")
        ax.set_xticks(range(len(jobs_list)), [f"j{j}" for j in jobs_list])
        ax.set_title(b, fontsize=10, color=INK)
        ax.set_axisbelow(True); ax.grid(axis="y", color=GRID, lw=0.8)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        ax.tick_params(colors=INK2, labelsize=9)
    axes[0].set_ylabel("device write MB/s (mean)", color=INK2, fontsize=9)
    axes[0].legend(frameon=False, fontsize=9, loc="lower right")
    fig.suptitle("Device write bandwidth vs measured ceilings",
                 fontsize=11, color=INK, x=0.02, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(os.path.join(out, "fig11_device_bw.png"), dpi=150)
    plt.close(fig)

    with open(os.path.join(out, "REPORT.md"), "w") as f:
        f.write(f"# fig11 evidence — {os.path.basename(rundir)}\n\n")
        f.write("| bench | j | cfg | ops(avg 2inst) | dev W MB/s (p95) | busy% "
                "| stall% | WAL flush us | stall events |\n|---|" + "---|" * 8 + "\n")
        for r in rows:
            f.write(f"| {r['bench']} | j{r['jobs']} | {r['cfg']} | {r['ops_avg']:,.0f} "
                    f"| {r['dev_wMBps']:.0f} ({r['dev_wMBps_p95']:.0f}) "
                    f"| {r['dev_busy_pct']:.0f} | {r['stall_pct']:.1f} "
                    f"| {r['wal_us']:,.0f} | {r['stall_events']} |\n")
        f.write("\nCeilings: stream 583 MB/s, mixed ~480 MB/s (dev_bw.sh + "
                "fig10_mon). 3-panel timelines: <tag>_i<n>_ops.png\n")
    print(f"analysis11 -> {out}")


if __name__ == "__main__":
    main()
