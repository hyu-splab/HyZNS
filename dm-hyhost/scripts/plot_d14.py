#!/usr/bin/env python3
"""Plot D14 R-region size sweep results to SVG without matplotlib.

Reads dm-hyhost/results/<csv> (or first arg) and writes an SVG showing
bandwidth, p99 latency, and WAF as a function of r_end. Produces three
small panels in one image; log-2 x-axis for r_end.
"""
import csv, math, os, sys

CSV = sys.argv[1] if len(sys.argv) > 1 else \
    "/data/HYSSD/dm-hyhost/results/d14_first_cut_20260505_1518.csv"
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    CSV.rsplit(".", 1)[0] + ".svg"

with open(CSV) as f:
    rows = list(csv.DictReader(f))

xs = [int(r["r_end_gib"]) for r in rows]
bw = [float(r["bw_mib_s"]) for r in rows]
p99 = [int(r["p99_us"]) for r in rows]
waf = [float(r["waf"]) for r in rows]
gcm = [int(r["gc_mig"]) for r in rows]

# Layout: 3 panels horizontal, each panel 280x220, 30px gutter, 60px left margin.
P_W, P_H = 280, 220
GUTTER = 60
L_MARG, R_MARG = 60, 30
T_MARG, B_MARG = 40, 50
TITLE_H = 30
W = L_MARG + 3 * P_W + 2 * GUTTER + R_MARG
H = T_MARG + TITLE_H + P_H + B_MARG


def x_to_px(panel_left, x_idx):
    # Linear by index for a clean spaced look (data is geometric in r_end).
    if len(xs) == 1:
        return panel_left + P_W / 2
    return panel_left + 30 + (x_idx / (len(xs) - 1)) * (P_W - 60)


def y_to_px(panel_top, v, vmin, vmax):
    if vmax == vmin:
        return panel_top + P_H / 2
    return panel_top + P_H - ((v - vmin) / (vmax - vmin)) * (P_H - 30) - 15


def panel(panel_idx, title, ys, ylabel, fmt, ymin=None, ymax=None,
          highlight_gc=False):
    pl = L_MARG + panel_idx * (P_W + GUTTER)
    pt = T_MARG + TITLE_H
    pr, pb = pl + P_W, pt + P_H

    if ymin is None:
        ymin = min(ys)
    if ymax is None:
        ymax = max(ys)
    span = ymax - ymin
    pad = span * 0.15 if span > 0 else max(abs(ymax) * 0.15, 1)
    ymin -= pad
    ymax += pad

    out = []
    # frame
    out.append(f'<rect x="{pl}" y="{pt}" width="{P_W}" height="{P_H}" '
               f'fill="white" stroke="#333" stroke-width="1"/>')
    # title
    out.append(f'<text x="{pl + P_W/2}" y="{pt - 8}" text-anchor="middle" '
               f'font-size="14" font-weight="bold">{title}</text>')

    # y-axis ticks (4 lines)
    for i in range(5):
        v = ymin + (ymax - ymin) * i / 4
        y = y_to_px(pt, v, ymin, ymax)
        out.append(f'<line x1="{pl}" y1="{y}" x2="{pr}" y2="{y}" '
                   f'stroke="#eee" stroke-width="1"/>')
        out.append(f'<text x="{pl - 6}" y="{y + 4}" text-anchor="end" '
                   f'font-size="10" fill="#555">{fmt(v)}</text>')

    # x-axis ticks: r_end labels
    for i, x in enumerate(xs):
        px = x_to_px(pl, i)
        out.append(f'<line x1="{px}" y1="{pb}" x2="{px}" y2="{pb + 4}" '
                   f'stroke="#333" stroke-width="1"/>')
        out.append(f'<text x="{px}" y="{pb + 18}" text-anchor="middle" '
                   f'font-size="10">{x}</text>')

    # y-axis label
    out.append(f'<text x="{pl - 45}" y="{pt + P_H/2}" '
               f'transform="rotate(-90 {pl - 45} {pt + P_H/2})" '
               f'text-anchor="middle" font-size="11">{ylabel}</text>')

    # x-axis label
    out.append(f'<text x="{pl + P_W/2}" y="{pb + 38}" text-anchor="middle" '
               f'font-size="11">r_end (GiB)</text>')

    # plot line
    pts = []
    for i, v in enumerate(ys):
        px = x_to_px(pl, i)
        py = y_to_px(pt, v, ymin, ymax)
        pts.append(f"{px},{py}")
    out.append(f'<polyline points="{" ".join(pts)}" fill="none" '
               f'stroke="#1f77b4" stroke-width="2"/>')

    # markers + value labels
    for i, v in enumerate(ys):
        px = x_to_px(pl, i)
        py = y_to_px(pt, v, ymin, ymax)
        # red ring if GC active for this point
        gc_active = highlight_gc and gcm[i] > 0
        color = "#d62728" if gc_active else "#1f77b4"
        out.append(f'<circle cx="{px}" cy="{py}" r="4" fill="{color}" '
                   f'stroke="white" stroke-width="1"/>')
        out.append(f'<text x="{px}" y="{py - 9}" text-anchor="middle" '
                   f'font-size="9" fill="#333">{fmt(v)}</text>')
    return out


svg = [
    f'<?xml version="1.0" encoding="UTF-8"?>',
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'font-family="DejaVu Sans, sans-serif">',
    f'<rect width="{W}" height="{H}" fill="white"/>',
    f'<text x="{W/2}" y="22" text-anchor="middle" font-size="16" '
    f'font-weight="bold">D14: R-region size sweep '
    f'(4 GiB random-write, 4 KiB blocks)</text>',
]

svg += panel(0, "Throughput", bw, "MiB/s", lambda v: f"{v:.1f}",
             ymin=min(bw + [14]), ymax=max(bw + [18]), highlight_gc=True)
svg += panel(1, "p99 latency", p99, "us", lambda v: f"{int(v)}",
             ymin=min(p99 + [260]), ymax=max(p99 + [340]), highlight_gc=True)
svg += panel(2, "Write Amplification", waf, "WAF",
             lambda v: f"{v:.2f}", ymin=0.95, ymax=1.35, highlight_gc=True)

svg.append(
    f'<g transform="translate({W - 220},{H - 20})">'
    f'<circle cx="0" cy="-4" r="4" fill="#d62728"/>'
    f'<text x="10" y="0" font-size="11">GC active (workload &gt; r_end)</text>'
    f'</g>'
)

svg.append("</svg>")

with open(OUT, "w") as f:
    f.write("\n".join(svg))
print("wrote", OUT, "(%d bytes)" % os.path.getsize(OUT))
