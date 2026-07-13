#!/usr/bin/env python3
"""Plot D14 time-series runs to a single SVG.

Reads one or more (bw_log, dm_log, meta) tuples and overlays bandwidth
curves on a primary axis with the cumulative GC-migration count on a
secondary axis (only for runs that actually triggered GC). Uses fio's
log_avg_msec=1000 output and the 1 Hz dmsetup status snapshots emitted
by d14_timeseries.sh.

Usage:
    plot_d14_timeseries.py <tag>[,<tag>]... [out.svg]

Each <tag> matches the prefix of the files written by d14_timeseries.sh,
e.g. 20260505_155130_r2g_w8g.
"""
import csv, os, re, sys

RESULTS = "/data/HYSSD/dm-hyhost/results"

if len(sys.argv) < 2:
    print(__doc__, file=sys.stderr); sys.exit(2)
tags = sys.argv[1].split(",")
OUT = sys.argv[2] if len(sys.argv) > 2 else \
    os.path.join(RESULTS, "d14_timeseries_" + "_vs_".join(tags) + ".svg")


def load_run(tag):
    bw_path = os.path.join(RESULTS, f"{tag}_bw_bw.1.log")
    dm_path = os.path.join(RESULTS, f"{tag}_dm.log")
    meta_path = os.path.join(RESULTS, f"{tag}.meta")
    bw = []  # (t_sec, bw_KiB_s)
    with open(bw_path) as f:
        for line in f:
            parts = [p.strip() for p in line.strip().split(",")]
            if len(parts) < 2: continue
            t_ms = int(parts[0]); bw_kib = int(parts[1])
            bw.append((t_ms / 1000.0, bw_kib))
    dm = []  # (t_sec, gc_mig, w, free_lines)
    with open(dm_path) as f:
        for line in f:
            m = re.match(r"^(\d+)\s+(.*)$", line.strip())
            if not m: continue
            t = int(m.group(1)); status = m.group(2)
            def grab(k):
                mm = re.search(rf"\b{k}=(\d+)", status)
                return int(mm.group(1)) if mm else 0
            dm.append((t, grab("gc_mig"), grab("w"), grab("free_lines")))
    meta = {}
    with open(meta_path) as f:
        for line in f:
            if "=" in line:
                k, v = line.strip().split("=", 1); meta[k] = v
    return {"tag": tag, "bw": bw, "dm": dm, "meta": meta}


runs = [load_run(t) for t in tags]

# Layout
W, H = 1100, 480
L, R = 70, 70
T = 60; B = 70
PW = W - L - R; PH = H - T - B

# Time axis: max across all runs.
tmax = max(max((p[0] for p in r["bw"]), default=0) for r in runs) or 1
# bw axis (KiB/s -> MiB/s).
bw_max = max(max((p[1] for p in r["bw"]), default=0) for r in runs) / 1024.0
bw_max = max(bw_max, 1) * 1.1
# gc axis.
gc_max = max(max((d[1] for d in r["dm"]), default=0) for r in runs) or 1


def x(t):  return L + (t / tmax) * PW
def yL(v): return T + PH - (v / bw_max) * PH
def yR(v): return T + PH - (v / gc_max) * PH

colors = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd"]
gc_colors = ["#aec7e8", "#ff9896", "#98df8a", "#c5b0d5"]

svg = [
    f'<?xml version="1.0" encoding="UTF-8"?>',
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'font-family="DejaVu Sans, sans-serif">',
    f'<rect width="{W}" height="{H}" fill="white"/>',
    f'<text x="{W/2}" y="28" text-anchor="middle" font-size="17" '
    f'font-weight="bold">D14 time series - bandwidth + cumulative GC migrations</text>',
    f'<rect x="{L}" y="{T}" width="{PW}" height="{PH}" fill="white" '
    f'stroke="#333" stroke-width="1"/>',
]

