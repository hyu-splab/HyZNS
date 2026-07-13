# HYSSD Block Allocation Strategy

This document explains how blocks are allocated to lines (zones) when using configurable `chs_per_line` and `luns_per_line` parameters with contiguous block allocation.

## Overview

HYSSD supports configurable parallelism with dynamic zone sizing:
- **Full parallelism (8ch×8lun)**: 128 MiB zones, 128 total zones
- **Reduced parallelism (all others)**: 64 MiB zones, 256 total zones
- **Fixed total capacity**: 16 GiB (determined by hardware parameters)

The `chs_per_line` and `luns_per_line` parameters control parallelism. When parallelism is reduced from full (8×8), zone size is halved and zone count is doubled to maintain total capacity.

## Key Concepts

### Zone vs Line
- **Zone**: User-facing term (NVMe ZNS interface)
- **Line**: Implementation term (FTL internal management)

Both refer to the same entity: a collection of blocks across multiple channels and LUNs that are written together.

### blks_per_position
Number of blocks each (ch,lun) position uses within a line.

```
blks_per_zone = Full ? (nchs × luns_per_ch) : (nchs × luns_per_ch) / 2
blks_per_position = blks_per_zone / (chs_per_line × luns_per_line)
```

Examples:
- **8×8 (Full)**: 64 blocks/zone ÷ 64 positions = 1 block/position
- **4×4 (Reduced)**: 32 blocks/zone ÷ 16 positions = 2 blocks/position
- **2×2 (Reduced)**: 32 blocks/zone ÷ 4 positions = 8 blocks/position

### parallel_groups
Number of distinct (ch_group, lun_group) combinations for load balancing.

```
parallel_groups = (nchs / chs_per_line) × (luns_per_ch / luns_per_line)
```

Examples:
- **8×8**: 1 group
- **4×4**: 4 groups
- **2×2**: 16 groups

**Note**: In reduced parallelism mode, `parallel_groups` equals `blks_per_position` numerically (both = 2, 4, 8, 16, etc.), but they serve different semantic purposes:
- `blks_per_position`: Physical allocation (block range calculations)
- `parallel_groups`: Logical grouping (line-to-group assignment)

### line_set
Lines that use the same block number range.

```
line_set = line_id / parallel_groups
```

## Example Configurations

### Example 1: 8ch×8lun (Full Parallelism)

```bash
nchs=8, luns_per_ch=8, blks_per_lun=128
chs_per_line=8, luns_per_line=8
```

**Hardware Layout:**
- Total: 8 channels × 8 LUNs = 64 positions
- Blocks per LUN: 128
- **Total zones/lines: 128** (full parallelism)
- **Zone size: 128 MiB** (64 blocks per zone)
- Parallel groups: 1
- Blocks per position: 1

**Parallel Groups Configuration:**
```
Group 0: ch[0-7] × lun[0-7]  (64 positions, all hardware)
```

**Line to Group Mapping:**
```
Line 0   → Group 0 (0 % 1 = 0)
Line 1   → Group 0 (1 % 1 = 0)
...
Line 127 → Group 0 (127 % 1 = 0)
```

**Block Allocation:**
```
Line 0:   ch[0-7], lun[0-7], blk[0]    → 64 pos × 1 blk = 64 blocks (128 MiB)
Line 1:   ch[0-7], lun[0-7], blk[1]    → 64 pos × 1 blk = 64 blocks (128 MiB)
Line 2:   ch[0-7], lun[0-7], blk[2]    → 64 pos × 1 blk = 64 blocks (128 MiB)
...
Line 127: ch[0-7], lun[0-7], blk[127]  → 64 pos × 1 blk = 64 blocks (128 MiB)
```

Each line uses a single block number across all 64 (ch,lun) positions.

---

### Example 2: 8ch×4lun (Reduced Parallelism)

```bash
nchs=8, luns_per_ch=8, blks_per_lun=128
chs_per_line=8, luns_per_line=4
```

**Hardware Layout:**
- Total: 8 channels × 8 LUNs = 64 positions
- Blocks per LUN: 128
- **Total zones/lines: 256** (reduced parallelism = 2× zones)
- **Zone size: 64 MiB** (32 blocks per zone)
- Parallel groups: 2
- Blocks per position: 1

**Parallel Groups Configuration:**
```
Group 0: ch[0-7] × lun[0-3]  (32 positions)
Group 1: ch[0-7] × lun[4-7]  (32 positions)
```

**Line to Group Mapping:**
```
Line 0   → Group 0 (0 % 2 = 0)
Line 1   → Group 1 (1 % 2 = 1)
Line 2   → Group 0 (2 % 2 = 0)
Line 3   → Group 1 (3 % 2 = 1)
...
Line 254 → Group 0 (254 % 2 = 0)
Line 255 → Group 1 (255 % 2 = 1)
```

