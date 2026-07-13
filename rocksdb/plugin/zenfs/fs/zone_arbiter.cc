// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "zone_arbiter.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <set>
#include <map>

#include "f2fs_ioctl.h"
#include "fs_zenfs.h"
#include "rocksdb/env.h"
#include "snapshot.h"
#include "zbd_zenfs.h"

namespace ROCKSDB_NAMESPACE {

ZoneArbiter::ZoneArbiter(ZenFS* zenfs, ZonedBlockDevice* zbd,
                         std::shared_ptr<Logger> logger,
                         const ZoneArbiterConfig& config)
    : zenfs_(zenfs), zbd_(zbd), logger_(logger), config_(config) {}

ZoneArbiter::~ZoneArbiter() { Stop(); }

IOStatus ZoneArbiter::Start() {
  if (running_.load()) {
    return IOStatus::OK();
  }

  if (config_.aux_path.empty()) {
    return IOStatus::InvalidArgument("ZoneArbiter: aux_path is not set");
  }

  Info(logger_, "ZoneArbiter: Starting with aux_path=%s, check_interval=%lu us",
       config_.aux_path.c_str(), config_.check_interval_us);

  printf("[ZoneArbiter] Started - aux_path=%s, interval=%lu sec, "
         "expand_threshold=%.0f%%, initial R-zone=%lu\n",
         config_.aux_path.c_str(), config_.check_interval_us / 1000000,
         config_.f2fs_expand_threshold, zbd_->GetReservedZones());
  fflush(stdout);

  stop_requested_.store(false);
  running_.store(true);

  monitor_thread_.reset(new std::thread(&ZoneArbiter::MonitorLoop, this));

  return IOStatus::OK();
}

void ZoneArbiter::Stop() {
  if (!running_.load()) {
    return;
  }

  Info(logger_, "ZoneArbiter: Stopping...");
  printf("[ZoneArbiter] Stopping...\n");
  fflush(stdout);

  stop_requested_.store(true);

  // Wake up the monitor thread
  {
    std::lock_guard<std::mutex> lock(monitor_mtx_);
    monitor_cv_.notify_all();
  }

  // Resume writes if paused
  if (write_paused_.load()) {
    ResumeWrites();
  }

  if (monitor_thread_ && monitor_thread_->joinable()) {
    monitor_thread_->join();
  }

  running_.store(false);
  Info(logger_, "ZoneArbiter: Stopped");
}

void ZoneArbiter::MonitorLoop() {
  Info(logger_, "ZoneArbiter: Monitor loop started");

  while (!stop_requested_.load()) {
    // Wait for check interval or wakeup
    {
      std::unique_lock<std::mutex> lock(monitor_mtx_);
      monitor_cv_.wait_for(
          lock, std::chrono::microseconds(config_.check_interval_us),
          [this] { return stop_requested_.load(); });
    }

    if (stop_requested_.load()) break;

    // hyhostd contract: publish current zone status every poll so the daemon's
    // fs R_Z source can read it (<aux>/.hyzns_status).
    if (config_.publish_status) PublishStatus();

    // hyhostd contract: execute an absolute-target resize request if present.
    if (HandleResizeReq()) continue;

    // Manual/scripted trigger: a control file `<aux>/.zenfs_grow` holding a zone
    // count forces an immediate grow (bypasses thresholds + cooldown). This gives
    // a deterministic trigger for the dynamic-partitioning experiment.
    {
      std::string ctl = config_.aux_path;
      if (!ctl.empty() && ctl.back() != '/') ctl += "/";
      ctl += ".zenfs_grow";
      FILE* f = fopen(ctl.c_str(), "r");
      if (f) {
        unsigned cnt = 0;
        if (fscanf(f, "%u", &cnt) != 1 || cnt == 0) cnt = 1;
        fclose(f);
        unlink(ctl.c_str());
        printf("[ZoneArbiter] manual grow trigger: +%u zones (R=%lu)\n", cnt,
               zbd_->GetReservedZones());
        fflush(stdout);
        IOStatus ms = GrowCNS(cnt);
        last_adjustment_time_ = Env::Default()->NowMicros();
        printf("[ZoneArbiter] manual grow %s -> R=%lu\n",
               ms.ok() ? "OK" : ("FAILED: " + ms.ToString()).c_str(),
               zbd_->GetReservedZones());
        fflush(stdout);
        continue;
      }
    }

    // Manual/scripted shrink trigger: `<aux>/.zenfs_shrink` holding a zone count.
    {
      std::string ctl = config_.aux_path;
      if (!ctl.empty() && ctl.back() != '/') ctl += "/";
      ctl += ".zenfs_shrink";
      FILE* f = fopen(ctl.c_str(), "r");
      if (f) {
        unsigned cnt = 0;
        if (fscanf(f, "%u", &cnt) != 1 || cnt == 0) cnt = 1;
        fclose(f);
        unlink(ctl.c_str());
        printf("[ZoneArbiter] manual shrink trigger: -%u zones (R=%lu)\n", cnt,
               zbd_->GetReservedZones());
        fflush(stdout);
        IOStatus ms = ShrinkCNS(cnt);
        last_adjustment_time_ = Env::Default()->NowMicros();
        printf("[ZoneArbiter] manual shrink %s -> R=%lu\n",
               ms.ok() ? "OK" : ("FAILED: " + ms.ToString()).c_str(),
               zbd_->GetReservedZones());
        fflush(stdout);
        continue;
      }
    }

    // External-driver mode: hyhostd owns the policy. Skip the in-process
    // threshold decider; only publish + execute requests (handled above).
    if (config_.external) continue;

    // Check cooldown
    uint64_t now = Env::Default()->NowMicros();
    if (now - last_adjustment_time_ < config_.adjustment_cooldown_us) {
      continue;
    }

    // Query F2FS status
    F2FSStats f2fs_stats;
    IOStatus s = QueryF2FSStats(&f2fs_stats);
    if (!s.ok()) {
      Warn(logger_, "ZoneArbiter: Failed to query F2FS stats: %s",
           s.ToString().c_str());
      continue;
    }

    // Query ZenFS status
    ZenFSZoneStats zenfs_stats;
    s = QueryZenFSStats(&zenfs_stats);
    if (!s.ok()) {
      Warn(logger_, "ZoneArbiter: Failed to query ZenFS stats: %s",
           s.ToString().c_str());
      continue;
    }

    // Log current status (both logger and printf for visibility)
    Debug(logger_,
          "ZoneArbiter: F2FS util=%.1f%% (free=%lu MB), "
          "ZenFS util=%.1f%% (free=%lu MB)",
          f2fs_stats.utilization, f2fs_stats.free_space / (1024 * 1024),
          zenfs_stats.utilization, zenfs_stats.free_space / (1024 * 1024));

    // Printf for console visibility
    printf("[ZoneArbiter] R-zone=%lu | F2FS: %.1f%% used (free=%lu MB) | "
           "ZenFS: %.1f%% used (free=%lu MB, %u/%u zones)\n",
           zbd_->GetReservedZones(),
           f2fs_stats.utilization, f2fs_stats.free_space / (1024 * 1024),
           zenfs_stats.utilization, zenfs_stats.free_space / (1024 * 1024),
           zenfs_stats.used_zones, zenfs_stats.total_zones);
    fflush(stdout);

    // Decide action
    AdjustmentAction action = DecideAction(f2fs_stats, zenfs_stats);

    if (action != AdjustmentAction::kNone) {
      uint32_t zone_count = CalculateExpansionCount(f2fs_stats, zenfs_stats);

      Info(logger_, "ZoneArbiter: Executing GrowCNS for %u zones",
           zone_count);
      printf("[ZoneArbiter] >>> Expanding R-zone by %u zones (current=%lu)\n",
             zone_count, zbd_->GetReservedZones());
      fflush(stdout);

      s = ExecuteAdjustment(action, zone_count);
      if (s.ok()) {
        last_adjustment_time_ = Env::Default()->NowMicros();
        Info(logger_, "ZoneArbiter: Adjustment completed successfully");
        printf("[ZoneArbiter] >>> Expansion complete! New R-zone=%lu\n",
               zbd_->GetReservedZones());
        fflush(stdout);
      } else {
        Error(logger_, "ZoneArbiter: Adjustment failed: %s",
              s.ToString().c_str());
        printf("[ZoneArbiter] >>> Expansion FAILED: %s\n",
               s.ToString().c_str());
        fflush(stdout);
      }
    }
  }

  Info(logger_, "ZoneArbiter: Monitor loop stopped");
}

// hyhostd contract: publish ZenFS zone status atomically (temp + rename) so the
// daemon's `rz_source=fs` reader gets a consistent snapshot. Format matches
// hyhostd's parser (key=value lines; it reads `free_zones`).
void ZoneArbiter::PublishStatus() {
  std::string base = config_.aux_path;
  if (!base.empty() && base.back() != '/') base += "/";
  std::string path = base + ".hyzns_status";
  std::string tmp = path + ".tmp";

  ZenFSZoneStats zs;
  if (!QueryZenFSStats(&zs).ok()) return;

  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) return;
  fprintf(f,
          "ts=%lu\n"
          "total_zones=%u\n"
          "free_zones=%u\n"
          "used_zones=%u\n"
          "resettable_zones=%u\n"
          "cur_rzone=%lu\n",
          (unsigned long)(Env::Default()->NowMicros() / 1000000ULL),
          zs.total_zones, zs.free_zones, zs.used_zones, zs.resettable_zones,
          (unsigned long)zbd_->GetReservedZones());
  fflush(f);
  fclose(f);
  rename(tmp.c_str(), path.c_str());  // atomic for the reader
}

