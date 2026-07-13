#!/usr/bin/env python3
# plot_ops_timeline.py <ops_csv> <out_prefix> [--events events.csv]
#                      [--rdb rdb_events.csv] [--phases phases.csv]
#                      [--xmin 0] [--title T]
#
# Without --rdb: single panel — interval throughput (100ms -> Kops/s) with resize
# windows shaded and phase boundary lines.
# With --rdb (parse_rocksdb_events.py output): THREE stacked panels, shared x:
#   1. throughput + resize windows + stall/stop markers colored by CAUSE
#      (memtable / L0 / pending-compaction)
#   2. L0 file count (lsm_state) + slowdown/stop trigger lines — the causal
#      variable behind stall_l0 throttling
#   3. background-job LANES (gantt): every flush / intra-L0 / L0->L1 / deeper
#      compaction as its own bar, overlapping jobs stacked in sub-lanes —
#      replaces the old overlapping-shading (darkness == #concurrent jobs) that
#      made concurrent compactions invisible.
# Also prints + saves <out>_dipstats.txt: write dips (<50% of median) and their
# overlap with (merged) flush/compaction windows.
import sys, csv, os, argparse
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("ops_csv"); ap.add_argument("out_prefix")
ap.add_argument("--events", default=None)
ap.add_argument("--rdb", default=None)
ap.add_argument("--phases", default=None, help="phases.csv (ts_epoch,bench): FR/OW boundary lines")
ap.add_argument("--xmin", type=float, default=0.0)
ap.add_argument("--title", default="")
ap.add_argument("--dip-frac", type=float, default=0.5)
a = ap.parse_args()

ts, rd, wr = [], [], []
with open(a.ops_csv) as f:
    for r in csv.DictReader(f):
        try:
            ts.append(float(r["ts_epoch"]))
            rd.append(int(r["read_ops_per_100ms"]) * 10 / 1000)
            wr.append(int(r["write_ops_per_100ms"]) * 10 / 1000)
        except (KeyError, ValueError): pass
if not ts: sys.exit("empty ops csv")
t0 = ts[0]
xr = [(t - t0, r, w) for t, r, w in zip(ts, rd, wr) if (t - t0) >= a.xmin]
x = [v[0] for v in xr]; rd_s = [v[1] for v in xr]; wr_s = [v[2] for v in xr]

wins = []
if a.events and os.path.exists(a.events):
    with open(a.events) as f:
        for r in csv.DictReader(f):
            try:
                e = float(r["echo_t_epoch"]) - t0; d = float(r["done_t_epoch"]) - t0
                wins.append((e, d, r["R_pre"], r["R_post"],
                             r.get("dm_gcmig_MiB", "") or r.get("roll_mib", "")))
            except (KeyError, ValueError): pass

phases = []
if a.phases and os.path.exists(a.phases):
    with open(a.phases) as f:
        for r in csv.DictReader(f):
            try: phases.append((float(r["ts_epoch"]) - t0, r["bench"]))
            except (KeyError, ValueError): pass

fl_w, cp_w, stalls, l0pts = [], [], [], []   # flush/compaction windows, stalls, L0 count
if a.rdb and os.path.exists(a.rdb):
    open_j = {}
    with open(a.rdb) as f:
        for r in csv.DictReader(f):
            t = float(r["ts_epoch"]) - t0; kind = r["kind"]; job = r["job"]
            if kind in ("flush_start", "compact_start"):
                open_j[(kind[:2], job)] = t
            elif kind == "flush_end" and ("fl", job) in open_j:
                fl_w.append((open_j.pop(("fl", job)), t))
            elif kind == "compact_end" and ("co", job) in open_j:
                lvl = r["detail"].split()[0][5:] if r["detail"].startswith("out_L") else "?"
                cp_w.append((open_j.pop(("co", job)), t, lvl))
            elif kind.startswith(("stall_", "stop_")):
                stalls.append((t, kind))
            if r.get("l0") not in ("", None):
                try: l0pts.append((t, int(r["l0"])))
                except ValueError: pass

CAUSE_C = {"memtable": "#9467bd", "l0": "#8c564b", "pending": "#e377c2"}
three = bool(fl_w or cp_w or l0pts)
if three:
    fig, (ax, axl, axj) = plt.subplots(
        3, 1, figsize=(12, 9), sharex=True,
        gridspec_kw={"height_ratios": [3, 1.1, 1.6], "hspace": 0.08})
else:
    fig, ax = plt.subplots(figsize=(12, 5.5))
    axl = axj = None

