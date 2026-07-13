# rocksdb/scripts/legacy — previous-generation harnesses

Early experiment harnesses, kept for reference; superseded by the
fig7/fig8/fig10/fig12 scripts one directory up. Internal paths were adjusted
so they still run in place (e.g. `sudo ./scripts/legacy/figB.sh`).

Prereqs: dm-hyhost module built (`../dm-hyhost/scripts/build.sh`),
`/dev/nvme0n1` = FEMU HYHOSTSSD (femu_mode=5), patched `mkfs.f2fs` installed.

| script | role |
|---|---|
| `build_wal_variants.sh` | build `db_bench_{zns,posix[,cns]}` by toggling the WAL placement in `fs_zenfs.cc`, then restore the source |
| `build_l0_variants.sh` | build the L0-placement db_bench variants |
| `figB.sh` | single-stack WAL placement (WAL→ZNS vs WAL→R/CNS), fillrandom + auto plot |
| `figB_dual.sh`, `figB_dual_fig1.sh` | dual-instance WAL placement variants |
| `plot_figB.py` | throughput-over-time + summary bars (png/svg) from figB logs |
| `figC.sh`, `figC_modify_live.sh` | L0 placement (L0→CNS vs L0→ZNS); live-resize variant |
| `fig9_*.sh`, `fig9_parse_ts.py` | CNS resize (grow/shrink) harnesses and log parser |
| `modify_*.sh` | ResizeCNS / modify-zone smoke and verification tests |
| `l0_readcold_dual.sh`, `l0_readdrop_diag.sh` | L0-on-CNS read diagnostics |
| `phaseB_4way.sh`, `phaseD_dual.sh` | 4-way and dual-instance phase sweeps |

## Usage

```bash
cd rocksdb
# 1) build the WAL variants (toggles fs_zenfs.cc, restores the source after)
sudo ./scripts/legacy/build_wal_variants.sh     # -> db_bench_zns, db_bench_posix

# 2) FigB: both variants on the identical dm stack (static ABA), auto plot
sudo ./scripts/legacy/figB.sh                   # default NUM=40M, R=4GiB
sudo NUM=20000000 R_ZONES=16 VAL=800 ./scripts/legacy/figB.sh

# 3) manual plot
python3.9 scripts/legacy/plot_figB.py results/<TAG>_zns.log results/<TAG>_posix.log
```

Outputs land in `rocksdb/results/<ts>_figB_*` (logs, `_dmstatus.txt`,
`_plot.{png,svg}`). The `w`/`ovr`/`discards` counters in `_dmstatus.txt` show
whether the WAL actually went through the R-region L2P (with WAL on ZNS, R
traffic is near zero).
