#!/usr/bin/env python3
# fig10_post.py — post-process a fig10 run directory into the WAL-placement
# evidence pack: a tidy metrics CSV, publication figures, and REPORT.md.
#
# The throughput table alone under-sells WAL_CNS at high concurrency (both
# configs hit the 8x4 device write-BW ceiling, so ops/s converge). The logs
# already carry the rest of the story per (cfg, jobs, bench, instance):
#   - RocksDB write-stall time  ("Interval stall: ..., NN.N percent")
#   - write-latency histogram   (P50..P99.99, Max, StdDev)
#   - stall stop/delay counts   ("Write Stall (count): ... total-stops: N")
#   - ZenFS WAL flush latency   ("wal write elapsed : us-per-flush")
#   - ZNS zone hygiene          ("# Total Zone Reset", "Zone-Reset avg utils")
#   - per-second throughput     ("... thread_0: i,c ops_and r1,r2 ops/second_in ...")
#
# Usage:  python3 scripts/fig10_post.py results/fig10/20260705_220624_8x4
#         -> <rundir>/analysis/{fig10_metrics.csv, fig_*.png, REPORT.md}
import csv, os, re, sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# series colors
C_ZNS, C_CNS = "#2a78d6", "#1baf7a"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e5e4e0"
BENCHES = ("fillrandom", "overwrite")

LOG_RE = re.compile(r"(WAL_[A-Z]+)_n(\d+)_j(\d+)_i(\d)\.log$")
SUM_RE = re.compile(r"^(fillrandom|overwrite)\s*:\s*([\d.]+) micros/op (\d+) ops/sec.*?([\d.]+) MB/s")
PCT_RE = re.compile(r"^Percentiles: P50: ([\d.]+) P75: ([\d.]+) P99: ([\d.]+) P99.9: ([\d.]+) P99.99: ([\d.]+)")
CNT_RE = re.compile(r"^Count: \d+\s+Average: ([\d.]+)\s+StdDev: ([\d.]+)")
MAX_RE = re.compile(r"^Min: \d+\s+Median: [\d.]+\s+Max: (\d+)")
STALLP_RE = re.compile(r"^Interval stall: [\d:.]+ H:M:S, ([\d.]+) percent")
STOPS_RE = re.compile(r"total-delays: (\d+), total-stops: (\d+)")
WAL_RE = re.compile(r"^wal write elapsed : (\d+)")
SST_RE = re.compile(r"^sst write elapsed : (\d+)")
RST_RE = re.compile(r"^# Total Zone Reset : (\d+)")
UTL_RE = re.compile(r"^Zone-Reset avg utils : ([\d.]+)")
TL_RE = re.compile(r"thread_0: (\d+),(\d+) ops_and ([\d.]+),([\d.]+) ops/second_in ([\d.]+),([\d.]+) seconds")


def parse_log(path):
    """Return {bench: metrics-dict}; timeline under key 'tl'."""
    out = {b: {} for b in BENCHES}
    tl = {b: [] for b in BENCHES}       # (cum_sec, int_rate, int_sec)
    cur = None                          # bench whose stats region we are in
    want_hist = False
    tl_bench, tl_prev_cum = BENCHES[0], -1
    with open(path, errors="replace") as f:
        for line in f:
            m = TL_RE.search(line)
            if m:
                iops, cops = int(m.group(1)), int(m.group(2))
                irate, isec, csec = float(m.group(3)), float(m.group(5)), float(m.group(6))
                if cops < tl_prev_cum:  # cum ops reset -> next bench started
                    tl_bench = BENCHES[1]
                tl_prev_cum = cops
                tl[tl_bench].append((csec, irate, isec))
                continue
            m = SUM_RE.match(line)
            if m:
                cur = m.group(1)
                out[cur].update(micros_op=float(m.group(2)), ops_sec=int(m.group(3)),
                                mb_s=float(m.group(4)))
                want_hist = True        # next histogram block = this bench's writes
                continue
            if cur is None:
                continue
            d = out[cur]
            if line.startswith("Microseconds per write:"):
                want_hist = True
                continue
            if want_hist:
                m = CNT_RE.match(line)
                if m:
                    d.setdefault("avg_us", float(m.group(1)))
                    d.setdefault("stddev_us", float(m.group(2)))
                    continue
                m = MAX_RE.match(line)
                if m:
                    d.setdefault("max_us", int(m.group(1)))
                    continue
                m = PCT_RE.match(line)
                if m:
                    d.setdefault("p50", float(m.group(1)))
                    d.setdefault("p99", float(m.group(3)))
                    d.setdefault("p999", float(m.group(4)))
                    d.setdefault("p9999", float(m.group(5)))
                    want_hist = False
                    continue
            m = STALLP_RE.match(line)
            if m:
                d["stall_pct"] = float(m.group(1))   # last dump before next bench wins
                continue
            m = STOPS_RE.search(line)
            if m:
                d.setdefault("stall_delays", int(m.group(1)))
                d.setdefault("stall_stops", int(m.group(2)))
                continue
            for rx, key in ((WAL_RE, "wal_us"), (SST_RE, "sst_us"),
                            (RST_RE, "zone_resets"), (UTL_RE, "reset_util")):
                m = rx.match(line)
                if m:
                    d.setdefault(key, float(m.group(1)))  # first ZenFS block after bench
                    break
    out["tl"] = tl
    return out


