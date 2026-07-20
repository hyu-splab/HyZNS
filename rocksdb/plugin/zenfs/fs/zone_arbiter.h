// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rocksdb/io_status.h"

namespace ROCKSDB_NAMESPACE {

class ZenFS;
class ZonedBlockDevice;
class Logger;

// F2FS space statistics
struct F2FSStats {
  uint64_t total_space;      // bytes
  uint64_t free_space;       // bytes
  uint64_t available_space;  // bytes
  double utilization;        // percentage (0-100)
};

// ZenFS zone statistics
struct ZenFSZoneStats {
  uint64_t total_space;       // bytes
  uint64_t used_space;        // bytes
  uint64_t free_space;        // bytes
  uint64_t reclaimable_space; // bytes
  uint32_t total_zones;
  uint32_t used_zones;
  uint32_t free_zones;        // truly EMPTY (wp==start): allocatable NOW
  uint32_t resettable_zones;  // written but 0 valid (all-garbage): free after a reset
  double utilization;         // percentage (0-100)
};

// ZoneArbiter configuration
struct ZoneArbiterConfig {
  // Monitoring interval (microseconds)
  uint64_t check_interval_us = 5 * 1000 * 1000;  // 5 seconds

  // F2FS expansion threshold (R-zone expand when F2FS utilization exceeds this)
  double f2fs_expand_threshold = 85.0;  // 85%

  // ZenFS shrink threshold (R-zone can expand when ZenFS utilization is below this)
  double zenfs_shrink_threshold = 30.0;  // 30%

  // Maximum zones to adjust per operation
  uint32_t max_zones_per_adjustment = 2;

  // Cooldown after adjustment (microseconds)
  uint64_t adjustment_cooldown_us = 30 * 1000 * 1000;  // 30 seconds

  // aux_path (F2FS mount point)
  std::string aux_path;

  // Minimum R-zone count (never shrink below this)
  uint32_t min_rzone_count = 4;

  // Maximum R-zone count (0 = no limit) — absolute hard cap on R growth.
  uint32_t max_rzone_count = 0;

  // Grow trigger: expand only when ABSOLUTE free ZONES drop to/below this floor
  // (replaces the fill-ratio trigger, which over-grows at large R). 0 disables.
  // In zones (1 GiB) — a dm-hyzns line is 64 MiB, so the trigger converts
  // free_pages -> free_zones rather than counting raw free_lines.
  uint32_t free_floor = 2;

  // Zones to add per grow event (incremental; avoids 32-zone over-shoot).
  uint32_t grow_step = 1;

  // Write pause timeout (microseconds)
  uint64_t write_pause_timeout_us = 60 * 1000 * 1000;  // 60 seconds

  // External-driver mode (hyznsd): the in-process threshold decider is gated
  // OFF; the arbiter only publishes status (<aux>/.hyzns_status) and executes
  // absolute-target resize requests (<aux>/.hyzns_resize). The daemon owns the
  // policy. Manual .zenfs_grow/.zenfs_shrink triggers still work.
  bool external = false;

  // Publish <aux>/.hyzns_status every poll (R_Z source for the daemon's fs path).
  bool publish_status = true;
};

class ZoneArbiter {
 public:
  ZoneArbiter(ZenFS* zenfs, ZonedBlockDevice* zbd,
              std::shared_ptr<Logger> logger, const ZoneArbiterConfig& config);
  ~ZoneArbiter();

  // Lifecycle
  IOStatus Start();
  void Stop();
  bool IsRunning() const { return running_.load(); }

  // Manual trigger (for testing/debugging)
  IOStatus TriggerCheck();
  IOStatus ForceGrowCNS(uint32_t count);
  IOStatus ForceShrinkCNS(uint32_t count);

  // Status queries
  IOStatus GetF2FSStats(F2FSStats* stats);
  IOStatus GetZenFSStats(ZenFSZoneStats* stats);
  uint64_t GetCurrentRZoneCount() const;

  // Write pause interface (called by ZenFS write path)
  void WaitIfPaused();
  void IncrementPendingWriters();
  void DecrementPendingWriters();
  bool IsWritePaused() const { return write_paused_.load(); }

  // Configuration
  void SetConfig(const ZoneArbiterConfig& config);
  ZoneArbiterConfig GetConfig() const;

 private:
  // Monitor thread main loop
  void MonitorLoop();

  // hyznsd contract: publish <aux>/.hyzns_status (R_Z = free_zones) and execute
  // an absolute-target <aux>/.hyzns_resize request, acking in .hyzns_resize.ack.
  void PublishStatus();
  bool HandleResizeReq();   // returns true if a request was processed this poll

  // F2FS status query
  IOStatus QueryF2FSStats(F2FSStats* stats);

  // dm-hyzns R-region physical state (via `dmsetup status`). Returns true and
  // fills the requested non-null out-params (lines/free_lines/free_pages/
  // zone_pblocks/nospc). free_pages+zone_pblocks give free capacity in ZONES,
  // the correct grow signal (free_lines is in 64-MiB lines).
  bool QueryDmHyznsLines(uint64_t* lines, uint64_t* free_lines,
                          uint64_t* free_pages, uint64_t* zone_pblocks,
                          uint64_t* nospc);

  // ZenFS status query
  IOStatus QueryZenFSStats(ZenFSZoneStats* stats);

  // Decision logic
  enum class AdjustmentAction {
    kNone,
    kGrowCNS,   // R-zone increase (S-zone -> R-zone)
  };
  AdjustmentAction DecideAction(const F2FSStats& f2fs_stats,
                                 const ZenFSZoneStats& zenfs_stats);
  uint32_t CalculateExpansionCount(const F2FSStats& f2fs_stats,
                                    const ZenFSZoneStats& zenfs_stats);

  // Zone adjustment execution
  IOStatus ExecuteAdjustment(AdjustmentAction action, uint32_t zone_count);
  IOStatus GrowCNS(uint32_t count);
  IOStatus ShrinkCNS(uint32_t count);

  // Write pause control
  IOStatus PauseWrites();
  IOStatus ResumeWrites();

  // F2FS ioctl calls
  IOStatus SendResizeCNS(uint32_t new_rzone);
  IOStatus SendF2FSGcForce(uint32_t start_zone, uint32_t end_zone);
  int OpenAuxPathFile();

  // ZenFS migration (reuse existing logic)
  IOStatus ResRec(uint32_t start_zone_id, uint32_t end_zone_id);

 private:
  ZenFS* zenfs_;
  ZonedBlockDevice* zbd_;
  std::shared_ptr<Logger> logger_;
  ZoneArbiterConfig config_;

  // Monitor thread
  std::unique_ptr<std::thread> monitor_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};

  // Condition variable for wakeup
  std::mutex monitor_mtx_;
  std::condition_variable monitor_cv_;

  // Write pause state
  std::atomic<bool> write_paused_{false};
  std::mutex write_pause_mtx_;
  std::condition_variable write_pause_cv_;
  std::atomic<uint32_t> pending_writers_{0};

  // Cooldown tracking
  uint64_t last_adjustment_time_{0};

  // dm-hyzns no-space counter from the previous check (rising == grow now)
  uint64_t last_nospc_{0};
};

}  // namespace ROCKSDB_NAMESPACE
