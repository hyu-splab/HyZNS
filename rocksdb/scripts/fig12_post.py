#!/usr/bin/env python3
# fig12_post.py — post-process a fig12 run dir (readwhilewriting under WAL
# placement) into the paper's two panels + a tidy CSV:
#   fig12_a  RWW read throughput (dual total) per background-thread count,
#            WAL_ZNS vs WAL_CNS (writer throughput annotated in the CSV/report)
#   fig12_b  read-latency CDF, straight from db_bench's --histogram bucket
#            table ("Microseconds per read", i1+i2 bucket counts merged)
#
# Usage: python3 scripts/fig12_post.py results/fig12/<rundir>
#   -> <rundir>/analysis/{fig12_metrics.csv, fig12_cdf.csv,
#                         fig12_a_rww_throughput.{png,eps},
#                         fig12_b_read_cdf.{png,eps}, REPORT.md}
import csv, glob, io, os, re, sys, tarfile
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

C_ZNS, C_CNS = "#2a78d6", "#1baf7a"   # series colors
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e5e4e0"

LOG_RE = re.compile(r"((?:WAL|L0)_[A-Z]+)_n\d+_j(\d+)_i(\d)\.log$")
RWW_RE = re.compile(r"^readwhilewriting\s*:\s*([\d.]+) micros/op (\d+) ops/sec")
WRT_RE = re.compile(r"^rww-writes\s*:\s*[\d.]+ micros/op (\d+) ops/sec")
PCT_RE = re.compile(r"^Percentiles: P50: ([\d.]+) P75: [\d.]+ P99: ([\d.]+) "
                    r"P99.9: ([\d.]+) P99.99: ([\d.]+)")
BKT_RE = re.compile(r"^[(\[]\s*(\d+),\s*(\d+)\s*[)\]]\s+(\d+)\s")
MAX_RE = re.compile(r"^Min: \d+\s+Median: [\d.]+\s+Max: (\d+)")
STALLP_RE = re.compile(r"^Interval stall: [\d:.]+ H:M:S, ([\d.]+) percent")
STOPS_RE = re.compile(r"total-delays: (\d+), total-stops: (\d+)")


def parse_log(path):
    """RWW summary + writer rate + the 'Microseconds per read' bucket table."""
    d, buckets = {}, []
    in_read_hist = grab = False
    with open(path, errors="replace") as f:
        for line in f:
            m = RWW_RE.match(line)
            if m:
                d["read_us_op"], d["read_ops"] = float(m.group(1)), int(m.group(2))
                continue
            m = WRT_RE.match(line)
            if m:
                d["write_ops"] = int(m.group(1))
                continue
            m = STALLP_RE.match(line)
            if m:
                d["stall_pct"] = float(m.group(1))   # RWW-phase write stall %
                continue
            m = STOPS_RE.search(line)
            if m:
                d.setdefault("stall_delays", int(m.group(1)))
                d.setdefault("stall_stops", int(m.group(2)))
                continue
            if line.startswith("Microseconds per read:"):
                in_read_hist, grab = True, False
                continue
            if in_read_hist:
                m = MAX_RE.match(line)
                if m:
                    d.setdefault("read_max_us", int(m.group(1)))
                    continue
                m = PCT_RE.match(line)
                if m:
                    d["p50"], d["p99"] = float(m.group(1)), float(m.group(2))
                    d["p999"], d["p9999"] = float(m.group(3)), float(m.group(4))
                    continue
                if line.startswith("---"):
                    grab = True
                    continue
                if grab:
                    m = BKT_RE.match(line)
                    if m:
                        buckets.append((int(m.group(2)), int(m.group(3))))  # (hi_us, count)
                        continue
                    in_read_hist = grab = False   # blank/next section ends the table
    d["buckets"] = buckets
    return d


