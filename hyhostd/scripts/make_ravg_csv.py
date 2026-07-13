#!/usr/bin/env python3
# make_ravg_csv.py <out_dir> <run_dir> [<run_dir> ...]
#
# Post-process ftr_valid_run.sh run folders (results/ftr_valid/<CFG>_<ts>/) into
# the fig12/fig9b-format CSVs — same files make_fig10_dual_ravg.py produced:
#   ops_<name>.csv       : t_s, agg_ops_per_s                  (i1+i2 per-sec sum)
#   cum_<name>.csv       : t_s, cumulative_agg_ops
#   ravg_<name>.csv      : t_s, running_avg_agg_ops_per_s      (= cum/t)
#   ravg_mod_resizes.csv : t_s, ravg_at_t, R    (daemon run: first row = initial
#   cum_mod_resizes.csv  : t_s, cum_at_t,  R     R at t=0, then EVERY resize —
#                                                grow AND shrink — in one file)
#   cum_nomod_death.csv  : t_s, cum              (static death point)
#   resize_snap_rel.csv  : t_rel + the daemon's before/after fs/dm
#                          valid/invalid/free snapshot rows (from resize_snap.csv)
# If the run has an overwrite phase (FR->OW chain), ALSO emit overwrite-only
# series rebased to the OW phase start (each instance's ops counted only after
# ITS OWN ===PHASE_START overwrite; t=0 = earliest instance's OW start):
#   ops_<name>_ow.csv  cum_<name>_ow.csv  ravg_<name>_ow.csv
#   ravg_mod_resizes_ow.csv / cum_mod_resizes_ow.csv  (R timeline on the OW axis)
# name mapping: VANILLA -> VANILLA, L0_CNS_static -> L0_CNS_nomod,
#               L0_CNS_daemon -> L0_CNS_mod.
# No separate runner is needed: ===REPORT is always 1 s (db_bench reporter),
# independent of the harness monitor interval.
import os, sys, glob

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
NAME_OF = {"VANILLA": "VANILLA", "L0_CNS_static": "L0_CNS_nomod",
           "L0_CNS_daemon": "L0_CNS_mod"}

def mode_of(d):
    b = os.path.basename(os.path.normpath(d))
    for cfg, name in NAME_OF.items():
        if b.startswith(cfg + "_"): return cfg, name
    # fig13_frow layout: dir is a timestamp; the EXACT config is in the copied
    # per-run runinfo ("config=L0_CNS_static|L0_CNS_daemon|VANILLA"). The *_i1.log
    # filename can't distinguish static vs daemon (both named L0_CNS), so read
    # the runinfo first; fall back to the filename for the ZNS/CNS family.
    for rf in sorted(glob.glob(os.path.join(d, "*.runinfo"))):
        try:
            head = open(rf, errors="ignore").readline()
        except OSError:
            continue
        for cfg, name in NAME_OF.items():
            if "config=" + cfg in head:
                return cfg, name
    for f in sorted(glob.glob(os.path.join(d, "*_i1.log"))):
        fb = os.path.basename(f)
        if fb.startswith("L0_ZNS_"): return "VANILLA", "VANILLA"
        if fb.startswith("L0_CNS_"): return "L0_CNS_daemon", "L0_CNS_mod"
    return None, None

def prefix_of(d):
    l = glob.glob(os.path.join(d, "*_i1.log"))
    return l[0][:-len("_i1.log")] if l else None

def phase0(log, bench="fillrandom"):
    rep0 = None
    for ln in open(log, errors="ignore"):
        if f"===PHASE_START {bench}" in ln:
            try: return float(ln.replace(",", " ").split()[0])
            except: pass
        if rep0 is None and "===REPORT" in ln:
            try: rep0 = float(ln.replace(",", " ").split()[0]) - 1.0
            except: pass
    return rep0 if bench == "fillrandom" else None

def reports(log):
    d = {}
    for ln in open(log, errors="ignore"):
        if "===REPORT" in ln:
            f = ln.replace(",", " ").split()
            try: d[int(float(f[0]))] = float(f[2])
            except: pass
    return d

def cum(rows):
    c = 0.0; o = []
    for t, v in rows: c += v; o.append((t, c))
    return o

def wr(p, rows):
    with open(p, "w") as f:
        for r in rows:
            f.write(",".join(f"{x:.1f}" if isinstance(x, float) else str(x) for x in r) + "\n")

