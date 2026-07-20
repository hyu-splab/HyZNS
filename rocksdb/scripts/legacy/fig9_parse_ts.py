#!/usr/bin/env python3
"""fig9_parse_ts.py - parse one unified-dual cell's 3-layer logs into epoch-T0-relative
gnuplot CSVs. Aligns db_bench (i1/i2.log, RocksDB wall-clock), dmstatus.log (epoch.ns),
and iostat.log (ISO8601) onto ONE relative-time axis (seconds since T0).

usage:  fig9_parse_ts.py <PFX> [dev]
        fig9_parse_ts.py --all <OUTDIR> [dev]     # every u_*_i1.log cell in OUTDIR
  PFX  = .../u_<CONFIG>_n<NUM>_j<JOBS>   (reads _i1.log _i2.log .dmstatus.log .iostat.log .t0)
  dev  = backing device basename for iostat rows (default nvme0n1)

emits next to PFX:
  .ts_thr_i1.csv  .ts_thr_i2.csv   t_rel_s, ops_per_s, phase (per-instance; phase=fillrandom|overwrite)
  .ts_thr_agg.csv                   t_rel_s, agg_ops_per_s    (i1+i2, 1s-binned sum)
  .ts_rzones.csv                    t_rel_s, r_zones          (dm R-region size; mod grows)
  .ts_dev.csv                       t_rel_s, dev_wkB_s        (device write bandwidth)
  .ts_gc.csv                        t_rel_s, gc_runs          (one row per GC event)
  .ts_phases.csv                    inst, phase, t_start_s, t_end_s   (workload-change points)

NOTE: db_bench's own ',NNN seconds' field RESETS at each benchmark phase (fillrandom->
overwrite); we use the RocksDB wall-clock ts -> epoch instead, so the axis is continuous.
"""
import sys, os, re, glob
from datetime import datetime

def db_epoch(ts):   # "2026/06/25-02:34:38.267104" (local tz) -> epoch
    return datetime.strptime(ts, "%Y/%m/%d-%H:%M:%S.%f").timestamp()

PHASE_NAMES = ["fillrandom", "overwrite"]          # order of BENCH=fillrandom,stats,overwrite,stats
def phase_name(i): return PHASE_NAMES[i] if i < len(PHASE_NAMES) else "phase%d" % i

def parse_throughput(path):   # -> [(epoch, ops_per_s, phase_idx)]
    # The per-second line's cumulative-seconds field RESETS to ~1.0 at each new benchmark
    # (fillrandom -> stats -> overwrite); we detect that drop to tag FR vs OW so the
    # workload-change point is recorded. Assumes a single foreground thread (thread_0),
    # which holds for our runs (no --threads; only --max_background_jobs).
    out = []
    tail = r'thread_\d+:.*?ops_and\s+([\d.]+),[\d.]+\s+ops/second_in\s+[\d.]+,([\d.]+)'
    rx_ep  = re.compile(r'^(\d{10,}\.\d+)\s+.*?' + tail)                       # native epoch prefix (patched db_bench)
    rx_rdb = re.compile(r'^(\d{4}/\d\d/\d\d-\d\d:\d\d:\d\d\.\d+)\s+' + tail)   # RocksDB local ts (old logs, converted)
    if not os.path.exists(path): return out
    phase, prev = 0, None
    for line in open(path, errors='ignore'):
        ep = ops = cum = None
        m = rx_ep.search(line)
        if m:
            try: ep, ops, cum = float(m.group(1)), float(m.group(2)), float(m.group(3))
            except ValueError: ep = None
        if ep is None:
            m = rx_rdb.search(line)
            if not m: continue
            try: ep, ops, cum = db_epoch(m.group(1)), float(m.group(2)), float(m.group(3))
            except ValueError: continue
        if prev is not None and cum < prev - 0.5:   # cumulative-s reset -> new phase
            phase += 1
        prev = cum
        out.append((ep, ops, phase))
    return out

