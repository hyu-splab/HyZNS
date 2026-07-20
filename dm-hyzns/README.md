# dm-hyzns

Out-of-tree Linux device-mapper target (paper name **dm-hyzns**) that
provides the host-side FTL for the HyZNS FEMU device
(`femu_mode=5`). It stacks on `/dev/nvme0n1` inside the guest VM and
exposes `/dev/mapper/hyzns0`, splitting the namespace into a
random-write **R-region** (host-managed, page-mapped) and a sequential
**S-region** (standard ZNS, pass-through). The runtime-movable boundary
between them, the **ABA** (Area Boundary Address), is what makes the
device a *resizable* hybrid.

Built and loaded **inside the guest**, never on the host. The host runs
FEMU; the guest sees `/dev/nvme0n1` and stacks dm-hyzns on it.

```
   F2FS / ZenFS / RocksDB / fio
              │
              ▼
      /dev/mapper/hyzns0          ← dm-hyzns (this module)
  ┌───────────────────────┬─────────────────────────────┐
  │ R-region [0 .. ABA)   │ S-region [ABA .. EOD)        │
  │ random write,         │ sequential zones,            │
  │ 4 KiB-page L2P + GC   │ pass-through (LBA remap only) │
  └───────────────────────┴─────────────────────────────┘
              │
              ▼
      /dev/nvme0n1                 ← FEMU HyZNS (femu_mode=5)
```

## Quick start

```bash
# inside the guest
sudo apt install -y linux-headers-$(uname -r) build-essential
./scripts/build.sh                       # → dm-hyzns.ko  (KDIR= to override)
sudo ./scripts/load.sh                    # rmmod (if loaded) + insmod
sudo ./scripts/setup.sh                   # /dev/mapper/hyzns0, R = whole device
#   sudo ./scripts/setup.sh /dev/nvme0n1 33554432 hyzns0   # R = 16 GiB
sudo ./scripts/unload.sh                  # remove target + module
```

`setup.sh`'s second argument is the **initial/current R-region size in
512B sectors** (the live R/S boundary, `r_end`). The dm target table is:

```
0 <len> hyzns <backing_dev> <r_end> [max_r_end_lba]
```

The optional 3rd arg `max_r_end_lba` **max-provisions** the L2P / reverse
map / line rings for that maximum boundary, so the live `r_end` can later
**grow** anywhere in `[0, max_r_end_lba]` without reallocation - the
dm-side analogue of `mkfs.f2fs -A -P`. If omitted, the cap defaults to the
initial `r_end` and the boundary can only shrink/grow within `[0, r_end]`
(i.e. no growth past the initial size). See *Boundary reshape*.

## Geometry

The FTL mirrors the FEMU NAND layout so the device timing model sees
realistic parallelism:

| | value |
|---|---|
| host page | 4 KiB (`HYZNS_PAGE_SHIFT=3`; F2FS/RocksDB/fio block size) |
| NAND block | 2 MiB = 512 host pages (`HYZNS_PGS_PER_BLK`) |
| channels × LUNs | 8 × 4 = 32 |
| **line** | 64 MiB = 16,384 host pages (`HYZNS_LINE_PBLOCKS`) |
| **zone** | 1 GiB = 16 lines (`HYZNS_ZONE_PAGES` = 262,144) |

A **line** is the allocation/GC/erase unit: one NAND block on every
(channel, LUN). Consecutive pblocks within a line stripe across all 32
(ch, LUN) lanes (page-major, BBSSD-style), so a write burst sees full
NAND parallelism and a whole-line erase charges all (ch, LUN) erases
in parallel (about one erase round). A **zone** is the logical unit
F2FS/ZenFS see; the ABA always falls on a 1 GiB zone boundary.

## How the R-region FTL works

**Mapping.** R-region traffic is translated through a 4 KiB-page L2P
map with a reverse map (`rmap[pblock] → lpage`) for GC. Writes are
out-of-place (log-structured): a write allocates a fresh pblock from the
current write line and the old pblock is invalidated. Per-line valid
counts drive recycling - when a line's valid count hits zero it returns
to the free ring.

