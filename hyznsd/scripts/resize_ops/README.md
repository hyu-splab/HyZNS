# resize_ops — ResizeCNS (GROW/SHRINK) ops-timeline experiments

Measures the impact of ResizeCNS (grow/shrink) on a running workload (RocksDB
db_bench on ZenFS + F2FS-aux) at 100 ms resolution, decomposed across the three
layers (F2FS / dm-hyzns / ZenFS arbiter) plus the RocksDB LOG
(flush/compaction events). Result PNG/CSV files land in
`../../results/resize_ops/`.

## Measurement infrastructure

- **Ops sampling = db_bench's internal ReporterAgent `===REPORT`**
  (db_bench_tool.cc). The 100 ms interval is enabled with
  `ROCKSDB_REPORT_INTERVAL_MS=100` (the stock `--report_interval_seconds` only
  takes integer seconds). Line format: `===REPORT <ts> <total> <read> <write>
  <maxrd> <maxwr>` — emitted from a background timed-wait thread, so samples
  keep coming during stalls (qps=0 included). Plots convert
  `ops/100ms x 10 = ops/s`.
- **Resize instrumentation (kernel/module, built separately)**:
  - F2FS `===[ResizeCNS GROW|SHRINK]` summary (gc.c): resize_total /
    user-BLOCK (freeze) / device ABA (modify) / freeze-thaw-curseg-geom /
    drain online (pass1) and frozen (pass2) / checkpoint / do_garbage_collect /
    gc_data_segment phases / move_data_page-block / valid blocks moved.
  - dm-hyzns: `[GC]` (autonomous) vs `[SHRINK force-GC] TOTAL` (forced) tags,
    and `[ZMOD]` forcegc / device_submit (ZSA) / quiesce phase split.
  - ZenFS: `[GrowLAT]` (begin -> migrated (roll-off) -> reset -> devaba ->
    done), `[ShrinkLAT]`, `[ShrinkCNS-SUM]`
    (= SendResizeCNS(kernel) + ShrinkReservedZones).
- **kmsg markers**: each step brackets dmesg with
  `echo "===HYEXP <tag> BEGIN/END" > /dev/kmsg` and only that window is parsed.
  Do NOT use `dmesg -C` per step — it drops the dm worker's async log lines;
  instead `sync; sleep 0.5` after each step.
- **Logs are written to tmpfs (`/dev/shm/hyexp`)** so the measured device
  (/dev/nvme0n1) is not polluted; tmpfs is volatile, so results are copied to
  `results/resize_ops/` afterwards.
- **Every CSV has a header row.** ops: `ts_epoch,total_ops_per_100ms,read,write`;
  phases: `ts_epoch,bench_name`; events: `dir,target_s,req_t_epoch,echo_t_epoch,
  done_t_epoch,R_pre,R_post,roll_mib,f2_mib,dm_mib,kgrow_ms`.

## Invariants (violating these invalidates a run)

- **ZenFS `--start_zone` = the current ABA.** No gap layout (empty zones
  between R and S) — a gap hides the grow-side ZenFS roll-off. To observe grow,
  build with no gap: `mkfs full -> gate down to R0 -> ZenFS mkfs at R0`.
- **GROW must go through the arbiter (`.zenfs_grow`)** — calling
  `f2fs_io resize_cns` directly skips the ZenFS roll-off (migration of data in
  the absorbed S-zones) and corrupts data. SHRINK likewise via
  `.zenfs_shrink`. Arbiter env: `ZENFS_ZONE_ARBITER=1 ARBITER_EXTERNAL=1`.
- **db_bench `--max_background_jobs=2`** (the RocksDB default). When reporting
  a configuration, state the background-thread count too.
- ao_zones: single instance = 14. **compression = none** (avoids data-volume
  distortion); ops-timeline runs only look at throughput, but keep none for
  consistency.

## Script inventory

| script | what | plot |
|---|---|---|
| `ops_vanilla.sh` | baseline without resize: fillrandom -> overwrite, 100 ms ops | `plot_vanilla.py` |
| `ops_vanilla_resize.sh` | vanilla + wall-clock-forced resizes (grow 250/260/270, shrink 290/300/310) | `plot_vr.py`, `plot_vr_zoom.py` |
| `ops_grow_heavy.sh` | fillrandom 10M, grows every 10 s from R4 (heavy grow) | `plot_gh_zoom.py` (zoom on grows 1/3/5/7) |
| `ops_shrink_heavy.sh` | R0=8, inject valid data into R (A 3G + B 2G, rm A, reclaim), fillrandom + shrink 8->4 (t+10/20/30/40). Knobs: `SHRINK_ON=0/1`, `RUNTAG=base/sh` | `plot_shrink_vs_base.py` |
| `parse_events.py` | RocksDB LOG EVENT_LOG -> flush/compaction (epoch) CSV | — |
| earlier harnesses: `zenfs_grow2/shrink2/shrink_v4`, `grow_chain`, `ops_grow`, `ops_grow_shrink`, `ops_modify`, `throttle_sweep` | intermediate development stages, kept for reference | matching `plot_*` |

Example run (tmpfs copy):
```
cp *.sh *.py /dev/shm/hyexp/
SHRINK_ON=0 RUNTAG=base bash /dev/shm/hyexp/ops_shrink_heavy.sh   # baseline
SHRINK_ON=1 RUNTAG=sh   bash /dev/shm/hyexp/ops_shrink_heavy.sh   # with shrink
python3 plot_shrink_vs_base.py
```

## Notes

- Heavy-shrink victim recipe: `A (low filler) + B (high data) + rm A +
  gc_urgent reclaim`. F2FS allocates low-first, so A pushes B to the upper
  zones; removing A leaves B valid in the released tail. Do not fill R
  completely (keep free_lines headroom, ~75% of usable R) or the reclaim pass
  can relocate B's valid blocks as well.
- Write-only workloads (fillrandom) cannot separate a shrink dip from a
  compaction write-stall (ops=0 in both). Use `readwhilewriting` when that
  separation matters: reads hold the baseline up through compaction stalls but
  dip during a shrink's bandwidth occupation.