# ---- panel 1: throughput -----------------------------------------------------
ax.plot(x, wr_s, color="#1f77b4", lw=1.0, label="write throughput")
ax.plot(x, rd_s, color="#2ca02c", lw=1.0, label="read throughput")
for e, d, rp, rpost, mig in wins:
    ax.axvspan(e, d, color="#ff7f0e", alpha=0.25)
    lab = f"R{rp}->{rpost}" + (f" {mig}MiB/{d-e:.1f}s" if mig not in ("", "0") else "")
    ax.annotate(lab, xy=(e, ax.get_ylim()[1]), fontsize=7,
                color="#d62728", rotation=90, va="top", ha="right")
ax.set_ylim(bottom=0)
ytop = ax.get_ylim()[1]
seen = set()
for t, kind in stalls:
    stop = kind.startswith("stop_"); cause = kind.split("_", 1)[1]
    lab = None
    if kind not in seen:
        seen.add(kind); lab = ("STOP " if stop else "stall ") + cause
    ax.plot(t, ytop * (0.97 if stop else 0.93), "X" if stop else "v",
            color=CAUSE_C.get(cause, "k"), ms=7 if stop else 5,
            mfc=CAUSE_C.get(cause, "k") if stop else "none", label=lab)
if wins: ax.axvspan(0, 0, color="#ff7f0e", alpha=0.25, label="resize window")
for t, name in phases:
    for axx in ([ax, axl, axj] if three else [ax]):
        if axx: axx.axvline(max(t, a.xmin), color="k", ls="--", lw=1.2, alpha=0.7)
    ax.annotate(name, xy=(max(t, a.xmin), ytop * 0.995), fontsize=9,
                fontweight="bold", ha="left", va="top")
ax.set_ylabel("throughput (Kops/s, per 100ms)")
ax.set_title(a.title or os.path.basename(a.ops_csv))
ax.grid(alpha=0.3); ax.legend(loc="upper right", fontsize=8, ncol=2)
ax.set_xlim(left=a.xmin)

# ---- panel 2: L0 file count ----------------------------------------------------
if three and l0pts:
    l0pts.sort()
    axl.step([t for t, _ in l0pts], [v for _, v in l0pts], where="post",
             color="#d62728", lw=1.3)
    top = max(v for _, v in l0pts)
    for trig, lab in ((20, "slowdown trigger (20)"), (36, "stop trigger (36)")):
        if trig <= top * 1.3 + 4:
            axl.axhline(trig, color="#d62728", ls=":", lw=1, alpha=0.6)
            axl.annotate(lab, xy=(a.xmin, trig), fontsize=7, color="#d62728",
                         va="bottom", ha="left")
    axl.set_ylabel("L0 files"); axl.set_ylim(bottom=0); axl.grid(alpha=0.3)

# ---- panel 3: background-job lanes (gantt) -------------------------------------
def pack(bars):
    """assign overlapping (s,e) bars to sub-lanes; returns [(s,e,lane)], nlanes"""
    lanes, out = [], []
    for s, e in sorted(bars):
        for i in range(len(lanes)):
            if s >= lanes[i]:
                lanes[i] = e; out.append((s, e, i)); break
        else:
            lanes.append(e); out.append((s, e, len(lanes) - 1))
    return out, max(1, len(lanes))

if three and axj:
    cats = [("flush",    [(s, e) for s, e in fl_w],                  "#2ca02c"),
            ("intra-L0", [(s, e) for s, e, l in cp_w if l == "0"],   "#ff9896"),
            ("L0->L1",   [(s, e) for s, e, l in cp_w if l == "1"],   "#d62728"),
            ("deeper",   [(s, e) for s, e, l in cp_w if l not in ("0", "1")], "#7f7f7f")]
    ybase, yticks, ylabels = 0.0, [], []
    for name, bars, color in cats:
        placed, n = pack(bars) if bars else ([], 1)
        h = 0.9 / n
        for s, e, lane in placed:
            axj.broken_barh([(s, max(e - s, 0.05))], (ybase + lane * h, h * 0.9),
                            facecolors=color, edgecolors="none")
        yticks.append(ybase + 0.45); ylabels.append(f"{name} ({len(bars)})")
        ybase += 1.0
    axj.set_yticks(yticks); axj.set_yticklabels(ylabels, fontsize=8)
    axj.set_ylim(0, ybase); axj.grid(alpha=0.3, axis="x")
    axj.set_xlabel("time since first report (s)")
    for e, d, *_ in wins: axj.axvspan(e, d, color="#ff7f0e", alpha=0.2)
elif not three:
    ax.set_xlabel("time since first report (s)")

fig.tight_layout()
png = a.out_prefix + "_ops.png"; fig.savefig(png, dpi=120)
print("saved", png)

# ---- companion figure: ONE read+write line (summed view) ------------------------
figt, axt = plt.subplots(figsize=(12, 5.5))
axt.plot(x, [ri + wi for ri, wi in zip(rd_s, wr_s)], color="#111111", lw=1.0,
         label="read+write")