// hyhostd contract: a control file `<aux>/.hyzns_resize` holding an ABSOLUTE
// target R-zone count. Compute the delta vs the current R, Grow or Shrink, then
// write `<aux>/.hyzns_resize.ack` = "<result_R> OK|EIO|BUSY" and remove the req.
// Returns true if a request was found and processed this poll.
bool ZoneArbiter::HandleResizeReq() {
  std::string base = config_.aux_path;
  if (!base.empty() && base.back() != '/') base += "/";
  std::string req = base + ".hyzns_resize";
  std::string ack = base + ".hyzns_resize.ack";

  FILE* f = fopen(req.c_str(), "r");
  if (!f) return false;
  unsigned target = 0;
  int k = fscanf(f, "%u", &target);
  fclose(f);
  unlink(req.c_str());
  if (k != 1 || target == 0) return true;  // malformed: consume, no-op

  uint64_t cur = GetCurrentRZoneCount();
  printf("[ZoneArbiter] hyhostd resize req: R %lu -> %u\n", cur, target);
  fflush(stdout);

  IOStatus s = IOStatus::OK();
  if (target > cur) {
    s = GrowCNS(static_cast<uint32_t>(target - cur));
  } else if (target < cur) {
    s = ShrinkCNS(static_cast<uint32_t>(cur - target));
  }
  last_adjustment_time_ = Env::Default()->NowMicros();

  uint64_t result_r = GetCurrentRZoneCount();
  const char* verdict = s.ok() ? "OK" : (s.IsBusy() ? "BUSY" : "EIO");
  printf("[ZoneArbiter] hyhostd resize %s -> R=%lu\n", verdict, result_r);
  fflush(stdout);

  FILE* af = fopen(ack.c_str(), "w");
  if (af) {
    fprintf(af, "%lu %s\n", (unsigned long)result_r, verdict);
    fflush(af);
    fclose(af);
  }
  return true;
}

