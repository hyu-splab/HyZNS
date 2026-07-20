# Ztree — ZTree and Hy-ZTree index structures (paper Fig.13)

Two Copy-on-Write B+-tree variants for the HyZNS, compared in paper Fig.13:

- **ZTree** (`ztree/`) — ZNS-only. Every node is written sequentially to a ZNS
  zone; an NLT (Node Location Table) keeps a stable `node_id → (zone, slot)`
  mapping so a child move rewrites only the NLT, not the parent. Concurrency is
  ORWC (exact per-node latches, rd-lock coupling on descent, preemptive splits).

- **Hy-ZTree** (`hyztree/`) — hybrid. Internal nodes live on a CNS area (F2FS
  over the dm-hyzns R-region) while leaves are written to the ZNS S-region and
  spill to CNS under ZNS lock contention. `hyznsd` grows the CNS on demand
  through the ctree 2-phase handshake (`coord=ctree`). Same NLT + ORWC core.

The `hyztree/` sources are the CTree hybrid variant; only the filenames are
renamed to `hyztree_*` (identifiers, macros, and log strings keep the original
`ctree`/`CTREE_` names).

## Build

Needs `libzbd` and pthreads.

```bash
make            # both -> build/ztree, build/hy-ztree
make ztree
make hy-ztree
```

## Run (paper Fig.13)

Use the experiment wrapper, which sweeps both trees over threads 4/8/…/256 and
emits a merged CSV:

```bash
sudo -E ../experiments/fig13_ztree_hy-ztree/run.sh
```

Or drive a single tree directly (inside the FEMU VM, as root):

```bash
sudo -E bash scripts/sweep_ztree.sh   10000000 4 8 16 32 64 128 256
sudo -E bash scripts/sweep_hyztree.sh 10000000 4 8 16 32 64 128 256
```

Both sweeps run on the emulated device through the dm-hyzns target
(`/dev/mapper/hyzns0`). ZTree sets `r_end=0` (whole device is one ZNS region);
Hy-ZTree builds the dm + F2FS + hyznsd stack per thread count. Hy-ZTree needs
the `hyznsd` daemon and a patched `f2fs_io` (`resize_cns`) — see the top-level
`experiments/README.md` build step.

`hyznsd` is launched with its built-in FTR policy defaults; the sweep passes
only `coord=ctree` plus the device/mount paths and the R clamp.
