// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#if !defined(ROCKSDB_LITE) && defined(OS_LINUX)

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <future>

#include "rocksdb/file_system.h"
#include "rocksdb/io_status.h"
#include "zbd_zenfs.h"

namespace ROCKSDB_NAMESPACE {
class ZoneFile;

struct WriteArg {
  ZonedBlockDevice *zbd;
  char *data;
  uint64_t offset;
  uint32_t size;
};

class ZoneExtent {
 public:
  uint64_t start_;
  uint64_t length_;
  Zone* zone_;

  explicit ZoneExtent(uint64_t start, uint64_t length, Zone* zone);
  Status DecodeFrom(Slice* input);
  void EncodeTo(std::string* output);
  void EncodeJson(std::ostream& json_stream);
};

class ZoneFile;

/* Interface for persisting metadata for files */
class MetadataWriter {
 public:
  virtual ~MetadataWriter();
  virtual IOStatus Persist(ZoneFile* zoneFile) = 0;
};

class ZoneFile {
 private:
  const uint64_t NO_EXTENT = 0xffffffffffffffff;

  ZonedBlockDevice* zbd_;

  std::vector<ZoneExtent*> extents_;
  std::vector<std::string> linkfiles_;

  Zone* active_zone_;
  uint64_t extent_start_ = NO_EXTENT;
  uint64_t extent_filepos_ = 0;

  Env::WriteLifeTimeHint lifetime_;
  IOType io_type_; /* Only used when writing */
  uint64_t file_size_;
  uint64_t file_id_;

  uint32_t nr_synced_extents_ = 0;
  bool open_for_wr_ = false;
  std::mutex open_for_wr_mtx_;

  time_t m_time_;
  bool is_sparse_ = false;
  bool is_deleted_ = false;

  MetadataWriter* metadata_writer_ = NULL;

  /* Excludes the single RocksDB writer from concurrent extent-list surgery:
   * held by every zenfs append path and CloseWR, and by MigrateFileExtents
   * when it migrates an OPEN-for-write file during a CNS grow (the WR lock is
   * held for the file's whole open lifetime, so it cannot be used there). */
  std::mutex writer_mtx_;
  std::atomic<int> readers_{0};

  int level_ {-1};
  int type_ {-1};

 public:
  static const int SPARSE_HEADER_SIZE = 8;

  explicit ZoneFile(ZonedBlockDevice* zbd, uint64_t file_id_,
                    MetadataWriter* metadata_writer);

  virtual ~ZoneFile();

  void AcquireWRLock();
  bool TryAcquireWRLock();
  void ReleaseWRLock();
  std::mutex& WriterMtx() { return writer_mtx_; }
  /* Evict this open file's ACTIVE extent out of a CNS-grow absorb zone (seal:
   * push + finish + close). Caller must hold writer_mtx_. No-op otherwise. */
  IOStatus SealForAbsorb();

  IOStatus CloseWR();
  bool IsOpenForWR();

  IOStatus PersistMetadata();

  void GetWalFutures();

  IOStatus Append(void* buffer, int data_size);
  IOStatus BufferedAppend(char* data, uint32_t size);
  IOStatus SparseAppend(char* data, uint32_t size);
  IOStatus SetWriteLifeTimeHint(Env::WriteLifeTimeHint lifetime);
  void SetIOType(IOType io_type);
  std::string GetFilename();
  time_t GetFileModificationTime();
  void SetFileModificationTime(time_t mt);
  uint64_t GetFileSize();
  void SetFileSize(uint64_t sz);
  void ClearExtents();

  void SetFileLevel(int level) {
    if (level == 0 && zbd_->l0_aux == 1) {
      // O_DIRECT -> pread error (SST block reads are unaligned), so the L0 fd
      // stays buffered. To stay fair vs the ZNS path (which is forced O_DIRECT,
      // never cached, durable at sync points): drop O_SYNC (no per-write sync;
      // durability comes from fdatasync in DataSync), disable readahead, and
      // evict pages after each read so the page cache gives no unfair advantage.
      this->fd = open(filename.c_str(), O_RDWR | O_CREAT, 0644);
      this->l0_aux = 1;
      fprintf(stderr, "[ZL0+] create aux L0 SST: %s\n", filename.c_str());
      if (this->fd == -1) {
        fprintf(stdout, "file open failed : %d\n", errno);
      } else {
        posix_fadvise(this->fd, 0, 0, POSIX_FADV_RANDOM);
      }

      // fprintf(stdout, "L0 fd : %d filename : %s\n", this->fd, filename.c_str());
    }

    level_ = level;
  }

