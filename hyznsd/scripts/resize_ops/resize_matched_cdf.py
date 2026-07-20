#!/usr/bin/env python3
# resize_matched_cdf.py <run_dir> <out_dir> [--pad 15]
#
# Matched-window CDF for the paper: for EACH resize event (window
# [echo,done], duration W, inside benchmark phase p) compare the write-op
# latency of the resize window against equal-duration baseline windows taken
# from the SAME phase with NO resize and NO write-stall:
#   base_near   : nearest stall-free window of length W ending before the event
#                 (fallback: nearest after) — one representative window
#   base_pooled : ALL disjoint stall-free windows of length W tiled over the
#                 phase (resize windows excluded) — pooled distribution
# stall-free = every 100ms bin in the window has write-ops >= 0.5 x the phase
# median AND no RocksDB stall/stop event falls inside it.
#
# Outputs (all CSVs with header rows):
#   cdf_points.csv    event,series,quantile_pct,latency_us   (plot-ready CDF)
#   event_summary.csv per-event metadata + P50/P99/P99.9/P100 per series
#   zoom_e<k>.csv     t_rel_s,write_kops,read_kops,in_resize_window (+-pad)
#   throughput.csv    copy of the full 100ms ops csv
#   cdf_e<k>.png      quick-look figure (resize vs base_near vs base_pooled)
import sys, os, csv, glob, io, tarfile, argparse, shutil
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

ap = argparse.ArgumentParser()
ap.add_argument("run_dir"); ap.add_argument("out_dir")
ap.add_argument("--pad", type=float, default=15.0)
ap.add_argument("--op", choices=["write", "read"], default="write",
                help="which op's latency to compare (RWW/paper fig10: read)")
ap.add_argument("--base-at", default="",
                help="comma list of MANUAL baseline-window starts (seconds "
                     "relative to first report), one per event in order; "
                     "empty entries fall back to the auto base_near search")
ap.add_argument("--zoom-win", default="",
                help="comma list of EXPLICIT zoom ranges 'A:B' (rel seconds), "
                     "one per event; empty entries use event+-pad")
ap.add_argument("--dump-raw", action="store_true",
                help="also dump raw per-window latencies raw_e<k>_{resize,base}.csv")
a = ap.parse_args()
R, O = a.run_dir, a.out_dir
BASE_AT = [float(x) if x.strip() else None for x in a.base_at.split(",")] \
          if a.base_at else []
def _parse_win(x):
    x = x.strip()
    if not x: return None
    parts = x.split(":")
    if len(parts) != 2:
        print(f"  WARN --zoom-win entry '{x}' is not 'A:B' (colon = range, "
              "comma separates events) — ignored")
        return None
    return (float(parts[0]), float(parts[1]))
ZOOM_WIN = [_parse_win(x) for x in a.zoom_win.split(",")] if a.zoom_win else []
os.makedirs(O, exist_ok=True)

# ---- load run artifacts -----------------------------------------------------
def rows_of(p): return list(csv.DictReader(open(p)))
events = rows_of(os.path.join(R, "events.csv"))
ops = rows_of(os.path.join(R, "ops.csv"))
phases = rows_of(os.path.join(R, "phases.csv"))
stalls = [float(r["ts_epoch"]) for r in rows_of(os.path.join(R, "rdb_events.csv"))
          if r["kind"].startswith(("stall_", "stop_"))] \
         if os.path.exists(os.path.join(R, "rdb_events.csv")) else []

bin_ts = np.array([float(r["ts_epoch"]) for r in ops])
bin_wr = np.array([int(r["write_ops_per_100ms"]) for r in ops])
bin_rd = np.array([int(r["read_ops_per_100ms"]) for r in ops])
act = bin_rd if a.op == "read" else bin_wr   # flatness/median series
t0 = bin_ts[0]

OPC = "0" if a.op == "read" else "1"
ts_l, lat_l = [], []
def _oplat_readers():
    tgz = os.path.join(R, "oplat.tar.gz")
    if os.path.exists(tgz):
        t = tarfile.open(tgz)
        for m in t.getmembers():
            if m.name.endswith(".csv") and "_fioprobe_" not in m.name:
                yield io.TextIOWrapper(t.extractfile(m))
    else:                                  # live/tmpfs run dir: plain oplat/
        for fn in sorted(glob.glob(os.path.join(R, "oplat", "*.csv"))):
            if "_fioprobe_" not in fn:
                yield open(fn)
