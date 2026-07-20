// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "zbd_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "snapshot.h"
#include "zbdlib_zenfs.h"
#include "zonefs_zenfs.h"

#define KB (1024)
#define MB (1024 * KB)

/* Number of reserved zones for metadata
 * Two non-offline meta zones are needed to be able
 * to roll the metadata log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_META_ZONES (3)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
           std::unique_ptr<ZoneList> &zones, unsigned int idx)
    : zbd_(zbd),
      zbd_be_(zbd_be),
      busy_(false),
      start_(zbd_be->ZoneStart(zones, idx)),
      max_capacity_(zbd_be->ZoneMaxCapacity(zones, idx)),
      wp_(zbd_be->ZoneWp(zones, idx)) {
  lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  if (zbd_be->ZoneIsWritable(zones, idx))
    capacity_ = max_capacity_ - (wp_ - start_);
}

bool Zone::IsUsed() { return (used_capacity_ > 0); }
uint64_t Zone::GetCapacityLeft() { return capacity_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }

void Zone::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"capacity\":" << capacity_ << ",";
  json_stream << "\"max_capacity\":" << max_capacity_ << ",";
  json_stream << "\"wp\":" << wp_ << ",";
  json_stream << "\"lifetime\":" << lifetime_ << ",";
  json_stream << "\"used_capacity\":" << used_capacity_;
  json_stream << "}";
}

uint64_t Zone::Utilization() {
  if (IsFull()) {
    return 100;
  }
  return ((double)(wp_ - start_) / max_capacity_) * 100;
}

IOStatus Zone::Reset() {
  bool offline;
  uint64_t max_capacity;

  assert(!IsUsed());
  assert(IsBusy());

  IOStatus ios = zbd_be_->Reset(start_, &offline, &max_capacity);
  if (ios != IOStatus::OK()) return ios;

  if (offline)
    capacity_ = 0;
  else
    max_capacity_ = capacity_ = max_capacity;

  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;
  min_lifetime_ = Env::WLTH_NOT_SET;
  max_lifetime_ = Env::WLTH_NOT_SET;

  return IOStatus::OK();
}

IOStatus Zone::Finish() {
  assert(IsBusy());

  IOStatus ios = zbd_be_->Finish(start_);
  if (ios != IOStatus::OK()) return ios;

  capacity_ = 0;
  wp_ = start_ + zbd_->GetZoneSize();

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  assert(IsBusy());

  if (!(IsEmpty() || IsFull())) {
    IOStatus ios = zbd_be_->Close(start_);
    if (ios != IOStatus::OK()) return ios;
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_ZONE_WRITE_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportThroughput(ZENFS_ZONE_WRITE_THROUGHPUT, size);
  char *ptr = data;
  uint32_t left = size;
  int ret;

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  // At this point size is the final amount ZenFS issues to the device
  // (already aligned to the block size).
  // fprintf(stderr, "Zone::Append zone=%lu size=%u (capacity_left=%lu) \n",
  //                GetZoneNr(), size, capacity_);

  while (left) {
    ret = zbd_be_->Write(ptr, left, wp_);
    if (ret < 0) {
      return IOStatus::IOError(strerror(errno));
    }

    ptr += ret;
    wp_ += ret;
    capacity_ -= ret;
    left -= ret;
    zbd_->AddBytesWritten(ret);
  }

  return IOStatus::OK();
}

inline IOStatus Zone::CheckRelease() {
  if (!Release()) {
    assert(false);
    return IOStatus::Corruption("Failed to unset busy flag of zone " +
                                std::to_string(GetZoneNr()));
  }

  return IOStatus::OK();
}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones)
    if (z->start_ <= offset && offset < (z->start_ + zbd_be_->GetZoneSize()))
      return z;
  return nullptr;
}

ZonedBlockDevice::ZonedBlockDevice(std::string path, ZbdBackendType backend,
                                   std::shared_ptr<Logger> logger,
                                   std::shared_ptr<ZenFSMetrics> metrics)
    : logger_(logger), metrics_(metrics) {
  if (backend == ZbdBackendType::kBlockDev) {
    zbd_be_ = std::unique_ptr<ZbdlibBackend>(new ZbdlibBackend(path));
    Info(logger_, "New Zoned Block Device: %s", zbd_be_->GetFilename().c_str());
  } else if (backend == ZbdBackendType::kZoneFS) {
    zbd_be_ = std::unique_ptr<ZoneFsBackend>(new ZoneFsBackend(path));
    Info(logger_, "New zonefs backing: %s", zbd_be_->GetFilename().c_str());
  }

  fprintf(stdout, "ZonedBlockDevice\n");
  env_ = Env::Default();
  start_time_ = env_->NowMicros();
  prev_time_ = start_time_;
}

IOStatus ZonedBlockDevice::Open(bool readonly, bool exclusive, bool allow_shared, int start_zone, int end_zone, 
                                int ao_zones, uint64_t cns_start, uint64_t cns_len) {
  std::unique_ptr<ZoneList> zone_rep;
  unsigned int max_nr_active_zones;
  unsigned int max_nr_open_zones;
  Status s;
  int i = 0;
  uint64_t m = 0;
  // Reserve one zone for metadata and another one for extent migration
  int reserved_zones = 2;

  // if (!allow_shared && !readonly && !exclusive)
  //   return IOStatus::InvalidArgument("Write opens must be exclusive");

  if (allow_shared) {
    Warn(logger_, "HYSSD mode: Opening device in shared mode");
  }

  IOStatus ios = zbd_be_->Open(readonly, exclusive, &max_nr_active_zones,
                               &max_nr_open_zones, allow_shared);
  if (ios != IOStatus::OK()) return ios;

  if (zbd_be_->GetNrZones() < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported("To few zones on zoned backend (" +
                                  std::to_string(ZENFS_MIN_ZONES) +
                                  " required)");
  }

  // conventional area
  cns_start_ = cns_start;
  cns_len_ = cns_len;
  cns_offset_ = cns_start_;

  // If not already set by SetReservedZones(), get from backend
  // Device is the single source of truth for reserved zones
  fprintf(stdout, "BEFORE auto-detect: reserved_zone_count_=%lu, allow_shared=%d\n",
          reserved_zone_count_, allow_shared);
  if (reserved_zone_count_ == 0) {
    uint64_t detected = zbd_be_->GetReservedZoneCount();
    // On a pure ZNS device (no hyzns ABA) the vendor report degenerates to
    // the whole device; a real hyzns R-region is always < nr_zones. Treat
    // the degenerate value as "no reserved area" instead of failing Open.
    if (detected >= zbd_be_->GetNrZones()) {
      fprintf(stdout,
              "Reserved-zone report covers the whole device (%lu) — "
              "pure ZNS backend, using 0 reserved zones\n", detected);
      detected = 0;
    }
    reserved_zone_count_ = detected;
    fprintf(stdout, "Auto-detecting reserved zones: %lu (from backend: %lu)\n",
            reserved_zone_count_, zbd_be_->GetReservedZoneCount());
    if (reserved_zone_count_ > 0) {
      Info(logger_, "Auto-detected %lu reserved zones from device\n",
           reserved_zone_count_);
    }
  }
  fprintf(stdout, "AFTER auto-detect: reserved_zone_count_=%lu\n",
          reserved_zone_count_);

  if (reserved_zone_count_ > 0) {
    if (reserved_zone_count_ >= zbd_be_->GetNrZones() - ZENFS_META_ZONES) {
      return IOStatus::InvalidArgument(
          "Reserved zones (" + std::to_string(reserved_zone_count_) + 
          ") too large. Total zones: " + std::to_string(zbd_be_->GetNrZones()));
    }
    Info(logger_, "Reserved %lu zones at front for aux filesystem\n", 
         reserved_zone_count_);
  }

  if (max_nr_active_zones == 0)
    max_nr_active_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_active_io_zones_ = max_nr_active_zones - reserved_zones;

  /* Defaults: start_zone == reserved_zone_count_ (skip the front aux/CNS area),
   * end_zone == last zone of the device. The meta-zone search at line 320 and
   * the IO-zone loop at 347 both index zone_rep with these bounds, so leaving
   * them at -1 dereferences a negative index and segfaults. */
  if (start_zone < 0)
    start_zone = (int)reserved_zone_count_;
  if (end_zone < 0)
    end_zone = (int)zbd_be_->GetNrZones() - 1;

  fprintf(stdout, "[ZoneBlockDevice] start_zone: %d end_zone: %d ao_zones: %d\n",
      start_zone, end_zone, ao_zones);

  if (ao_zones == -1) {
    if (max_nr_active_zones == 0)
      max_nr_active_io_zones_ = zbd_be_->GetNrZones();
    else
      max_nr_active_io_zones_ = max_nr_active_zones - reserved_zones;

    if (max_nr_open_zones == 0)
      max_nr_open_io_zones_ = zbd_be_->GetNrZones();
    else
      max_nr_open_io_zones_ = max_nr_open_zones - reserved_zones;
  } else {
      max_nr_active_io_zones_ = ao_zones - reserved_zones;
      max_nr_open_io_zones_ = ao_zones - reserved_zones;
      fprintf(stdout, "[ZonedBlockDevice Static Zone Setting] nr zones: %u max active: %u max open: %u\n",
      end_zone - start_zone + 1, max_nr_active_io_zones_, max_nr_open_io_zones_);
  }

  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n",
       zbd_be_->GetNrZones(), max_nr_active_zones, max_nr_open_zones);

  zone_rep = zbd_be_->ListZones();

  if (ao_zones != -1 ) {
    zone_rep->zone_count_ = end_zone - start_zone + 1;
  }

  fprintf(stdout, "ZoneCount : %u\n",zone_rep->ZoneCount());

  // if (zone_rep == nullptr || zone_rep->ZoneCount() != zbd_be_->GetNrZones()) {
  if (zone_rep == nullptr) {
    Error(logger_, "Failed to list zones");
    return IOStatus::IOError("Failed to list zones");
  }
  
  fprintf(stdout, "reserved_zone_count_ = %lu\n", reserved_zone_count_);

  // Meta zones are allocated from the END of the device to prevent
  // conflicts with reserved zones at the front, which may change when
  // device size is modified.
  // We scan backwards from the end to find ZENFS_META_ZONES suitable zones.

  uint64_t total_zones = zone_rep->ZoneCount();
  
  fprintf(stdout, "Searching for %d meta zones from end (total zones: %lu)\n",
          ZENFS_META_ZONES, total_zones);

  // Search backwards from the end to find meta zones
  int zones_checked = 0;
  for (int j = end_zone - ZENFS_META_ZONES + 1; j <= end_zone && m < ZENFS_META_ZONES; j++) {
    zones_checked++;
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, j)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, j)) {
        // fprintf(stdout, "[+]zone %ld added to meta zone (searched %d zones)\n",
        //         j, zones_checked);
        meta_zones.push_back(new Zone(this, zbd_be_.get(), zone_rep, j));
      }
      m++;
    }
  }

  if (m < ZENFS_META_ZONES) {
    Error(logger_, "Not enough zones for metadata");
    return IOStatus::IOError("Not enough zones for metadata");
  }

  // Calculate the minimum zone index used by meta zones
  // uint64_t first_meta_zone_idx = meta_zones.front()->GetZoneNr();
  // fprintf(stdout, "Meta zones occupy zone indices %lu to %lu\n",
  //         first_meta_zone_idx, end_zone);

  active_io_zones_ = 0;
  open_io_zones_ = 0;

  // Reserve for the whole device so io_zones never reallocates. ShrinkReservedZones grows
  // io_zones at runtime by append; a fixed backing buffer lets that happen safely without a
  // global write pause (concurrent index-iteration in the allocator keeps valid pointers).
  io_zones.reserve(zbd_be_->GetNrZones());
  // Allocate IO zones from reserved_zone_count_ up to (but not including) meta zones
  for (i = start_zone; i < end_zone - ZENFS_META_ZONES + 1; i++) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        Zone *newZone = new Zone(this, zbd_be_.get(), zone_rep, i);
        if (!newZone->Acquire()) {
          assert(false);
          return IOStatus::Corruption("Failed to set busy flag of zone " +
                                      std::to_string(newZone->GetZoneNr()));
        }
        io_zones.push_back(newZone);
        if (zbd_be_->ZoneIsActive(zone_rep, i)) {
          active_io_zones_++;
          if (zbd_be_->ZoneIsOpen(zone_rep, i)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
        IOStatus status = newZone->CheckRelease();
        if (!status.ok()) {
          return status;
        }
      }
    }
  }

  fprintf(stdout, "[meta zones] %ld-%ld\n", 
      meta_zones[0]->GetZoneNr(), meta_zones[meta_zones.size() - 1]->GetZoneNr());
  fprintf(stdout, "[io zones] %ld-%ld\n", io_zones[0]->GetZoneNr(), 
            io_zones[io_zones.size() - 1]->GetZoneNr());
  Info(logger_, 
       "ZenFS initialized: %lu IO zones, %lu meta zones, %lu reserved zones\n",
       io_zones.size(), meta_zones.size(), reserved_zone_count_);

  fprintf(stdout, "ZenFS initialized: %lu IO zones, %lu meta zones, %lu reserved zones\n",
       io_zones.size(), meta_zones.size(), reserved_zone_count_);

  start_time_ = time(NULL);

  return IOStatus::OK();
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones) {
    free += z->capacity_;
  }
  return free;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  // Garbage = written-but-invalid bytes in EVERY zone (open, closed-partial,
  // full). Counting only full zones hides the dead bytes in non-full zones
  // ("ghost garbage") and makes the GC worker's free% read too high, so GC
  // starts too late.
  uint64_t reclaimable = 0;
  for (const auto z : io_zones) {
    uint64_t written = z->max_capacity_ - z->capacity_;
    if (written > z->used_capacity_)
      reclaimable += written - z->used_capacity_;
  }
  return reclaimable;
}