  uint32_t GetBlockSize() { return zbd_->GetBlockSize(); }
  ZonedBlockDevice* GetZbd() { return zbd_; }
  std::vector<ZoneExtent*> GetExtents() { return extents_; }
  Env::WriteLifeTimeHint GetWriteLifeTimeHint() { return lifetime_; }

  IOStatus PositionedRead(uint64_t offset, size_t n, Slice* result,
                          char* scratch, bool direct);
  ZoneExtent* GetExtent(uint64_t file_offset, uint64_t* dev_offset);
  void PushExtent();
  IOStatus AllocateNewZone();

  void EncodeTo(std::string* output, uint32_t extent_start);
  void EncodeUpdateTo(std::string* output) {
    EncodeTo(output, nr_synced_extents_);
  };
  void EncodeSnapshotTo(std::string* output) { EncodeTo(output, 0); };
  void EncodeJson(std::ostream& json_stream);
  void MetadataSynced() { nr_synced_extents_ = extents_.size(); };
  void MetadataUnsynced() { nr_synced_extents_ = 0; };

  IOStatus MigrateData(uint64_t offset, uint32_t length, Zone* target_zone);

  Status DecodeFrom(Slice* input);
  Status MergeUpdate(std::shared_ptr<ZoneFile> update, bool replace);

  uint64_t GetID() { return file_id_; }

  bool IsSparse() { return is_sparse_; };
  int GetLevel() { return level_; };   /* -1 = never set (non-SST or hook not called) */

  void SetSparse(bool is_sparse) { is_sparse_ = is_sparse; };
  uint64_t HasActiveExtent() { return extent_start_ != NO_EXTENT; };
  uint64_t GetExtentStart() { return extent_start_; };

  IOStatus Recover();

  void ReplaceExtentList(std::vector<ZoneExtent*> new_list);
  void AddLinkName(const std::string& linkfile);
  IOStatus RemoveLinkName(const std::string& linkfile);
  IOStatus RenameLink(const std::string& src, const std::string& dest);
  uint32_t GetNrLinks() { return linkfiles_.size(); }
  const std::vector<std::string>& GetLinkFiles() const { return linkfiles_; }
  
  // Conventional WAL Writes
  std::vector<std::future<void>> futures;
  int fd {0}; // file descriptor for WAL files
  int wal_type {0}; // 0 : ZNS, 1 : CNS, 2 : Posix
  int l0_aux {0};
  // Always-open metadata (MANIFEST/CURRENT/OPTIONS/IDENTITY): allocate its zones
  // from the END so the low S-zones (the CNS-grow absorb range) hold only
  // migratable SSTs -> online grow isn't blocked by an open file pinning zone 0.
  bool meta_high {false};
  std::string filename;
  std::mutex pool_lock;

  IOStatus InvalidateCache(uint64_t pos, uint64_t size);

 private:
  void ReleaseActiveZone();
  void SetActiveZone(Zone* zone);
  IOStatus CloseActiveZone();

 public:
  std::shared_ptr<ZenFSMetrics> GetZBDMetrics() { return zbd_->GetMetrics(); };
  IOType GetIOType() const { return io_type_; };
  bool IsDeleted() const { return is_deleted_; };
  void SetDeleted() { is_deleted_ = true; };
  IOStatus RecoverSparseExtents(uint64_t start, uint64_t end, Zone* zone);

 public:
  class ReadLock {
   public:
    ReadLock(ZoneFile* zfile) : zfile_(zfile) {
      zfile_->writer_mtx_.lock();
      zfile_->readers_++;
      zfile_->writer_mtx_.unlock();
    }
    ~ReadLock() { zfile_->readers_--; }

   private:
    ZoneFile* zfile_;
  };
  class WriteLock {
   public:
    WriteLock(ZoneFile* zfile) : zfile_(zfile) {
      zfile_->writer_mtx_.lock();
      while (zfile_->readers_ > 0) {
      }
    }
    ~WriteLock() { zfile_->writer_mtx_.unlock(); }

   private:
    ZoneFile* zfile_;
  };
};

class ZonedWritableFile : public FSWritableFile {
 public:
  explicit ZonedWritableFile(ZonedBlockDevice* zbd, bool buffered,
                             std::shared_ptr<ZoneFile> zoneFile);
  virtual ~ZonedWritableFile();