for f in _oplat_readers():
    next(f, None)
    for ln in f:
        c = ln.split(",")
        if len(c) < 3 or c[2].strip() != OPC: continue   # selected op only
        ts_l.append(int(c[0])); lat_l.append(int(c[1]))
ots = np.array(ts_l, np.uint64); olat = np.array(lat_l, np.int64)
order = np.argsort(ots); ots, olat = ots[order], olat[order]

# phase intervals [start, end) in epoch seconds
ph = [(float(r["ts_epoch"]), r["bench"]) for r in phases]
ph_iv = [(s, (ph[i+1][0] if i+1 < len(ph) else bin_ts[-1] + 0.1), b)
         for i, (s, b) in enumerate(ph)]

rz_wins = [(float(r["echo_t_epoch"]), float(r["done_t_epoch"])) for r in events]

# per-event phase segments — canonical paper vocabulary Migration/Reset/
# UpdateABA, shared with plot_ops_zoom.py (phase_canon.py)
from phase_canon import CANON, PHASE_C, snap, paper_segments, \
    remap_zero_move, event_moved_zero
rphases = {}
_rp = os.path.join(R, "resize_phases.csv")
if os.path.exists(_rp):
    for r in rows_of(_rp):
        try:
            rphases.setdefault(int(r["event"]), []).append(
                (float(r["start_epoch"]), float(r["end_epoch"]),
                 CANON.get(r["phase"], r["phase"]), r.get("info", "")))
        except (KeyError, ValueError): pass
for _k, _ev in enumerate(events, 1):
    if _k in rphases and event_moved_zero(_ev):
        rphases[_k] = remap_zero_move(rphases[_k])

def phase_of(t):
    for s, e, b in ph_iv:
        if s <= t < e: return (s, e, b)
    return ph_iv[-1]

def bins_in(s, e):
    m = (bin_ts >= s) & (bin_ts < e)
    return act[m]

def stall_free(s, e, med):
    w = bins_in(s, e)
    if len(w) == 0 or w.min() < 0.5 * med: return False
    return not any(s <= st <= e for st in stalls)

def overlaps_resize(s, e):
    return any(s < d + 0.5 and e > x - 0.5 for x, d in rz_wins)

def lat_in(s, e):
    lo = np.searchsorted(ots, np.uint64(s * 1e6))
    hi = np.searchsorted(ots, np.uint64(e * 1e6))
    return olat[lo:hi]


def lat_ts_in(s, e):
    lo = np.searchsorted(ots, np.uint64(s * 1e6))
    hi = np.searchsorted(ots, np.uint64(e * 1e6))
    return ots[lo:hi], olat[lo:hi]

QG = np.unique(np.concatenate([np.linspace(0, 99, 199),
                               100 - np.logspace(-3, 0, 60)[::-1], [100.0]]))
TAILQ = [50, 99, 99.9, 100]

cdf_rows = [("event", "series", "quantile_pct", "latency_us")]
sum_rows = [("event", "dir", "R_pre", "R_post", "t_rel_s", "window_s", "moved_mib",
             "resize_stall_free", "base_near_t_rel_s", "n_resize", "n_base_near",
             "n_base_pooled", "n_pool_windows",
             *[f"resize_P{q}" for q in TAILQ], *[f"base_near_P{q}" for q in TAILQ],
             *[f"base_pooled_P{q}" for q in TAILQ])]