IOStatus ZoneArbiter::TriggerCheck() {
  std::lock_guard<std::mutex> lock(monitor_mtx_);
  monitor_cv_.notify_one();
  return IOStatus::OK();
}

IOStatus ZoneArbiter::ForceGrowCNS(uint32_t count) {
  Info(logger_, "ZoneArbiter: Force expanding R-zone by %u zones", count);
  return GrowCNS(count);
}

uint64_t ZoneArbiter::GetCurrentRZoneCount() const {
  return zbd_->GetReservedZones();
}

IOStatus ZoneArbiter::GetF2FSStats(F2FSStats* stats) {
  return QueryF2FSStats(stats);
}

IOStatus ZoneArbiter::GetZenFSStats(ZenFSZoneStats* stats) {
  return QueryZenFSStats(stats);
}

void ZoneArbiter::SetConfig(const ZoneArbiterConfig& config) {
  config_ = config;
}

ZoneArbiterConfig ZoneArbiter::GetConfig() const { return config_; }

// ============================================================================
// F2FS Status Query
// ============================================================================

// Read dm-hyhost physical line state via `dmsetup status`. The aux F2FS is
// max-provisioned (its statvfs reports against the whole 256GB device, so its
// "utilization" is always ~92% and useless as an expand trigger). The faithful
// pressure signal for the host FTL is the R-region's free physical lines:
// dm-hyhost write-amplifies F2FS writes, so it can be physically exhausted
// (free_lines=0 -> nospc/EIO) while F2FS still thinks it has logical free space.
// dm name comes from ZENFS_DM_HYHOST_NAME (default "hyhost0").
bool ZoneArbiter::QueryDmHyhostLines(uint64_t* lines, uint64_t* free_lines,
                                     uint64_t* free_pages,
                                     uint64_t* zone_pblocks, uint64_t* nospc) {
  const char* nm = getenv("ZENFS_DM_HYHOST_NAME");
  std::string name = nm ? nm : "hyhost0";
  std::string cmd = "dmsetup status " + name + " 2>/dev/null";
  FILE* f = popen(cmd.c_str(), "r");
  if (!f) return false;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  pclose(f);
  if (n == 0) return false;
  buf[n] = '\0';
  std::string s(buf);
  auto grab = [&](const char* key, uint64_t* out) -> bool {
    if (!out) return false;
    size_t p = s.find(key);
    if (p == std::string::npos) return false;
    *out = strtoull(s.c_str() + p + strlen(key), nullptr, 10);
    return true;
  };
  bool ok = grab("lines=", lines) & grab("free_lines=", free_lines);
  grab("free_pages=", free_pages);
  grab("zone_pblocks=", zone_pblocks);
  uint64_t ns = 0;
  grab("nospc=", &ns);
  if (nospc) *nospc = ns;
  return ok && lines && *lines > 0;
}