**Block Allocation (Line Set 0 - lines 0-1 using block 0):**
```
Line 0 (Group 0): ch[0-7], lun[0-3], blk[0]  → 32 pos × 1 blk = 32 blocks (64 MiB)
Line 1 (Group 1): ch[0-7], lun[4-7], blk[0]  → 32 pos × 1 blk = 32 blocks (64 MiB)
```

**Block Allocation (Line Set 1 - lines 2-3 using block 1):**
```
Line 2 (Group 0): ch[0-7], lun[0-3], blk[1]  → 32 pos × 1 blk = 32 blocks (64 MiB)
Line 3 (Group 1): ch[0-7], lun[4-7], blk[1]  → 32 pos × 1 blk = 32 blocks (64 MiB)
```

Lines in the same set share the same block number but use different LUN groups.

---

### Example 3: 4ch×4lun (Reduced Parallelism)

```bash
nchs=8, luns_per_ch=8, blks_per_lun=128
chs_per_line=4, luns_per_line=4
```

**Hardware Layout:**
- Total: 8 channels × 8 LUNs = 64 positions
- Blocks per LUN: 128
- **Total zones/lines: 256** (reduced parallelism = 2× zones)
- **Zone size: 64 MiB** (32 blocks per zone)
- Parallel groups: 4
- Blocks per position: 2

**Parallel Groups Configuration:**
```
Group 0: ch[0-3] × lun[0-3]  (16 positions)
Group 1: ch[4-7] × lun[0-3]  (16 positions)
Group 2: ch[0-3] × lun[4-7]  (16 positions)
Group 3: ch[4-7] × lun[4-7]  (16 positions)
```

**Line to Group Mapping:**
```
Line 0   → Group 0 (0 % 4 = 0)
Line 1   → Group 1 (1 % 4 = 1)
Line 2   → Group 2 (2 % 4 = 2)
Line 3   → Group 3 (3 % 4 = 3)
Line 4   → Group 0 (4 % 4 = 0)
...
Line 255 → Group 3 (255 % 4 = 3)
```

**Block Allocation (Line Set 0 - lines 0-3 using blocks 0-1):**
```
Line 0 (Group 0): ch[0-3], lun[0-3], blk[0-1]  → 16 pos × 2 blks = 32 blocks (64 MiB)
Line 1 (Group 1): ch[4-7], lun[0-3], blk[0-1]  → 16 pos × 2 blks = 32 blocks (64 MiB)
Line 2 (Group 2): ch[0-3], lun[4-7], blk[0-1]  → 16 pos × 2 blks = 32 blocks (64 MiB)
Line 3 (Group 3): ch[4-7], lun[4-7], blk[0-1]  → 16 pos × 2 blks = 32 blocks (64 MiB)
```

**Block Allocation (Line Set 1 - lines 4-7 using blocks 2-3):**
```
Line 4 (Group 0): ch[0-3], lun[0-3], blk[2-3]  → 16 pos × 2 blks = 32 blocks (64 MiB)
Line 5 (Group 1): ch[4-7], lun[0-3], blk[2-3]  → 16 pos × 2 blks = 32 blocks (64 MiB)
Line 6 (Group 2): ch[0-3], lun[4-7], blk[2-3]  → 16 pos × 2 blks = 32 blocks (64 MiB)
Line 7 (Group 3): ch[4-7], lun[4-7], blk[2-3]  → 16 pos × 2 blks = 32 blocks (64 MiB)
```

Lines in the same set share the same block range but use different channel/LUN groups.

## Block Allocation (Contiguous Method)

With contiguous allocation, each line uses a **contiguous range** of blocks determined by its line_set.

### Line Set to Block Range Mapping

**Line Set 0** (lines 0-3, using blocks 0-1):
```
Line 0  (Group 0): ch[0-3], lun[0-3], blk[0-1]  → 16 pos × 2 blks = 32 blocks
Line 1  (Group 1): ch[4-7], lun[0-3], blk[0-1]  → 16 pos × 2 blks = 32 blocks
Line 2  (Group 2): ch[0-3], lun[4-7], blk[0-1]  → 16 pos × 2 blks = 32 blocks
Line 3  (Group 3): ch[4-7], lun[4-7], blk[0-1]  → 16 pos × 2 blks = 32 blocks

Each line: 32 blocks (64 MiB zone)
```

**Line Set 1** (lines 4-7, using blocks 2-3):
- Each line uses 16 positions × 2 blocks = 32 blocks

**Line Set 63** (lines 252-255, using blocks 126-127):
- Each line uses 16 positions × 2 blocks = 32 blocks

### Block Number Calculation

