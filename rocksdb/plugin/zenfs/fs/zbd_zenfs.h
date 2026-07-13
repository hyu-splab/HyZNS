// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <cstdint>
#if !defined(ROCKSDB_LITE) && defined(OS_LINUX)

#include <errno.h>
#include <libzbd/zbd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "metrics.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/io_status.h"

// #include "io_zenfs.h"

#define CONV_SPACE_BYTES (3221225472) //CNS SSD size : 4G - 4294967296

namespace ROCKSDB_NAMESPACE {

class ZonedBlockDevice;
class ZonedBlockDeviceBackend;
class ZoneSnapshot;
class ZenFSSnapshotOptions;
class ZoneArbiter;

class ZoneList {
 private:
  void *data_;
  // unsigned int zone_count_;

 public:
  unsigned int zone_count_;
  ZoneList(void *data, unsigned int zone_count)
      : data_(data), zone_count_(zone_count){};
  void *GetData() { return data_; };
  unsigned int ZoneCount() { return zone_count_; };
  ~ZoneList() { free(data_); };
};

class Zone {
  ZonedBlockDevice *zbd_;
  ZonedBlockDeviceBackend *zbd_be_;
  std::atomic_bool busy_;
  // Excluded from allocation while the CNS area is absorbing this zone (online
  // grow): the allocator/GC skip it so no new writes land here, letting the
  // force-GC drain it to empty even if it was the active write frontier.
  std::atomic_bool absorbing_{false};
  Env *env_;

 public:
  bool IsAbsorbing() const { return absorbing_.load(std::memory_order_relaxed); }
  void SetAbsorbing(bool v) { absorbing_.store(v, std::memory_order_relaxed); }
  explicit Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
                std::unique_ptr<ZoneList> &zones, unsigned int idx);

  uint64_t start_;
  uint64_t capacity_; /* remaining capacity */
  uint64_t max_capacity_;
  uint64_t wp_;
  Env::WriteLifeTimeHint lifetime_;
  Env::WriteLifeTimeHint min_lifetime_;
  Env::WriteLifeTimeHint max_lifetime_;
  std::atomic<uint64_t> used_capacity_;

  IOStatus Reset();
  IOStatus Finish();
  IOStatus Close();
  uint64_t Utilization();

  IOStatus Append(char *data, uint32_t size);
  bool IsUsed();
  bool IsFull();
  bool IsEmpty();
  uint64_t GetZoneNr();
  uint64_t GetCapacityLeft();
  bool IsBusy() { return this->busy_.load(std::memory_order_relaxed); }
  bool Acquire() {
    bool expected = false;
    return this->busy_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel);
  }
  bool Release() {
    bool expected = true;
    return this->busy_.compare_exchange_strong(expected, false,
                                               std::memory_order_acq_rel);
  }

  void EncodeJson(std::ostream &json_stream);

  inline IOStatus CheckRelease();
};

class ZonedBlockDeviceBackend {
 public:
  uint32_t block_sz_ = 0;
  uint64_t zone_sz_ = 0;
  uint32_t nr_zones_ = 0;
  uint64_t reserved_zone_count_ = 0;

 public:
  virtual IOStatus Open(bool readonly, bool exclusive,
                        unsigned int *max_active_zones,
                        unsigned int *max_open_zones, bool allow_shared = false) = 0;

  virtual std::unique_ptr<ZoneList> ListZones() = 0;
  virtual IOStatus Reset(uint64_t start, bool *offline,
                         uint64_t *max_capacity) = 0;
  virtual IOStatus Finish(uint64_t start) = 0;
  virtual IOStatus Close(uint64_t start) = 0;
  virtual int Read(char *buf, int size, uint64_t pos, bool direct) = 0;
  virtual int Write(char *data, uint32_t size, uint64_t pos) = 0;
  virtual int ConvRead(char *buf, int size, uint64_t pos, bool direct) = 0;
  virtual int ConvWrite(char *data, uint32_t size, uint64_t pos) = 0;
  virtual int InvalidateCache(uint64_t pos, uint64_t size) = 0;
  virtual bool ZoneIsSwr(std::unique_ptr<ZoneList> &zones,
                         unsigned int idx) = 0;
  virtual bool ZoneIsOffline(std::unique_ptr<ZoneList> &zones,
                             unsigned int idx) = 0;
  virtual bool ZoneIsWritable(std::unique_ptr<ZoneList> &zones,
                              unsigned int idx) = 0;
  virtual bool ZoneIsActive(std::unique_ptr<ZoneList> &zones,
                            unsigned int idx) = 0;
  virtual bool ZoneIsOpen(std::unique_ptr<ZoneList> &zones,
                          unsigned int idx) = 0;
  virtual uint64_t ZoneStart(std::unique_ptr<ZoneList> &zones,
                             unsigned int idx) = 0;
  virtual uint64_t ZoneMaxCapacity(std::unique_ptr<ZoneList> &zones,
                                   unsigned int idx) = 0;
  virtual uint64_t ZoneWp(std::unique_ptr<ZoneList> &zones,
                          unsigned int idx) = 0;
  virtual std::string GetFilename() = 0;
  uint32_t GetBlockSize() { return block_sz_; };
  uint64_t GetZoneSize() { return zone_sz_; };
  uint32_t GetNrZones() { return nr_zones_; };
  uint64_t GetReservedZoneCount() { return reserved_zone_count_; };
  virtual ~ZonedBlockDeviceBackend(){};
};