# y-axis grid + left labels (MiB/s)
for i in range(6):
    v = bw_max * i / 5
    yy = yL(v)
    svg.append(f'<line x1="{L}" y1="{yy}" x2="{L+PW}" y2="{yy}" '
               f'stroke="#eee" stroke-width="1"/>')
    svg.append(f'<text x="{L-8}" y="{yy+4}" text-anchor="end" '
               f'font-size="11" fill="#1f77b4">{v:.1f}</text>')

# right labels (gc_mig)
for i in range(6):
    v = gc_max * i / 5
    yy = yR(v)
    svg.append(f'<text x="{L+PW+8}" y="{yy+4}" text-anchor="start" '
               f'font-size="11" fill="#d62728">{int(v):,}</text>')

# x-axis ticks every 30s
nticks = max(1, int(tmax // 30))
for i in range(nticks + 1):
    t = i * 30
    if t > tmax: break
    xx = x(t)
    svg.append(f'<line x1="{xx}" y1="{T+PH}" x2="{xx}" y2="{T+PH+4}" '
               f'stroke="#333" stroke-width="1"/>')
    svg.append(f'<line x1="{xx}" y1="{T}" x2="{xx}" y2="{T+PH}" '
               f'stroke="#f4f4f4" stroke-width="1"/>')
    svg.append(f'<text x="{xx}" y="{T+PH+18}" text-anchor="middle" '
               f'font-size="11">{t}</text>')

# axis labels
svg.append(f'<text x="{L+PW/2}" y="{T+PH+44}" text-anchor="middle" '
           f'font-size="12">elapsed (s)</text>')
svg.append(f'<text x="20" y="{T+PH/2}" '
           f'transform="rotate(-90 20 {T+PH/2})" '
           f'text-anchor="middle" font-size="12" fill="#1f77b4">'
           f'bandwidth (MiB/s)</text>')
svg.append(f'<text x="{W-20}" y="{T+PH/2}" '
           f'transform="rotate(90 {W-20} {T+PH/2})" '
           f'text-anchor="middle" font-size="12" fill="#d62728">'
           f'cumulative GC migrations (pblocks)</text>')

# plot each run
legend_y = T + 18
for idx, r in enumerate(runs):
    c = colors[idx % len(colors)]
    cgc = gc_colors[idx % len(gc_colors)]
    label_r = r["meta"].get("r_end_gib", "?")

    # bw polyline (left axis, MiB/s)
    pts = " ".join(f"{x(t)},{yL(b/1024.0)}" for (t, b) in r["bw"])
    svg.append(f'<polyline points="{pts}" fill="none" '
               f'stroke="{c}" stroke-width="1.8"/>')

    # gc cumulative line (right axis); only draw if there's any GC.
    has_gc = any(d[1] > 0 for d in r["dm"])
    if has_gc:
        pts = " ".join(f"{x(t)},{yR(g)}" for (t, g, _, _) in r["dm"])
        svg.append(f'<polyline points="{pts}" fill="none" '
                   f'stroke="{c}" stroke-width="1.4" stroke-dasharray="6,3" '
                   f'opacity="0.85"/>')

    # legend
    lx = L + 14 + idx * 250
    ly = legend_y
    svg.append(f'<line x1="{lx}" y1="{ly}" x2="{lx+24}" y2="{ly}" '
               f'stroke="{c}" stroke-width="2.2"/>')
    svg.append(f'<text x="{lx+30}" y="{ly+4}" font-size="12">'
               f'r_end={label_r} GiB - bw</text>')
    if has_gc:
        svg.append(f'<line x1="{lx+150}" y1="{ly}" x2="{lx+174}" y2="{ly}" '
                   f'stroke="{c}" stroke-width="2.2" stroke-dasharray="6,3"/>')
        svg.append(f'<text x="{lx+180}" y="{ly+4}" font-size="12">gc_mig</text>')

svg.append("</svg>")

with open(OUT, "w") as f:
    f.write("\n".join(svg))
print("wrote", OUT, "(%d bytes)" % os.path.getsize(OUT))