void ZonedBlockDevice::LogZoneStats() {
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;

  for (const auto z : io_zones) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active,
       active_io_zones_.load(), open_io_zones_.load());
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n",
            z->start_, used, used / MB);
    }
  }
}

void ZonedBlockDevice::LogGarbageInfo() {
  // Log zone garbage stats vector.
  //
  // The values in the vector represents how many zones with target garbage
  // percent. Garbage percent of each index: [0%, <10%, < 20%, ... <100%, 100%]
  // For example `[100, 1, 2, 3....]` means 100 zones are empty, 1 zone has less
  // than 10% garbage, 2 zones have  10% ~ 20% garbage ect.
  //
  // We don't need to lock io_zones since we only read data and we don't need
  // the result to be precise.
  int zone_gc_stat[12] = {0};
  for (auto z : io_zones) {
    if (!z->Acquire()) {
      continue;
    }

    if (z->IsEmpty()) {
      zone_gc_stat[0]++;
      z->Release();
      continue;
    }

    double garbage_rate = 0;
    if (z->IsFull()) {
      garbage_rate =
          double(z->max_capacity_ - z->used_capacity_) / z->max_capacity_;
    } else {
      garbage_rate =
          double(z->wp_ - z->start_ - z->used_capacity_) / z->max_capacity_;
    }
    assert(garbage_rate >= 0);
    int idx = int((garbage_rate + 0.1) * 10);
    zone_gc_stat[idx]++;

    z->Release();
  }

  std::stringstream ss;
  ss << "Zone Garbage Stats: [";
  for (int i = 0; i < 12; i++) {
    ss << zone_gc_stat[i] << " ";
  }
  ss << "]";
  Info(logger_, "%s", ss.str().data());
}