**endio-deferred L2P swap.** Out-of-place writes (and GC migrations)
reserve the new pblock at `.map()` time but defer the L2P/rmap swap to
backing-IO completion (`.end_io`). Concurrent readers keep seeing the
old, still-valid mapping until the new data has actually landed.
Same-lpage concurrent writes resolve last-endio-wins (NVMe-undefined,
permitted).

**Garbage collection.** A background kthread reclaims partially-valid
lines. It wakes when free lines drop below the low watermark and runs to
the high watermark. Two victim policies (`dmsetup message hyzns0 0
gc_policy {greedy|cb}`):
- **greedy** (default) - smallest valid count wins.
- **cb** - cost-benefit, `age × (line_pblocks − valid) / valid`.

Migration is **batched**: valid pblocks are collected in batches of 128,
read in one concurrent wave, written in another, then committed under one
lock pass (≈ 30–40× faster than per-page sync copy). GC migrates into a
**private destination line** (`gc_cur`), separate from the user write
line, so a busy writer can never starve a migration mid-victim.
`dmsetup message hyzns0 0 gc` forces a sweep.

**Erase.** When GC (or a discard) drains a line to zero valid, the line
goes on a dirty ring and an **erase kthread** issues one
`REQ_OP_ZONE_R_LINE_ERASE` bio per line → NVMe Zone Mgmt Send with
vendor **ZSA 0x23** (`NVME_ZONE_R_LINE_ERASE`), SLBA = line start. FEMU
charges a parallel NAND erase on every (ch, LUN) of that line.

**Discard absorption.** F2FS checkpoint discards / `fstrim` over the
R-region (`REQ_OP_DISCARD`) are absorbed at the dm layer - no command
reaches the device. Each fully-covered 4 KiB page has its L2P/rmap
invalidated and its line's valid count decremented; a line that hits
zero is queued for erase. `.io_hints` advertises `discard_granularity =
4096` so the block layer splits large discards on page boundaries (a
512B granularity leaks one page per >4 GiB split). This turns a file
delete directly into reclaimable space instead of leaving dead-but-valid
pages for GC to copy.

**Back-pressure.** When the ring is exhausted and the target lpage is
already mapped, the write falls back to **in-place** (reuses the existing
pblock). When the ring is exhausted and the lpage is unmapped, the
submitter **stalls in `.map()`** (process context - sleeping is allowed)
until GC/erase frees a line. It fails with honest `BLK_STS_NOSPC` only
after GC has scanned and parked victimless (genuinely full); `REQ_NOWAIT`
bios get `BLK_STS_AGAIN`. (`DM_MAPIO_REQUEUE` is *not* used - bio-based dm
turns it into EIO outside a noflush suspend.)

The S-region path is pure pass-through (LBA remap only).

## `dmsetup status`

```
0 <len> hyzns r_end=<lba> pages=<n> lines=<n> line_pblocks=<n>
  zone_pblocks=<n> block_pages=<n> gc_policy=<greedy|cb> free_lines=<n>
  dirty=<n> cur_off=<n> r_bios=<n> s_bios=<n> splits=<n> w=<n> r=<n>
  unmapped=<n> ovr=<n> inplace=<n> recycles=<n> gc_runs=<n>
  gc_mig=<n> gc_skip=<n> erases=<n> erase_fail=<n> requeue=<n>
  nospc=<n> wfail=<n> discards=<n> discard_pgs=<n> valid_pages=<n>
  valid_lines=<n> vh=<a:b:c:d:e> best_victim_vpc=<n> free_pages=<n>
  gc_blocked=<0|1>
```

