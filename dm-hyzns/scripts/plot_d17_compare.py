#!/usr/bin/env python3
"""Plot Greedy vs Cost-Benefit GC comparison from two D14-shape sweep CSVs.

Usage:
    plot_d17_compare.py <greedy.csv> <cb.csv> [out.svg]
"""
import csv, os, sys

if len(sys.argv) < 3:
    print(__doc__, file=sys.stderr); sys.exit(2)
GREEDY_CSV = sys.argv[1]
CB_CSV     = sys.argv[2]
OUT = sys.argv[3] if len(sys.argv) > 3 else \
      os.path.join(os.path.dirname(CB_CSV) or ".", "d17_greedy_vs_cb.svg")


def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append({"r_end": int(r["r_end_gib"]),
                         "bw":    float(r["bw_mib_s"]),
                         "p99":   int(r["p99_us"]),
                         "waf":   float(r["waf"]),
                         "gc_mig": int(r["gc_mig"])})
    rows.sort(key=lambda x: x["r_end"])
    return rows


greedy = load(GREEDY_CSV)
cb     = load(CB_CSV)

# Use the union of r_end values (linear x-spacing by index).
xs = sorted(set(r["r_end"] for r in greedy) | set(r["r_end"] for r in cb))


def lookup(rows, x, key):
    for r in rows:
        if r["r_end"] == x: return r[key]
    return None


# Layout
P_W, P_H = 320, 230
GUTTER = 60
L_MARG, R_MARG = 60, 30
T_MARG, B_MARG = 60, 60
TITLE_H = 30
W = L_MARG + 3 * P_W + 2 * GUTTER + R_MARG
H = T_MARG + TITLE_H + P_H + B_MARG


def x_to_px(panel_left, idx):
    if len(xs) == 1: return panel_left + P_W / 2
    return panel_left + 32 + (idx / (len(xs) - 1)) * (P_W - 64)


def y_to_px(panel_top, v, vmin, vmax):
    if vmax == vmin: return panel_top + P_H / 2
    return panel_top + P_H - ((v - vmin) / (vmax - vmin)) * (P_H - 30) - 15


def panel(idx, title, key, ylabel, fmt, vmin, vmax):
    pl = L_MARG + idx * (P_W + GUTTER)
    pt = T_MARG + TITLE_H
    pr, pb = pl + P_W, pt + P_H
    out = []
    out.append(f'<rect x="{pl}" y="{pt}" width="{P_W}" height="{P_H}" '
               f'fill="white" stroke="#333" stroke-width="1"/>')
    out.append(f'<text x="{pl + P_W/2}" y="{pt - 8}" text-anchor="middle" '
               f'font-size="14" font-weight="bold">{title}</text>')

    # y grid + tick labels
    for i in range(5):
        v = vmin + (vmax - vmin) * i / 4
        y = y_to_px(pt, v, vmin, vmax)
        out.append(f'<line x1="{pl}" y1="{y}" x2="{pr}" y2="{y}" '
                   f'stroke="#eee" stroke-width="1"/>')
        out.append(f'<text x="{pl - 6}" y="{y + 4}" text-anchor="end" '
                   f'font-size="10" fill="#555">{fmt(v)}</text>')

    # x ticks
    for i, x in enumerate(xs):
        px = x_to_px(pl, i)
        out.append(f'<line x1="{px}" y1="{pb}" x2="{px}" y2="{pb + 4}" '
                   f'stroke="#333" stroke-width="1"/>')
        out.append(f'<text x="{px}" y="{pb + 18}" text-anchor="middle" '
                   f'font-size="10">{x}</text>')

    out.append(f'<text x="{pl - 45}" y="{pt + P_H/2}" '
               f'transform="rotate(-90 {pl - 45} {pt + P_H/2})" '
               f'text-anchor="middle" font-size="11">{ylabel}</text>')
    out.append(f'<text x="{pl + P_W/2}" y="{pb + 38}" text-anchor="middle" '
               f'font-size="11">r_end (GiB)</text>')

    # Greedy line (blue)
    pts = []
    for i, x in enumerate(xs):
        v = lookup(greedy, x, key)
        if v is None: continue
        pts.append((x_to_px(pl, i), y_to_px(pt, v, vmin, vmax), x, v))
    if len(pts) > 1:
        path = " ".join(f"{p[0]},{p[1]}" for p in pts)
        out.append(f'<polyline points="{path}" fill="none" '
                   f'stroke="#1f77b4" stroke-width="2"/>')
    for px, py, x, v in pts:
        out.append(f'<circle cx="{px}" cy="{py}" r="4" fill="#1f77b4" '
                   f'stroke="white" stroke-width="1"/>')

    # CB line (red)
    pts = []
    for i, x in enumerate(xs):
        v = lookup(cb, x, key)
        if v is None: continue
        pts.append((x_to_px(pl, i), y_to_px(pt, v, vmin, vmax), x, v))
    if len(pts) > 1:
        path = " ".join(f"{p[0]},{p[1]}" for p in pts)
        out.append(f'<polyline points="{path}" fill="none" '
                   f'stroke="#d62728" stroke-width="2" stroke-dasharray="5,3"/>')
    for px, py, x, v in pts:
        out.append(f'<circle cx="{px}" cy="{py}" r="4" fill="#d62728" '
                   f'stroke="white" stroke-width="1"/>')

    return out


# Compute axis ranges
all_bw = [r["bw"] for r in greedy] + [r["bw"] for r in cb]
all_p99 = [r["p99"] for r in greedy] + [r["p99"] for r in cb]
all_waf = [r["waf"] for r in greedy] + [r["waf"] for r in cb]

svg = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'font-family="DejaVu Sans, sans-serif">',
    f'<rect width="{W}" height="{H}" fill="white"/>',
    f'<text x="{W/2}" y="22" text-anchor="middle" font-size="16" '
    f'font-weight="bold">D17: Greedy vs Cost-Benefit GC '
    f'(4 GiB random-write, 4 KiB)</text>',
    # legend
    f'<g transform="translate({W/2 - 180},36)">'
    f'<line x1="0" y1="0" x2="20" y2="0" stroke="#1f77b4" stroke-width="2.5"/>'
    f'<text x="26" y="4" font-size="11">Greedy</text>'
    f'<line x1="100" y1="0" x2="120" y2="0" stroke="#d62728" '
    f'stroke-width="2.5" stroke-dasharray="5,3"/>'
    f'<text x="126" y="4" font-size="11">Cost-Benefit</text>'
    f'</g>',
]

svg += panel(0, "Throughput", "bw", "MiB/s",
             lambda v: f"{v:.1f}",
             min(all_bw) - 1, max(all_bw) + 1)
svg += panel(1, "p99 latency", "p99", "us",
             lambda v: f"{int(v)}",
             min(all_p99) - 50, max(all_p99) + 50)
svg += panel(2, "Write Amplification", "waf", "WAF",
             lambda v: f"{v:.2f}",
             0.95, max(all_waf) + 0.05)

svg.append("</svg>")

with open(OUT, "w") as f:
    f.write("\n".join(svg))
print("wrote", OUT, f"({os.path.getsize(OUT)} bytes)")