IOStatus ZoneArbiter::QueryF2FSStats(F2FSStats* stats) {
  struct statvfs vfs;

  if (statvfs(config_.aux_path.c_str(), &vfs) != 0) {
    return IOStatus::IOError("statvfs failed: " + std::string(strerror(errno)));
  }

  stats->total_space = vfs.f_blocks * vfs.f_frsize;
  stats->free_space = vfs.f_bfree * vfs.f_frsize;
  stats->available_space = vfs.f_bavail * vfs.f_frsize;

  // Prefer the dm-hyhost physical free-capacity as the expand trigger; fall
  // back to (max-prov-skewed) statvfs only if dm status is unreadable.
  // A dm-hyhost line is 64 MiB but a zone is 1 GiB (=16 lines), so the raw
  // `free_lines` field is 16x the zone count. Convert real free blocks
  // (free_pages) to ZONES using the device's reported zone_pblocks (1 GiB / 4
  // KiB = 262144, also the fallback default). free_pages is the truest free
  // signal — immune to the free-line fragmentation pinning that keeps
  // free_lines low under in-place overwrite.
  uint64_t dev_lines = 0, free_lines = 0, free_pages = 0, zone_pblocks = 0,
           nospc = 0;
  if (QueryDmHyhostLines(&dev_lines, &free_lines, &free_pages, &zone_pblocks,
                         &nospc)) {
    uint64_t r0 = GetCurrentRZoneCount();
    uint64_t zpb = zone_pblocks ? zone_pblocks : 262144;  // pages/zone (1 GiB)
    uint64_t free_zones = free_pages / zpb;
    if (free_zones > r0) free_zones = r0;  // clamp to R (zones)
    // Trigger on ABSOLUTE free zones, not fill ratio. At large R a healthy
    // absolute free count still reads >50% "full" by ratio and drives runaway
    // grow (R explodes while ~50% of it is invalid/empty). Grow only when
    // genuinely few free zones remain, OR when dm-hyhost's nospc counter rises
    // (it requeues on no-space, so rising nospc is an early hard "grow now"
    // that races ahead of the EIO).
    bool grow_now = (config_.free_floor > 0 && free_zones <= config_.free_floor);
    if (nospc > last_nospc_) grow_now = true;
    last_nospc_ = nospc;
    double fill = grow_now ? 100.0 : 0.0;
    stats->utilization = fill;
    printf("[ZoneArbiter] dm-hyhost R: R0=%lu free_zones=%lu (free_lines=%lu) "
           "floor=%u grow=%d nospc=%lu\n",
           r0, free_zones, free_lines, config_.free_floor, (int)grow_now, nospc);
  } else if (stats->total_space > 0) {
    uint64_t used = stats->total_space - stats->free_space;
    stats->utilization = 100.0 * static_cast<double>(used) /
                         static_cast<double>(stats->total_space);
  } else {
    stats->utilization = 0.0;
  }

  return IOStatus::OK();
}

// ============================================================================
// ZenFS Status Query
// ============================================================================

IOStatus ZoneArbiter::QueryZenFSStats(ZenFSZoneStats* stats) {
  ZenFSSnapshot snapshot;
  ZenFSSnapshotOptions options;
  options.zbd_ = 1;
  options.zone_ = 1;

  zenfs_->GetZenFSSnapshot(snapshot, options);

  stats->total_space = 0;
  stats->used_space = 0;
  stats->free_space = 0;
  stats->reclaimable_space = snapshot.zbd_.reclaimable_space;
  stats->total_zones = static_cast<uint32_t>(snapshot.zones_.size());
  stats->used_zones = 0;
  stats->free_zones = 0;
  stats->resettable_zones = 0;

  for (const auto& zone : snapshot.zones_) {
    stats->total_space += zone.max_capacity;
    stats->used_space += zone.used_capacity;
    stats->free_space += zone.capacity;

    // R_Z semantics: remaining capacity EXCLUDING valid+invalid. A
    // written-but-0-valid (all-garbage) zone is NOT free until reset;
    // counting it as free over-reports R_Z while allocation is failing.
    if (zone.wp == zone.start) {
      stats->free_zones++;
    } else if (zone.used_capacity > 0) {
      stats->used_zones++;
    } else {
      stats->resettable_zones++;
    }
  }

  if (stats->total_space > 0) {
    stats->utilization = 100.0 * static_cast<double>(stats->used_space) /
                         static_cast<double>(stats->total_space);
  } else {
    stats->utilization = 0.0;
  }

  return IOStatus::OK();
}

// ============================================================================
// Decision Logic
// ============================================================================

ZoneArbiter::AdjustmentAction ZoneArbiter::DecideAction(
    const F2FSStats& f2fs_stats, const ZenFSZoneStats& zenfs_stats) {
  uint64_t current_rzone = GetCurrentRZoneCount();

  // Check if F2FS needs more space
  if (f2fs_stats.utilization >= config_.f2fs_expand_threshold) {
    // ZenFS must have enough free space to yield
    if (zenfs_stats.utilization < config_.zenfs_shrink_threshold) {
      // Check max limit
      if (config_.max_rzone_count == 0 ||
          current_rzone < config_.max_rzone_count) {
        Info(logger_,
             "ZoneArbiter: F2FS needs space (util=%.1f%%), "
             "ZenFS can yield (util=%.1f%%) -> GrowCNS",
             f2fs_stats.utilization, zenfs_stats.utilization);
        return AdjustmentAction::kGrowCNS;
      } else {
        Warn(logger_,
             "ZoneArbiter: F2FS needs space but max R-zone limit reached (%u)",
             config_.max_rzone_count);
      }
    } else {
      Warn(logger_,
           "ZoneArbiter: F2FS needs space (util=%.1f%%) but "
           "ZenFS cannot yield (util=%.1f%%)",
           f2fs_stats.utilization, zenfs_stats.utilization);
    }
  }

  return AdjustmentAction::kNone;
}

