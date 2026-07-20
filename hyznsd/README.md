# hyznsd - HyZNS CNS-area autoscaling daemon

Userspace policy daemon implementing paper **§5.6.3 "File System-triggered CNS
Area Resizing"**. It periodically reads the free capacity of the CNS area (**R**,
from `dm-hyzns`) and the ZNS area (**S**, from the zone-aware FS or
`nvme report-zones`) and triggers an absolute-target `ResizeCNS` (grow/shrink)
when policy conditions are met.

**Separation of concerns:** the daemon owns *policy* (when / how much); the file
system provides the *mechanism* (`GrowCNS`/`ShrinkCNS`). This replaces the
in-process ZenFS `ZoneArbiter` as the *decider*; the arbiter acts as a status
*publisher* + resize *executor*. See `DESIGN.md` for the full spec.

All capacities are in **zones**: 1 zone = 1 GiB = `zone_pblocks*8` sectors
(= 16 dm-hyzns lines at the current geometry).

## Build
```sh
make            # -> ./hyznsd
make test       # offline policy self-test (no device needed)
```

## Files
| File | Role |
|---|---|
| `hyznsd.h` | contract constants, config/state/decision types, prototypes |
| `hyznsd.c` | main: config parse, monitor loop, cooldown/backoff, logging, util |
| `monitor.c` | R_C reader (`dmsetup status`) + R_Z reader (`fs` \| `report`) |
| `policy.c` | decide (grow/shrink) + execute (f2fs_io \| ZenFS ctl) |
| `scripts/selftest.sh` | offline policy validation with mock dm/report inputs |
| `scripts/poc.sh` | live one-cycle contract demo on F2FS |
| `hyznsd.conf.example` | annotated config |

## The contract (interfaces)
- **R_C / current_R**: `dmsetup status <dm>` -> `free_pages`, `r_end`,
  `zone_pblocks`. `current_R = r_end / (zone_pblocks*8)`.
- **R_Z**: two interchangeable sources (knob `rz_source`):
  - `report` (universal): `blkzone report <backing>`; S-region = zones at
    SLBA >= ABA, free = zone condition EMPTY. (The parser also accepts
    `nvme zns report-zones` text, but nvme needs an exact `-d` count, so the
    daemon uses `blkzone`, which returns all zones with no count arg.)
  - `fs` (precise): ZenFS publishes `<aux>/.hyzns_status` (`free_zones=<n>`).
    F2FS has no zone-granular query exposed and falls back to `report`.
- **Resize trigger** (absolute target R):
  - F2FS: `f2fs_io resize_cns <target> <mnt>/.hyznsd_probe` (kernel orchestrates
    grow=device-first / shrink=FS-gate-first by direction).
  - ZenFS: write `<target>` to `<aux>/.hyzns_resize`; executor replies in
    `<aux>/.hyzns_resize.ack` = `<result_R> OK|EIO|BUSY`.

## Policy (paper §5.6.3, FTR)
- **Grow** `target = cur_R + 1` when `R_C < ftr_grow_rc*S_Z` **and**
  `R_Z >= ftr_grow_rz` (clamped to `r_max`).
- **Shrink** `target = cur_R - 1` when `R_C > ftr_shrink_rc*S_Z` **and**
  `R_Z < ftr_shrink_rz` (clamped to `r_min`).
- Paper defaults 2/5/3/4; thresholds are administrator-configurable in
  `hyznsd.conf`. Exponential backoff on resize failure and a post-resize
  cooldown are enforced by the main loop.

## Deploy as a systemd daemon
`hyznsd` is daemon-shaped (continuous loop, SIGTERM clean-shutdown, config
file, restart-safe: it re-reads live R_C/R_Z on start). `Type=simple` supervises
the foreground process and (with no `logfile` set) its stderr goes to journald.
```sh
sudo make install        # -> /usr/local/sbin/hyznsd, /etc/systemd/system/hyznsd.service,
                         #    /etc/hyznsd.conf (kept if present)
sudo systemctl daemon-reload
sudo systemctl enable --now hyznsd
journalctl -u hyznsd -f          # watch decisions/grows live
```
`Restart=on-failure` makes a device hiccup self-healing (the daemon re-derives
state on restart); `ConditionPathExists=/dev/mapper/hyzns0` keeps it from starting
before the device exists. Files: `systemd/hyznsd.service`, `systemd/hyznsd.conf`.

## Run
```sh
# offline policy check
make test

# live one-cycle demo (F2FS), dry-run then apply
sudo ./scripts/poc.sh                 # decide + print only
sudo ./scripts/poc.sh --apply         # execute the resize

# daemon
sudo ./hyznsd -c hyznsd.conf            # continuous
sudo ./hyznsd -c hyznsd.conf --dry-run  # decide + log, no act
sudo ./hyznsd --once -v --set dm=hyzns0 --set fs=f2fs --set mnt=/mnt/hyzns
```

### Live demo
```sh
make
sudo ./scripts/live_demo.sh 8 12     # R_init=8, fill 12 GiB; daemon auto-grows R
# artifacts: results/live_demo/{timeline_*.csv, daemon_*.log}
```
The patched `f2fs_io` (with `resize_cns`) is required; the demo defaults to
`../f2fs-tools-1.16.0/tools/f2fs_io/f2fs_io` (override with `F2FS_IO=`).

### Driving ZenFS with the daemon
The ZenFS side lives in `rocksdb/plugin/zenfs/fs/zone_arbiter.cc`: it publishes
`<aux>/.hyzns_status` each poll (atomic temp+rename) and consumes
`<aux>/.hyzns_resize`, running Grow/ShrinkCNS and acking in `.hyzns_resize.ack`.
Env `ZENFS_ARBITER_EXTERNAL=1` gates the in-process threshold decider OFF so the
daemon drives.
```sh
# ZenFS started with the arbiter in external mode + a tight publish interval:
ZENFS_ZONE_ARBITER=1 ZENFS_ARBITER_EXTERNAL=1 ZENFS_ARBITER_INTERVAL_MS=200 \
  db_bench ... --fs_uri=zenfs://dev:nvme0n1
# daemon drives it (aux = the ZenFS aux/control dir):
sudo ./hyznsd --set fs=zenfs --set rz_source=fs \
  --set aux=/mnt/zenfs_aux --set dm=hyzns0
```
