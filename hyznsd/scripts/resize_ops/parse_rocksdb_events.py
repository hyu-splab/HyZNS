#!/usr/bin/env python3
# parse_rocksdb_events.py <rocksdb_LOG> <out_csv>
#
# RocksDB LOG -> timestamped event CSV for the stall/compaction evidence graphs:
#   ts_epoch,kind,job,l0,detail
# l0 = L0 file count AFTER the event (from flush_finished/compaction_finished
# "lsm_state") — the causal variable behind stall_l0 write throttling.
# kinds:
#   flush_start/flush_end       (EVENT_LOG_v1 flush_started/flush_finished, epoch us)
#   compact_start/compact_end   (compaction_started/compaction_finished)
#   stall_memtable stall_l0 stall_pending   ("Stalling writes because ...")
#   stop_memtable  stop_l0  stop_pending    ("Stopping writes because ..." = full stop)
# Stall/stop lines carry only the textual LOG timestamp (localtime) -> converted
# with the local timezone; EVENT_LOG lines use their embedded time_micros.
# stdout: per-kind counts + total flush/compaction window seconds (paired by job).
import sys, re, json, time, csv

log_path, out_csv = sys.argv[1], sys.argv[2]

TS_RE = re.compile(r"^(\d{4}/\d{2}/\d{2}-\d{2}:\d{2}:\d{2})\.(\d{6})")
STALLS = [  # (needle, kind)
    ("Stopping writes because we have", None),   # refined below by cause
    ("Stalling writes because", None),
]
def stall_kind(line):
    stop = "Stopping writes" in line
    if "immutable memtables" in line: c = "memtable"
    elif "level-0 files" in line:     c = "l0"
    elif "pending compaction" in line: c = "pending"
    else: return None
    return ("stop_" if stop else "stall_") + c

EV_KIND = {"flush_started": "flush_start", "flush_finished": "flush_end",
           "compaction_started": "compact_start", "compaction_finished": "compact_end"}

rows = []
for line in open(log_path, errors="ignore"):
    if "EVENT_LOG_v1" in line:
        try:
            j = json.loads(line.split("EVENT_LOG_v1", 1)[1].strip())
        except (json.JSONDecodeError, IndexError):
            continue
        kind = EV_KIND.get(j.get("event", ""))
        if not kind: continue
        ts = j.get("time_micros", 0) / 1e6
        job = j.get("job", "")
        lsm = j.get("lsm_state")
        l0 = lsm[0] if isinstance(lsm, list) and lsm else ""
        if kind == "flush_start":
            det = f"memtables={j.get('num_memtables','')}"
        elif kind == "compact_start":
            det = j.get("compaction_reason", "")
        elif kind == "compact_end":
            det = f"out_L{j.get('output_level','')} {round(j.get('total_output_size',0)/1048576)}MiB"
        else:
            det = ""
        rows.append((ts, kind, job, l0, det))
    elif "Stalling writes because" in line or "Stopping writes because" in line:
        kind = stall_kind(line)
        m = TS_RE.match(line)
        if not (kind and m): continue
        ts = time.mktime(time.strptime(m.group(1), "%Y/%m/%d-%H:%M:%S")) + int(m.group(2)) / 1e6
        rows.append((ts, kind, "", "", line.strip().split("] ")[-1][:120]))

rows.sort(key=lambda r: r[0])
with open(out_csv, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["ts_epoch", "kind", "job", "l0", "detail"])
    for r in rows: w.writerow([f"{r[0]:.6f}", r[1], r[2], r[3], r[4]])

# summary: counts + paired window durations
from collections import Counter, defaultdict
cnt = Counter(r[1] for r in rows)
def windows(start_k, end_k):
    open_j, spans = {}, []
    for ts, kind, job, _, _ in rows:
        if kind == start_k: open_j[job] = ts
        elif kind == end_k and job in open_j: spans.append((open_j.pop(job), ts))
    return spans
fl = windows("flush_start", "flush_end"); cp = windows("compact_start", "compact_end")
print(f"wrote {out_csv}: {len(rows)} events")
print("  " + "  ".join(f"{k}={v}" for k, v in sorted(cnt.items())))
if fl: print(f"  flush windows: {len(fl)}, total {sum(b-a for a,b in fl):.1f}s")
if cp: print(f"  compaction windows: {len(cp)}, total {sum(b-a for a,b in cp):.1f}s")
