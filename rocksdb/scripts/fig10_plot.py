#!/usr/bin/env python3
"""fig10_plot.py — Fig10 WAL placement (dual-instance, HyZNS): aggregate
db_bench throughput vs background-thread count, WAL_ZNS vs WAL_CNS, for the FR
and OW benchmarks.

Input: the cumulative fig10_log.csv written by fig10_run.sh
       (utc,run_tag,geom,cfg,num,jobs,inst,fillrandom_ops,overwrite_ops,...).
Uses the aggregate (inst=agg) rows; when a (cfg,jobs) cell was measured more
than once, the latest run wins.

    python3 scripts/fig10_plot.py results/fig10/fig10_log.csv -o results/fig10/fig10.png
    python3 scripts/fig10_plot.py results/fig10/fig10_log.csv --geom 8x4

Writes <out>.png (300 dpi) + <out>.eps (paper).
"""
import argparse
import csv
import os
import sys

CFGS = ["WAL_ZNS", "WAL_CNS"]
LABELS = {"WAL_ZNS": "WAL_ZNS", "WAL_CNS": "WAL_CNS"}
# Okabe-Ito, colorblind-safe; hatch double-encodes for grayscale EPS.
COLORS = {"WAL_ZNS": "#D55E00", "WAL_CNS": "#0072B2"}
HATCHES = {"WAL_ZNS": "", "WAL_CNS": "//"}
BENCHES = [("fillrandom_ops", "FR (fillrandom)"), ("overwrite_ops", "OW (overwrite)")]


def read_agg(path, geom):
    # latest agg value per (cfg, jobs) -> {(cfg,jobs): {field: ops}}
    cell = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            if r["inst"] != "agg":
                continue
            if geom and r["geom"] != geom:
                continue
            cell[(r["cfg"], int(r["jobs"]))] = r  # later rows win
    if not cell:
        sys.exit(f"no agg rows in {path}" + (f" for geom={geom}" if geom else ""))
    jobs = sorted({j for _, j in cell})
    return cell, jobs


def plot(cell, jobs, out, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 2, figsize=(7.2, 3.0), sharex=True)
    n = len(jobs)
    bw = 0.38
    for ax, (field, bench_label) in zip(axes, BENCHES):
        for k, cfg in enumerate(CFGS):
            xs, hs = [], []
            for i, j in enumerate(jobs):
                r = cell.get((cfg, j))
                v = float(r[field]) / 1000.0 if r and r[field] else 0.0  # kops/s
                xs.append(i + (k - 0.5) * (bw + 0.02))
                hs.append(v)
            bars = ax.bar(xs, hs, width=bw, label=LABELS[cfg], color=COLORS[cfg],
                          hatch=HATCHES[cfg], edgecolor="white", linewidth=0.6, zorder=3)
        # CNS/ZNS ratio annotation above each thread group
        for i, j in enumerate(jobs):
            rz = cell.get(("WAL_ZNS", j)); rc = cell.get(("WAL_CNS", j))
            z = float(rz[field]) if rz and rz[field] else 0
            c = float(rc[field]) if rc and rc[field] else 0
            if z > 0 and c > 0:
                top = max(z, c) / 1000.0
                ax.annotate(f"{c/z:.2f}x", (i, top), ha="center", va="bottom",
                            fontsize=7.5, color="#333")
        ax.set_title(bench_label, fontsize=9)
        ax.set_xticks(range(n))
        ax.set_xticklabels([str(j) for j in jobs], fontsize=9)
        ax.set_xlabel("background threads", fontsize=8.5)
        ax.grid(axis="y", color="#dddddd", linewidth=0.6, zorder=0)
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)
    axes[0].set_ylabel("aggregate throughput\n(kops/s, both instances)", fontsize=8.5)
    axes[1].legend(fontsize=8, frameon=False, loc="upper left")
    if title:
        fig.suptitle(title, fontsize=10)
    fig.tight_layout()
    stem = os.path.splitext(out)[0]
    fig.savefig(stem + ".png", dpi=300)
    fig.savefig(stem + ".eps")
    print(f"wrote {stem}.png and {stem}.eps")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("csv", help="fig10_log.csv")
    p.add_argument("-o", "--out", default="results/fig10/fig10.png")
    p.add_argument("--geom", default="", help="filter rows by geometry tag")
    p.add_argument("--title", default="")
    args = p.parse_args()
    cell, jobs = read_agg(args.csv, args.geom)
    plot(cell, jobs, args.out, args.title)


if __name__ == "__main__":
    main()