Helper functions calculate block ranges:
```c
line_set = get_line_set(line_id, spp);
blk_start = get_line_blk_start(line_id, spp);
blk_end = get_line_blk_end(line_id, spp);  // exclusive
```

Examples:
- Line 0: [0,1]
- Line 4: [2,3]
- Line 127: [62,63]
- Line 255: [126,127]

## Write Pointer Behavior

Write sequence: ch scan → lun scan → page scan → block advance (contiguous)

```c
wpp->pg = 0;
wpp->blk++;  // Contiguous increment

int blk_end = get_line_blk_end(curline->id, spp);
if (wpp->blk >= blk_end) {
    wpp->curline = next_line;
    wpp->blk = get_line_blk_start(wpp->curline->id, spp);
    wpp->ch = wpp->curline->ch_start;
    wpp->lun = wpp->curline->lun_start;
}
```

## GC and Zone Reset Behavior

```c
int blk_start = get_line_blk_start(victim_line->id, spp);
int blk_end = get_line_blk_end(victim_line->id, spp);

for (blk = blk_start; blk < blk_end; blk++) {
    for (ch = ch_start; ch < ch_end; ch++) {
        for (lun = lun_start; lun < lun_end; lun++) {
            erase(ch, lun, blk);
        }
    }
}
```

## PPA to Page Index Mapping

```c
int line_id = get_line_id_from_ppa(ppa, spp);
struct line *line = &lm->lines[line_id];

int rel_ch = ppa->g.ch - line->ch_start;
int rel_lun = ppa->g.lun - line->lun_start;
int blk_offset = ppa->g.blk - get_line_blk_start(line_id, spp);

int pos_in_line = blk_offset * (line->ch_cnt * line->lun_cnt) +
                  rel_ch * line->lun_cnt + rel_lun;
uint64_t pgidx = line->id * spp->pgs_per_line +
                 pos_in_line * spp->pgs_per_blk + ppa->g.pg;
```

## Zone Size and Capacity

### Full Parallelism (8ch×8lun)
Zone size:
```
zone_size = nchs × luns_per_ch × pgs_per_blk × secs_per_pg × secsz
          = 8 × 8 × 512 × 8 × 512 = 128 MiB
zones = 128, blocks = 64 per zone
```

### Reduced Parallelism (all others)
Zone size:
```
zone_size = (nchs × luns_per_ch / 2) × pgs_per_blk × secs_per_pg × secsz
          = 32 × 512 × 8 × 512 = 64 MiB
zones = 256, blocks = 32 per zone
```

### Total Capacity (constant)
```
total = 8 × 8 × 128 × 512 × 8 × 512 = 16 GiB
blocks = 8,192 total
```

**Note**: Zone size and count change based on parallelism, but total capacity remains constant at 16 GiB.

## Configuration Comparison Table

| Parameter | 8×8 (Full) | 4×4 (Reduced) | 2×2 (Reduced) |
|-----------|------------|---------------|---------------|
| parallel_groups | 1 | 4 | 16 |
| blks_per_position | 1 | 2 | 8 |
| Zone count | **128** | **256** | **256** |
| Zone size | **128 MiB** | **64 MiB** | **64 MiB** |
| Blocks per zone | 64 | 32 | 32 |
| Total capacity | 16 GiB | 16 GiB | 16 GiB |
| Parallelism | All 64 positions | 16 positions | 4 positions |

### Block Allocation Summary

**8×8 (Full)**: Line N uses block N across all 64 positions (1 block each)
**4×4 (Reduced)**: Line N uses blocks [(N/4)×2, (N/4)×2+1] across 16 positions (2 blocks each)
**2×2 (Reduced)**: Line N uses blocks [(N/16)×8 ... (N/16)×8+7] across 4 positions (8 blocks each)

## Scalability: Changing Device Capacity

The design automatically scales with `blks_per_pl` parameter. All calculations are dynamic and support arbitrary capacities.

### Capacity Scaling Examples

| blks_per_pl | blks_per_lun | Full (8×8) Zones | Reduced (4×4) Zones | Total Capacity |
|-------------|--------------|------------------|---------------------|----------------|
| 128 (default) | 128 | 128 × 128 MiB | 256 × 64 MiB | 16 GiB |
| 256 | 256 | 256 × 128 MiB | 512 × 64 MiB | 32 GiB |
| 512 | 512 | 512 × 128 MiB | 1024 × 64 MiB | 64 GiB |
| 1024 | 1024 | 1024 × 128 MiB | 2048 × 64 MiB | 128 GiB |

**Formula:**
```
blks_per_lun = blks_per_pl × pls_per_lun
Total Capacity = nchs × luns_per_ch × blks_per_lun × pgs_per_blk × secs_per_pg × secsz
               = 8 × 8 × blks_per_lun × 512 × 8 × 512
```