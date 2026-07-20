#!/usr/bin/env python3
# plot_ops_zoom.py <ops_csv> <out_prefix> [--events events.csv] [--pad 15|1x]
#                  [--window A:B] [--title T]
#
# Zoomed interval-throughput views of the 100ms ops csv
# (header: ts_epoch,total_ops_per_100ms,read_ops_per_100ms,write_ops_per_100ms):
#   --events + --pad : one figure PER resize event, x = [echo-pad, done+pad] s,
#                      resize window shaded  ->  <out>_zoom<k>_R<pre>to<post>.png
#   --window A:B     : one figure of the arbitrary range [A,B] s (rel. to first
#                      report; no resize needed)  ->  <out>_win_<A>-<B>.png
# Every figure is written TWICE: read/write as separate lines, and a *_total
# variant with one read+write line.
#
# --resize-phases: shade WHAT the resize window was spent on, using the paper's
# canonical 3-phase vocabulary (mapped from the pipeline's internal names):
#   Migration  = data movement (grow: ZenFS roll-off; shrink: F2FS drain + dm GC)
#   Reset      = ResetRzone / ZoneReset (incl. the freed zone's adopt on shrink)
#   UpdateABA  = sending the ABA update (freeze + checkpoint + device ABA move)
# Segments are SNAPPED to cover the whole [echo, done] window with no gaps
# (sub-100ms trigger/poll slivers are absorbed into the neighbouring phase).
import sys, csv, os, argparse
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("ops_csv"); ap.add_argument("out_prefix")
ap.add_argument("--events", default=None)
ap.add_argument("--phases", default=None, help="phases.csv (ts_epoch,bench): FR/OW boundary lines")
ap.add_argument("--resize-phases", default=None,
                help="resize_phases.csv (resize_phases.py): shade WHAT the window was spent on")
ap.add_argument("--pad", default="15",
                help="per-event zoom padding: seconds, or '<f>x' = f * event "
                     "duration (min 0.5s) — e.g. 1x pads a 0.4s resize by 0.5s")
ap.add_argument("--window", default=None, help="A:B seconds (relative)")
ap.add_argument("--title", default="")
a = ap.parse_args()

# canonical phase names/colors/layout shared with resize_matched_cdf.py
from phase_canon import CANON, PHASE_C, snap, paper_segments

t, rd, wr = [], [], []
with open(a.ops_csv) as f:
    for r in csv.DictReader(f):
        try:
            t.append(float(r["ts_epoch"]))
            rd.append(int(r["read_ops_per_100ms"]) * 10 / 1000)   # Kops/s
            wr.append(int(r["write_ops_per_100ms"]) * 10 / 1000)
        except (KeyError, ValueError): pass
if not t: sys.exit("empty ops csv")
t0 = t[0]; t = [x - t0 for x in t]

phases = []
if a.phases and os.path.exists(a.phases):
    with open(a.phases) as f:
        for r in csv.DictReader(f):
            try: phases.append((float(r["ts_epoch"]) - t0, r["bench"]))
            except (KeyError, ValueError): pass

rphases = {}          # event idx -> [(t0_rel, t1_rel, canonical phase, info)]
if a.resize_phases and os.path.exists(a.resize_phases):
    with open(a.resize_phases) as f:
        for r in csv.DictReader(f):
            try:
                name = CANON.get(r["phase"], r["phase"])
                rphases.setdefault(int(r["event"]), []).append(
                    (float(r["start_epoch"]) - t0, float(r["end_epoch"]) - t0,
                     name, r.get("info", "")))
            except (KeyError, ValueError): pass

# events that moved NOTHING (grow: roll_mib=0 -> the "migration" slice is just
# trigger-poll lead; shrink: f2/dm/gcmig all 0 -> the "dm force-GC + ABA" slice
# is pure ABA command time): painting those Migration is misleading — remap
# their sub-0.1s Migration slivers to UpdateABA.
no_dm_move = set()

events = []
if a.events and os.path.exists(a.events):
    with open(a.events) as f:
        for r in csv.DictReader(f):
            try:
                # moved-data label: grow = roll_mib (ZenFS zone drain);
                # shrink = f2 (F2FS LBA drain) + dm (force-GC) + gcmig
                if r.get("dir") == "shrink":
                    mig = " ".join(f"{k}={r[c]}MiB"
                                   for k, c in (("f2", "f2_mib"), ("dm", "dm_mib"),
                                                ("gcmig", "dm_gcmig_MiB"))
                                   if r.get(c, "") not in ("", "0"))
                else:
                    mig = (f"moved={r['roll_mib']}MiB"
                           if r.get("roll_mib", "") not in ("", "0") else "")
                events.append((float(r["echo_t_epoch"]) - t0, float(r["done_t_epoch"]) - t0,
                               r.get("dir", "resize"), r["R_pre"], r["R_post"], mig))
                moved0 = (float(r.get("dm_gcmig_MiB") or 0) == 0 and
                          float(r.get("dm_mib") or 0) == 0 and
                          float(r.get("f2_mib") or 0) == 0) \
                         if r.get("dir") == "shrink" else \
                         float(r.get("roll_mib") or 0) == 0
                if moved0:
                    no_dm_move.add(len(events))   # 1-based event index
            except (KeyError, ValueError): pass

for k in no_dm_move:                # remap AFTER both events+rphases are loaded
    if k in rphases:
        rphases[k] = [(p0, p1, "UpdateABA" if nm == "Migration" and p1 - p0 < 0.1
                       else nm, "" if nm == "Migration" else info)
                      for p0, p1, nm, info in rphases[k]]