ZonedBlockDevice::~ZonedBlockDevice() {
  for (const auto z : meta_zones) {
    delete z;
  }

  for (const auto z : io_zones) {
    delete z;
  }
}

#define LIFETIME_DIFF_NOT_GOOD (100)
#define LIFETIME_DIFF_COULD_BE_WORSE (50)

unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
                             Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime <= Env::WLTH_EXTREME);

  if ((file_lifetime == Env::WLTH_NOT_SET) ||
      (file_lifetime == Env::WLTH_NONE)) {
    if (file_lifetime == zone_lifetime) {
      return 0;
    } else {
      return LIFETIME_DIFF_NOT_GOOD;
    }
  }

  if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;
  if (zone_lifetime == file_lifetime) return LIFETIME_DIFF_COULD_BE_WORSE;

  return LIFETIME_DIFF_NOT_GOOD;
}

IOStatus ZonedBlockDevice::AllocateMetaZone(Zone **out_meta_zone) {
  assert(out_meta_zone);
  *out_meta_zone = nullptr;
  ZenFSMetricsLatencyGuard guard(metrics_, ZENFS_META_ALLOC_LATENCY,
                                 Env::Default());
  metrics_->ReportQPS(ZENFS_META_ALLOC_QPS, 1);

  for (const auto z : meta_zones) {
    /* If the zone is not used, reset and use it */
    if (z->Acquire()) {
      if (!z->IsUsed()) {
        if (!z->IsEmpty() && !z->Reset().ok()) {
          Warn(logger_, "Failed resetting zone!");
          IOStatus status = z->CheckRelease();
          if (!status.ok()) return status;
          continue;
        }
        *out_meta_zone = z;
        return IOStatus::OK();
      }
    }
  }
  assert(true);
  Error(logger_, "Out of metadata zones, we should go to read only now.");
  return IOStatus::NoSpace("Out of metadata zones");
}

