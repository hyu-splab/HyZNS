# Experiments — one command per paper figure

Each subdirectory reproduces one figure of the paper with the exact parameters
used for the published data. Every `run.sh` launches the underlying harness,
waits for completion, and runs all post-processing, so the plot-ready CSVs and
preview PNG/EPS files appear in the printed output path.

## Prerequisites

1. **Boot the FEMU VM** (host side): `femu/femu-scripts/run-hyzns-dual.sh`
   (256 GiB HyZNS device, 8x4 geometry). All commands below run inside the VM.
2. **Build** (inside the VM, once):
   ```bash
   cd rocksdb && sudo ./scripts/build_wal_variants.sh   # db_bench_zns / _posix / _l0cns
   cd ../hyznsd && make                                # hyznsd daemon
   cd ../dm-hyzns && ./scripts/build.sh                # dm-hyzns kernel module
   ```
   (Harnesses reload the freshly built `.ko` automatically at the start of each run.)
3. Run everything as root with the environment preserved: `sudo -E ./run.sh`.

## Figure map

| Paper figure | Command | Duration | Output |
|---|---|---|---|
| Fig. 7 (validation, multi-device) | `fig07_validation/run.sh` | ~30 min | `rocksdb/results/fig7/` |
| Fig. 8 (validation, single-device) | `fig08_validation/run.sh` | ~25 min | `rocksdb/results/fig8/` |
| Fig. 9a + 10a (GrowCNS under RWW) | `fig09_10_resize_rww/run.sh grow` | ~15 min | `hyznsd/results/resize_ops/<run>/` |
| Fig. 9b + 10b (ShrinkCNS under RWW) | `fig09_10_resize_rww/run.sh shrink` | ~15 min | `hyznsd/results/resize_ops/<run>/` |
| Fig. 11 (WAL placement, dual) | `fig11_wal/run.sh` | ~3.5 h | `rocksdb/results/fig10/` |
| Fig. 12a/b (L0 placement, FR/OW) | `fig12ab_l0_frow/run.sh` | ~40 min | `hyznsd/results/fig13ab/<ts>/` |
| Fig. 12c (L0 placement, RWW) | `fig12c_l0_rww/run.sh` | ~45 min | `hyznsd/results/fig13c/<ts>/` |
| Fig. 13 (ZTree vs Hy-ZTree) | `fig13_ztree_hy-ztree/run.sh` | — | see `Ztree/README.md` |
| extra: WAL under RWW (unused) | `extra_wal_rww/run.sh` | ~3 h | `rocksdb/results/fig12/<run>/analysis/` |

Notes
- Internal harness names predate the paper's figure numbering (e.g.
  `rocksdb/scripts/fig10_run.sh` produces paper Fig. 11); the wrappers hide
  this — see `docs/figure_number_map.md` for the full mapping.
- Fig. 1 (real ZN540 motivation) and the Fig. 7/8 `zn540` columns require real
  hardware and are not covered. The ZTree/Hy-ZTree experiment lives in `Ztree/`.
- The `L0_CNS_static` legs of Fig. 12 are *supposed* to die with an
  out-of-space error partway through — that truncated curve is the data point.