  virtual IOStatus Append(const Slice& data, const IOOptions& options,
                          IODebugContext* dbg) override;
  virtual IOStatus Append(const Slice& data, const IOOptions& opts,
                          const DataVerificationInfo& /* verification_info */,
                          IODebugContext* dbg) override {
    return Append(data, opts, dbg);
  }
  virtual IOStatus PositionedAppend(const Slice& data, uint64_t offset,
                                    const IOOptions& options,
                                    IODebugContext* dbg) override;
  virtual IOStatus PositionedAppend(
      const Slice& data, uint64_t offset, const IOOptions& opts,
      const DataVerificationInfo& /* verification_info */,
      IODebugContext* dbg) override {
    return PositionedAppend(data, offset, opts, dbg);
  }
  virtual IOStatus Truncate(uint64_t size, const IOOptions& options,
                            IODebugContext* dbg) override;
  virtual IOStatus Close(const IOOptions& options,
                         IODebugContext* dbg) override;
  virtual IOStatus Flush(const IOOptions& options,
                         IODebugContext* dbg) override;
  virtual IOStatus Sync(const IOOptions& options, IODebugContext* dbg) override;
  virtual IOStatus RangeSync(uint64_t offset, uint64_t nbytes,
                             const IOOptions& options,
                             IODebugContext* dbg) override;
  virtual IOStatus Fsync(const IOOptions& options,
                         IODebugContext* dbg) override;

  virtual uint64_t GetFileSize(const IOOptions& /*options*/,
                               IODebugContext* /*dbg*/) override {
    return zoneFile_->GetFileSize();
  }

  bool use_direct_io() const override { return !buffered; }
  bool IsSyncThreadSafe() const override { return true; };
  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }
  void SetWriteLifeTimeHint(Env::WriteLifeTimeHint hint) override;
  virtual Env::WriteLifeTimeHint GetWriteLifeTimeHint() override {
    return zoneFile_->GetWriteLifeTimeHint();
  }

  void SetFileLevel(int level) override {
    zoneFile_->SetFileLevel(level);
  }

 private:
  IOStatus BufferedWrite(const Slice& data);
  IOStatus FlushBuffer();
  IOStatus DataSync();
  IOStatus CloseInternal();

  bool buffered;
  char* sparse_buffer;
  char* buffer;
  size_t buffer_sz;
  uint32_t block_sz;
  uint32_t buffer_pos;
  uint64_t wp;
  int write_temp;
  bool open;

  std::shared_ptr<ZoneFile> zoneFile_;
  MetadataWriter* metadata_writer_;

  std::mutex buffer_mtx_;
};

class ZonedSequentialFile : public FSSequentialFile {
 private:
  std::shared_ptr<ZoneFile> zoneFile_;
  uint64_t rp;
  bool direct_;

 public:
  explicit ZonedSequentialFile(std::shared_ptr<ZoneFile> zoneFile,
                               const FileOptions& file_opts)
      : zoneFile_(zoneFile),
        rp(0),
        direct_(file_opts.use_direct_reads && !zoneFile->IsSparse()) {}

  IOStatus Read(size_t n, const IOOptions& options, Slice* result,
                char* scratch, IODebugContext* dbg) override;
  IOStatus PositionedRead(uint64_t offset, size_t n, const IOOptions& options,
                          Slice* result, char* scratch,
                          IODebugContext* dbg) override;
  IOStatus Skip(uint64_t n) override;

  bool use_direct_io() const override { return direct_; };

  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }

  IOStatus InvalidateCache(size_t offset, size_t length) override {
    (void)offset;
    (void)length;
    return IOStatus::OK();
    // return zoneFile_->InvalidateCache(offset, length);
  }
};

class ZonedRandomAccessFile : public FSRandomAccessFile {
 private:
  std::shared_ptr<ZoneFile> zoneFile_;
  bool direct_;

 public:
  explicit ZonedRandomAccessFile(std::shared_ptr<ZoneFile> zoneFile,
                                 const FileOptions& file_opts)
      : zoneFile_(zoneFile),
        direct_(file_opts.use_direct_reads && !zoneFile->IsSparse()) {}

  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override;

  IOStatus Prefetch(uint64_t /*offset*/, size_t /*n*/,
                    const IOOptions& /*options*/,
                    IODebugContext* /*dbg*/) override {
    return IOStatus::OK();
  }

  bool use_direct_io() const override { return direct_; }

  size_t GetRequiredBufferAlignment() const override {
    return zoneFile_->GetBlockSize();
  }

  IOStatus InvalidateCache(size_t offset, size_t length) override {
    (void)offset;
    (void)length;
    return IOStatus::OK();
    // return zoneFile_->InvalidateCache(offset, length);
  }
};

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && defined(OS_LINUX)