enum class ZbdBackendType {
  kBlockDev,
  kZoneFS,
};

class ZonedBlockDevice {
public:
  std::atomic<long> active_io_zones_;
  std::atomic<long> open_io_zones_;
 private:
  // std::unique_ptr<ZonedBlockDeviceBackend> zbd_be_;
  // std::vector<Zone *> io_zones;
  std::vector<Zone *> meta_zones;
  // time_t start_time_;
  std::shared_ptr<Logger> logger_;
  uint32_t finish_threshold_ = 0;
  std::atomic<uint64_t> bytes_written_{0};
  std::atomic<uint64_t> gc_bytes_written_{0};

  /* Protects zone_resuorces_  condition variable, used
     for notifying changes in open_io_zones_ */
  std::mutex zone_resources_mtx_;
  std::condition_variable zone_resources_;
  std::mutex zone_deferred_status_mutex_;
  IOStatus zone_deferred_status_;

  std::condition_variable migrate_resource_;
  std::mutex migrate_zone_mtx_;
  std::atomic<bool> migrating_{false};

  unsigned int max_nr_active_io_zones_;
  unsigned int max_nr_open_io_zones_;
  uint64_t reserved_zone_count_ {0};

  std::shared_ptr<ZenFSMetrics> metrics_;

  void EncodeJsonZone(std::ostream &json_stream,
                      const std::vector<Zone *> zones);

 public:
  std::unique_ptr<ZonedBlockDeviceBackend> zbd_be_;
  explicit ZonedBlockDevice(std::string path, ZbdBackendType backend,
                            std::shared_ptr<Logger> logger,
                            std::shared_ptr<ZenFSMetrics> metrics =
                                std::make_shared<NoZenFSMetrics>());
  virtual ~ZonedBlockDevice();

  IOStatus Open(bool readonly, bool exclusive, bool allow_shared = false,
                int start_zone = -1, int end_zone = -1, 
                int ao_zones = -1, uint64_t cns_start = -1, uint64_t cns_len = -1);

  Zone *GetIOZone(uint64_t offset);

  IOStatus AllocateIOZone(Env::WriteLifeTimeHint file_lifetime, IOType io_type,
                          Zone **out_zone, bool from_end = false);
  IOStatus AllocateMetaZone(Zone **out_meta_zone);

  bool do_workload {false};

  uint64_t GetFreeSpace();
  uint64_t GetUsedSpace();
  uint64_t GetReclaimableSpace();

  std::string GetFilename();
  uint32_t GetBlockSize();

  IOStatus ResetUnusedIOZones();
  void LogZoneStats();
  void LogZoneUsage();
  void LogGarbageInfo();

  uint64_t GetZoneSize();
  uint32_t GetNrZones();
  std::vector<Zone *> GetMetaZones() { return meta_zones; }

  void SetFinishTreshold(uint32_t threshold) { finish_threshold_ = threshold; }

  void PutOpenIOZoneToken();
  void PutActiveIOZoneToken();

  void EncodeJson(std::ostream &json_stream);

  void SetZoneDeferredStatus(IOStatus status);

  std::shared_ptr<ZenFSMetrics> GetMetrics() { return metrics_; }

