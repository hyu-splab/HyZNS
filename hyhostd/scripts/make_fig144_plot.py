#!/usr/bin/env python3
# make_fig144_plot.py <fig144_dir> — cumulative moving-average throughput of L0
# placement, in THREE views (each its own figure):
#   fig144_all.{png,eps}  whole run, FR+OW as one continuous running avg
#   fig144_fr.{png,eps}   fillrandom-only running avg (each inst up to its OW start)
#   fig144_ow.{png,eps}   overwrite-only running avg (each inst from its OW start)
# Series per view: VANILLA(L0_ZNS) / L0_CNS_nomod(static, dies) / L0_CNS_mod
# (daemon). daemon R-grow points and static nospc-death are marked on the FR and
# whole-run views. Reads the CSVs make_ravg_csv.py emits:
#   ravg_<name>[_fr|_ow].csv        = t,ravg
#   ravg_mod_resizes[_fr|_ow].csv   = t,ravg,R
#   cum_nomod_death.csv             = t,cum   (static death, FR phase)
import os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

B = sys.argv[1]
C = {"VANILLA": ("#2a78d6", "L0_ZNS"),
     "L0_CNS_nomod": ("#d62728", "L0_fixed_CNS (static)"),
     "L0_CNS_mod": ("#1baf7a", "L0_res_CNS (daemon)")}
INK2, GRID = "#52514e", "#e5e4e0"


def rd(p, n=2):
    if not os.path.exists(p):
        return [[] for _ in range(n)]
    cols = [[] for _ in range(n)]
    for ln in open(p):
        f = ln.strip().split(",")
        if len(f) >= n:
            for i in range(n):
                cols[i].append(float(f[i]))
    return cols


def style(ax, ylab, title):
    ax.set_axisbelow(True)
    ax.grid(color=GRID, lw=0.8)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.set_xlabel("time since phase start (s)", color=INK2, fontsize=9)
    ax.set_ylabel(ylab, color=INK2, fontsize=9)
    ax.set_title(title, fontsize=10, loc="left")


def plot_view(suffix, title, tag, show_death):
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    for key, (col, lab) in C.items():
        x, y = rd(os.path.join(B, f"ravg_{key}{suffix}.csv"))
        if x:
            ax.plot(x, [v / 1000 for v in y], color=col, lw=2, label=lab)

    # daemon R-grow markers on this view's axis (+ R value annotations)
    rx, ry, rR = rd(os.path.join(B, f"ravg_mod_resizes{suffix}.csv"), 3)
    prevR = None
    for t, v, R in zip(rx, ry, rR):
        if prevR is not None and R != prevR:      # a grow happened at t
            ax.plot(t, v / 1000, "^", color=C["L0_CNS_mod"][0], ms=7, zorder=5)
            ax.annotate(f"R{int(R)}", (t, v / 1000), fontsize=7, color=INK2,
                        ha="center", va="bottom")
        prevR = R
    if rx:
        ax.plot([], [], "^", color=C["L0_CNS_mod"][0], ms=7,
                label="daemon grow (R↑)")

    # static nospc-death marker (FR / whole-run views only — it dies in FR)
    if show_death:
        dx, _ = rd(os.path.join(B, "cum_nomod_death.csv"))
        if dx:
            ax.axvline(dx[0], color=C["L0_CNS_nomod"][0], ls="--", lw=1.2)
            ax.annotate("static nospc death", (dx[0], ax.get_ylim()[1] * 0.5),
                        color=C["L0_CNS_nomod"][0], fontsize=8, rotation=90,
                        ha="right", va="center")

    style(ax, "cumulative moving-avg throughput (k ops/s, i1+i2)", title)
    ax.legend(frameon=False, fontsize=9, loc="lower right")
    fig.tight_layout()
    for ext in ("png", "eps"):
        fig.savefig(os.path.join(B, f"fig144_{tag}.{ext}"), dpi=150)
    plt.close(fig)
    print(f"-> fig144_{tag}.png/.eps")


plot_view("",    "whole run (fillrandom→overwrite) — L0 placement", "all", True)
plot_view("_fr", "fillrandom only — L0 placement",                   "fr",  True)
plot_view("_ow", "overwrite only (OW-only running avg) — L0 placement", "ow", False)