for e, d, rp, rpost, mig in wins:
    axt.axvspan(e, d, color="#ff7f0e", alpha=0.25)
    lab = f"R{rp}->{rpost}" + (f" {mig}MiB/{d-e:.1f}s" if mig not in ("", "0") else "")
    axt.annotate(lab, xy=(e, axt.get_ylim()[1]), fontsize=7,
                 color="#d62728", rotation=90, va="top", ha="right")
if wins: axt.axvspan(0, 0, color="#ff7f0e", alpha=0.25, label="resize window")
for tt, name in phases:
    axt.axvline(max(tt, a.xmin), color="k", ls="--", lw=1.2, alpha=0.7)
    axt.annotate(name, xy=(max(tt, a.xmin), axt.get_ylim()[1] * 0.995),
                 fontsize=9, fontweight="bold", ha="left", va="top")
axt.set_xlabel("time since first report (s)")
axt.set_ylabel("throughput (Kops/s, per 100ms)")
axt.set_title(a.title or os.path.basename(a.ops_csv))
axt.grid(alpha=0.3); axt.set_ylim(bottom=0); axt.set_xlim(left=a.xmin)
axt.legend(loc="upper right", fontsize=8)
figt.tight_layout()
pngt = a.out_prefix + "_ops_total.png"; figt.savefig(pngt, dpi=120)
print("saved", pngt)

# ---- numeric evidence ----------------------------------------------------------
def mean(v): return sum(v) / len(v) if v else 0.0
def inany(t, spans): return any(e <= t <= d for e, d, *_ in spans)
lines = []
if wins:
    ri = [v for t, v in zip(x, rd_s) if inany(t, wins)]; ro = [v for t, v in zip(x, rd_s) if not inany(t, wins)]
    wi = [v for t, v in zip(x, wr_s) if inany(t, wins)]; wo = [v for t, v in zip(x, wr_s) if not inany(t, wins)]
    lines.append(f"read  Kops/s: baseline {mean(ro):.1f}  resize {mean(ri):.1f} ({mean(ri)/mean(ro)*100 if ro and mean(ro) else 0:.0f}%)")
    lines.append(f"write Kops/s: baseline {mean(wo):.1f}  resize {mean(wi):.1f} ({mean(wi)/mean(wo)*100 if wo and mean(wo) else 0:.0f}%)")

wpos = sorted(v for v in wr_s if v > 0)
if wpos:
    med = wpos[len(wpos) // 2]; thr = med * a.dip_frac
    dips, cur = [], None
    for t, v in zip(x, wr_s):
        if v < thr:
            cur = (cur[0], t) if cur else (t, t)
        elif cur:
            if cur[1] - cur[0] >= 0.5: dips.append(cur)
            cur = None
    if cur and cur[1] - cur[0] >= 0.5: dips.append(cur)
    def merged(spans):   # union of possibly-overlapping windows (bg jobs overlap a lot)
        out = []
        for e, d in sorted((e, d) for e, d, *_ in spans):
            if out and e <= out[-1][1]: out[-1][1] = max(out[-1][1], d)
            else: out.append([e, d])
        return out
    def overlap(spans):
        tot = 0.0
        for a0, b0 in dips:
            for e, d in merged(spans):
                lo, hi = max(a0, e), min(b0, d)
                if hi > lo: tot += hi - lo
        return tot
    diptime = sum(b - a0 for a0, b in dips)
    lines.append(f"write dips (<{a.dip_frac:.0%} of median {med:.1f}K): {len(dips)} intervals, {diptime:.1f}s total")
    if fl_w or cp_w:
        lines.append(f"  dip time inside flush windows      : {overlap(fl_w):.1f}s ({overlap(fl_w)/diptime*100 if diptime else 0:.0f}%)")
        lines.append(f"  dip time inside compaction windows : {overlap(cp_w):.1f}s ({overlap(cp_w)/diptime*100 if diptime else 0:.0f}%)")
        mfl, mcp = merged(fl_w), merged(cp_w)
        both = [d for d in dips if any(e <= (d[0]+d[1])/2 <= x1 for e, x1 in mfl) or
                                    any(e <= (d[0]+d[1])/2 <= x1 for e, x1 in mcp)]
        lines.append(f"  dips whose midpoint is inside a flush/compaction window: {len(both)}/{len(dips)}")
    if stalls:
        from collections import Counter
        c = Counter(k for _, k in stalls)
        lines.append("  stall/stop events: " + "  ".join(f"{k}={v}" for k, v in sorted(c.items())))

with open(a.out_prefix + "_dipstats.txt", "w") as f:
    f.write("\n".join(lines) + "\n")
for ln in lines: print("  " + ln)