uint32_t ZoneArbiter::CalculateExpansionCount(
    const F2FSStats& f2fs_stats, const ZenFSZoneStats& zenfs_stats) {
  // Simple strategy: expand by 1-2 zones at a time
  // TODO: More sophisticated calculation based on F2FS free space need
  (void)zenfs_stats;  // Reserved for future use

  (void)f2fs_stats;
  // Incremental grow: add a small fixed step so R tracks the actual L0
  // footprint; a large per-event escalation over-shoots badly.
  return config_.grow_step ? config_.grow_step : 1;
}

// ============================================================================
// Zone Adjustment Execution
// ============================================================================

IOStatus ZoneArbiter::ExecuteAdjustment(AdjustmentAction action,
                                         uint32_t zone_count) {
  switch (action) {
    case AdjustmentAction::kGrowCNS:
      return GrowCNS(zone_count);
    default:
      return IOStatus::OK();
  }
}

IOStatus ZoneArbiter::GrowCNS(uint32_t count) {
  uint64_t current_rzone = GetCurrentRZoneCount();
  uint64_t new_rzone = current_rzone + count;

  uint32_t lo = static_cast<uint32_t>(current_rzone);
  uint32_t hi = static_cast<uint32_t>(new_rzone);
  Info(logger_, "ZoneArbiter: Expanding R-zone from %u to %u (online force-GC)",
       lo, hi);
  // GrowLAT: phase timestamps (us) for resize-latency breakdown. Fires only during
  // a resize -> zero steady-state cost. begin->migrated->reset->devaba->done.
  printf("[GrowLAT] begin lo=%u hi=%u movebytes=%lu ts=%lu\n", lo, hi,
         (unsigned long)zbd_->RangeUsedBytes(lo, hi),
         (unsigned long)Env::Default()->NowMicros()); fflush(stdout);

  // [GrowDump] ground truth of the absorb range at grow begin, so "what did
  // the grow actually move" is answerable from the log: per-zone fill vs live
  // vs invalid, and every live file in the range with its identity signals
  // (name: .log=WAL / .sst; level from the SetFileLevel hook, -1 if unset;
  // sparse=WAL write path; open=active writer holds the file).
  {
    uint64_t zsz = zbd_->GetZoneSize();
    uint64_t blo = (uint64_t)lo * zsz, bhi = (uint64_t)hi * zsz;
    ZenFSSnapshot gsnap;
    ZenFSSnapshotOptions gopts;
    gopts.zone_ = 1; gopts.zone_file_ = 1;
    zenfs_->GetZenFSSnapshot(gsnap, gopts);
    for (const auto& z : gsnap.zones_) {
      if (z.start >= blo && z.start < bhi) {
        double written = (double)(z.wp - z.start);
        printf("[GrowDump] zone %lu: written=%.1fMiB live=%.1fMiB invalid=%.1fMiB\n",
               (unsigned long)(z.start / zsz), written / 1048576.0,
               z.used_capacity / 1048576.0,
               (written - z.used_capacity) / 1048576.0);
      }
    }
    std::map<std::string, uint64_t> live_by_file;
    for (const auto& ext : gsnap.extents_)
      if (ext.zone_start >= blo && ext.zone_start < bhi)
        live_by_file[ext.filename] += ext.length;
    for (const auto& kv : live_by_file) {
      auto zf = zenfs_->GetFile(kv.first);
      printf("[GrowDump] file %s: %.1fMiB in-range level=%d sparse=%d open=%d\n",
             kv.first.c_str(), kv.second / 1048576.0,
             zf ? zf->GetLevel() : -9, zf ? (int)zf->IsSparse() : -9,
             zf ? (int)zf->IsOpenForWR() : -9);
    }
    fflush(stdout);
  }

  // Online in-process grow WITHOUT a global write pause (which deadlocks).
  // Step 1: EXCLUDE the absorb range from allocation -> no new writes land there.
  zbd_->SetAbsorbingRange(lo, hi, true);

  // Step 2: FORCE-GC the range to empty. MigrateExtents moves every closed file's
  // data above the new ABA; an actively-written file keeps its extent here until
  // it closes (seconds), but since the range is excluded nothing new arrives, so
  // it drains. Loop until empty (bounded), then proceed.
  IOStatus s;
  bool drained = false;
  // Drain-check poll granularity: fine-grained and env-tunable
  // (ZENFS_GROW_DRAIN_POLL_MS) so the measured migrate time reflects real
  // drain work rather than the poll period. Default budget ~30s.
  uint32_t poll_ms = 50;
  if (const char* e = getenv("ZENFS_GROW_DRAIN_POLL_MS")) poll_ms = (uint32_t)atoi(e);
  if (poll_ms == 0) poll_ms = 1;
  uint64_t budget_ms = 30000;
  if (const char* e = getenv("ZENFS_GROW_DRAIN_BUDGET_MS")) budget_ms = (uint64_t)atoll(e);
  uint64_t drain_deadline = Env::Default()->NowMicros() + budget_ms * 1000ULL;
  // [GrowDump] drain progression: a steady decline = extents being copied,
  // a plateau then a step = waiting on an open writer's seal/close.
  uint64_t drain_t0 = Env::Default()->NowMicros();
  uint64_t next_prog = drain_t0 + 500000;
  for (;;) {
    s = ResRec(lo, hi);
    if (!s.ok()) {
      Error(logger_, "ZoneArbiter: force-GC migrate failed: %s",
            s.ToString().c_str());
      zbd_->SetAbsorbingRange(lo, hi, false);
      return s;
    }
    if (zbd_->RangeAllEmpty(lo, hi)) { drained = true; break; }
    uint64_t nowus = Env::Default()->NowMicros();
    if (nowus >= next_prog) {
      printf("[GrowDump] drain remaining=%.1fMiB t=+%.2fs\n",
             zbd_->RangeUsedBytes(lo, hi) / 1048576.0,
             (nowus - drain_t0) / 1e6);
      fflush(stdout);
      next_prog = nowus + 500000;
    }
    if (nowus >= drain_deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
  }
  if (!drained) {
    // Name the blockers (files still holding extents in the absorb range)
    // so a wedged grow is diagnosable from the log.
    uint64_t zsz = zbd_->GetZoneSize();
    uint64_t blo = (uint64_t)lo * zsz, bhi = (uint64_t)hi * zsz;
    ZenFSSnapshot bsnap;
    ZenFSSnapshotOptions bopts;
    bopts.zone_ = 1; bopts.zone_file_ = 1;
    zenfs_->GetZenFSSnapshot(bsnap, bopts);
    int shown = 0;
    for (auto& ext : bsnap.extents_) {
      if (ext.zone_start >= blo && ext.zone_start < bhi && shown < 8) {
        printf("[ZoneArbiter] drain blocker: %s (extent %lu+%lu in zone %lu)\n",
               ext.filename.c_str(), (unsigned long)ext.start,
               (unsigned long)ext.length,
               (unsigned long)(ext.zone_start / zsz));
        shown++;
      }
    }
    printf("[ZoneArbiter] absorb [%u,%u) not drained: %lu bytes left (%d blockers shown)\n",
           lo, hi, (unsigned long)zbd_->RangeUsedBytes(lo, hi), shown);
    fflush(stdout);
    Warn(logger_, "ZoneArbiter: absorb range [%u,%u) not drained in time", lo, hi);
    zbd_->SetAbsorbingRange(lo, hi, false);
    return IOStatus::Busy("GrowCNS: absorb range not drained");
  }
  printf("[GrowLAT] migrated ts=%lu\n",
         (unsigned long)Env::Default()->NowMicros()); fflush(stdout);

  // Step 3: acquire+reset the now-empty zones (held -> stay excluded), so the
  // device sees them EMPTY for the S->R transition.
  s = zbd_->AcquireHoldFrontZones(lo, hi);
  if (!s.ok()) {
    Error(logger_, "ZoneArbiter: AcquireHoldFrontZones failed: %s",
          s.ToString().c_str());
    zbd_->SetAbsorbingRange(lo, hi, false);
    return s;
  }
  s = zbd_->ResetHeldFrontZones(lo, hi);
  if (!s.ok()) {
    Error(logger_, "ZoneArbiter: ResetHeldFrontZones failed: %s",
          s.ToString().c_str());
    return s;
  }
  printf("[GrowLAT] reset ts=%lu\n",
         (unsigned long)Env::Default()->NowMicros()); fflush(stdout);

  // Step 4: device ABA change (S-zones -> R-zones).
  s = SendResizeCNS(static_cast<uint32_t>(new_rzone));
  if (!s.ok()) {
    Error(logger_, "ZoneArbiter: F2FS modify_zone failed: %s",
          s.ToString().c_str());
    return s;
  }
  printf("[GrowLAT] devaba ts=%lu\n",
         (unsigned long)Env::Default()->NowMicros()); fflush(stdout);

  // Step 5: bump reserved count. The absorbed zones stay held (excluded from
  // allocation); a remount rebuilds io_zones cleanly from the new boundary.
  zbd_->SetReservedZones(new_rzone);

  Info(logger_, "ZoneArbiter: online R-zone expansion complete (now %lu zones)",
       new_rzone);
  printf("[GrowLAT] done ts=%lu\n",
         (unsigned long)Env::Default()->NowMicros()); fflush(stdout);
  return IOStatus::OK();
}

IOStatus ZoneArbiter::ForceShrinkCNS(uint32_t count) {
  Info(logger_, "ZoneArbiter: Force shrinking R-zone by %u zones", count);
  return ShrinkCNS(count);
}

IOStatus ZoneArbiter::ShrinkCNS(uint32_t count) {
  uint64_t current_rzone = GetCurrentRZoneCount();
  if (count == 0 || current_rzone < count + config_.min_rzone_count) {
    return IOStatus::InvalidArgument(
        "ShrinkCNS: would drop below min R-zone count");
  }
  uint64_t new_rzone = current_rzone - count;

  Info(logger_, "ZoneArbiter: Shrinking R-zone from %lu to %lu", current_rzone,
       new_rzone);
  // ShrinkLAT: phase timestamps (us) for resize-latency breakdown. The heavy work
  // (f2fs tail-drain + freeze + checkpoint + dm-hyhost R-tail force-GC + device ABA)
  // is inside SendResizeCNS (kernel) — decompose that with ftrace.
  uint64_t t_begin = Env::Default()->NowMicros();
  printf("[ShrinkLAT] begin cur=%lu new=%lu ts=%lu\n", current_rzone, new_rzone,
         (unsigned long)t_begin); fflush(stdout);

  // NOTE: shrink does NOT pause ZenFS writes. The old global PauseWrites() deadlocked
  // with the zone allocator: a writer past the pause gate (pending++) could block in
  // WaitForOpenIOZoneToken, and the only path to release a zone token is another Append,
  // which is itself blocked at the gate -> PauseWrites never reaches pending==0 (60s
  // timeout, shrink aborts). It is also unnecessary: the kernel F2FS drain (SendResizeCNS)
  // touches only the R/S boundary, not ZenFS's existing S zones, and ShrinkReservedZones
  // publishes the freed zones by append into a reserve()'d io_zones (no realloc, no element
  // shift), which is safe against concurrent allocation without a global pause.
  // Step 1: single MODIFY_ZONE ioctl. The kernel handler gates the aux F2FS usable area
  // DOWN (free_segment_range drain + MAIN_SECS gate + freeze_super + checkpoint) BEFORE
  // moving the device ABA, so the released tail zones are quiesced before ZenFS sees them.
  IOStatus s = SendResizeCNS(static_cast<uint32_t>(new_rzone));
  if (!s.ok()) {
    Error(logger_, "ZoneArbiter: F2FS modify_zone failed: %s",
          s.ToString().c_str());
    return s;
  }
  uint64_t t_ioctl = Env::Default()->NowMicros();
  printf("[ShrinkLAT] ioctl ts=%lu\n", (unsigned long)t_ioctl); fflush(stdout);

  // Step 2: ZenFS-side shrink: add the freed S-zones to the allocatable pool.
  s = zbd_->ShrinkReservedZones(count);
  if (!s.ok()) {
    Error(logger_, "ZoneArbiter: ShrinkReservedZones failed: %s",
          s.ToString().c_str());
    return s;
  }

  uint64_t t_done = Env::Default()->NowMicros();
  Info(logger_, "ZoneArbiter: R-zone shrink complete (now %lu zones)", new_rzone);
  printf("[ShrinkLAT] done ts=%lu\n", (unsigned long)t_done); fflush(stdout);
  // ZenFS-side accounting: total = SendResizeCNS (the kernel F2FS_IOC_RESIZE_CNS
  // ioctl == the kernel ===[ResizeCNS] resize_total) + ShrinkReservedZones (ZenFS
  // resets the freed R-tail zones and publishes them into io_zones). These two add
  // up to the whole ZenFS ShrinkCNS; the kernel side breaks the first term down.
  printf("[ShrinkCNS-SUM] total=%lu us = SendResizeCNS(kernel)=%lu us + ShrinkReservedZones=%lu us\n",
         (unsigned long)(t_done - t_begin),
         (unsigned long)(t_ioctl - t_begin),
         (unsigned long)(t_done - t_ioctl)); fflush(stdout);
  return s;
}

IOStatus ZoneArbiter::SendF2FSGcForce(uint32_t start_zone, uint32_t end_zone) {
  if (end_zone <= start_zone)
    return IOStatus::InvalidArgument("SendF2FSGcForce: empty range");
  int fd = OpenAuxPathFile();
  if (fd < 0)
    return IOStatus::IOError("ZoneArbiter: open aux for gc_force failed");

  struct f2fs_gc_range range;
  uint32_t num_zones = end_zone - start_zone;
  range.sync = 1;
  range.start = static_cast<__u64>(start_zone) * F2FS_ZONE_BLOCKS;
  range.len = static_cast<__u64>(num_zones - 1) * F2FS_ZONE_BLOCKS;
  range.force = 1;

  int ret = ioctl(fd, F2FS_IOC_GARBAGE_COLLECT_FORCE, &range);
  int saved_errno = errno;
  close(fd);
  if (ret < 0)
    return IOStatus::IOError("ZoneArbiter: gc_force ioctl failed: " +
                             std::string(strerror(saved_errno)));

  Info(logger_, "ZoneArbiter: gc_force zones [%u, %u) done", start_zone,
       end_zone);
  return IOStatus::OK();
}

// ============================================================================
// Write Pause Control
// ============================================================================

IOStatus ZoneArbiter::PauseWrites() {
  Info(logger_, "ZoneArbiter: Pausing writes...");

  write_paused_.store(true);

  // Wait for pending writes to complete
  uint64_t start_time = Env::Default()->NowMicros();
  uint64_t timeout = config_.write_pause_timeout_us;

  while (pending_writers_.load() > 0) {
    if (Env::Default()->NowMicros() - start_time > timeout) {
      write_paused_.store(false);
      return IOStatus::TimedOut(
          "ZoneArbiter: Write pause timeout - pending writers: " +
          std::to_string(pending_writers_.load()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  Info(logger_, "ZoneArbiter: Writes paused successfully");
  return IOStatus::OK();
}

IOStatus ZoneArbiter::ResumeWrites() {
  Info(logger_, "ZoneArbiter: Resuming writes...");

  write_paused_.store(false);

  // Notify all waiting writers
  {
    std::lock_guard<std::mutex> lock(write_pause_mtx_);
    write_pause_cv_.notify_all();
  }

  return IOStatus::OK();
}

void ZoneArbiter::WaitIfPaused() {
  if (!write_paused_.load()) {
    return;
  }

  std::unique_lock<std::mutex> lock(write_pause_mtx_);
  write_pause_cv_.wait(lock, [this] {
    return !write_paused_.load() || stop_requested_.load();
  });
}

void ZoneArbiter::IncrementPendingWriters() {
  pending_writers_.fetch_add(1, std::memory_order_relaxed);
}

void ZoneArbiter::DecrementPendingWriters() {
  pending_writers_.fetch_sub(1, std::memory_order_relaxed);
}

// ============================================================================
// F2FS ioctl Communication
// ============================================================================

int ZoneArbiter::OpenAuxPathFile() {
  // Try to open/create a temporary file for ioctl
  std::string file_path = config_.aux_path;
  if (file_path.back() != '/') {
    file_path += "/";
  }
  file_path += ".zenfs_arbiter";

  int fd = open(file_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    // Try opening an existing file in aux_path
    std::string alt_path = config_.aux_path;
    fd = open(alt_path.c_str(), O_RDONLY);
  }

  return fd;
}

IOStatus ZoneArbiter::SendResizeCNS(uint32_t new_rzone) {
  int fd = OpenAuxPathFile();
  if (fd < 0) {
    return IOStatus::IOError(
        "ZoneArbiter: Failed to open aux_path file for ioctl: " +
        std::string(strerror(errno)));
  }

  __u32 rzone = static_cast<__u32>(new_rzone);
  int ret = ioctl(fd, F2FS_IOC_RESIZE_CNS, &rzone);
  int saved_errno = errno;
  close(fd);

  if (ret < 0) {
    return IOStatus::IOError("ZoneArbiter: F2FS modify_zone ioctl failed: " +
                             std::string(strerror(saved_errno)));
  }

  Info(logger_, "ZoneArbiter: F2FS modify_zone ioctl succeeded (new_rzone=%u)",
       new_rzone);

  return IOStatus::OK();
}

// ============================================================================
// ZenFS Migration
// ============================================================================

IOStatus ZoneArbiter::ResRec(uint32_t start_zone_id,
                                         uint32_t end_zone_id) {
  uint64_t zone_size = zbd_->GetZoneSize();

  Info(logger_, "ZoneArbiter: Migrating zones [%u, %u)", start_zone_id,
       end_zone_id);

  // Get ZenFS snapshot to find extents in target zones
  ZenFSSnapshot snapshot;
  ZenFSSnapshotOptions options;
  options.zone_ = 1;
  options.zone_file_ = 1;

  zenfs_->GetZenFSSnapshot(snapshot, options);

  // Identify zones that need migration
  std::set<uint64_t> migrate_zones_start;
  for (uint32_t zone_id = start_zone_id; zone_id < end_zone_id; zone_id++) {
    uint64_t zone_start = static_cast<uint64_t>(zone_id) * zone_size;
    migrate_zones_start.insert(zone_start);
  }

  // Collect extents that need migration
  std::vector<ZoneExtentSnapshot*> migrate_exts;
  for (auto& ext : snapshot.extents_) {
    if (migrate_zones_start.find(ext.zone_start) != migrate_zones_start.end()) {
      migrate_exts.push_back(&ext);
    }
  }

  if (migrate_exts.empty()) {
    Info(logger_, "ZoneArbiter: No extents to migrate in zones [%u, %u)",
         start_zone_id, end_zone_id);
    return IOStatus::OK();
  }

  Info(logger_, "ZoneArbiter: Found %lu extents to migrate",
       migrate_exts.size());

  // Call ZenFS MigrateExtents
  uint64_t copied_data = 0;
  // Keep migrated data above the new ABA so the absorbed zones end up EMPTY.
  IOStatus s = zenfs_->MigrateExtents(migrate_exts, &copied_data,
                                      /*sst_only=*/false,
                                      (uint64_t)end_zone_id * zone_size);

  if (s.ok()) {
    Info(logger_, "ZoneArbiter: Migration complete - copied %lu bytes",
         copied_data);
  }

  return s;
}

}  // namespace ROCKSDB_NAMESPACE
