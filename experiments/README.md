# Experiments — one command per paper figure

Each subdirectory reproduces one figure of the paper with the exact parameters
used for the published data. Every `run.sh` launches the underlying harness,
waits for completion, and runs all post-processing, so the plot-ready CSVs and
preview PNG/EPS files appear in the printed output path.

## Prerequisites

1. **Boot the FEMU VM** (host side): `femu/femu-scripts/run-hyhost-dual.sh`
   (256 GiB HYHOSTSSD device, 8x4 geometry). All commands below run inside the VM.
2. **Build** (inside the VM, once):
   ```bash
   cd rocksdb && sudo ./scripts/build_wal_variants.sh   # db_bench_zns / _posix / _l0cns
   cd ../hyhostd && make                                # hyhostd daemon
   cd ../dm-hyhost && ./scripts/build.sh                # dm-hyhost kernel module
   ```
   (Harnesses reload the freshly built `.ko` automatically at the start of each run.)
3. Run everything as root with the environment preserved: `sudo -E ./run.sh`.

## Figure map

| Paper figure | Command | Duration | Output |
|---|---|---|---|
| Fig. 7 (validation, multi-device) | `fig07_validation/run.sh` | ~30 min | `rocksdb/results/fig7/` |
| Fig. 8 (validation, single-device) | `fig08_validation/run.sh` | ~25 min | `rocksdb/results/fig8/` |
| Fig. 9a + 10a (GrowCNS under RWW) | `fig09_10_resize_rww/run.sh grow` | ~15 min | `hyhostd/results/resize_ops/<run>/` |
| Fig. 9b + 10b (ShrinkCNS under RWW) | `fig09_10_resize_rww/run.sh shrink` | ~15 min | `hyhostd/results/resize_ops/<run>/` |
| Fig. 11 (WAL placement, dual) | `fig11_wal/run.sh` | ~3.5 h | `rocksdb/results/fig10/` |
| Fig. 13a/b (L0 placement, FR/OW) | `fig13ab_l0_frow/run.sh` | ~40 min | `hyhostd/results/fig13ab/<ts>/` |
| Fig. 13c (L0 placement, RWW) | `fig13c_l0_rww/run.sh` | ~45 min | `hyhostd/results/fig13c/<ts>/` |

Notes
- Internal harness names predate the paper's figure numbering (e.g.
  `rocksdb/scripts/fig10_run.sh` produces paper Fig. 11); the wrappers hide
  this — see `docs/figure_number_map.md` for the full mapping.
- Fig. 1 (real ZN540 motivation), the Fig. 7/8 `zn540` columns, and the ZTree
  figures require hardware or code outside this repository and are not covered.
- The `L0_CNS_static` legs of Fig. 13 are *supposed* to die with an
  out-of-space error partway through — that truncated curve is the data point.
