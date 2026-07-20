#!/usr/bin/env python3
"""fig_plot.py — shared normalized grouped-bar plotter for Fig7 and Fig8.

Per-workload throughput of 2-3 modes, normalized to a base mode = 1. The mode
columns are read from the values-CSV header, so the SAME script serves:

    Fig7 (multi-device):  workload,zn540,zns,hyzns   (3 series)
    Fig8 (single-device): workload,zn540,hyzns        (2 series, HyZNS only)

A mode column with no data in any workload is dropped (no empty legend entry).

Values CSV (edit by hand; raw ops/sec, blank cell = bar omitted):

    workload,zn540,zns,hyzns
    RS,1184106,1089579,1116074
    RR,135178,99605,115731
    RWW-R,,,           <- readwhilewriting read side
    RWW-W,,,           <- readwhilewriting write side (patched db_bench line)
    FS,483213,358648,397396
    FR,251385,192124,205815
    OW,215593,162395,167227

Usage:
    # Fig7
    python3 scripts/fig_plot.py --template --log results/fig7/fig7_log.csv \
        results/fig7/fig7_values.csv                 # prefill zns/hyzns
    python3 scripts/fig_plot.py results/fig7/fig7_values.csv -o results/fig7/fig7.png
    python3 scripts/fig_plot.py results/fig7/fig7_values.csv --base zns   # no ZN540 yet
    # Fig8 (2 series)
    python3 scripts/fig_plot.py --template --log results/fig8/fig8_log.csv \
        --modes zn540,hyzns results/fig8/fig8_values.csv
    python3 scripts/fig_plot.py results/fig8/fig8_values.csv -o results/fig8/fig8.png

Writes <out>.png (300 dpi) and <out>.eps (paper; EPS => no alpha anywhere).
"""
import argparse
import csv
import os
import sys

KNOWN_MODES = ["zn540", "zns", "hyzns"]
LABELS = {"zn540": "ZN540", "zns": "femu-ZNS", "hyzns": "femu-HyZNS"}
# Okabe-Ito subset (colorblind-safe); hatches double-encode identity for
# grayscale print of the EPS.
COLORS = {"zn540": "#009E73", "zns": "#D55E00", "hyzns": "#0072B2"}
HATCHES = {"zn540": "", "zns": "//", "hyzns": "\\\\"}
DEFAULT_ORDER = ["RS", "RR", "RWW-R", "RWW-W", "RWW-T", "FS", "FR", "OW"]


def read_values(path):
    """Return (modes, rows). Modes come from the header (cols after workload)."""
    modes, rows = None, []
    with open(path) as f:
        for rec in csv.reader(f):
            if not rec or not rec[0].strip() or rec[0].strip().startswith("#"):
                continue
            if rec[0].strip().lower() == "workload":
                modes = [c.strip() for c in rec[1:] if c.strip()]
                bad = [m for m in modes if m not in KNOWN_MODES]
                if bad:
                    sys.exit(f"unknown mode column(s) {bad} in {path} header")
                continue
            if modes is None:
                sys.exit(f"{path}: missing 'workload,<mode>,...' header row")
            w = rec[0].strip()
            vals = {}
            for i, m in enumerate(modes, start=1):
                cell = rec[i].strip() if len(rec) > i else ""
                vals[m] = float(cell) if cell else None
            rows.append((w, vals))
    if not rows:
        sys.exit(f"no data rows in {path}")
    return modes, rows


def write_template(log_path, out_path, geom, modes):
    """Prefill each non-zn540 mode with the latest ops_sec per workload."""
    latest = {}  # (workload, mode) -> ops_sec ; later rows win
    if os.path.exists(log_path):
        with open(log_path) as f:
            for rec in csv.DictReader(f):
                if geom and rec["geom"] != geom:
                    continue
                latest[(rec["workload"], rec["mode"])] = rec["ops_sec"]
    else:
        print(f"note: {log_path} not found — emitting an empty template")
    with open(out_path, "w") as f:
        f.write("workload," + ",".join(modes) + "\n")
        for w in DEFAULT_ORDER:
            cells = [latest.get((w, m), "") if m != "zn540" else "" for m in modes]
            f.write(f"{w}," + ",".join(cells) + "\n")
    print(f"template ({','.join(modes)}) -> {out_path}  "
          f"(fill the zn540 column, then plot)")