def parse_dmstatus(path):     # -> ([(epoch, zones)], [(epoch, gc_runs)])
    rz, gc, prev = [], [], None
    if not os.path.exists(path): return rz, gc
    for line in open(path, errors='ignore'):
        line = line.strip()
        if not line or not line[0].isdigit(): continue
        try: ep = float(line.split()[0])
        except ValueError: continue
        mr = re.search(r'r_end=(\d+)', line); ml = re.search(r'zone_pblocks=(\d+)', line)
        if mr and ml:
            zsec = int(ml.group(1)) * 8           # pages/line * 8 sectors/page = sectors/zone
            if zsec: rz.append((ep, int(mr.group(1)) / zsec))
        mg = re.search(r'gc_runs=(\d+)', line)
        if mg:
            g = int(mg.group(1))
            if prev is not None and g > prev: gc.append((ep, g))
            prev = g
    return rz, gc

def parse_iostat(path, dev):  # -> [(epoch, wkB_s)]
    out, cur = [], None
    iso = re.compile(r'^(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+\-]\d{4})')
    us  = re.compile(r'^(\d\d/\d\d/\d{4}\s+\d\d?:\d\d:\d\d(?:\s+[AP]M)?)')
    if not os.path.exists(path): return out
    for line in open(path, errors='ignore'):
        s = line.strip()
        m = iso.match(s)
        if m:
            try: cur = datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S%z").timestamp()
            except ValueError: cur = None
            continue
        m = us.match(s)
        if m:
            tok = m.group(1)
            fmt = "%m/%d/%Y %I:%M:%S %p" if ('AM' in tok or 'PM' in tok) else "%m/%d/%Y %H:%M:%S"
            try: cur = datetime.strptime(' '.join(tok.split()), fmt).timestamp()
            except ValueError: cur = None
            continue
        if cur is not None and s.startswith(dev):
            f = s.split()
            if len(f) > 8:
                try: out.append((cur, float(f[8])))   # field 8 = wkB/s
                except ValueError: pass
    return out

def t0_of(pfx, *serieses):
    p = pfx + ".t0"
    if os.path.exists(p):
        try: return float(open(p).read().split()[0])
        except (ValueError, IndexError): pass
    cands = [s[0][0] for s in serieses if s]      # fallback: earliest epoch seen
    return min(cands) if cands else 0.0

def binned_sum(*serieses):
    from collections import defaultdict
    acc = defaultdict(float)
    for ser in serieses:
        b = {}
        for t, v, *_ in ser: b[round(t)] = v      # last sample wins within a 1s bin
        for k, v in b.items(): acc[k] += v
    return sorted(acc.items())

def phase_bounds(ser_rel):   # ser_rel = [(t_rel, ops, phase_idx)] -> [(phase_name, t_start, t_end)]
    from collections import defaultdict
    d = defaultdict(list)
    for t, _ops, ph in ser_rel: d[ph].append(t)
    return [(phase_name(ph), min(ts), max(ts)) for ph, ts in sorted(d.items())]

def write_csv(path, rows, header):
    with open(path, 'w') as f:
        f.write(header + "\n")
        for r in rows:
            f.write(",".join(("%.3f" % x) if isinstance(x, float) else str(x) for x in r) + "\n")

def parse_report(path):   # ===REPORT lines -> [(epoch, interval_qps)]  (stall-immune background thread)
    out = []
    rx = re.compile(r'^([\d.]+)\s+===REPORT\s+(-?\d+)')
    if not os.path.exists(path): return out
    for line in open(path, errors='ignore'):
        m = rx.match(line)
        if m:
            try: out.append((float(m.group(1)), float(m.group(2))))
            except ValueError: pass
    return out

def parse_phase_markers(path):   # ===PHASE_START lines -> [(epoch, name)]  (official db_bench markers)
    out = []
    rx = re.compile(r'^([\d.]+)\s+===PHASE_START\s+(\S+)')
    if not os.path.exists(path): return out
    for line in open(path, errors='ignore'):
        m = rx.match(line)
        if m:
            try: out.append((float(m.group(1)), m.group(2)))
            except ValueError: pass
    return out

def label_by_markers(t_epoch, markers):   # phase name whose marker is the latest <= t
    name = "?"
    for e, n in markers:
        if e <= t_epoch: name = n
        else: break
    return name