| field | meaning |
|---|---|
| `r_end` | current ABA (sectors); the live R/S boundary (active R) |
| `pages` / `lines` | **max-provisioned** L2P capacity in pblocks / lines (`max_r_end_lba`, fixed at ctr; ≥ active R) |
| `line_pblocks` / `zone_pblocks` / `block_pages` | 16384 (64 MiB line) / 262144 (1 GiB zone) / 512 (2 MiB block) |
| `gc_policy` | active victim policy |
| `free_lines` / `dirty` | free ring depth / lines awaiting erase |
| `cur_off` | pblocks used in the active user write line |
| `r_bios` / `s_bios` | bios routed to R / S region |
| `splits` | bios split across the ABA |
| `w` / `r` | R-region writes / reads (pages) |
| `unmapped` | R reads of unmapped pages (zero-filled) |
| `ovr` / `inplace` | overwrites / subset that fell back to in-place |
| `recycles` | lines returned to the ring (valid hit 0, any trigger) |
| `gc_runs` / `gc_mig` / `gc_skip` | GC victim picks / pages copied (WAF numerator) / migrations skipped on L2P race |
| `erases` / `erase_fail` | R_LINE_ERASE bios issued / failed |
| `requeue` | write stall episodes (waited for a free line) |
| `nospc` / `wfail` | writes failed ENOSPC / backing-write error |
| `discards` / `discard_pgs` | discard bios absorbed / L2P pages invalidated |
| `valid_pages` / `valid_lines` | live-mapped pages in R / lines inside the active R |
| `vh` | per-line validity histogram: empty, <25%, <50%, <75%, ≥75% |
| `best_victim_vpc` | greedy victim's valid% (GC cost proxy) |
| `free_pages` | truly-unwritten pblocks (free lines + open-line tails) |
| `gc_blocked` | 1 while GC is parked (no useful victim) |

WAF = `(w + inplace + gc_mig) / (w + inplace)` over a measured window.

## Boundary reshape (ABA modify)

```bash
sudo ./scripts/set_r_end.sh [TARGET] <NEW_R_END_LBA>   # suspend-aware wrapper
#   or, if no concurrent IO:
sudo dmsetup message hyzns0 0 set_r_end <NEW_R_END_LBA>
```

`NEW_R_END_LBA` is page-aligned (multiple of 8 sectors), within the
**max-provisioned** capacity (`max_r_end_lba`). The same path is driven
either by this `dmsetup message`, or by an in-kernel `REQ_OP_ZONE_MODIFY`
bio from F2FS's ABA-modify ioctl arriving via `.map()` (handed to the
zmod workqueue). The handler runs in a workqueue (process context):

- **Shrink** (R↓): high-LBA R-lines being handed to the S-region are
  **force-GC'd first** - their valid pages migrate to lower R-lines
  (data is *not* dropped) - then the device ABA is moved and the dm-side
  L2P / free ring are quiesced to the new boundary.
- **Grow** (R↑): the device validates that the reshaped S-zones are all
  EMPTY (else it rejects the resize, leaving state untouched); the
  newly-R lines - already provisioned at ctr (`max_r_end_lba`) - are
  re-exposed onto the free ring (`hyzns_quiesce_grow`).

Wire: a single `REQ_OP_ZONE_MODIFY` bio carries the new ABA in
`bi_sector`, which the NVMe driver maps to **SLBA** of a Zone Mgmt Send
with vendor **ZSA 0x20** (no CDW14 - that legacy path is gone). On
rejection the message returns the error and dm-side state is unchanged.
Growth past the max-provisioned cap (3rd ctr arg, default = initial
`r_end`) is rejected with `BLK_STS_IOERR`.

**ABA query.** `BLKREPORTABA` (vendor **ZRA 0x21**) reports the current
R-zone count. dm-hyzns **answers this locally from its own `r_end`**
(no device round-trip) - dm core mangles a forwarded zone-mgmt clone's
payload, and the dm target is the authoritative boundary for the stack
above it. mkfs.f2fs and the ZenFS utility use this to auto-detect the R
size at format time.

### F2FS aux online ABA-resize

In the ZenFS+aux topology (aux F2FS = R-region only, ZenFS = S-region),
F2FS tracks the ABA **online** - both directions, no reformat, no
unmount, no data loss - when its modify ioctl fires. The full stack is
max-provisioned at four layers so the boundary can move freely while the
filesystem stays mounted:

1. `mkfs.f2fs -A -P` lays F2FS metadata for the whole device (max R) and
   clusters its cursegs in the low (initial-ABA) zones.