def collect(rundir):
    rows, timelines = [], {}
    for fn in sorted(os.listdir(rundir)):
        m = LOG_RE.search(fn)
        if not m or fn.startswith("zmkfs"):
            continue
        cfg, _num, jobs, inst = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        parsed = parse_log(os.path.join(rundir, fn))
        timelines[(cfg, jobs, inst)] = parsed["tl"]
        for b in BENCHES:
            r = {"cfg": cfg, "jobs": jobs, "inst": inst, "bench": b}
            r.update(parsed[b])
            rows.append(r)
    return rows, timelines


def agg(rows, cfg, jobs, bench):
    """Aggregate the two instances. Convention: ops is the MEAN of the two
    instances (legacy/paper tables are per-instance averages), stall/latency
    are means, event counts (stops/resets) are sums."""
    sel = [r for r in rows if r["cfg"] == cfg and r["jobs"] == jobs and r["bench"] == bench]
    if not sel:
        return None
    def mean(k):
        vs = [r[k] for r in sel if k in r]
        return sum(vs) / len(vs) if vs else float("nan")
    return {
        "ops": mean("ops_sec"),
        "stall_pct": mean("stall_pct"), "stops": sum(r.get("stall_stops", 0) for r in sel),
        "p99": mean("p99"), "p999": mean("p999"), "p9999": mean("p9999"),
        "max_us": max((r.get("max_us", 0) for r in sel), default=0),
        "stddev": mean("stddev_us"), "wal_us": mean("wal_us"),
        "resets": sum(r.get("zone_resets", 0) for r in sel), "util": mean("reset_util"),
    }


def style(ax, ylab):
    ax.set_axisbelow(True)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.set_ylabel(ylab, color=INK2, fontsize=9)


def grouped(ax, jobs_list, zns, cns, ylab, fmt="{:.0f}", logy=False):
    """Two thin bars per jobs tick, direct-labeled."""
    x = range(len(jobs_list))
    w = 0.32
    bz = ax.bar([i - w / 2 for i in x], zns, w * 0.94, color=C_ZNS, label="WAL_ZNS")
    bc = ax.bar([i + w / 2 for i in x], cns, w * 0.94, color=C_CNS, label="WAL_CNS")
    if logy:
        ax.set_yscale("log")
    for bars in (bz, bc):
        for b in bars:
            v = b.get_height()
            ax.annotate(fmt.format(v), (b.get_x() + b.get_width() / 2, v),
                        ha="center", va="bottom", fontsize=8, color=INK)
    ax.set_xticks(list(x), [f"j{j}" for j in jobs_list])
    style(ax, ylab)


