#!/usr/bin/env python3
# resize_phases.py <db.log> <out_csv> [kernel.log]
#
# Extract per-resize phase segments (epoch s) into long-format CSV consumed by
# plot_ops_zoom.py / resize_matched_cdf.py --resize-phases:
#   event,phase,start_epoch,end_epoch,info
#
# GROW ([GrowLAT] begin/migrated/reset/devaba/done — each printed right after
# its step):
#   zenfs migration | zone reset | resize_cns (freeze+ckpt+ABA) | finalize
#
# SHRINK ([ShrinkLAT] begin/ioctl/done — NOTE: begin->ioctl IS the kernel
# F2FS_IOC_RESIZE_CNS call itself; ioctl->done is ZenFS adopting the freed
# zones). With kernel.log the ioctl is decomposed using the kernel's own
# ===[ResizeCNS SHRINK] duration summary (validated: pass1+freeze+devABA ==
# ShrinkLAT window to ~ms):
#   f2fs drain (online)   = [begin, begin+pass1]        LBA-space data migration
#   freeze (pass2+ckpt)   = [+, +user-BLOCK]            the only app-blocking part
#   dm force-GC + ABA     = [+, ioctl]                  host-FTL page relocation
#   zenfs zone adopt      = [ioctl, done]
# Kernel summaries are paired to shrink events by COUNT (extra leading
# summaries = the setup gate, which always precedes the workload); a secs=512
# signature filter is unreliable since real one-zone events print it too.
# Light shrinks print no "drain online (pass1)" line — fields default to 0.
import sys, re

log, out = sys.argv[1], sys.argv[2]
kern = sys.argv[3] if len(sys.argv) > 3 else None

blocks, cur = [], None
for ln in open(log, errors="ignore"):
    m = re.search(r"\[(Grow|Shrink)LAT\] (\w+).*?ts=(\d+)", ln)
    if not m: continue
    kind, ph, ts = m.group(1), m.group(2), int(m.group(3)) / 1e6
    if ph == "begin":
        mb = re.search(r"movebytes=(\d+)", ln)
        cur = {"kind": kind, "begin": ts,
               "mib": round(int(mb.group(1)) / 1048576) if mb else ""}
        blocks.append(cur)
    elif cur is not None:
        cur[ph] = ts

# kernel per-event shrink durations (ms), one record per ===[ResizeCNS SHRINK]
# ... ===[ResizeCNS end] block; missing lines (light shrinks) default to 0.
kshr = []
if kern:
    rec = None
    for ln in open(kern, errors="ignore"):
        if "ResizeCNS SHRINK" in ln:
            rec = {"pass1": 0, "freeze": 0, "devaba": 0}
            continue
        if rec is None: continue
        for key, pat in (("pass1", r"drain online \(pass1\)\s*: (\d+) ms"),
                         ("freeze", r"user-BLOCK \(freeze\)\s*: (\d+) ms"),
                         ("devaba", r"device ABA \(modify\)\s*: (\d+) ms")):
            m = re.search(pat, ln)
            if m: rec[key] = int(m.group(1))
        if "ResizeCNS end" in ln:
            kshr.append(rec); rec = None

# count-based gate skip: summaries beyond the event count are the setup
# gate(s), which always come FIRST (before the workload's shrinks)
n_shrink = sum(1 for b in blocks if b["kind"] != "Grow" and "ioctl" in b)
rows, ishr = [], max(0, len(kshr) - n_shrink)
for k, b in enumerate(blocks, 1):
    def seg(name, a, z, info=""):
        rows.append((k, name, f"{a:.6f}", f"{z:.6f}", info))
    if b["kind"] == "Grow":
        pairs = [("zenfs migration", "begin", "migrated", f"{b['mib']}MiB"),
                 ("zone reset", "migrated", "reset", ""),
                 ("resize_cns (freeze+ckpt+ABA)", "reset", "devaba", ""),
                 ("finalize", "devaba", "done", "")]
        for name, a, z, info in pairs:
            if a in b and z in b and b[z] > b[a]: seg(name, b[a], b[z], info)
    else:
        if "ioctl" not in b: continue
        t0, t1 = b["begin"], b["ioctl"]
        if ishr < len(kshr):
            d = kshr[ishr]; ishr += 1
            p1 = t0 + d["pass1"] / 1e3
            fz = p1 + d["freeze"] / 1e3
            if d["pass1"] > 0: seg("f2fs drain (online)", t0, min(p1, t1))
            seg("freeze (pass2+ckpt)", min(p1, t1), min(fz, t1))
            if t1 > fz: seg("dm force-GC + ABA", fz, t1)
        else:
            seg("resize_cns (kernel)", t0, t1)
        if "done" in b and b["done"] > t1:
            seg("zenfs zone adopt", t1, b["done"])

with open(out, "w") as f:
    f.write("event,phase,start_epoch,end_epoch,info\n")
    for r in rows: f.write(",".join(str(x) for x in r) + "\n")
print(f"wrote {out}: {len(blocks)} resize(s), {len(rows)} phase segments")
for k, b in enumerate(blocks, 1):
    tot = b.get("done", b["begin"]) - b["begin"]
    parts = [f"{n}={float(z)-float(a):.2f}s" for (e, n, a, z, i) in rows if e == k]
    print(f"  #{k} {b['kind']}: total {tot:.2f}s | " + " ".join(parts))
