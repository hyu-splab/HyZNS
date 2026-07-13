#!/usr/bin/env python3
# fig10_cdf.py <oplat_dir> <events.csv> <out_prefix> [--bench readwhilewriting]
#
# Paper Fig10 (fig:resize-cdf): READ-ONLY per-op latency CDFs around resize.
# For each resize direction present in events.csv (grow -> (a), shrink -> (b)):
#   <out>_<dir>_cdf.png/.eps       linear x, clipped at the max P99.9 of the
#                                  two curves (one 100ms op must not flatten it)
#   <out>_<dir>_cdf_logx.png/.eps  log x to P100 (the full tail)
# Curves: "during <dir>" = reads completed inside that direction's resize
# window(s) [echo_t, done_t]; "resize excluded" = reads outside ALL resize
# windows (any direction) — so a run with one grow AND one shrink yields the
# SAME excluded curve in (a) and (b), by construction.
# Also writes <out>_fig10_tails.csv (count + P50/90/99/99.9/99.99/max per curve).
import argparse, csv, glob, os, re, sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

C_EXCL, C_IN = "#2a78d6", "#1baf7a"       # curve colors (excluded / in-window)
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e5e4e0"
QS = (50, 90, 99, 99.9, 99.99, 100)

ap = argparse.ArgumentParser()
ap.add_argument("oplat_dir"); ap.add_argument("events_csv"); ap.add_argument("out_prefix")
ap.add_argument("--bench", default="readwhilewriting")
a = ap.parse_args()

files = sorted(glob.glob(os.path.join(a.oplat_dir, f"oplat_p*_{a.bench}_t*.csv")))
if not files:
    sys.exit(f"no oplat_p*_{a.bench}_t*.csv under {a.oplat_dir}")
ts_l, lat_l, op_l = [], [], []
for fn in files:
    d = np.loadtxt(fn, delimiter=",", skiprows=1,
                   dtype={"names": ("ts", "lat", "op"),
                          "formats": (np.uint64, np.int64, np.int8)}, ndmin=1)
    if d.size:
        ts_l.append(d["ts"]); lat_l.append(d["lat"]); op_l.append(d["op"])
ts = np.concatenate(ts_l); lat = np.concatenate(lat_l); op = np.concatenate(op_l)
rd = op == 0
print(f"[fig10_cdf] {len(files)} files, {rd.sum():,} reads / {len(op):,} ops")

wins = []                                  # (t0_us, t1_us, dir)
with open(a.events_csv) as f:
    for r in csv.DictReader(f):
        try:
            wins.append((float(r["echo_t_epoch"]) * 1e6,
                         float(r["done_t_epoch"]) * 1e6, r["dir"]))
        except (KeyError, ValueError):
            pass
if not wins:
    sys.exit("events.csv has no resize events (baseline run -> nothing to split)")

in_any = np.zeros(len(ts), bool)
in_dir = {}
for t0, t1, dr in wins:
    m = (ts >= t0) & (ts <= t1)
    in_any |= m
    in_dir[dr] = in_dir.get(dr, np.zeros(len(ts), bool)) | m
excl = rd & ~in_any


def style(ax):
    ax.set_axisbelow(True)
    ax.grid(color=GRID, linewidth=0.8)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)


def cdf_xy(v):
    v = np.sort(v)
    return v, np.arange(1, len(v) + 1) / len(v) * 100.0


tails = [("curve", "count") + tuple(f"P{q}" for q in QS)]
for dr, mask in sorted(in_dir.items()):
    during = lat[rd & mask]
    base = lat[excl]
    if not len(during):
        print(f"  {dr}: no reads inside window — skipped"); continue
    for name, v in ((f"during {dr}", during), ("resize excluded", base)):
        tails.append((f"{dr}:{name}", len(v)) +
                     tuple(int(np.percentile(v, q)) for q in QS))
    for logx in (False, True):
        fig, ax = plt.subplots(figsize=(4.6, 3.4))
        for v, name, col, ls in ((base, "resize excluded", C_EXCL, "-"),
                                 (during, f"during {dr}", C_IN, "--")):
            x, y = cdf_xy(v)
            ax.plot(x, y, color=col, ls=ls, lw=2, label=name)
        if logx:
            ax.set_xscale("log")
        else:
            ax.set_xlim(0, max(np.percentile(base, 99.9),
                               np.percentile(during, 99.9)))
        ax.set_ylim(0, 101)
        ax.set_xlabel("read latency (us)" + (" (log)" if logx else ""),
                      color=INK2, fontsize=9)
        ax.set_ylabel("cumulative % of reads", color=INK2, fontsize=9)
        ax.set_title(f"{dr}: reads during resize vs excluded", fontsize=10, color=INK)
        style(ax)
        ax.legend(frameon=False, fontsize=9, loc="lower right")
        fig.tight_layout()
        sfx = "_logx" if logx else ""
        for ext in ("png", "eps"):
            fig.savefig(f"{a.out_prefix}_{dr}_cdf{sfx}.{ext}", dpi=150)
        plt.close(fig)
        print(f"saved {a.out_prefix}_{dr}_cdf{sfx}.png/.eps")

with open(f"{a.out_prefix}_fig10_tails.csv", "w", newline="") as f:
    csv.writer(f).writerows(tails)
for row in tails:
    print("  " + ",".join(str(x) for x in row))

# CDF point data (quantile-sampled, 1024 pts/curve) so the figures can be
# re-plotted from CSV alone: curve,latency_us,cum_pct
with open(f"{a.out_prefix}_fig10_cdf_points.csv", "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["curve", "latency_us", "cum_pct"])
    qs = np.linspace(0, 100, 1025)
    curves = [("resize_excluded", lat[excl])]
    curves += [(f"during_{dr}", lat[rd & mask]) for dr, mask in sorted(in_dir.items())]
    for name, v in curves:
        if not len(v):
            continue
        for q, x in zip(qs, np.percentile(v, qs)):
            w.writerow([name, int(x), f"{q:.2f}"])
print("saved", f"{a.out_prefix}_fig10_cdf_points.csv")