def main():
    rundir = sys.argv[1] if len(sys.argv) > 1 else "results/fig10/20260705_220624_8x4"
    outdir = os.path.join(rundir, "analysis")
    os.makedirs(outdir, exist_ok=True)
    rows, timelines = collect(rundir)
    jobs_list = sorted({r["jobs"] for r in rows})

    with open(os.path.join(outdir, "fig10_metrics.csv"), "w", newline="") as f:
        keys = ["cfg", "jobs", "inst", "bench", "ops_sec", "mb_s", "micros_op", "avg_us",
                "stddev_us", "p50", "p99", "p999", "p9999", "max_us", "stall_pct",
                "stall_delays", "stall_stops", "wal_us", "sst_us", "zone_resets", "reset_util"]
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)

    A = {(c, j, b): agg(rows, c, j, b)
         for c in ("WAL_ZNS", "WAL_CNS") for j in jobs_list for b in BENCHES}

    def per_bench_fig(fname, key, ylab, fmt="{:.0f}", logy=False, title=""):
        fig, axes = plt.subplots(1, 2, figsize=(9, 3.4))
        for ax, b in zip(axes, BENCHES):
            zns = [A[("WAL_ZNS", j, b)][key] for j in jobs_list]
            cns = [A[("WAL_CNS", j, b)][key] for j in jobs_list]
            grouped(ax, jobs_list, zns, cns, ylab if ax is axes[0] else "", fmt, logy)
            ax.set_title(b, fontsize=10, color=INK)
        axes[0].legend(frameon=False, fontsize=9, loc="upper left")
        fig.suptitle(title, fontsize=11, color=INK, x=0.02, ha="left")
        fig.tight_layout(rect=(0, 0, 1, 0.95))
        fig.savefig(os.path.join(outdir, fname), dpi=150)
        plt.close(fig)

    # 1 throughput + ratio; 2 stall%; 3 tail p99.99; 4 WAL flush; 5 zone hygiene
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4))
    for ax, b in zip(axes, BENCHES):
        zns = [A[("WAL_ZNS", j, b)]["ops"] / 1e3 for j in jobs_list]
        cns = [A[("WAL_CNS", j, b)]["ops"] / 1e3 for j in jobs_list]
        grouped(ax, jobs_list, zns, cns, "ops/s (x1000, avg of 2 inst)" if ax is axes[0] else "", "{:.0f}")
        for i, j in enumerate(jobs_list):
            r = A[("WAL_CNS", j, b)]["ops"] / A[("WAL_ZNS", j, b)]["ops"]
            ax.annotate(f"{r:.2f}x", (i, max(zns[i], cns[i]) * 1.12), ha="center",
                        fontsize=9, color=INK, fontweight="bold")
        ax.set_ylim(0, max(zns + cns) * 1.3)
        ax.set_title(b, fontsize=10, color=INK)
    axes[0].legend(frameon=False, fontsize=9, loc="upper left")
    fig.suptitle("Throughput (avg of 2 instances) — CNS/ZNS ratio on top", fontsize=11,
                 color=INK, x=0.02, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(os.path.join(outdir, "fig_throughput.png"), dpi=150)
    plt.close(fig)

    per_bench_fig("fig_stall.png", "stall_pct", "write-stall (% of wall time)",
                  "{:.1f}", title="RocksDB foreground write stall — time the client is frozen")
    per_bench_fig("fig_tail.png", "max_us", "worst single write (us, log)", "{:,.0f}", True,
                  title="Worst-case single write — a stall freezes one op for seconds")
    per_bench_fig("fig_walflush.png", "wal_us", "WAL flush (us/flush, log)", "{:.0f}", True,
                  title="WAL 1MiB-buffer flush latency (ZenFS wal-write-elapsed)")

    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4))
    for ax, (key, ylab, fmt) in zip(axes, (("resets", "zone resets (count, FR+OW)", "{:.0f}"),
                                           ("util", "avg valid % at reset", "{:.0f}"))):
        zns = [sum(A[("WAL_ZNS", j, b)][key] for b in BENCHES) / (1 if key == "resets" else 2)
               for j in jobs_list]
        cns = [sum(A[("WAL_CNS", j, b)][key] for b in BENCHES) / (1 if key == "resets" else 2)
               for j in jobs_list]
        grouped(ax, jobs_list, zns, cns, ylab, fmt)
    axes[0].legend(frameon=False, fontsize=9, loc="upper left")
    fig.suptitle("ZNS zone hygiene — resets and how full zones were when reset",
                 fontsize=11, color=INK, x=0.02, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(os.path.join(outdir, "fig_zone.png"), dpi=150)
    plt.close(fig)

    # 6) per-second throughput timeline at j8 (i1), FR & OW — one panel per
    #    config (small multiples; an overlay of two sawtooths is unreadable)
    fig, axes = plt.subplots(2, 2, figsize=(10, 4.8), sharey="row")
    for r, b in enumerate(BENCHES):
        for cidx, (cfg, col) in enumerate((("WAL_ZNS", C_ZNS), ("WAL_CNS", C_CNS))):
            ax = axes[r][cidx]
            t = timelines.get((cfg, 8, 1), {}).get(b, [])
            if t:
                ax.plot([p[0] for p in t], [p[1] / 1e3 for p in t], color=col, linewidth=1.6)
            style(ax, f"{b}\nops/s (x1000, i1)" if cidx == 0 else "")
            ax.set_ylim(bottom=0)
            if r == 0:
                ax.set_title(cfg, fontsize=10, color=INK)
            if r == 1:
                ax.set_xlabel("seconds into benchmark", color=INK2, fontsize=9)
    fig.suptitle("j8 per-second throughput (instance 1) — sawtooth-to-zero = write stalls",
                 fontsize=11, color=INK, x=0.02, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(os.path.join(outdir, "fig_timeline_j8.png"), dpi=150)
    plt.close(fig)

    # 7) time-weighted CDF of per-second throughput at j8 — predictability in
    #    one curve: steep middle = steady, mass near 0 = frozen seconds
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.4), sharey=True)
    for ax, b in zip(axes, BENCHES):
        for cfg, col in (("WAL_ZNS", C_ZNS), ("WAL_CNS", C_CNS)):
            pts = []
            for inst in (1, 2):
                pts += timelines.get((cfg, 8, inst), {}).get(b, [])
            if not pts:
                continue
            pts = sorted((rate / 1e3, isec) for _, rate, isec in pts)
            total = sum(w for _, w in pts) or 1.0
            xs, ys, acc = [], [], 0.0
            for v, w in pts:
                acc += w
                xs.append(v)
                ys.append(100.0 * acc / total)
            ax.plot(xs, ys, color=col, linewidth=2, label=cfg)
        style(ax, "% of wall time at <= x" if ax is axes[0] else "")
        ax.set_xlabel("per-second throughput (x1000 ops/s)", color=INK2, fontsize=9)
        ax.set_title(b, fontsize=10, color=INK)
    axes[0].legend(frameon=False, fontsize=9, loc="lower right")
    fig.suptitle("j8 throughput steadiness — time-weighted CDF (i1+i2)",
                 fontsize=11, color=INK, x=0.02, ha="left")
    fig.tight_layout(rect=(0, 0, 1, 0.95))
    fig.savefig(os.path.join(outdir, "fig_cdf_j8.png"), dpi=150)
    plt.close(fig)

    # REPORT.md — full numbers behind every figure
    with open(os.path.join(outdir, "REPORT.md"), "w") as f:
        f.write(f"# fig10 WAL-placement evidence pack — {os.path.basename(rundir)}\n\n")
        f.write("| bench | jobs | ops ZNS | ops CNS | ratio | stall% ZNS | stall% CNS "
                "| stops Z/C | worst-op ZNS | worst-op CNS | P99.99 Z/C (us) "
                "| WALflush Z/C (us) | resets Z/C | reset-util Z/C |\n|---|" + "---|" * 13 + "\n")
        for b in BENCHES:
            for j in jobs_list:
                z, c = A[("WAL_ZNS", j, b)], A[("WAL_CNS", j, b)]
                f.write(f"| {b} | j{j} | {z['ops']:,.0f} | {c['ops']:,.0f} | "
                        f"**{c['ops']/z['ops']:.2f}x** | {z['stall_pct']:.1f} | {c['stall_pct']:.1f} "
                        f"| {z['stops']}/{c['stops']} "
                        f"| {z['max_us']/1e6:.2f}s | {c['max_us']/1e6:.3f}s "
                        f"| {z['p9999']:,.0f}/{c['p9999']:,.0f} "
                        f"| {z['wal_us']:,.0f}/{c['wal_us']:,.0f} "
                        f"| {z['resets']:.0f}/{c['resets']:.0f} | {z['util']:.0f}%/{c['util']:.0f}% |\n")
        f.write("\nFigures: fig_throughput / fig_stall / fig_tail / fig_walflush / "
                "fig_zone / fig_timeline_j8 / fig_cdf_j8 (.png)\n")
        f.write("""
## Reading the figures

- fig_throughput: CNS/ZNS ratio per jobs. At high concurrency both configs
  can sit on the device write-BW ceiling, so ops/s alone under-states the
  difference.
- fig_stall / fig_timeline_j8 / fig_cdf_j8 / fig_tail: how each config
  delivers its throughput — write-stall share of wall time, per-second
  steadiness, and worst-case single-write latency.
- fig_walflush: per-flush WAL latency (negative control: the CNS win comes
  from unblocking flush/compaction, not from cheaper WAL flushes).
- fig_zone: zone-reset count and average valid % at reset (WAL churn on ZNS
  pollutes zones).

Caveats: --sync=0; absolute numbers are emulator (FEMU/dm-hyzns) values.
""")
    print(f"analysis -> {outdir}")


if __name__ == "__main__":
    main()