def plot(modes, rows, base, out, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    workloads = [w for w, _ in rows]
    norm = {m: [] for m in modes}
    skipped = []
    for w, vals in rows:
        b = vals.get(base)
        if not b:
            skipped.append(w)
            for m in modes:
                norm[m].append(None)
            continue
        for m in modes:
            norm[m].append(vals[m] / b if vals[m] else None)
    if skipped:
        print(f"warn: no '{base}' value for {skipped} — those groups are empty")
    # drop modes that have no bar anywhere (e.g. an all-blank column)
    modes = [m for m in modes if any(v is not None for v in norm[m])]
    if not modes:
        sys.exit("nothing to plot — every mode column is empty")

    fig, ax = plt.subplots(figsize=(6.4, 2.6))
    n = len(workloads)
    ng = len(modes)
    bw = min(0.26, 0.8 / ng)
    for k, m in enumerate(modes):
        off = (k - (ng - 1) / 2) * (bw + 0.02)  # center the group on each tick
        xs, hs = [], []
        for i, v in enumerate(norm[m]):
            if v is not None:
                xs.append(i + off)
                hs.append(v)
        if not xs:
            continue
        bars = ax.bar(xs, hs, width=bw, label=LABELS[m], color=COLORS[m],
                      hatch=HATCHES[m], edgecolor="white", linewidth=0.6, zorder=3)
        for b, v in zip(bars, hs):
            ax.annotate(f"{v:.2f}", (b.get_x() + b.get_width() / 2, v),
                        ha="center", va="bottom", fontsize=6.5, color="#333333")

    ax.axhline(1.0, color="#888888", linewidth=0.8, linestyle="--", zorder=2)
    ax.set_xticks(range(n))
    ax.set_xticklabels(workloads, fontsize=9)
    ax.set_ylabel(f"Normalized throughput\n({LABELS[base]} = 1)", fontsize=8.5)
    ax.tick_params(axis="y", labelsize=8)
    ax.grid(axis="y", color="#dddddd", linewidth=0.6, zorder=0)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.legend(ncol=len(modes), fontsize=8, frameon=False,
              loc="lower center", bbox_to_anchor=(0.5, 1.0))
    if title:
        ax.set_title(title, fontsize=9, pad=24)
    fig.tight_layout()

    stem = os.path.splitext(out)[0]
    fig.savefig(stem + ".png", dpi=300)
    fig.savefig(stem + ".eps")
    print(f"wrote {stem}.png and {stem}.eps")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("values", nargs="?", help="values CSV (workload,<mode>,...)")
    p.add_argument("-o", "--out", default="results/fig7/fig7.png")
    p.add_argument("--base", default="zn540", choices=KNOWN_MODES,
                   help="normalization base (use zns/hyzns before ZN540 is measured)")
    p.add_argument("--title", default="")
    p.add_argument("--template", action="store_true",
                   help="emit a values CSV prefilled from --log instead of plotting")
    p.add_argument("--log", default="results/fig7/fig7_log.csv")
    p.add_argument("--modes", default="zn540,zns,hyzns",
                   help="comma list of mode columns for --template header")
    p.add_argument("--geom", default="", help="filter --template rows by geometry tag")
    args = p.parse_args()

    if args.template:
        out = args.values or "results/fig7/fig7_values.csv"
        modes = [m.strip() for m in args.modes.split(",") if m.strip()]
        bad = [m for m in modes if m not in KNOWN_MODES]
        if bad:
            p.error(f"unknown --modes {bad} (known: {KNOWN_MODES})")
        write_template(args.log, out, args.geom, modes)
        return
    if not args.values:
        p.error("values CSV required (or use --template)")
    modes, rows = read_values(args.values)
    if args.base not in modes:
        p.error(f"--base {args.base} not among the CSV's modes {modes}")
    plot(modes, rows, args.base, args.out, args.title)


if __name__ == "__main__":
    main()