IOStatus ZonedBlockDevice::ResetUnusedIOZones() {
  total_reset_calls_++;
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (!z->IsEmpty() && !z->IsUsed()) {
        bool full = z->IsFull();
        uint64_t zutil = z->Utilization();

        if (zutil > reset_util_max_.load()) {
          reset_util_max_.store(zutil);
        }

        reset_util_sum_ += zutil;
        total_reset_zones_++;

        IOStatus reset_status = z->Reset();
        IOStatus release_status = z->CheckRelease();
        if (!reset_status.ok()) return reset_status;
        if (!release_status.ok()) return release_status;
        if (!full) PutActiveIOZoneToken();
      } else {
        IOStatus release_status = z->CheckRelease();
        if (!release_status.ok()) return release_status;
      }
    }
  }
  return IOStatus::OK();
}

void ZonedBlockDevice::WaitForOpenIOZoneToken(bool prioritized) {
  long allocator_open_limit;

  /* Avoid non-priortized allocators from starving prioritized ones */
  if (prioritized) {
    allocator_open_limit = max_nr_open_io_zones_;
  } else {
    allocator_open_limit = max_nr_open_io_zones_ - 1;
  }

  /* Wait for an open IO Zone token - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutOpenIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  zone_resources_.wait(lk, [this, allocator_open_limit] {
    if (open_io_zones_.load() < allocator_open_limit) {
      open_io_zones_++;
      return true;
    } else {
      return false;
    }
  });
}

bool ZonedBlockDevice::GetActiveIOZoneTokenIfAvailable() {
  /* Grap an active IO Zone token if available - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutActiveIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  if (active_io_zones_.load() < max_nr_active_io_zones_) {
    active_io_zones_++;
    return true;
  }
  return false;
}

void ZonedBlockDevice::PutOpenIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    open_io_zones_--;
  }
  zone_resources_.notify_one();
}

void ZonedBlockDevice::PutActiveIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    active_io_zones_--;
  }
  zone_resources_.notify_one();
}

IOStatus ZonedBlockDevice::ApplyFinishThreshold() {
  IOStatus s;

  if (finish_threshold_ == 0) return IOStatus::OK();

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      bool within_finish_threshold =
          z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100);
      if (!(z->IsEmpty() || z->IsFull()) && within_finish_threshold) {
        /* If there is less than finish_threshold_% remaining capacity in a
         * non-open-zone, finish the zone */
        s = z->Finish();
        if (!s.ok()) {
          z->Release();
          Debug(logger_, "Failed finishing zone");
          return s;
        }
        s = z->CheckRelease();
        if (!s.ok()) return s;
        PutActiveIOZoneToken();
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::FinishCheapestIOZone() {
  IOStatus s;
  Zone *finish_victim = nullptr;

  num_finish_called_++;
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty() || z->IsFull()) {
        s = z->CheckRelease();
        if (!s.ok()) return s;
        continue;
      }
      if (finish_victim == nullptr) {
        finish_victim = z;
        continue;
      }
      if (finish_victim->capacity_ > z->capacity_) {
        s = finish_victim->CheckRelease();
        if (!s.ok()) return s;
        finish_victim = z;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  // If all non-busy zones are empty or full, we should return success.
  if (finish_victim == nullptr) {
    Info(logger_, "All non-busy zones are empty or full, skip.");
    return IOStatus::OK();
  }

  s = finish_victim->Finish();
  num_finish_++;
  IOStatus release_status = finish_victim->CheckRelease();

  if (s.ok()) {
    PutActiveIOZoneToken();
  }

  if (!release_status.ok()) {
    return release_status;
  }

  return s;
}

IOStatus ZonedBlockDevice::GetBestOpenZoneMatch(
    Env::WriteLifeTimeHint file_lifetime, unsigned int *best_diff_out,
    Zone **zone_out, uint32_t min_capacity, uint64_t min_start) {
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (z->IsAbsorbing()) continue;  // excluded: CNS grow is absorbing it
    if (z->Acquire()) {
      if ((z->used_capacity_ > 0) && !z->IsFull() &&
          z->capacity_ >= min_capacity && z->start_ >= min_start) {
        unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
        if (diff <= best_diff) {
          if (allocated_zone != nullptr) {
            s = allocated_zone->CheckRelease();
            if (!s.ok()) {
              IOStatus s_ = z->CheckRelease();
              if (!s_.ok()) return s_;
              return s;
            }
          }
          allocated_zone = z;
          best_diff = diff;
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZone(Zone **zone_out, uint64_t min_start,
                                             bool from_end) {
  IOStatus s;
  Zone *allocated_zone = nullptr;
  size_t n = io_zones.size();
  // from_end: scan highest->lowest so always-open metadata (MANIFEST) lands in
  // high zones, away from the low CNS-grow absorb range.
  for (size_t i = 0; i < n; i++) {
    Zone *z = from_end ? io_zones[n - 1 - i] : io_zones[i];
    if (z->IsAbsorbing()) continue;  // excluded: CNS grow is absorbing it
    if (z->Acquire()) {
      if (z->IsEmpty() && z->start_ >= min_start) {
        allocated_zone = z;
        break;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }
  alloc_empty_++;
  *zone_out = allocated_zone;
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::InvalidateCache(uint64_t pos, uint64_t size) {
  int ret = zbd_be_->InvalidateCache(pos, size);

  if (ret) {
    return IOStatus::IOError("Failed to invalidate cache");
  }
  return IOStatus::OK();
}

int ZonedBlockDevice::Read(char *buf, uint64_t offset, int n, bool direct) {
  int ret = 0;
  int left = n;
  int r = -1;

  while (left) {
    r = zbd_be_->Read(buf, left, offset, direct);
    if (r <= 0) {
      if (r == -1 && errno == EINTR) {
        continue;
      }
      break;
    }
    ret += r;
    buf += r;
    left -= r;
    offset += r;
  }

  if (r < 0) return r;
  return ret;
}

IOStatus ZonedBlockDevice::ReleaseMigrateZone(Zone *zone) {
  IOStatus s = IOStatus::OK();
  {
    std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
    migrating_ = false;
    if (zone != nullptr) {
      s = zone->CheckRelease();
      Info(logger_, "ReleaseMigrateZone: %lu", zone->start_);
    }
  }
  migrate_resource_.notify_one();
  return s;
}

IOStatus ZonedBlockDevice::TakeMigrateZone(Zone **out_zone,
                                           Env::WriteLifeTimeHint file_lifetime,
                                           uint32_t min_capacity,
                                           uint64_t min_start) {
  std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
  migrate_resource_.wait(lock, [this] { return !migrating_; });

  migrating_ = true;

  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  auto s = GetBestOpenZoneMatch(file_lifetime, &best_diff, out_zone,
                                min_capacity, min_start);
  // CNS grow (min_start>0): if no open zone above the boundary qualifies, open a
  // fresh empty zone there so migrated data stays out of the absorb range.
  if (s.ok() && (*out_zone) == nullptr && min_start > 0) {
    s = AllocateEmptyZone(out_zone, min_start);
  }
  if (s.ok() && (*out_zone) != nullptr) {
    Info(logger_, "TakeMigrateZone: %lu", (*out_zone)->start_);
  } else {
    migrating_ = false;
  }

  return s;
}

IOStatus ZonedBlockDevice::AllocateIOZone(Env::WriteLifeTimeHint file_lifetime,
                                          IOType io_type, Zone **out_zone,
                                          bool from_end) {
  Zone *allocated_zone = nullptr;
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  int new_zone = 0;
  IOStatus s;

  allocate_io_zones++;
  
  auto tag = ZENFS_WAL_IO_ALLOC_LATENCY;
  if (io_type != IOType::kWAL) {
    // L0 flushes have lifetime MEDIUM
    if (file_lifetime == Env::WLTH_MEDIUM) {
      tag = ZENFS_L0_IO_ALLOC_LATENCY;
    } else {
      tag = ZENFS_NON_WAL_IO_ALLOC_LATENCY;
    }
  }

  ZenFSMetricsLatencyGuard guard(metrics_, tag, Env::Default());
  metrics_->ReportQPS(ZENFS_IO_ALLOC_QPS, 1);

  // Check if a deferred IO error was set
  s = GetZoneDeferredStatus();
  if (!s.ok()) {
    return s;
  }

  if (io_type != IOType::kWAL) {
    s = ApplyFinishThreshold();
    if (!s.ok()) {
      return s;
    }
  }

  WaitForOpenIOZoneToken(io_type == IOType::kWAL);
  num_concurrent_alloc_++;
  /* Try to fill an already open zone(with the best life time diff).
   * Skip for meta_high (from_end): we want a fresh HIGH empty zone, not reuse of
   * a low open SST zone. best_diff stays NOT_GOOD -> falls to the new-zone path. */
  if (!from_end) {
    s = GetBestOpenZoneMatch(file_lifetime, &best_diff, &allocated_zone);
    if (!s.ok()) {
      PutOpenIOZoneToken();
      return s;
    }
  }

  // Holding allocated_zone if != nullptr

  if (best_diff >= LIFETIME_DIFF_COULD_BE_WORSE) {
    bool got_token = GetActiveIOZoneTokenIfAvailable();

    /* If we did not get a token, try to use the best match, even if the life
     * time diff not good but a better choice than to finish an existing zone
     * and open a new one
     */
    if (allocated_zone != nullptr) {
      if (!got_token && best_diff == LIFETIME_DIFF_COULD_BE_WORSE) {
        Debug(logger_,
              "Allocator: avoided a finish by relaxing lifetime diff "
              "requirement\n");
      } else {
        s = allocated_zone->CheckRelease();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          if (got_token) PutActiveIOZoneToken();
          return s;
        }
        allocated_zone = nullptr;
      }
    }

    /* If we haven't found an open zone to fill, open a new zone */
    if (allocated_zone == nullptr) {
      /* We have to make sure we can open an empty zone */
      while (!got_token && !GetActiveIOZoneTokenIfAvailable()) {
        s = FinishCheapestIOZone();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          return s;
        }
      }

      s = AllocateEmptyZone(&allocated_zone, 0, from_end);
      if (!s.ok()) {
        PutActiveIOZoneToken();
        PutOpenIOZoneToken();
        return s;
      }

      if (allocated_zone != nullptr) {
        assert(allocated_zone->IsBusy());
        allocated_zone->lifetime_ = file_lifetime;
        allocated_zone->min_lifetime_ = file_lifetime;
        allocated_zone->max_lifetime_ = file_lifetime;

        if (file_lifetime == 2) {
          zone_lifetimes_[2]++;
        } else if (file_lifetime == 3) {
          zone_lifetimes_[3]++;
        } else if (file_lifetime == 4) {
          zone_lifetimes_[4]++;
        } else if (file_lifetime == 5) {
          zone_lifetimes_[5]++;
        }
        new_zone = true;
      } else {
        PutActiveIOZoneToken();
      }
    }
  }

  if (allocated_zone != nullptr && file_lifetime != 0) {
    zone_diff_[allocated_zone->lifetime_ - file_lifetime]++;
  }

  if (allocated_zone) {
    assert(allocated_zone->IsBusy());
    Debug(logger_,
          "Allocating zone(new=%d) start: 0x%lx wp: 0x%lx lt: %d file lt: %d\n",
          new_zone, allocated_zone->start_, allocated_zone->wp_,
          allocated_zone->lifetime_, file_lifetime);
  } else {
    PutOpenIOZoneToken();
  }

  if (io_type != IOType::kWAL) {
    LogZoneStats();
  }

  *out_zone = allocated_zone;

  metrics_->ReportGeneral(ZENFS_OPEN_ZONES_COUNT, open_io_zones_);
  metrics_->ReportGeneral(ZENFS_ACTIVE_ZONES_COUNT, active_io_zones_);

  return IOStatus::OK();
}

std::string ZonedBlockDevice::GetFilename() { return zbd_be_->GetFilename(); }

uint32_t ZonedBlockDevice::GetBlockSize() { return zbd_be_->GetBlockSize(); }

uint64_t ZonedBlockDevice::GetZoneSize() { return zbd_be_->GetZoneSize(); }

// ---- Online CNS-area resize (ZoneArbiter / ModifyZone) --------------------
// Reset the first n IO zones (the lowest S-zones, right after the current ABA).
// Called after their valid data has been migrated out and before the device
// ModifyZone, so the S->R transition starts from clean, empty zones.
IOStatus ZonedBlockDevice::ResetFrontIOZones(uint32_t n) {
  if (n == 0) return IOStatus::OK();
  if (io_zones.size() < (size_t)n)
    return IOStatus::InvalidArgument("ResetFrontIOZones: n > io_zones");
  for (uint32_t k = 0; k < n; k++) {
    Zone* z = io_zones[k];
    if (z->IsUsed())
      return IOStatus::Busy("ResetFrontIOZones: zone still has valid data");
    if (!z->Acquire()) return IOStatus::Busy("ResetFrontIOZones: zone busy");
    IOStatus s = z->Reset();
    z->CheckRelease();
    if (!s.ok()) return s;
  }
  return IOStatus::OK();
}

// Grow the CNS area by n zones (ZenFS side): drop the first n IO zones (now
// absorbed into the host-managed R-region) from the allocatable pool and bump
// the reserved-zone count. Caller migrated + reset them and issued the device
// ModifyZone first.
IOStatus ZonedBlockDevice::GrowReservedZones(uint32_t n) {
  if (n == 0) return IOStatus::OK();
  if (io_zones.size() <= (size_t)n)
    return IOStatus::InvalidArgument("GrowReservedZones: would empty io_zones");
  for (uint32_t k = 0; k < n; k++) {
    if (io_zones[k]->IsUsed())
      return IOStatus::Busy("GrowReservedZones: front zone not empty");
  }
  for (uint32_t k = 0; k < n; k++) {
    Zone* z = io_zones[k];
    z->Acquire();  // take ownership before deleting
    delete z;
  }
  io_zones.erase(io_zones.begin(), io_zones.begin() + n);
  reserved_zone_count_ += n;
  Info(logger_, "GrowReservedZones: +%u -> reserved=%lu io_zones=%lu", n,
       reserved_zone_count_, io_zones.size());
  return IOStatus::OK();
}

// Shrink the CNS area by n zones (ZenFS side): the last n R-zones (just before
// the ABA) have become S-zones; add them (reset, empty) to the front of the
// allocatable pool and reduce the reserved-zone count. Caller evacuated the
// F2FS side and issued the device ModifyZone first.
IOStatus ZonedBlockDevice::ShrinkReservedZones(uint32_t n) {
  if (n == 0) return IOStatus::OK();
  if (reserved_zone_count_ < n)
    return IOStatus::InvalidArgument("ShrinkReservedZones: reserved < n");
  auto _t0 = std::chrono::steady_clock::now();
  std::unique_ptr<ZoneList> zone_rep = zbd_be_->ListZones();
  if (!zone_rep)
    return IOStatus::IOError("ShrinkReservedZones: ListZones failed");
  auto _t1 = std::chrono::steady_clock::now();
  std::vector<Zone*> newzones;
  uint32_t first = (uint32_t)reserved_zone_count_ - n;
  for (uint32_t idx = first; idx < (uint32_t)reserved_zone_count_; idx++) {
    Zone* z = new Zone(this, zbd_be_.get(), zone_rep, idx);
    if (z->Acquire()) {
      IOStatus s = z->Reset();
      z->CheckRelease();
      if (!s.ok()) {
        delete z;
        for (auto* p : newzones) delete p;
        return s;
      }
    }
    newzones.push_back(z);
  }
  auto _t2 = std::chrono::steady_clock::now();
  // Append at the end (not insert-at-begin): io_zones is reserve()'d so push_back never
  // reallocates and never shifts existing elements, so a concurrent allocator iterating by
  // index keeps valid pointers (it may simply not see the new zones until its next pass).
  // This is what lets shrink publish freed zones without a global write pause.
  for (auto* z : newzones) io_zones.push_back(z);
  reserved_zone_count_ -= n;
  long _list_us = std::chrono::duration_cast<std::chrono::microseconds>(_t1 - _t0).count();
  long _reset_us = std::chrono::duration_cast<std::chrono::microseconds>(_t2 - _t1).count();
  Info(logger_, "ShrinkReservedZones: -%u -> reserved=%lu io_zones=%lu", n,
       reserved_zone_count_, io_zones.size());
  printf("[ShrinkRZ] ListZones=%ld us reset(%u zones)=%ld us\n", _list_us, n, _reset_us);
  fflush(stdout);
  return IOStatus::OK();
}

// Online CNS grow, step 1: acquire+hold every IO zone whose start_ lies in the
// absorb range [old_rz, new_rz) (zone units). A held zone fails others' Acquire()
// so the allocator/GC skip it -- this excludes the absorb range from new writes
// WITHOUT a global write pause (which deadlocks: a paused writer can hold an open
// zone that a pending writer needs). Returns Busy if a zone stays actively in use
// (caller aborts the resize; ABA unchanged, no data loss).
IOStatus ZonedBlockDevice::AcquireHoldFrontZones(uint32_t old_rz,
                                                 uint32_t new_rz) {
  uint64_t zsz = GetZoneSize();
  uint64_t lo = (uint64_t)old_rz * zsz, hi = (uint64_t)new_rz * zsz;
  for (auto z : io_zones) {
    if (z->start_ < lo || z->start_ >= hi) continue;
    bool got = false;
    for (int t = 0; t < 200; t++) {  // up to ~1s waiting for an active zone
      if (z->Acquire()) { got = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!got)
      return IOStatus::Busy("AcquireHoldFrontZones: zone " +
                            std::to_string(z->GetZoneNr()) + " busy");
  }
  return IOStatus::OK();
}

// Online CNS grow, step 2 (after migration emptied them): reset the held absorb
// zones so the device sees them EMPTY for the S->R transition. Zones stay held
// (never released) -> permanently excluded from allocation; the reserved count
// bump (caller's SetReservedZones) makes it official. Returns Busy if a zone is
// not empty (migration incomplete) -> caller aborts.
IOStatus ZonedBlockDevice::ResetHeldFrontZones(uint32_t old_rz,
                                               uint32_t new_rz) {
  uint64_t zsz = GetZoneSize();
  uint64_t lo = (uint64_t)old_rz * zsz, hi = (uint64_t)new_rz * zsz;
  for (auto z : io_zones) {
    if (z->start_ < lo || z->start_ >= hi) continue;
    if (z->IsUsed())
      return IOStatus::Busy("ResetHeldFrontZones: zone " +
                            std::to_string(z->GetZoneNr()) +
                            " not empty (migration incomplete)");
    IOStatus s = z->Reset();  // held (busy) + empty -> reset; keep held
    if (!s.ok()) return s;
  }
  Info(logger_, "ResetHeldFrontZones: held+reset zones [%u,%u)", old_rz, new_rz);
  return IOStatus::OK();
}

// Mark the absorb-range zones excluded from allocation (or clear it). With them
// excluded, no new writes land there, so a force-GC loop drains them to empty
// even if one was the active write frontier.
void ZonedBlockDevice::SetAbsorbingRange(uint32_t old_rz, uint32_t new_rz,
                                         bool v) {
  uint64_t zsz = GetZoneSize();
  uint64_t lo = (uint64_t)old_rz * zsz, hi = (uint64_t)new_rz * zsz;
  for (auto z : io_zones)
    if (z->start_ >= lo && z->start_ < hi) z->SetAbsorbing(v);
}

// True once every absorb-range zone holds no valid data (force-GC drained them).
bool ZonedBlockDevice::RangeAllEmpty(uint32_t old_rz, uint32_t new_rz) {
  uint64_t zsz = GetZoneSize();
  uint64_t lo = (uint64_t)old_rz * zsz, hi = (uint64_t)new_rz * zsz;
  for (auto z : io_zones)
    if (z->start_ >= lo && z->start_ < hi && z->used_capacity_ > 0) return false;
  return true;
}

uint64_t ZonedBlockDevice::RangeUsedBytes(uint32_t old_rz, uint32_t new_rz) {
  uint64_t zsz = GetZoneSize();
  uint64_t lo = (uint64_t)old_rz * zsz, hi = (uint64_t)new_rz * zsz, used = 0;
  for (auto z : io_zones)
    if (z->start_ >= lo && z->start_ < hi) used += z->used_capacity_;
  return used;
}

uint32_t ZonedBlockDevice::GetNrZones() { return zbd_be_->GetNrZones(); }

void ZonedBlockDevice::EncodeJsonZone(std::ostream &json_stream,
                                      const std::vector<Zone *> zones) {
  bool first_element = true;
  json_stream << "[";
  for (Zone *zone : zones) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    zone->EncodeJson(json_stream);
  }

  json_stream << "]";
}

void ZonedBlockDevice::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"meta\":";
  EncodeJsonZone(json_stream, meta_zones);
  json_stream << ",\"io\":";
  EncodeJsonZone(json_stream, io_zones);
  json_stream << "}";
}

IOStatus ZonedBlockDevice::GetZoneDeferredStatus() {
  std::lock_guard<std::mutex> lock(zone_deferred_status_mutex_);
  return zone_deferred_status_;
}

void ZonedBlockDevice::SetZoneDeferredStatus(IOStatus status) {
  std::lock_guard<std::mutex> lk(zone_deferred_status_mutex_);
  if (!zone_deferred_status_.ok()) {
    zone_deferred_status_ = status;
  }
}

void ZonedBlockDevice::GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot) {
  for (auto *zone : io_zones) {
    snapshot.emplace_back(*zone);
  }
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
