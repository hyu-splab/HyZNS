#!/usr/bin/env python3
"""
FigB plot — WAL->ZNS vs WAL->R(CNS) fillrandom on HyZNS.

Parses db_bench logs (one per WAL variant) and draws:
  (left)  throughput over elapsed time  (interval ops/sec, from --stats_interval_seconds)
  (right) summary bars: avg ops/sec and MB/s (from the final "fillrandom :" line)

Usage: python3 scripts/legacy/plot_figB.py <zns.log> <posix.log> [more.log ...]
Label of each series is inferred from the filename token after "figB_..._" (zns/posix/cns).
"""
import re, sys, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

LABELS = {
    "zns":   ("WAL→ZNS (S-region)",      "#1f77b4"),
    "posix": ("WAL→R (CNS, F2FS O_SYNC)", "#d62728"),
    "cns":   ("WAL→R (raw CNS)",          "#2ca02c"),
}
SUMMARY_RE = re.compile(
    r"^fillrandom\s*:\s*([\d.]+)\s*micros/op\s*(\d+)\s*ops/sec(?:.*?;\s*([\d.]+)\s*MB/s)?")
# This fork's db_bench interval line (no parens, underscores):
#   thread_0: <iv_ops>,<tot_ops> ops_and <iv_rate>,<tot_rate> ops/second_in <iv_s>,<tot_s> seconds
INTERVAL_RE = re.compile(
    r"thread_\d+:\s*\d+,\d+\s*ops_and\s*([\d.]+),([\d.]+)\s*ops/second_in\s*([\d.]+),([\d.]+)\s*seconds")

def variant_of(path):
    for k in LABELS:
        if re.search(rf"(^|[_/]){k}([_.]|$)", os.path.basename(path)):
            return k
    return os.path.basename(path).split(".")[0]

def parse(path):
    micros = ops = mbps = None
    ts, rate = [], []
    with open(path, errors="replace") as f:
        for line in f:
            m = SUMMARY_RE.search(line)
            if m:
                micros = float(m.group(1)); ops = int(m.group(2))
                mbps = float(m.group(3)) if m.group(3) else None
            m = INTERVAL_RE.search(line)
            if m:
                rate.append(float(m.group(1)))   # interval ops/sec
                ts.append(float(m.group(4)))      # cumulative seconds
    return dict(micros=micros, ops=ops, mbps=mbps, ts=ts, rate=rate)

def main(paths):
    series = [(variant_of(p), parse(p), p) for p in paths]
    fig, (axT, axB) = plt.subplots(1, 2, figsize=(13, 5),
                                   gridspec_kw=dict(width_ratios=[2.0, 1.0]))

    # left: throughput over time
    for key, d, p in series:
        lab, col = LABELS.get(key, (key, None))
        if d["ts"]:
            axT.plot(d["ts"], [r/1000 for r in d["rate"]], label=lab, color=col, lw=1.3)
    axT.set_xlabel("elapsed time (s)"); axT.set_ylabel("throughput (Kops/s)")
    axT.set_title("fillrandom throughput over time"); axT.grid(alpha=.3); axT.legend()

    # right: summary bars (avg ops/sec as Kops/s, and MB/s)
    keys = [k for k, _, _ in series]
    labs = [LABELS.get(k, (k, None))[0] for k in keys]
    cols = [LABELS.get(k, (k, "#888"))[1] for k in keys]
    kops = [(d["ops"] or 0)/1000 for _, d, _ in series]
    x = range(len(keys))
    axB.bar(x, kops, color=cols)
    for i, (d, v) in enumerate(zip([s[1] for s in series], kops)):
        txt = f"{v:.0f}K"
        if d["mbps"]: txt += f"\n{d['mbps']:.0f} MB/s"
        axB.text(i, v, txt, ha="center", va="bottom", fontsize=9)
    axB.set_xticks(list(x)); axB.set_xticklabels(labs, rotation=12, ha="right", fontsize=8)
    axB.set_ylabel("avg throughput (Kops/s)")
    axB.set_title("fillrandom summary"); axB.grid(axis="y", alpha=.3)
    if kops: axB.set_ylim(0, max(kops)*1.25)

    fig.suptitle("FigB — WAL placement on HyZNS (static ABA): WAL→ZNS vs WAL→R", fontsize=12)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    base = re.sub(r"_(zns|posix|cns)\.log$", "", paths[0]) + "_plot"
    for ext in ("png", "svg"):
        fig.savefig(f"{base}.{ext}", dpi=130, bbox_inches="tight")
    print("wrote", base + ".png /", base + ".svg")
    # console summary
    print(f"{'variant':<28}{'ops/sec':>12}{'MB/s':>9}{'micros/op':>11}")
    for key, d, _ in series:
        print(f"{LABELS.get(key,(key,))[0]:<28}{d['ops'] or 0:>12}"
              f"{(d['mbps'] or 0):>9.1f}{(d['micros'] or 0):>11.2f}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: plot_figB.py <zns.log> <posix.log> ...")
    main(sys.argv[1:])
