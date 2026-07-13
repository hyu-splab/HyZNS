# rocksdb/scripts — HyZNS benchmark harness

db_bench harnesses for the paper's RocksDB experiments: device validation
(Fig7/Fig8, target ratio HyZNS ≈ ZNS/ZN540 ~1.0) and dual-instance WAL
placement (Fig10/Fig11/Fig12).

Previous-generation harnesses (figB/figC/fig9/modify/L0) live in
[`legacy/`](legacy/README.md) and can still be run in place
(`sudo ./scripts/legacy/figB.sh`).

| file | role |
|---|---|
| `fig7_run.sh` | Fig7 multi-device: zns/hyzns auto-detect, measure, append CSV |
| `fig8_run.sh` | Fig8 single-device: HyZNS with aux on the CNS (R-region) |
| `fig10_run.sh` | Fig10 dual-instance WAL placement (WAL_ZNS vs WAL_CNS) |
| `fig10_plot.py` | Fig10 bar chart (FR/OW × threads × WAL placement) |
| `fig10_post.py` | Fig10 evidence pack: stalls, flush lat, zone resets, timeline |
| `fig10_mon.sh` | standalone 1s device/CPU sampler (mon.csv) |
| `fig11_evidence.py` | compaction/flush evidence tables from RocksDB LOGs |
| `fig12_run.sh` | Fig11(b)(c): readwhilewriting sweep + per-op read-latency trace |
| `fig12_post.py` | Fig12 post: RWW throughput bars + exact read-latency CDF |
| `fig12_rww_dur.sh` | time-bounded RWW wrapper around fig12_run.sh |
| `fig_plot.py` | shared normalized bar chart (2–3 series, png+eps) |
| `_fig_lib.sh` | shared helpers for fig7/8/10 (ABA probe, run_bench, CSV parse) |
| `build_wal_variants.sh` | builds db_bench_{zns,posix,l0cns} from fs_zenfs.cc toggles |
| `legacy/` | previous-generation harnesses |

## Build (once)

```bash
cd rocksdb
DEBUG_LEVEL=0 ROCKSDB_PLUGINS=zenfs make -j14 db_bench install
cd plugin/zenfs/util && make                                    # -> zenfs util
cd ../../../../dm-hyhost && KDIR=/data/HYSSD.bak/linux-5.15.0 ./scripts/build.sh
```

Local db_bench patch: the `readwhilewriting` writer thread is excluded from
the merged report (`SetExcludeFromMerge`), so at exit it prints an extra
`rww-writes : ... ops/sec` line in the same format (`tools/db_bench_tool.cc`,
end of `BGWriter`).
`zbd_zenfs.cc`: if the ABA report covers the whole device (pure ZNS),
reserved is treated as 0.

## Workload / thread policy (fig7/fig8)

Each workload measures on its OWN fresh DB (no cross-workload reuse, so any
subset such as `WORKLOADS="RR"` is self-contained). Prep fills run with 1
thread and are not recorded.

| workload | prep (not logged) | benchmark | threads |
|---|---|---|---|
| FS | — | fillseq | 1 |
| FR | — | fillrandom | 1 |
| OW | — | overwrite | 1 |
| RS | fillseq | readseq | 1 |
| RR | fillseq | readrandom | 16 (`--reads=READS` per thread, default totals NUM) |
| RWW | fillrandom | readwhilewriting | 64 readers + 1 writer; CSV rows `RWW-R`/`RWW-W`/`RWW-T` |

Defaults: `NUM=20M KEY=20 VAL=800 COMP=snappy` (~400 B per value on flash),
`AOZ=14` (single-instance active-zone budget), `JOBS=8` background threads.
hyzns mode validates the dm geometry contract (`line_pblocks`) at setup and
aborts on mismatch (`SKIP_D19_CHECK=1` to bypass). The mode is auto-detected
via the vendor ABA report — only HYHOSTSSD (femu_mode=5) answers it, which
distinguishes it from pure ZNS (both report `queue/zoned=host-managed`).

## Fig7 — multi-device (ZNS-area throughput, aux on a separate CNS)

