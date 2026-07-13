# hyhostd - HyZNS CNS-area autoscaling daemon

Implements paper §5.6.3 "File System-triggered CNS Area Resizing": a userspace
monitoring daemon that periodically reads the remaining capacity of the CNS area
(from `dm-hyhost`) and the ZNS area (from the zone-aware FS, with an `nvme
report-zones` fallback), and invokes `ResizeCNS` (grow/shrink) when policy
conditions are met. Policy lives in the daemon; **the FS provides the mechanism**
(GrowCNS/ShrinkCNS). Decouples the resize policy from the application (replaces
the in-process ZenFS ZoneArbiter as the *decider*).

## Roles (ownership)
- **dm-hyhost** owns the CNS page-FTL → reports **R_C** (free CNS zones) via `dmsetup status` (`free_lines`), current ABA via `r_end`.
- **zone-aware FS** owns ZNS zone allocation → reports **R_Z** (free ZNS zones) and executes the resize:
  - **F2FS**: R_Z via sysfs/report-zones; resize via `f2fs_io resize_cns <new_rzone> <file>` (ioctl `F2FS_IOC_RESIZE_CNS`).
  - **ZenFS** (in-process lib): R_Z via a published **status file**; resize via a **control file** the ZenFS arbiter-thread (demoted to executor) watches.
- **hyhostd** owns the **policy** (when/how much), backoff, hysteresis, logging.

## Units
`free_lines`, `r_end` are in the dm-hyhost line/sector space. 1 line = 1 zone = `S_Z`.
- `current_R` (zones) = `r_end / HYHOST_ZONE_SECTORS`
- `R_C` (zones) = `free_lines`
- `R_Z` (zones) = free ZNS zones (FS or report-zones)

## Interfaces (the contract)

### R_C - dm-hyhost
`dmsetup status <dm>` → parse `free_lines=<n>`, `r_end=<sectors>`, `line_pblocks=<p>` (S_Z sectors = p*8).

### R_Z - FS adapter (primary) + report-zones (fallback), switchable
- `--rz-source=fs`   : FS-precise.
  - ZenFS: read `<aux>/.hyzns_status` (see format below) → `zenfs_free_zones`.
  - F2FS: `/sys/fs/f2fs/<dev>/` zoned-free if exposed, else fall back.
- `--rz-source=report`: universal. `nvme zns report-zones <dev>` → S-region = zones with SLBA ≥ ABA; count state==EMPTY → R_Z.

### ZenFS status file  `<aux>/.hyzns_status`  (ZenFS writes, atomic temp+rename; daemon reads)
```
ts=<epoch_seconds>
total_zones=<N>
free_zones=<N>        # = R_Z
used_zones=<N>
cur_rzone=<N>         # ZenFS's view of current R (cross-check)
```

### Resize trigger
- F2FS: `f2fs_io resize_cns <new_rzone> <mnt>/.modtarget`.
- ZenFS: write `<new_rzone>\n` to `<aux>/.hyzns_resize` (req). ZenFS executes Grow/ShrinkCNS,
  then writes `<aux>/.hyzns_resize.ack` = `<result_rzone> <OK|EIO|BUSY>` and removes the req.
  (Reuses/extends the existing `.zenfs_grow` control-file path in zone_arbiter.cc.)

## Policy (paper §5.6.3)
Per poll (S_Z = 1 zone):
- **Grow** new_R = current_R + STEP, when `R_C ≤ GROW_FLOOR` AND `R_Z ≥ GROW_GUARD`.
  (paper: GROW_FLOOR=1·S_Z, GROW_GUARD=2·S_Z, STEP=1.)
- **Shrink** new_R = current_R − STEP, when `R_C ≥ SHRINK_HI` AND `R_Z ≤ SHRINK_GUARD`.
  (paper: SHRINK_HI=2·S_Z, SHRINK_GUARD=1·S_Z, STEP=1.)
- Clamp: `R_MIN ≤ new_R ≤ R_MAX`, and grow ≤ current_R + (R_Z − GROW_GUARD) (never starve ZNS).

### Hardening
1. **Proactive GROW_FLOOR > 1·S_Z**: a purely reactive "grow at 0 free" lets the
   FS overtake the ABA before the grow lands (EIO). Grow earlier.
2. **Adaptive STEP**: under burst (R_C dropping fast), STEP>1 to stay ahead; bounded by R_MAX.
3. **Backoff on resize failure**: a failed grow wastes GC; exponential backoff, never hammer ResizeCNS.
4. **Hysteresis** + **cooldown** between resizes; one resize in flight (serialize).

## Safety
- Single in-flight resize (lock). Idempotent on daemon restart (reads live state).
- Device-down detection (nvme/dmsetup timeout) → pause, don't spin.
- Shrink requires the FS to drain CNS tail first (FS guarantees); daemon only triggers.
- ZenFS-on-S: the absorb zone must be ZenFS-free → reservation layout OR on-demand roll-off.

## Config (per device)
`dm=hyhost0 backing=/dev/nvme0n1 fs=f2fs|zenfs mnt=/mnt/aux aux=/mnt/aux
 rz_source=fs|report poll_ms=500 grow_floor=2 grow_guard=2 shrink_hi=2 shrink_guard=1
 step=1 r_min=4 r_max=64 cooldown_ms=2000 backoff_max_ms=30000`