for k, ev in enumerate(events, 1):
    e0, e1 = float(ev["echo_t_epoch"]), float(ev["done_t_epoch"])
    W = e1 - e0
    ps, pe, bench = phase_of(e0)
    med = np.median(act[(bin_ts >= ps) & (bin_ts < pe) & (act > 0)])

    # base_near: manual override (--base-at, rel seconds) wins; else slide
    # back from the event in 0.1s steps; fallback forward
    near = None
    if k <= len(BASE_AT) and BASE_AT[k - 1] is not None:
        near = t0 + BASE_AT[k - 1]
        if overlaps_resize(near, near + W) or not stall_free(near, near + W, med):
            print(f"  WARN e{k}: manual base @{BASE_AT[k-1]}s overlaps a resize "
                  "or is not stall-free — using it anyway")
    s = min(e0 - W, pe - W)
    while near is None and s >= ps:
        if not overlaps_resize(s, s + W) and stall_free(s, s + W, med):
            near = s; break
        s -= 0.1
    if near is None:
        s = e1
        while s + W <= pe:
            if not overlaps_resize(s, s + W) and stall_free(s, s + W, med):
                near = s; break
            s += 0.1

    # base_pooled: disjoint tiling of the whole phase
    pool, s, npool = [], ps, 0
    while s + W <= pe:
        if not overlaps_resize(s, s + W) and stall_free(s, s + W, med):
            pool.append(lat_in(s, s + W)); npool += 1
        s += W
    pooled = np.concatenate(pool) if pool else np.array([], np.int64)

    series = [("resize", lat_in(e0, e1)),
              ("base_near", lat_in(near, near + W) if near is not None else np.array([], np.int64)),
              ("base_pooled", pooled)]
    for name, arr in series:
        if len(arr) < 10: continue
        for q, x in zip(QG, np.percentile(arr, QG)):
            cdf_rows.append((k, name, f"{q:.3f}", f"{x:.0f}"))

    def tails(arr):
        return [f"{np.percentile(arr, q):.0f}" if len(arr) >= 10 else "" for q in TAILQ]
    moved = ev.get("dm_gcmig_MiB") if ev["dir"] == "shrink" else ev.get("roll_mib")
    # contamination check for the RESIZE window: only "does a RocksDB stall/stop
    # event fall inside it" — the bin criterion would flag every shrink (the
    # drain itself dips throughput), which is the effect we measure, not noise.
    sum_rows.append((k, ev["dir"], ev["R_pre"], ev["R_post"], f"{e0 - t0:.1f}",
                     f"{W:.2f}", moved,
                     int(not any(e0 <= st <= e1 for st in stalls)),
                     f"{near - t0:.1f}" if near is not None else "",
                     len(series[0][1]), len(series[1][1]), len(pooled), npool,
                     *tails(series[0][1]), *tails(series[1][1]), *tails(pooled)))

    # raw latencies for both windows (replot-ready)
    if a.dump_raw:
        wins = [("resize", e0, e1)] + ([("base", near, near + W)] if near is not None else [])
        for nm, s_, e_ in wins:
            tts, lls = lat_ts_in(s_, e_)
            with open(os.path.join(O, f"raw_e{k}_{nm}.csv"), "w", newline="") as f:
                wcsv = csv.writer(f); wcsv.writerow(["ts_us", "lat_us"])
                wcsv.writerows(zip(tts.tolist(), lls.tolist()))

    # zoom csv/png: EXPLICIT --zoom-win range if given, else event +- pad
    # (auto mode extends to include the baseline window)
    explicit = k <= len(ZOOM_WIN) and ZOOM_WIN[k - 1] is not None
    if explicit:
        zlo, zhi = t0 + ZOOM_WIN[k - 1][0], t0 + ZOOM_WIN[k - 1][1]
        if near is not None and (near < zlo or near + W > zhi):
            print(f"  WARN e{k}: baseline window lies (partly) outside --zoom-win")
    else:
        zlo, zhi = e0 - a.pad, e1 + a.pad
        if near is not None:
            zlo = min(zlo, near - 2); zhi = max(zhi, near + W + 2)
    with open(os.path.join(O, f"zoom_e{k}.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_rel_s", "write_kops", "read_kops", "total_kops",
                    "in_resize_window", "in_base_window"])
        for i in range(len(bin_ts)):
            if zlo <= bin_ts[i] <= zhi:
                w.writerow([f"{bin_ts[i]-t0:.1f}", f"{bin_wr[i]*10/1000:.1f}",
                            f"{bin_rd[i]*10/1000:.1f}",
                            f"{(bin_wr[i]+bin_rd[i])*10/1000:.1f}",
                            int(e0 <= bin_ts[i] <= e1),
                            int(near is not None and near <= bin_ts[i] <= near + W)])

    zm = (bin_ts >= zlo) & (bin_ts <= zhi)
    rph = paper_segments(snap(list(rphases[k]), e0, e1), e0, e1) \
        if rphases.get(k) else None
    for view in ("split", "total"):
        fig, ax = plt.subplots(figsize=(10, 4.2))
        if near is not None:   # matched stall-free baseline window, same duration W
            ax.axvspan(near - t0, near + W - t0, color="#1f77b4", alpha=0.15,
                       label=f"baseline window (same {W:.1f}s)")
        if rph:                # canonical Migration/Reset/UpdateABA bands
            seen = set()
            for p0_, p1_, name, inf in rph:
                if p1_ - p0_ < 0.005: continue
                lab = None
                if name not in seen:
                    seen.add(name); lab = name + (f" ({inf})" if inf else "")
                ax.axvspan(p0_ - t0, p1_ - t0, color=PHASE_C.get(name, "#8c564b"),
                           alpha=0.35, label=lab)
        else:
            ax.axvspan(e0 - t0, e1 - t0, color="#ff7f0e", alpha=0.3, label="resize window")
        if view == "split":
            ax.plot(bin_ts[zm] - t0, bin_wr[zm] * 10 / 1000, color="#1f77b4", lw=1.2, label="write")
            ax.plot(bin_ts[zm] - t0, bin_rd[zm] * 10 / 1000, color="#2ca02c", lw=1.0, label="read")
        else:
            ax.plot(bin_ts[zm] - t0, (bin_wr[zm] + bin_rd[zm]) * 10 / 1000,
                    color="#111111", lw=1.4, label="read+write")
        zst = [st - t0 for st in stalls if zlo <= st <= zhi]
        if zst:
            arr = (bin_wr[zm] + (0 if view == "split" else bin_rd[zm])) * 10 / 1000
            yref = float(arr.max()) if zm.any() else 1.0
            ax.plot(zst, [yref * 1.04] * len(zst), "x", color="#d62728", ms=7,
                    ls="none", label=f"RocksDB stall/stop ({len(zst)})")
        ax.set_xlabel("time since first report (s)")
        ax.set_ylabel("throughput (Kops/s, per 100ms)")
        ax.set_ylim(bottom=0); ax.grid(alpha=0.3)
        ax.set_title(f"event {k}: {ev['dir']} R{ev['R_pre']}->{ev['R_post']} "
                     f"W={W:.2f}s moved={moved}MiB ({bench})", fontsize=10)
        ax.legend(loc="lower right", fontsize=8, ncol=2)
        sfx = "" if view == "split" else "_total"
        fig.tight_layout(); fig.savefig(os.path.join(O, f"zoom_e{k}{sfx}.png"), dpi=110)
        plt.close(fig)

    # quick-look figure: left = body (linear to max P99.9), right = full tail to P100
    fig, (axb, axt) = plt.subplots(1, 2, figsize=(11, 4.6))
    p999s = [np.percentile(arr, 99.9) for _, arr in series if len(arr) >= 10]
    for ax, xmax, ylo, rng in ((axb, max(p999s) if p999s else 1, 0.0, "body, to P99.9"),
                               (axt, None, 0.99, "tail zoom (y 0.99-1), to P100")):
        for (name, arr), c, ls in zip(series, ["#d62728", "#1f77b4", "#7f7f7f"],
                                      ["-", "--", ":"]):
            if len(arr) < 10: continue
            ax.plot(np.percentile(arr, QG), QG / 100.0, color=c, ls=ls, lw=2,
                    label=f"{name} (n={len(arr):,})")
        if xmax: ax.set_xlim(0, xmax * 1.05)
        ax.set_xlabel(f"{a.op} latency (us) — {rng}"); ax.set_ylabel("CDF")
        ax.set_ylim(ylo, 1.0005); ax.grid(alpha=0.3)
    axt.legend(loc="lower right", fontsize=8)
    fig.suptitle(f"event {k}: {ev['dir']} R{ev['R_pre']}->{ev['R_post']} "
                 f"W={W:.2f}s ({bench}) — resize vs matched stall-free windows",
                 fontsize=10)
    fig.tight_layout(); fig.savefig(os.path.join(O, f"cdf_e{k}.png"), dpi=110)
    plt.close(fig)

with open(os.path.join(O, "cdf_points.csv"), "w", newline="") as f:
    csv.writer(f).writerows(cdf_rows)
with open(os.path.join(O, "event_summary.csv"), "w", newline="") as f:
    csv.writer(f).writerows(sum_rows)
shutil.copy(os.path.join(R, "ops.csv"), os.path.join(O, "throughput.csv"))
print(f"{os.path.basename(R)} -> {O}: {len(events)} events")
for r in sum_rows[1:]:
    print("  e%s %s R%s->%s W=%ss moved=%sMiB resizeP100=%s baseP100(near/pool)=%s/%s"
          % (r[0], r[1], r[2], r[3], r[5], r[6], r[13+3], r[17+3], r[21+3]))