def emit_series(rows, name, suffix=""):
    """rows=(t_rel, ops/s) -> ops_/cum_/ravg_ files; returns cum table"""
    c = cum(rows)
    wr(os.path.join(OUT, f"ops_{name}{suffix}.csv"),  [(float(t), round(v)) for t, v in rows])
    wr(os.path.join(OUT, f"cum_{name}{suffix}.csv"),  [(float(t), round(v)) for t, v in c])
    wr(os.path.join(OUT, f"ravg_{name}{suffix}.csv"), [(float(t), round(v / t)) for t, v in c if t > 0])
    return c

def emit_resizes(events, c, name_suffix=""):
    """events=(t_rel,R) on the same axis as cum table c -> ravg/cum_mod_resizes files"""
    ravg = [(t, v / t) for t, v in c if t > 0]
    at = lambda t, tab: min(tab, key=lambda tv: abs(tv[0] - t))[1] if tab else 0
    wr(os.path.join(OUT, f"ravg_mod_resizes{name_suffix}.csv"),
       [(t, round(at(t, ravg)), R) for t, R in events])
    wr(os.path.join(OUT, f"cum_mod_resizes{name_suffix}.csv"),
       [(t, round(at(t, c)), R) for t, R in events])

for rundir in sys.argv[2:]:
    cfg, name = mode_of(rundir)
    if not cfg:
        print("skip (unknown mode):", rundir); continue
    pre = prefix_of(rundir)
    if not pre:
        print("skip (no i1 log):", rundir); continue
    l1, l2 = pre + "_i1.log", pre + "_i2.log"
    if not (os.path.exists(l1) and os.path.exists(l2)):
        print("skip (missing logs):", pre); continue
    t0s = [x for x in (phase0(l1), phase0(l2)) if x is not None]
    if not t0s: continue
    t0 = int(min(t0s))
    r1, r2 = reports(l1), reports(l2)

    # ---- full-run aggregate (FR + OW as before) --------------------------------
    rows = [(s - t0, r1.get(s, 0.0) + r2.get(s, 0.0))
            for s in sorted(set(r1) | set(r2)) if s - t0 > 0]
    if not rows: continue
    c = emit_series(rows, name)
    print(f"{name}: {len(c)} pts, end t={c[-1][0]:.0f}s cum={c[-1][1]:.0f} ravg={c[-1][1]/c[-1][0]:.0f}")

    if name == "L0_CNS_nomod":
        wr(os.path.join(OUT, "cum_nomod_death.csv"),
           [(float(c[-1][0]), round(c[-1][1]))])
        print(f"nomod death @ t={c[-1][0]:.0f}s")

    o1, o2 = phase0(l1, "overwrite"), phase0(l2, "overwrite")
    # RWW runs (fig13_rww_dur) have a readwhilewriting 2nd phase instead of
    # overwrite. Treat it symmetrically so _fr stays pure-FR and the RWW phase
    # gets its own _rww series (writer ops/sec) + R timeline.
    w1, w2 = phase0(l1, "readwhilewriting"), phase0(l2, "readwhilewriting")
    c_ow, ow_off, c_fr, c_rww, rww_off = None, None, None, None, None

    # ---- fillrandom-only: aggregate up to the FR phase end --------------------
    # Truncate at the EARLIEST OW start (min o1,o2): before that both instances
    # are still in fillrandom, so v = r1+r2 is the true dual FR throughput. We
    # must NOT keep emitting past it — the running avg (cum/t) would be dragged
    # down by trailing zeros once an instance leaves FR. No OW -> whole run.
    fr_end = min((x for x in (o1, o2, w1, w2) if x), default=None)
    fr_off = (fr_end - t0) if fr_end is not None else None
    rows_fr = []
    for s in sorted(set(r1) | set(r2)):
        rel = s - t0
        if rel <= 0: continue
        if fr_end is not None and s >= fr_end: break
        rows_fr.append((rel, r1.get(s, 0.0) + r2.get(s, 0.0)))
    if rows_fr:
        c_fr = emit_series(rows_fr, name, "_fr")
        print(f"  fillrandom-only: {len(c_fr)} pts, ravg={c_fr[-1][1]/c_fr[-1][0]:.0f}")

    # ---- overwrite-only: per-instance ops after its own OW start --------------
    if o1 or o2:
        t0o = int(min(x for x in (o1, o2) if x))
        ow_off = t0o - t0
        rows_ow = []
        for s in sorted(set(r1) | set(r2)):
            rel = s - t0o
            if rel <= 0: continue
            v = (r1.get(s, 0.0) if (o1 and s >= o1) else 0.0) + \
                (r2.get(s, 0.0) if (o2 and s >= o2) else 0.0)
            rows_ow.append((rel, v))
        if rows_ow:
            c_ow = emit_series(rows_ow, name, "_ow")
            print(f"  overwrite-only: starts @t={ow_off:.0f}s, {len(c_ow)} pts, "
                  f"ravg={c_ow[-1][1]/c_ow[-1][0]:.0f}")

    # ---- readwhilewriting-only: per-instance ops after its own RWW start ------
    if w1 or w2:
        t0w = int(min(x for x in (w1, w2) if x))
        rww_off = t0w - t0
        rows_rww = []
        for s in sorted(set(r1) | set(r2)):
            rel = s - t0w
            if rel <= 0: continue
            v = (r1.get(s, 0.0) if (w1 and s >= w1) else 0.0) + \
                (r2.get(s, 0.0) if (w2 and s >= w2) else 0.0)
            rows_rww.append((rel, v))
        if rows_rww:
            c_rww = emit_series(rows_rww, name, "_rww")
            print(f"  rww-only: starts @t={rww_off:.0f}s, {len(c_rww)} pts, "
                  f"ravg={c_rww[-1][1]/c_rww[-1][0]:.0f}")

    if name == "L0_CNS_mod":
        # R timeline from the harness mon csv (col1=t epoch, col2=R):
        # one merged file — row 0 is the initial R pinned to t=0, then every
        # resize event (grow AND shrink) as (t, ravg_at_t, R_after)
        # mon csv: fig10_dual_grow writes <pre>.csv; fig13_frow copies it as
        # <pre>.mon.csv. Try both.
        mon = pre + ".mon.csv" if os.path.exists(pre + ".mon.csv") else pre + ".csv"
        if os.path.exists(mon):
            rrows = []
            for ln in open(mon, errors="ignore"):
                f = ln.replace(",", " ").split()
                if len(f) < 2 or not f[0].isdigit(): continue
                rrows.append((float(f[0]) - t0, int(f[1])))
            events = []
            if rrows:
                pre0 = [R for t, R in rrows if t <= 0]
                events.append((0.0, pre0[-1] if pre0 else rrows[0][1]))
                prev = events[0][1]
                for t, R in rrows:
                    if t > 0 and R != prev:
                        events.append((round(t, 1), R)); prev = R
            emit_resizes(events, c)
            # FR-only axis: same t=0, but only grows during the FR phase
            if c_fr is not None and events:
                ev_fr = [(t, R) for t, R in events
                         if fr_off is None or t <= fr_off]
                if ev_fr:
                    emit_resizes(ev_fr, c_fr, "_fr")
            ups = sum(1 for i in range(1, len(events)) if events[i][1] > events[i-1][1])
            dns = len(events) - 1 - ups
            print(f"resizes: R {events[0][1]}->{events[-1][1]}"
                  f" (grows {ups}, shrinks {dns})" if events else "resizes: none")
            # same R timeline rebased onto the overwrite-only axis
            if c_ow is not None and events:
                pre_ow = [R for t, R in events if t <= ow_off]
                ev_ow = [(0.0, pre_ow[-1] if pre_ow else events[0][1])]
                for t, R in events:
                    if t > ow_off:
                        ev_ow.append((round(t - ow_off, 1), R))
                emit_resizes(ev_ow, c_ow, "_ow")
            # same R timeline rebased onto the readwhilewriting-only axis
            if c_rww is not None and events:
                pre_rww = [R for t, R in events if t <= rww_off]
                ev_rww = [(0.0, pre_rww[-1] if pre_rww else events[0][1])]
                for t, R in events:
                    if t > rww_off:
                        ev_rww.append((round(t - rww_off, 1), R))
                emit_resizes(ev_rww, c_rww, "_rww")
        # resize before/after fs/dm snapshot, re-based to the same t axis
        snap = pre + ".resize_snap.csv"
        if os.path.exists(snap):
            with open(os.path.join(OUT, "resize_snap_rel.csv"), "w") as out_f:
                for i, ln in enumerate(open(snap, errors="ignore")):
                    ln = ln.rstrip("\n")
                    if i == 0:
                        out_f.write("t_rel," + ln + "\n"); continue
                    try: trel = round(float(ln.split(",")[0]) - t0, 1)
                    except: continue
                    out_f.write(f"{trel}," + ln + "\n")
            print("resize_snap_rel.csv written")