The aux path lives on a separate device (F2FS on FEMU BBSSD `/dev/nvme1n1`),
so only the ZenFS/ZNS throughput is measured. Swap FEMU builds between runs;
results accumulate. Modes: `zns` (whole zoned nvme) / `hyzns` (dm-hyhost
S-region, R=4 GiB idle) / `real` (2-namespace SSD, logged as zn540).

```bash
sudo GEOM=8x4 ./scripts/fig7_run.sh
sudo GEOM=8x4 NUM=10000000 ./scripts/fig7_run.sh   # quicker
sudo WORKLOADS="RR RWW" ./scripts/fig7_run.sh      # subset (self-contained)
sudo DRYRUN=1 ./scripts/fig7_run.sh                # print commands only
```

- cumulative CSV: `results/fig7/fig7_log.csv`
  (`utc,run_tag,mode,geom,workload,threads,num,micros_op,ops_sec,mb_s,log`)
- prints the zns-vs-hyzns comparison table for the current geometry at exit

## Fig8 — single-device (HyZNS only, aux on CNS)

The aux F2FS lives on the CNS of the SAME device (dm-hyhost R-region); ZenFS
uses the S-region. Pure ZNS has no CNS, so this compares HyZNS directly
against a real ZN540.

```bash
sudo GEOM=8x4 ./scripts/fig8_run.sh                # R_ZONES=4 (4 GiB CNS)
sudo GEOM=8x4 NUM=10000000 ./scripts/fig8_run.sh
sudo MODE=real ./scripts/fig8_run.sh               # real 2-namespace SSD
```

- cumulative CSV: `results/fig8/fig8_log.csv` (mode=hyzns / zn540)

## Fig10 — WAL placement (dual instance, HyZNS)

Two db_bench instances share one HyZNS device (S-region split in half, one
ZenFS each; shared aux F2FS on the CNS R-region). Only the WAL path differs:
`WAL_ZNS` (db_bench_zns) vs `WAL_CNS` (db_bench_posix, WAL on the R-region
F2FS with O_SYNC). Sweeps background threads {2,4,8} over fillrandom then
overwrite; reported ops are the mean of the two instances.

```bash
# 0) build the WAL variants (once)
sudo ./scripts/build_wal_variants.sh          # -> db_bench_{zns,posix,l0cns}
# 1) sweep (paper: NUM=20M total, jobs 2 4 8)
sudo GEOM=8x4 ./scripts/fig10_run.sh
sudo JOBS="4" NUM=10000000 ./scripts/fig10_run.sh    # single cell
# 2) plot
python3 scripts/fig10_plot.py results/fig10/fig10_log.csv -o results/fig10/fig10.png
```

- cumulative CSV: `results/fig10/fig10_log.csv`
- prints the per-jobs FR/OW WAL_ZNS vs WAL_CNS ratio table at exit

## Fig12 — readwhilewriting under WAL placement

Same dual topology as Fig10; the measured phase is readwhilewriting on a
fillrandom-populated DB, with a per-op read-latency trace for the CDF.

```bash
sudo NUM=60000000 GEOM=8x4 ./scripts/fig12_run.sh
python3 scripts/fig12_post.py results/fig12/<run>
```

## Plotting (fig_plot.py — shared)

Renders 2–3 series according to the mode columns (zn540/zns/hyzns) of a
values CSV; empty columns are dropped from the legend automatically.

```bash
# Fig7 (3 series)
python3 scripts/fig_plot.py --template --log results/fig7/fig7_log.csv \
    results/fig7/fig7_values.csv                   # fills zns/hyzns
vi results/fig7/fig7_values.csv                    # fill the zn540 column
python3 scripts/fig_plot.py results/fig7/fig7_values.csv -o results/fig7/fig7.png
python3 scripts/fig_plot.py results/fig7/fig7_values.csv --base zns   # no ZN540 yet

# Fig8 (2 series: zn540 vs hyzns)
python3 scripts/fig_plot.py --template --log results/fig8/fig8_log.csv \
    --modes zn540,hyzns results/fig8/fig8_values.csv
python3 scripts/fig_plot.py results/fig8/fig8_values.csv -o results/fig8/fig8.png
```

Output: png (300 dpi) + eps. Normalized to base=1 per workload; Okabe-Ito
colors + hatches for grayscale printing.