def parse_cell(pfx, dev):
    rz, gc = parse_dmstatus(pfx + ".dmstatus.log")
    dv = parse_iostat(pfx + ".iostat.log", dev)
    # PRIMARY throughput = stall-immune ===REPORT (background thread, no gaps during stalls).
    rep1, rep2 = parse_report(pfx + "_i1.log"), parse_report(pfx + "_i2.log")
    mk1, mk2 = parse_phase_markers(pfx + "_i1.log"), parse_phase_markers(pfx + "_i2.log")
    stall_immune = bool(rep1 or rep2)
    if stall_immune:                                   # new logs: (epoch, qps) + official markers
        i1raw, i2raw = rep1, rep2
        lab1 = lambda e: label_by_markers(e, mk1); lab2 = lambda e: label_by_markers(e, mk2)
    else:                                              # old logs: stats_interval + cum_s heuristic
        t1, t2 = parse_throughput(pfx + "_i1.log"), parse_throughput(pfx + "_i2.log")
        i1raw = [(e, o) for e, o, _ in t1]; i2raw = [(e, o) for e, o, _ in t2]
        p1map = {e: phase_name(p) for e, _, p in t1}; p2map = {e: phase_name(p) for e, _, p in t2}
        lab1 = lambda e: p1map.get(e, "?"); lab2 = lambda e: p2map.get(e, "?")
    t0 = t0_of(pfx, i1raw, i2raw, rz, dv)
    rel = lambda ser: [(round(t - t0, 3),) + tuple(r) for t, *r in ser]
    write_csv(pfx + ".ts_thr_i1.csv", [(round(e - t0, 3), v, lab1(e)) for e, v in i1raw], "t_rel_s,ops_per_s,phase")
    write_csv(pfx + ".ts_thr_i2.csv", [(round(e - t0, 3), v, lab2(e)) for e, v in i2raw], "t_rel_s,ops_per_s,phase")
    write_csv(pfx + ".ts_thr_agg.csv", [(round(t - t0, 3), v) for t, v in binned_sum(i1raw, i2raw)], "t_rel_s,agg_ops_per_s")
    write_csv(pfx + ".ts_rzones.csv", rel(rz), "t_rel_s,r_zones")
    write_csv(pfx + ".ts_dev.csv",    rel(dv), "t_rel_s,dev_wkB_s")
    write_csv(pfx + ".ts_gc.csv",     rel(gc), "t_rel_s,gc_runs")
    # workload-change points: prefer official ===PHASE_START markers, else cum_s heuristic
    ph_rows = []
    if mk1 or mk2:
        for inst, mk, raw in (("i1", mk1, i1raw), ("i2", mk2, i2raw)):
            last = max([e for e, _ in raw], default=t0)
            for j, (e, n) in enumerate(mk):
                end = mk[j + 1][0] if j + 1 < len(mk) else last
                ph_rows.append((inst, n, round(e - t0, 3), round(end - t0, 3)))
        src = "official ===PHASE_START"
    else:
        for inst, raw, labf in (("i1", i1raw, lab1), ("i2", i2raw, lab2)):
            ser = [(round(e - t0, 3), v, labf(e)) for e, v in raw]
            from collections import defaultdict
            d = defaultdict(list)
            for t, _v, nm in ser: d[nm].append(t)
            for nm in d: ph_rows.append((inst, nm, min(d[nm]), max(d[nm])))
        src = "cum_s heuristic"
    write_csv(pfx + ".ts_phases.csv", ph_rows, "inst,phase,t_start_s,t_end_s")
    ph_str = "; ".join("%s:%.1f-%.1fs" % (n, a, b) for i, n, a, b in ph_rows if i == "i1")
    print("parsed %s  thr=%s(i1=%d i2=%d) rzones=%d dev=%d gc=%d  t0=%.3f%s\n    phases[%s](i1): %s"
          % (os.path.basename(pfx), "REPORT(stall-immune)" if stall_immune else "stats_interval",
             len(i1raw), len(i2raw), len(rz), len(dv), len(gc), t0,
             "" if os.path.exists(pfx + ".t0") else " (t0 fallback)", src, ph_str or "none"))

def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(2)
    if a[0] == "--all":
        outdir = a[1]; dev = a[2] if len(a) > 2 else "nvme0n1"
        pfxs = sorted({f[:-len("_i1.log")] for f in glob.glob(os.path.join(outdir, "u_*_i1.log"))})
        if not pfxs: print("no u_*_i1.log cells in " + outdir); sys.exit(1)
        for p in pfxs: parse_cell(p, dev)
    else:
        dev = a[1] if len(a) > 1 else "nvme0n1"
        parse_cell(a[0], dev)

if __name__ == "__main__":
    main()