def main():
    rundir = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else "results/fig12"
    outdir = os.path.join(rundir, "analysis")
    os.makedirs(outdir, exist_ok=True)

    cells = defaultdict(dict)             # (cfg, jobs) -> {inst: parsed}
    for fn in sorted(os.listdir(rundir)):
        m = LOG_RE.search(fn)
        if not m or "_fill_" in fn or fn.startswith("zmkfs"):
            continue
        cells[(m.group(1), int(m.group(2)))][int(m.group(3))] = \
            parse_log(os.path.join(rundir, fn))
    if not cells:
        sys.exit(f"no (WAL|L0)_*_n*_j*_i*.log found under {rundir}")

    # config pair (auto): ZNS-side first, CNS-side second; figure prefix by family
    _cfgs = sorted({c for c, _ in cells})
    cfgA = next((c for c in _cfgs if c.endswith("ZNS")), _cfgs[0])
    cfgB = next((c for c in _cfgs if c != cfgA), cfgA)
    P, PT, PC = (("fig11", "b", "c") if cfgA.startswith("WAL")
                 else ("fig13", "a", "b"))   # L0 runs: (a) throughput, (b) CDF
    jobs_list = sorted({j for _, j in cells})

    # tidy CSV (per instance) + merged CDF CSV (per cfg,jobs)
    with open(os.path.join(outdir, "fig12_metrics.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cfg", "jobs", "inst", "read_ops", "write_ops",
                    "read_us_op", "p50", "p99", "p999", "p9999", "read_max_us",
                    "stall_pct", "stall_stops"])
        for (cfg, j), insts in sorted(cells.items()):
            for i, d in sorted(insts.items()):
                w.writerow([cfg, j, i, d.get("read_ops"), d.get("write_ops"),
                            d.get("read_us_op"), d.get("p50"), d.get("p99"),
                            d.get("p999"), d.get("p9999"), d.get("read_max_us"),
                            d.get("stall_pct"), d.get("stall_stops")])

    def merged_cdf(cfg, j):
        acc = defaultdict(int)
        for d in cells[(cfg, j)].values():
            for hi, cnt in d["buckets"]:
                acc[hi] += cnt
        total = sum(acc.values()) or 1
        xs, ys, run = [], [], 0
        for hi in sorted(acc):
            run += acc[hi]
            xs.append(hi)
            ys.append(100.0 * run / total)
        return xs, ys

    with open(os.path.join(outdir, "fig12_cdf.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cfg", "jobs", "latency_us_upper", "cum_pct"])
        for (cfg, j) in sorted(cells):
            for x, y in zip(*merged_cdf(cfg, j)):
                w.writerow([cfg, j, x, f"{y:.4f}"])

    # dense quantile grid (tail-dense) for the EXACT per-op CDF
    QG = np.unique(np.concatenate([np.linspace(0, 99, 300),
                                   100 - np.logspace(-4, 0, 220)[::-1], [100.0]]))

    def oplat_read_lat(cfg, j):
        """All read-op (op==0) latencies (us) for a cell, both instances,
        from <tag>_oplat.tar.gz (per-thread oplat_p*_readwhilewriting_t*.csv)."""
        lats = []
        for tgz in glob.glob(os.path.join(rundir, f"{cfg}_n*_j{j}_oplat.tar.gz")):
            with tarfile.open(tgz) as t:
                for m in t.getmembers():
                    if not (m.name.endswith(".csv") and "readwhilewriting" in m.name):
                        continue
                    fh = io.TextIOWrapper(t.extractfile(m)); next(fh, None)
                    for ln in fh:
                        c = ln.split(",")
                        if len(c) >= 3 and c[2].strip() == "0":  # op 0 = read
                            lats.append(int(c[1]))
        return np.asarray(lats, np.int64)

    have_oplat = bool(glob.glob(os.path.join(rundir, "*_oplat.tar.gz")))

    def read_cdf(cfg, j):
        """(latency_us, cum_pct) — exact from oplat if present, else the
        histogram-bucket CDF as a fallback."""
        if have_oplat:
            arr = oplat_read_lat(cfg, j)
            if arr.size:
                return np.percentile(arr, QG), QG
        return merged_cdf(cfg, j)

    def style(ax, ylab):
        ax.set_axisbelow(True)
        ax.grid(axis="y", color=GRID, linewidth=0.8)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
        for s in ("left", "bottom"):
            ax.spines[s].set_color(GRID)
        ax.tick_params(colors=INK2, labelsize=9)
        ax.set_ylabel(ylab, color=INK2, fontsize=9)

    def avg_of(cfg, j, *keys):
        # convention: dual ops are reported as the MEAN of the two instances;
        # pass several keys to average their per-instance SUM (e.g. read+write)
        vs = [sum(d.get(k, 0) for k in keys) for d in cells.get((cfg, j), {}).values()]
        return sum(vs) / len(vs) if vs else 0

    # fig11(b) v1 — RWW TOTAL throughput (read + writer ops) bars, ratio on top
    fig, ax = plt.subplots(figsize=(5.2, 3.4))
    w = 0.32
    zns = [avg_of(cfgA, j, "read_ops", "write_ops") / 1e3 for j in jobs_list]
    cns = [avg_of(cfgB, j, "read_ops", "write_ops") / 1e3 for j in jobs_list]
    for off, vals, col, lab in ((-w / 2, zns, C_ZNS, cfgA),
                                (w / 2, cns, C_CNS, cfgB)):
        bars = ax.bar([i + off for i in range(len(jobs_list))], vals, w * 0.94,
                      color=col, label=lab)
        for b in bars:
            ax.annotate(f"{b.get_height():.0f}", (b.get_x() + b.get_width() / 2,
                        b.get_height()), ha="center", va="bottom", fontsize=8, color=INK)
    for i in range(len(jobs_list)):
        if zns[i] > 0 and cns[i] > 0:
            ax.annotate(f"{cns[i]/zns[i]:.2f}x", (i, max(zns[i], cns[i]) * 1.12),
                        ha="center", fontsize=9, color=INK, fontweight="bold")
    ax.set_ylim(0, max(zns + cns + [1]) * 1.3)
    ax.set_xticks(range(len(jobs_list)), [f"j{j}" for j in jobs_list])
    style(ax, "RWW read+write throughput (x1000 ops/s, avg of 2 inst)")
    ax.legend(frameon=False, fontsize=9, loc="upper left")
    fig.tight_layout()
    for ext in ("png", "eps"):
        fig.savefig(os.path.join(outdir, f"{P}_{PT}_rww_throughput_total.{ext}"), dpi=150)
    plt.close(fig)

    # fig11(b) v2 — read/write breakdown: per jobs, WAL_ZNS and WAL_CNS as
    # stacked bars (read bottom + write top = total height). One dodge pair per
    # jobs group; read = solid, write = hatched, same cfg hue.
    fig, ax = plt.subplots(figsize=(5.6, 3.6))
    w = 0.34
    for off, cfg, col in ((-w / 2, cfgA, C_ZNS), (w / 2, cfgB, C_CNS)):
        rd = [avg_of(cfg, j, "read_ops") / 1e3 for j in jobs_list]
        wr = [avg_of(cfg, j, "write_ops") / 1e3 for j in jobs_list]
        xs = [i + off for i in range(len(jobs_list))]
        ax.bar(xs, rd, w * 0.94, color=col, label=f"{cfg} read")
        # write segment: EPS-safe (no alpha) — white face + cfg-hued hatch/edge
        ax.bar(xs, wr, w * 0.94, bottom=rd, color="white", hatch="///",
               edgecolor=col, linewidth=0.7, label=f"{cfg} write")
        for x, r, wv in zip(xs, rd, wr):
            ax.annotate(f"{r+wv:.0f}", (x, r + wv), ha="center", va="bottom",
                        fontsize=7.5, color=INK)
    ax.set_xticks(range(len(jobs_list)), [f"j{j}" for j in jobs_list])
    style(ax, "RWW throughput by op (x1000 ops/s, avg of 2 inst)")
    ax.legend(frameon=False, fontsize=8, loc="upper left", ncol=2)
    fig.tight_layout()
    for ext in ("png", "eps"):
        fig.savefig(os.path.join(outdir, f"{P}_{PT}_rww_throughput_rw.{ext}"), dpi=150)
    plt.close(fig)

    # fig11(b) supplementary — P99.9 read latency per jobs (mean of 2 inst)
    fig, ax = plt.subplots(figsize=(5.2, 3.4))
    w = 0.32

    def mean_of(cfg, j, k):
        vs = [d[k] for d in cells.get((cfg, j), {}).values() if k in d]
        return sum(vs) / len(vs) if vs else float("nan")

    zns = [mean_of(cfgA, j, "p999") / 1e3 for j in jobs_list]
    cns = [mean_of(cfgB, j, "p999") / 1e3 for j in jobs_list]
    for off, vals, col, lab in ((-w / 2, zns, C_ZNS, cfgA),
                                (w / 2, cns, C_CNS, cfgB)):
        bars = ax.bar([i + off for i in range(len(jobs_list))], vals, w * 0.94,
                      color=col, label=lab)
        for b in bars:
            ax.annotate(f"{b.get_height():.1f}", (b.get_x() + b.get_width() / 2,
                        b.get_height()), ha="center", va="bottom", fontsize=8, color=INK)
    ax.set_xticks(range(len(jobs_list)), [f"j{j}" for j in jobs_list])
    style(ax, "read P99.9 (ms, avg of 2 inst)")
    ax.legend(frameon=False, fontsize=9, loc="upper left")
    fig.tight_layout()
    for ext in ("png", "eps"):
        fig.savefig(os.path.join(outdir, f"{P}_{PT}_read_p999.{ext}"), dpi=150)
    plt.close(fig)

    # fig11(c) — read-latency CDF (one panel per jobs; log-x; 2 lines each),
    # EXACT from per-op oplat when present (else histogram-bucket fallback)
    src = "per-op oplat" if have_oplat else "histogram buckets"
    cdf_pts = [("cfg", "jobs", "latency_us", "cum_pct", "source")]
    fig, axes = plt.subplots(1, len(jobs_list), figsize=(3.2 * len(jobs_list), 3.2),
                             sharey=True)
    axes = [axes] if len(jobs_list) == 1 else list(axes)
    for ax, j in zip(axes, jobs_list):
        for cfg, col in ((cfgA, C_ZNS), (cfgB, C_CNS)):
            if (cfg, j) not in cells:
                continue
            xs, ys = read_cdf(cfg, j)
            ax.plot(xs, ys, color=col, linewidth=2, label=cfg)
            for x, y in zip(xs, ys):
                cdf_pts.append((cfg, j, f"{x:.0f}", f"{y:.4f}", src))
        ax.set_xscale("log")
        ax.set_xlabel("read latency (us, log)", color=INK2, fontsize=9)
        ax.set_title(f"j{j}", fontsize=10, color=INK)
        style(ax, "cumulative % of reads" if ax is axes[0] else "")
        ax.set_ylim(0, 101)
    axes[0].legend(frameon=False, fontsize=9, loc="lower right")
    fig.suptitle(f"read-latency CDF ({src})", fontsize=9, color=INK2, x=0.01, ha="left")
    fig.tight_layout()
    for ext in ("png", "eps"):
        fig.savefig(os.path.join(outdir, f"{P}_{PC}_read_cdf.{ext}"), dpi=150)
    plt.close(fig)
    with open(os.path.join(outdir, f"{P}_{PC}_read_cdf.csv"), "w", newline="") as f:
        csv.writer(f).writerows(cdf_pts)

    with open(os.path.join(outdir, "REPORT.md"), "w") as f:
        f.write(f"# fig12 RWW WAL-placement — {os.path.basename(rundir)}\n\n")
        f.write("| jobs | total ZNS | total CNS | ratio | read Z/C | write Z/C "
                "| P99.9 Z/C (us) | worst-read Z/C (s) | stall% Z/C | stops Z/C |\n"
                "|---|" + "---|" * 9 + "\n")
        for j in jobs_list:
            def mean(cfg, k):
                vs = [d[k] for d in cells.get((cfg, j), {}).values() if k in d]
                return sum(vs) / len(vs) if vs else float("nan")
            def summ(cfg, k):
                return sum(d.get(k, 0) for d in cells.get((cfg, j), {}).values())
            def wmax(cfg):
                return max((d.get("read_max_us", 0)
                            for d in cells.get((cfg, j), {}).values()), default=0)
            tz = avg_of(cfgA, j, "read_ops", "write_ops")
            tc = avg_of(cfgB, j, "read_ops", "write_ops")
            ratio = f"**{tc/tz:.2f}x**" if tz else "-"
            f.write(f"| j{j} | {tz:,.0f} | {tc:,.0f} | {ratio} "
                    f"| {avg_of(cfgA,j,'read_ops'):,.0f}/{avg_of(cfgB,j,'read_ops'):,.0f} "
                    f"| {avg_of(cfgA,j,'write_ops'):,.0f}/{avg_of(cfgB,j,'write_ops'):,.0f} "
                    f"| {mean(cfgA,'p999'):,.0f}/{mean(cfgB,'p999'):,.0f} "
                    f"| {wmax(cfgA)/1e6:.2f}/{wmax(cfgB)/1e6:.2f} "
                    f"| {mean(cfgA,'stall_pct'):.1f}/{mean(cfgB,'stall_pct'):.1f} "
                    f"| {summ(cfgA,'stall_stops')}/{summ(cfgB,'stall_stops')} |\n")
        f.write(f"\n{P}({PT}): {P}_{PT}_rww_throughput_total + _rw (+ _read_p999)  |  "
                f"{P}({PC}): {P}_{PC}_read_cdf (.png/.eps)\n"
                f"CSVs: fig12_metrics.csv, {P}_{PC}_read_cdf.csv"
                + (" (exact per-op oplat)" if have_oplat else
                   " (histogram-bucket CDF; no oplat in run)") + "\n")
    print(f"analysis -> {outdir}  (read CDF source: "
          f"{'per-op oplat' if have_oplat else 'histogram buckets'})")


if __name__ == "__main__":
    main()