def draw(x0, x1, png, head, spans=(), rph=None):
    xs = [(x, r, w) for x, r, w in zip(t, rd, wr) if x0 <= x <= x1]
    if not xs: print(f"skip {png}: no data in [{x0:.0f},{x1:.0f}]"); return
    x, r, w = zip(*xs)
    for series in ("split", "total"):
        fig, ax = plt.subplots(figsize=(9, 4.5))
        if series == "split":
            ax.plot(x, w, color="#1f77b4", lw=1.2, label="write")
            ax.plot(x, r, color="#2ca02c", lw=1.2, label="read")
        else:
            ax.plot(x, [ri + wi for ri, wi in zip(r, w)], color="#111111",
                    lw=1.4, label="read+write")
        if rph:   # canonical 3-phase fill; window bounds as thin lines
            for e, d in spans:
                ax.axvline(e, color="#8c564b", lw=1, alpha=0.6)
                ax.axvline(d, color="#8c564b", lw=1, alpha=0.6)
            seen = set()
            for p0, p1, name, info in rph:
                q0, q1 = max(p0, x0), min(p1, x1)
                if q1 <= q0: continue
                lab = None
                if name not in seen:
                    seen.add(name)
                    lab = name + (f" ({info})" if info else "")
                ax.axvspan(q0, q1, color=PHASE_C.get(name, "#8c564b"),
                           alpha=0.35, label=lab)
        elif spans:
            for i, (e, d) in enumerate(spans):
                ax.axvspan(max(e, x0), min(d, x1), color="#ff7f0e", alpha=0.25,
                           label="resize window" if i == 0 else None)
        top = max((max(w), max(r)) if series == "split" else
                  [ri + wi for ri, wi in zip(r, w)])
        for pt, name in phases:
            if x0 <= pt <= x1:
                ax.axvline(pt, color="k", ls="--", lw=1.2, alpha=0.7)
                ax.annotate(name, xy=(pt, top), fontsize=8,
                            fontweight="bold", ha="left", va="top")
        ax.set_xlabel("time since first report (s)")
        ax.set_ylabel("throughput (Kops/s, per 100ms)")
        ax.set_title((a.title + "\n" if a.title else "") + head)
        ax.grid(alpha=0.3); ax.set_ylim(bottom=0); ax.set_xlim(x0, x1)
        ax.legend(loc="upper right", fontsize=9)
        out = png if series == "split" else png[:-4] + "_total.png"
        fig.tight_layout(); fig.savefig(out, dpi=120); plt.close(fig)
        print("saved", out)


def pad_for(dur):
    p = str(a.pad)
    if p.endswith("x"):
        return max(0.5, float(p[:-1]) * dur)
    return float(p)


def dump_window_csv(path, x0, x1, origin):
    """The exact data a zoom figure plots, replottable as-is: t_rel_s is
    relative to `origin` (per-event zoom: the resize echo, so the resize
    window is [0, dur]; --window: the range start A)."""
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_rel_s", "t_run_s", "read_kops", "write_kops", "total_kops"])
        for xt, r_, w_ in zip(t, rd, wr):
            if x0 <= xt <= x1:
                w.writerow([f"{xt-origin:.3f}", f"{xt:.3f}",
                            f"{r_:.3f}", f"{w_:.3f}", f"{r_+w_:.3f}"])
    print("saved", path)


# the exact (snapped, canonical) segments each zoom shades — as data, so the
# figures can be re-plotted elsewhere from CSV alone
if events and rphases:
    with open(f"{a.out_prefix}_phases_canonical.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["event", "dir", "phase", "start_rel_s", "end_rel_s", "dur_s",
                    "start_ev_s", "end_ev_s", "info"])   # *_ev_s: 0 = event echo
        for k, (e, d, dr, *_rest) in enumerate(events, 1):
            if rphases.get(k):
                for p0, p1, nm, info in paper_segments(snap(rphases[k], e, d), e, d):
                    w.writerow([k, dr, nm, f"{p0:.3f}", f"{p1:.3f}",
                                f"{p1-p0:.3f}", f"{p0-e:.3f}", f"{p1-e:.3f}", info])
    print("saved", f"{a.out_prefix}_phases_canonical.csv")

for k, (e, d, dr, rp, rpost, mig) in enumerate(events, 1):
    head = f"{dr} #{k}  R{rp}->{rpost}  {d-e:.1f}s" + (f"  {mig}" if mig else "")
    rph = paper_segments(snap(rphases[k], e, d), e, d) if rphases.get(k) else None
    pad = pad_for(d - e)
    base = f"{a.out_prefix}_zoom{k}_R{rp}to{rpost}"
    dump_window_csv(f"{base}.csv", e - pad, d + pad, origin=e)
    draw(e - pad, d + pad, f"{base}.png", head, spans=[(e, d)], rph=rph)

if a.window:
    A, B = (float(x) for x in a.window.split(":"))
    spans, rph_all = [], []
    for k, (e, d, *_) in enumerate(events, 1):
        if e < B and d > A:
            spans.append((e, d))
            if rphases.get(k):
                rph_all += paper_segments(snap(rphases[k], e, d), e, d)
    dump_window_csv(f"{a.out_prefix}_win_{A:.0f}-{B:.0f}.csv", A, B, origin=A)
    draw(A, B, f"{a.out_prefix}_win_{A:.0f}-{B:.0f}.png",
         f"window [{A:.0f}, {B:.0f}] s", spans=spans, rph=rph_all or None)