  void GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot);

  int Read(char *buf, uint64_t offset, int n, bool direct);
  IOStatus InvalidateCache(uint64_t pos, uint64_t size);

  IOStatus ReleaseMigrateZone(Zone *zone);

  // min_start (bytes): restrict the destination zone to start_ >= min_start.
  // Used by CNS grow so migrated data never lands back in the absorb range.
  IOStatus TakeMigrateZone(Zone **out_zone, Env::WriteLifeTimeHint lifetime,
                           uint32_t min_capacity, uint64_t min_start = 0);

  void AddBytesWritten(uint64_t written) { bytes_written_ += written; };
  void AddGCBytesWritten(uint64_t written) { gc_bytes_written_ += written; };
  uint64_t GetUserBytesWritten() {
    return bytes_written_.load() - gc_bytes_written_.load();
  };
  uint64_t GetTotalBytesWritten() { return bytes_written_.load(); };

  std::atomic<uint32_t> num_finish_{0};
  std::atomic<uint32_t> num_finish_called_{0};

  std::atomic<uint64_t> wal_writes_num {0};
  std::atomic<uint64_t> wal_writes_time {0};
  std::atomic<uint64_t> sst_writes_num {0};
  std::atomic<uint64_t> sst_writes_time {0};

  uint64_t start_time_;
  uint64_t prev_time_;
  Env *env_;

  std::atomic<uint32_t> num_concurrent_alloc_{0};
  
  std::vector<Zone*> io_zones;
  std::atomic<uint64_t> allocate_io_zones {0};

  std::atomic<uint32_t> diff_cnt_[200]{0};
  std::atomic<int32_t> zone_diff_[6]{0};
  std::atomic<uint64_t> alloc_empty_{0};
  std::atomic<uint64_t> zone_lifetimes_[6]{0};

  std::atomic<uint64_t> total_reset_zones_{0};
  std::atomic<uint64_t> total_reset_calls_{0};
  std::atomic<uint64_t> reset_util_sum_{0};
  std::atomic<uint64_t> reset_util_max_{0};

  // conventional area for WAL
  uint64_t cns_start_;
  uint64_t cns_len_;
  uint64_t cns_offset_;
  // std::atomic<uint64_t> cns_offset_;

  int l0_aux {0};
  
  std::string aux_fs_path;
  void SetReservedZones(uint64_t count) { reserved_zone_count_ = count; }
  uint64_t GetReservedZones() const { return reserved_zone_count_; }
  // ZoneArbiter (auto-modify) hook: write path quiesces via this pointer.
  void SetZoneArbiter(ZoneArbiter* arbiter) { zone_arbiter_ = arbiter; }
  ZoneArbiter* GetZoneArbiter() { return zone_arbiter_; }
  // Online CNS-area resize (ZoneArbiter / ModifyZone). These adjust the ZenFS
  // allocatable IO-zone list to match a device ABA change.
  IOStatus ResetFrontIOZones(uint32_t n);    // reset first n IO zones (pre-grow)
  IOStatus GrowReservedZones(uint32_t n);    // drop first n IO zones, reserved += n
  IOStatus ShrinkReservedZones(uint32_t n);  // add n IO zones at front, reserved -= n
  // Online (in-process) CNS grow without a global write pause: hold the absorb
  // zones via per-zone Acquire (allocation auto-skips held zones), reset them,
  // then the caller issues ModifyZone + bumps reserved. [old_rz,new_rz) = zones.
  IOStatus AcquireHoldFrontZones(uint32_t old_rz, uint32_t new_rz);
  IOStatus ResetHeldFrontZones(uint32_t old_rz, uint32_t new_rz);
  // Mark/unmark the absorb-range zones as excluded from allocation, and test
  // whether they have been fully drained (used_capacity == 0).
  void SetAbsorbingRange(uint32_t old_rz, uint32_t new_rz, bool v);
  bool RangeAllEmpty(uint32_t old_rz, uint32_t new_rz);
  // valid (live) bytes currently in the absorb range — the data GrowCNS must migrate.
  uint64_t RangeUsedBytes(uint32_t old_rz, uint32_t new_rz);
 private:
  ZoneArbiter* zone_arbiter_ = nullptr;
  IOStatus GetZoneDeferredStatus();
  bool GetActiveIOZoneTokenIfAvailable();
  void WaitForOpenIOZoneToken(bool prioritized);
  IOStatus ApplyFinishThreshold();
  IOStatus FinishCheapestIOZone();
  IOStatus GetBestOpenZoneMatch(Env::WriteLifeTimeHint file_lifetime,
                                unsigned int *best_diff_out, Zone **zone_out,
                                uint32_t min_capacity = 0,
                                uint64_t min_start = 0);
  IOStatus AllocateEmptyZone(Zone **zone_out, uint64_t min_start = 0,
                             bool from_end = false);
};

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX)