2. At mount the kernel gates the usable area (`MAIN_SECS`) down to the
   current ABA, keeping the on-disk checkpoint at max so the gate is
   idempotent across remount.
3. `f2fs_io modify_zone <n>` → `f2fs_aux_resize` force-GC-drains (shrink)
   or re-exposes (grow) F2FS sections, and issues the `REQ_OP_ZONE_MODIFY`
   that reaches **this module**.
4. dm-hyzns, max-provisioned via `max_r_end_lba`, runs the shrink/grow
   quiesce above and moves the device ABA.

`scripts/f2fs_aux_resize_verify.sh` exercises the full path end-to-end
(mkfs -A -P → gate → grow → shrink → remount) with md5 checks at every step.

## Filesystems on top

**R-region only** (flat conventional device, no S access):
```bash
sudo ./scripts/f2fs_setup.sh [BACKING] [R_END_SECTORS] [MNT]   # default 8 GiB, /mnt/hyzns
sudo ./scripts/f2fs_teardown.sh [MNT]
```

**Hybrid R+S** (single F2FS mount spanning both regions, HYSSD-style):
```bash
sudo ./scripts/f2fs_hybrid_setup.sh [R_END_SECTORS] [MNT]      # default R = device/2
sudo ./scripts/f2fs_hybrid_teardown.sh [MNT]
```
This builds a whole-device target, syncs the device ABA, and runs
`mkfs.f2fs -m -H -B <num_R_zones>`; the kernel's HYSSD F2FS patches query
the R count via ZRA 0x21 and mount it as a multi-device volume
(R = conventional, S = zoned).

**ZenFS / RocksDB** stack on the same `/dev/mapper/hyzns0` with an F2FS
aux mount (`mkfs.f2fs -m -H -A`) holding `LOG`/`LOCK` (and, with the
`WAL_POSIX` build, WAL files). Driven from `rocksdb/`.

## Scripts

| script | purpose |
|---|---|
| `build.sh` / `load.sh` / `unload.sh` | build, (re)load, tear down module |
| `setup.sh` | create a target (R size = arg in sectors) |
| `set_r_end.sh` | suspend-aware ABA reshape wrapper |
| `f2fs_setup.sh` / `_teardown.sh` | F2FS on the R-region only |
| `f2fs_hybrid_setup.sh` / `_teardown.sh` | single F2FS over R+S |
| `f2fs_aux_resize_verify.sh` | end-to-end aux online ABA-resize verify (mkfs -A -P → gate → grow/shrink → remount, md5-checked) |
| `_lib.sh` | shared helpers (`reset_device_full_r`, `report_rzones`, `extract`) - sourced, not run |
| `d14_sweep.sh` / `d14_timeseries.sh` | R-size sweep / time-series (bw/WAF/GC) |
| `d17_zipf_policy.sh` | GC policy comparison (greedy/cb × zipf/uniform/phased) |
| `dev_bw.sh` | raw device bandwidth matrix vs the FEMU timing model |
| `gc_lat_hyznsr.sh` | host-FTL line-GC relocation latency |
| `plot_*.py` | plot the corresponding CSV outputs |

Experiment scripts write CSV/PNG/SVG under `results/`.

## Constraints & pitfalls

- R-region IO must be 4 KiB aligned (offset and length); misaligned bios
  complete `BLK_STS_NOTSUPP`.
- `R_END`/`set_r_end` values are **512B sectors**, page-aligned (×8);
  `f2fs_hybrid_setup.sh` additionally wants 1 GiB-zone alignment.
- Growing the device ABA is refused while the reshaped S-zones hold data.
  `reset_device_full_r` (in `_lib.sh`, used by the hybrid scripts) resets
  the S-region before growing; `blkzone reset <dev>` forces it manually.
- The L2P walk during reshape holds the per-target lock and blocks
  in-flight `.map()` for its duration (tens of ms at 64 GiB R).
- Module unload after a crash can fail (leaked refcount) - reboot the
  guest. `dmsetup remove` EBUSY → check mounts/`fuser`/`losetup`.
- `dm-hyzns.ko` needs `linux-headers` matching `uname -r` exactly.
