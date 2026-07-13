// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include <dirent.h>
#include <fcntl.h>
#include <gflags/gflags.h>
#include <rocksdb/file_system.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <iostream>
#include <memory>
#include <sstream>
#include <streambuf>

#ifdef WITH_TERARKDB
#include <fs/fs_zenfs.h>
#include <fs/version.h>
#else
#include <rocksdb/plugin/zenfs/fs/fs_zenfs.h>
#include <rocksdb/plugin/zenfs/fs/f2fs_ioctl.h>
#include <rocksdb/plugin/zenfs/fs/snapshot.h>
#include <rocksdb/plugin/zenfs/fs/zone_arbiter.h>
#include <rocksdb/plugin/zenfs/fs/version.h>
#endif

using GFLAGS_NAMESPACE::ParseCommandLineFlags;
using GFLAGS_NAMESPACE::RegisterFlagValidator;
using GFLAGS_NAMESPACE::SetUsageMessage;

DEFINE_string(zbd, "", "Path to a zoned block device.");
DEFINE_string(zonefs, "", "Path to a zonefs mountpoint.");
DEFINE_string(aux_path, "",
              "Path for auxiliary file storage (log and lock files).");
DEFINE_bool(
    force, false,
    "Force the action. May result in data loss.\n"
    "If used with mkfs or rmdir commands, data will be lost on an existing "
    "file system. If used with backup, data copied from "
    "the drive will likely be incomplete and/or corrupt "
    "- only use this for testing purposes.");
DEFINE_string(path, "", "File path");
DEFINE_int32(finish_threshold, 0, "Finish used zones if less than x% left");
DEFINE_string(restore_path, "", "Path to restore files");
DEFINE_string(backup_path, "", "Path to backup files");
DEFINE_string(src_file, "", "Source file path");
DEFINE_string(dest_file, "", "Destination file path");
DEFINE_bool(enable_gc, false, "Enable garbage collection");
DEFINE_bool(wal_on_aux, false, "Enable storing WAL files in [aux_path]");
DEFINE_int32(start_zone, -1, "start zone number");
DEFINE_int32(end_zone, -1, "end zone number");
DEFINE_int32(ao_zones, -1, "number of active/open zones");
DEFINE_bool(hyssd, false, "Hyssd mode, Allow opening device without O_EXCL");
DEFINE_int32(aux_size, 0, "Reserve front area in zone number for other filesystem (e.g F2FS for aux_path). If 0 in hyssd mode, auto-detect from device");

// HYSSD modify (ResizeCNS) options:
DEFINE_int32(new_rzone, -1, "New R-zone count for the resize-cns command");
DEFINE_int32(gc_start_zone, 0, "Start zone ID for the gc-force command");
DEFINE_int32(gc_num_zones, 1, "Number of zones for gc-force/gc-n-modify (>0)");

namespace ROCKSDB_NAMESPACE {

void AddDirSeparatorAtEnd(std::string &path) {
  if (path.empty() || path.back() != '/') path = path + "/";
}

std::unique_ptr<ZonedBlockDevice> zbd_open(bool readonly, bool exclusive,
                    int start_zone = FLAGS_start_zone, int end_zone = FLAGS_end_zone, 
                    int ao_zones = FLAGS_ao_zones) {
  std::unique_ptr<ZonedBlockDevice> zbd{new ZonedBlockDevice(
      FLAGS_zbd.empty() ? FLAGS_zonefs : FLAGS_zbd,
      FLAGS_zbd.empty() ? ZbdBackendType::kZoneFS : ZbdBackendType::kBlockDev,
      nullptr)};
  
  fprintf(stdout, "zbd_open\n");
  IOStatus open_status = zbd->Open(readonly, exclusive, FLAGS_hyssd, start_zone, end_zone, ao_zones);

  if (!open_status.ok()) {
    fprintf(stderr, "Failed to open zoned block device: %s, error: %s\n",
            FLAGS_zbd.c_str(), open_status.ToString().c_str());
    zbd.reset();
  }

  return zbd;
}

// Here we pass 'zbd' by non-const reference to be able to pass its ownership
// to 'zenFS'
Status zenfs_mount(std::unique_ptr<ZonedBlockDevice> &zbd,
                   std::unique_ptr<ZenFS> *zenFS, bool readonly) {
  Status s;
  fprintf(stdout, "zenfs_mount\n");
  std::unique_ptr<ZenFS> localZenFS{
      new ZenFS(zbd.release(), FileSystem::Default(), nullptr)};
  s = localZenFS->Mount(readonly);
  if (!s.ok()) {
    localZenFS.reset();
  }
  *zenFS = std::move(localZenFS);

  return s;
}

int is_dir(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    fprintf(stderr, "Failed to stat %s\n", path);
    return 1;
  }
  return S_ISDIR(st.st_mode);
}

// Create or check pre-existing aux directory and fail if it is
// inaccessible by current user and if it has previous data
int create_aux_dir(const char *path) {
  struct dirent *dent;
  size_t nfiles = 0;
  int ret = 0;

  errno = 0;
  ret = mkdir(path, 0750);
  if (ret < 0 && EEXIST != errno) {
    fprintf(stderr, "Failed to create aux directory %s: %s\n", path,
            strerror(errno));
    return 1;
  }
  // The aux_path is now available, check if it is a directory infact
  // and is empty and the user has access permission

  if (!is_dir(path)) {
    fprintf(stderr, "Aux path %s is not a directory\n", path);
    return 1;
  }

  errno = 0;

  auto closedirDeleter = [](DIR *d) {
    if (d != nullptr) closedir(d);
  };
  std::unique_ptr<DIR, decltype(closedirDeleter)> aux_dir{
      opendir(path), std::move(closedirDeleter)};
  if (errno) {
    fprintf(stderr, "Failed to open aux directory %s: %s\n", path,
            strerror(errno));
    return 1;
  }

  // Consider the directory as non-empty if any files/dir other
  // than . and .. are found.
  while ((dent = readdir(aux_dir.get())) != NULL && nfiles <= 2) ++nfiles;
  if (nfiles > 2) {
    fprintf(stderr, "Aux directory %s is not empty.\n", path);
    return 1;
  }

  if (access(path, R_OK | W_OK | X_OK) < 0) {
    fprintf(stderr,
            "User does not have access permissions on "
            "aux directory %s\n",
            path);
    return 1;
  }

  return 0;
}

int zenfs_tool_mkfs() {
  Status s;

  if (FLAGS_aux_path.empty()) {
    fprintf(stderr, "You need to specify --aux_path\n");
    return 1;
  }

  if (FLAGS_aux_size > 0 && !FLAGS_hyssd) {
    fprintf(stderr,
        "WARNING: --aux_size specified but --hyssd not enabled\n"
        "         Ignoring aux_size\n");
  }

  if (!FLAGS_hyssd) {
    fprintf(stdout, "Normal mode\n");
  }

  if (create_aux_dir(FLAGS_aux_path.c_str())) return 1;

  // std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, true);
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, false, 
          FLAGS_start_zone, FLAGS_end_zone, FLAGS_ao_zones);
  if (!zbd) return 1;

  if (FLAGS_hyssd) {
    uint64_t reserved_zones_to_use;
    if (FLAGS_aux_size > 0) {
      // User explicitly specified aux_size
      reserved_zones_to_use = FLAGS_aux_size;
      fprintf(stdout, "HYSSD mode: using user-specified %u zones at front\n",
              FLAGS_aux_size);
    } else {
      // Auto-detect from device
      reserved_zones_to_use = zbd->GetReservedZones();
      if (reserved_zones_to_use == 0) {
        fprintf(stderr,
                "ERROR: HYSSD mode but device reports 0 reserved zones.\n"
                "       Either specify --aux_size manually or check device.\n");
        return 1;
      }
      fprintf(stdout, "HYSSD mode: auto-detected %lu reserved zones from device\n",
              reserved_zones_to_use);
    }
    zbd->SetReservedZones(reserved_zones_to_use);
  }

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, false);
  if ((s.ok() || !s.IsNotFound()) && !FLAGS_force) {
    fprintf(
        stderr,
        "Existing filesystem found, use --force if you want to replace it.\n");
    return 1;
  }

  zenFS.reset();

  // zbd = zbd_open(false, true);
  zbd = zbd_open(false, false, FLAGS_start_zone, FLAGS_end_zone, FLAGS_ao_zones);
  ZonedBlockDevice *zbdRaw = zbd.get();

  if (FLAGS_hyssd) {
    uint64_t reserved_zones_to_use;
    if (FLAGS_aux_size > 0) {
      reserved_zones_to_use = FLAGS_aux_size;
    } else {
      reserved_zones_to_use = zbdRaw->GetReservedZones();
      if (reserved_zones_to_use == 0) {
        fprintf(stderr,
                "ERROR: HYSSD mode but device reports 0 reserved zones.\n");
        return 1;
      }
    }
    zbdRaw->SetReservedZones(reserved_zones_to_use);
  }

  zenFS.reset(new ZenFS(zbd.release(), FileSystem::Default(), nullptr));

  AddDirSeparatorAtEnd(FLAGS_aux_path);

  s = zenFS->MkFS(FLAGS_aux_path, FLAGS_finish_threshold, FLAGS_enable_gc, FLAGS_wal_on_aux);
  if (!s.ok()) {
    fprintf(stderr, "Failed to create file system, error: %s\n",
            s.ToString().c_str());
    return 1;
  }

  fprintf(stdout, "ZenFS file system created. Free space: %lu MB\n",
          zbdRaw->GetFreeSpace() / (1024 * 1024));

  return 0;
}

void list_children(const std::unique_ptr<ZenFS> &zenFS,
                   const std::string &path) {
  IOOptions opts;
  IODebugContext dbg;
  std::vector<std::string> result;
  uint64_t size;
  IOStatus io_status = zenFS->GetChildren(path, opts, &result, &dbg);

  if (!io_status.ok()) {
    fprintf(stderr, "Error: %s %s\n", io_status.ToString().c_str(),
            path.c_str());
    return;
  }

  for (const auto &f : result) {
    io_status = zenFS->GetFileSize(path + f, opts, &size, &dbg);
    if (!io_status.ok()) {
      fprintf(stderr, "Failed to get size of file %s\n", f.c_str());
      return;
    }
    uint64_t mtime;
    io_status = zenFS->GetFileModificationTime(path + f, opts, &mtime, &dbg);
    if (!io_status.ok()) {
      fprintf(stderr,
              "Failed to get modification time of file %s, error = %s\n",
              f.c_str(), io_status.ToString().c_str());
      return;
    }
    time_t mt = (time_t)mtime;
    struct tm *fct = std::localtime(&mt);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%b %d %Y %H:%M:%S", fct);
    std::string mdtime;
    mdtime.assign(buf, sizeof(buf));

    fprintf(stdout, "%12lu\t%-32s%-32s\n", size, mdtime.c_str(), f.c_str());
  }
}

int zenfs_tool_list() {
  Status s;
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(true, false);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }
  AddDirSeparatorAtEnd(FLAGS_path);
  list_children(zenFS, FLAGS_path);

  return 0;
}

int zenfs_tool_df() {
  Status s;
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(true, false);
  if (!zbd) return 1;
  ZonedBlockDevice *zbdRaw = zbd.get();

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }
  uint64_t used = zbdRaw->GetUsedSpace();
  uint64_t free = zbdRaw->GetFreeSpace();
  uint64_t reclaimable = zbdRaw->GetReclaimableSpace();

  /* Avoid divide by zero */
  if (used == 0) used = 1;

  fprintf(stdout,
          "Free: %lu MB\nUsed: %lu MB\nReclaimable: %lu MB\nSpace "
          "amplification: %lu%%\n",
          free / (1024 * 1024), used / (1024 * 1024),
          reclaimable / (1024 * 1024), (100 * reclaimable) / used);

  return 0;
}

int zenfs_tool_lsuuid() {
  std::map<std::string, std::pair<std::string, ZbdBackendType>> zenFileSystems;
  Status s = ListZenFileSystems(zenFileSystems);
  if (!s.ok()) {
    fprintf(stderr, "Failed to enumerate file systems: %s",
            s.ToString().c_str());
    return 1;
  }

  for (const auto &p : zenFileSystems)
    fprintf(stdout, "%s\t%s\n", p.first.c_str(), p.second.first.c_str());

  return 0;
}

static std::map<std::string, Env::WriteLifeTimeHint> wlth_map;

Env::WriteLifeTimeHint GetWriteLifeTimeHint(const std::string &filename) {
  if (wlth_map.find(filename) != wlth_map.end()) {
    return wlth_map[filename];
  }
  return Env::WriteLifeTimeHint::WLTH_NOT_SET;
}

int SaveWriteLifeTimeHints() {
  std::ofstream wlth_file(FLAGS_path + "/write_lifetime_hints.dat");

  if (!wlth_file.is_open()) {
    fprintf(stderr, "Failed to store time hints\n");
    return 1;
  }

  for (auto it = wlth_map.begin(); it != wlth_map.end(); it++) {
    wlth_file << it->first << "\t" << it->second << "\n";
  }

  wlth_file.close();
  return 0;
}

void ReadWriteLifeTimeHints() {
  std::ifstream wlth_file(FLAGS_path + "/write_lifetime_hints.dat");

  if (!wlth_file.is_open()) {
    fprintf(stderr, "WARNING: failed to read write life times\n");
    return;
  }

  std::string filename;
  uint32_t lth;

  while (wlth_file >> filename >> lth) {
    wlth_map.insert(std::make_pair(filename, (Env::WriteLifeTimeHint)lth));
  }

  wlth_file.close();
}

IOStatus zenfs_tool_copy_file(FileSystem *f_fs, const std::string &f,
                              FileSystem *t_fs, const std::string &t) {
  FileOptions fopts;
  IOOptions iopts;
  IODebugContext dbg;
  IOStatus s;
  std::unique_ptr<FSSequentialFile> f_file;
  std::unique_ptr<FSWritableFile> t_file;
  size_t buffer_sz = 1024 * 1024;
  uint64_t to_copy;

  fprintf(stdout, "%s\n", f.c_str());

  s = f_fs->GetFileSize(f, iopts, &to_copy, &dbg);
  if (!s.ok()) {
    return s;
  }

  s = f_fs->NewSequentialFile(f, fopts, &f_file, &dbg);
  if (!s.ok()) {
    return s;
  }

  s = t_fs->NewWritableFile(t, fopts, &t_file, &dbg);
  if (!s.ok()) {
    return s;
  }

  t_file->SetWriteLifeTimeHint(GetWriteLifeTimeHint(t));

  std::unique_ptr<char[]> buffer{new (std::nothrow) char[buffer_sz]};
  if (!buffer) {
    return IOStatus::IOError("Failed to allocate copy buffer");
  }

  while (to_copy > 0) {
    size_t chunk_sz = to_copy;
    Slice chunk_slice;

    if (chunk_sz > buffer_sz) chunk_sz = buffer_sz;

    s = f_file->Read(chunk_sz, iopts, &chunk_slice, buffer.get(), &dbg);
    if (!s.ok()) {
      break;
    }

    s = t_file->Append(chunk_slice, iopts, &dbg);
    to_copy -= chunk_slice.size();
  }

  if (!s.ok()) {
    return s;
  }

  return t_file->Fsync(iopts, &dbg);
}

IOStatus zenfs_tool_copy_dir(FileSystem *f_fs, const std::string &f_dir,
                             FileSystem *t_fs, const std::string &t_dir) {
  IOOptions opts;
  IODebugContext dbg;
  IOStatus s;
  std::vector<std::string> files;

  s = f_fs->GetChildren(f_dir, opts, &files, &dbg);
  if (!s.ok()) {
    return s;
  }

  for (const auto &f : files) {
    std::string filename = f_dir + f;
    bool is_dir;

    if (f == "." || f == ".." || f == "write_lifetime_hints.dat") continue;

    s = f_fs->IsDirectory(filename, opts, &is_dir, &dbg);
    if (!s.ok()) {
      return s;
    }

    std::string dest_filename;

    if (t_dir == "") {
      dest_filename = f;
    } else {
      if (t_dir.back() == '/') {
        dest_filename = t_dir + f;
      } else {
        dest_filename = t_dir + "/" + f;
      }
    }

    if (is_dir) {
      s = t_fs->CreateDir(dest_filename, opts, &dbg);
      if (!s.ok()) {
        return s;
      }
      s = zenfs_tool_copy_dir(f_fs, filename + "/", t_fs, dest_filename);
      if (!s.ok()) {
        return s;
      }
    } else {
      s = zenfs_tool_copy_file(f_fs, filename, t_fs, dest_filename);
      if (!s.ok()) {
        return s;
      }
    }
  }

  return s;
}
IOStatus zenfs_create_directories(FileSystem *fs, std::string path) {
  std::string dir_name;
  IODebugContext dbg;
  IOOptions opts;
  IOStatus s;
  std::size_t p = 0;

  AddDirSeparatorAtEnd(path);

  while ((p = path.find_first_of('/', p)) != std::string::npos) {
    dir_name = path.substr(0, p++);
    if (dir_name.size() == 0) continue;
    s = fs->CreateDirIfMissing(dir_name, opts, &dbg);
    if (!s.ok()) break;
  }

  return s;
}

int zenfs_tool_backup() {
  Status status;
  IOStatus io_status;
  IOOptions opts;
  IODebugContext dbg;
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(true, true);

  if (!zbd) {
    if (FLAGS_force) {
      fprintf(stderr,
              "WARNING: attempting to back up a zoned block device in use! "
              "Expect data loss and corruption.\n");
      zbd = zbd_open(true, false);
    }
  }

  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  status = zenfs_mount(zbd, &zenFS, true);
  if (!status.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            status.ToString().c_str());
    return 1;
  }

  bool is_dir;
  io_status = zenFS->IsDirectory(FLAGS_backup_path, opts, &is_dir, &dbg);
  if (!io_status.ok()) {
    fprintf(stderr, "IsDirectory failed, error: %s\n",
            io_status.ToString().c_str());
    return 1;
  }

  if (!is_dir) {
    std::string dest_filename =
        FLAGS_path + "/" +
        FLAGS_backup_path.substr(FLAGS_backup_path.find_last_of('/') + 1);
    io_status =
        zenfs_tool_copy_file(zenFS.get(), FLAGS_backup_path,
                             FileSystem::Default().get(), dest_filename);
  } else {
    io_status =
        zenfs_create_directories(FileSystem::Default().get(), FLAGS_path);
    if (!io_status.ok()) {
      fprintf(stderr, "Create directory failed, error: %s\n",
              io_status.ToString().c_str());
      return 1;
    }

    std::string backup_path = FLAGS_backup_path;
    if (backup_path.size() > 0 && backup_path.back() != '/') backup_path += "/";
    io_status = zenfs_tool_copy_dir(zenFS.get(), backup_path,
                                    FileSystem::Default().get(), FLAGS_path);
  }
  if (!io_status.ok()) {
    fprintf(stderr, "Copy failed, error: %s\n", io_status.ToString().c_str());
    return 1;
  }

  wlth_map = zenFS->GetWriteLifeTimeHints();
  return SaveWriteLifeTimeHints();
}

int zenfs_tool_link() {
  Status s;
  IOStatus io_s;
  IOOptions iopts;
  IODebugContext dbg;

  if (FLAGS_src_file.empty() || FLAGS_dest_file.empty()) {
    fprintf(stderr, "Error: Specify --src_file and --dest_file to be linked\n");
    return 1;
  }
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, true);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, false);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }

  io_s = zenFS->LinkFile(FLAGS_src_file, FLAGS_dest_file, iopts, &dbg);
  if (!io_s.ok()) {
    fprintf(stderr, "Link failed, error: %s\n", io_s.ToString().c_str());
    return 1;
  }
  fprintf(stdout, "Linked file %s to %s\n", FLAGS_dest_file.c_str(),
          FLAGS_src_file.c_str());

  return 0;
}

int zenfs_tool_delete_file() {
  Status s;
  IOStatus io_s;
  IOOptions iopts;
  IODebugContext dbg;

  if (FLAGS_path.empty()) {
    fprintf(stderr, "Error: Specify --path of the file to be deleted.\n");
    return 1;
  }
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, true);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, false);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }

  io_s = zenFS->DeleteFile(FLAGS_path, iopts, &dbg);
  if (!io_s.ok()) {
    fprintf(stderr, "Delete failed with error: %s\n", io_s.ToString().c_str());
    return 1;
  }
  fprintf(stdout, "Deleted file %s\n", FLAGS_path.c_str());

  return 0;
}

int zenfs_tool_rename_file() {
  Status s;
  IOStatus io_s;
  IOOptions iopts;
  IODebugContext dbg;

  if (FLAGS_src_file.empty() || FLAGS_dest_file.empty()) {
    fprintf(stderr,
            "Error: Specify --src_file and --dest_file to be renamed\n");
    return 1;
  }
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, true);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, false);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }

  io_s = zenFS->RenameFile(FLAGS_src_file, FLAGS_dest_file, iopts, &dbg);
  if (!io_s.ok()) {
    fprintf(stderr, "Rename failed, error: %s\n", io_s.ToString().c_str());
    return 1;
  }
  fprintf(stdout, "Renamed file %s to %s\n", FLAGS_src_file.c_str(),
          FLAGS_dest_file.c_str());

  return 0;
}

int zenfs_tool_remove_directory() {
  Status s;
  IOStatus io_s;
  IOOptions iopts;
  IODebugContext dbg;

  if (FLAGS_path.empty()) {
    fprintf(stderr, "Error: Specify --path of the directory to be deleted.\n");
    return 1;
  }
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, true);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, false);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }

  if (FLAGS_force) {
    io_s = zenFS->DeleteDirRecursive(FLAGS_path, iopts, &dbg);
    if (!io_s.ok()) {
      fprintf(stderr,
              "Force delete for given directory failed with error: %s\n",
              io_s.ToString().c_str());
      return 1;
    }
    fprintf(stdout, "Force deleted directory %s\n", FLAGS_path.c_str());
  } else {
    io_s = zenFS->DeleteDir(FLAGS_path, iopts, &dbg);
    if (!io_s.ok()) {
      fprintf(stderr, "Delete for given directory failed with error: %s\n",
              io_s.ToString().c_str());
      return 1;
    }
    fprintf(stdout, "Deleted directory %s\n", FLAGS_path.c_str());
  }

  return 0;
}

int zenfs_tool_restore() {
  Status status;
  IOStatus io_status;
  IOOptions opts;
  IODebugContext dbg;
  bool is_dir;

  if (FLAGS_path.empty()) {
    fprintf(stderr, "Error: Specify --path to be restored.\n");
    return 1;
  }

  AddDirSeparatorAtEnd(FLAGS_restore_path);
  fs::path fpath(FLAGS_path);
  FLAGS_path = fpath.lexically_normal().string();
  FileSystem *f_fs = FileSystem::Default().get();
  status = f_fs->IsDirectory(FLAGS_path, opts, &is_dir, &dbg);
  if (!status.ok()) {
    fprintf(stderr, "IsDirectory failed, error: %s\n",
            status.ToString().c_str());
    return 1;
  }

  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, true);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  status = zenfs_mount(zbd, &zenFS, false);
  if (!status.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            status.ToString().c_str());
    return 1;
  }

  io_status = zenfs_create_directories(zenFS.get(), FLAGS_restore_path);
  if (!io_status.ok()) {
    fprintf(stderr, "Create directory failed, error: %s\n",
            io_status.ToString().c_str());
    return 1;
  }

  if (!is_dir) {
    std::string dest_file =
        FLAGS_restore_path + fpath.lexically_normal().filename().string();
    io_status = zenfs_tool_copy_file(f_fs, FLAGS_path, zenFS.get(), dest_file);
  } else {
    AddDirSeparatorAtEnd(FLAGS_path);
    ReadWriteLifeTimeHints();
    io_status =
        zenfs_tool_copy_dir(f_fs, FLAGS_path, zenFS.get(), FLAGS_restore_path);
  }

  if (!io_status.ok()) {
    fprintf(stderr, "Copy failed, error: %s\n", io_status.ToString().c_str());
    return 1;
  }

  return 0;
}

int zenfs_tool_dump() {
  Status s;
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(true, false);
  if (!zbd) return 1;
  ZonedBlockDevice *zbdRaw = zbd.get();

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }

  std::ostream &json_stream = std::cout;
  json_stream << "{\"zones\":";
  zbdRaw->EncodeJson(json_stream);
  json_stream << ",\"files\":";
  zenFS->EncodeJson(json_stream);
  json_stream << "}";

  return 0;
}

int zenfs_tool_fsinfo() {
  Status s;
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(true, false);
  if (!zbd) return 1;

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, true);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount filesystem, error: %s\n",
            s.ToString().c_str());
    return 1;
  }
  std::string superblock_report;
  zenFS->ReportSuperblock(&superblock_report);
  fprintf(stdout, "%s\n", superblock_report.c_str());
  return 0;
}

static int open_aux_path_file() {
  if (FLAGS_aux_path.empty()) {
    fprintf(stderr, "You need to specify --aux_path for F2FS operations\n");
    return -1;
  }

  // Create a temporary file path in aux_path for ioctl
  std::string file_path = FLAGS_aux_path;
  if (file_path.back() != '/') file_path += "/";
  file_path += ".zenfs_ioctl_tmp";

  printf("%s\n\n", file_path.c_str());
  
  // Try to open/create the file
  int fd = open(file_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    // If creation fails, try opening any existing file in aux_path
    DIR *dir = opendir(FLAGS_aux_path.c_str());
    if (!dir) {
      fprintf(stderr, "Failed to open aux_path directory: %s\n",
              strerror(errno));
      return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (entry->d_type == DT_REG) {
        file_path = FLAGS_aux_path;
        if (file_path.back() != '/') file_path += "/";
        file_path += entry->d_name;
        fd = open(file_path.c_str(), O_RDWR);
        if (fd >= 0) break;
      }
    }
    closedir(dir);

    if (fd < 0) {
      fprintf(stderr, "Failed to open any file in aux_path for ioctl: %s\n",
              strerror(errno));
      return -1;
    }
  }

  return fd;
}

// Migrate ZenFS data from specific zone range [start_zone, end_zone)
// This is needed when R-zone increases (S-zone -> R-zone conversion)
int zenfs_migrate_zones(int start_zone_id, int end_zone_id) {
  Status s;

  if (FLAGS_zbd.empty() && FLAGS_zonefs.empty()) {
    fprintf(stderr, "You need to specify --zbd or --zonefs for ZenFS migration\n");
    return 1;
  }

  fprintf(stdout, "ZenFS: Migrating data from zones [%d - %d)...\n",
          start_zone_id, end_zone_id);

  // Open ZenFS in read-write mode
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(false, false);
  if (!zbd) {
    fprintf(stdout, "Failed to open zoned block device\n");
    return 1;
  } 

  // Get zone size before mounting (zbd ownership will be transferred)
  uint64_t zone_size = zbd->GetZoneSize();

  std::unique_ptr<ZenFS> zenFS;
  s = zenfs_mount(zbd, &zenFS, false);
  if (!s.ok()) {
    fprintf(stderr, "Failed to mount ZenFS: %s\n", s.ToString().c_str());
    return 1;
  }

  // Get snapshot of current ZenFS state
  ZenFSSnapshot snapshot;
  ZenFSSnapshotOptions options;
  options.zone_ = 1;
  options.zone_file_ = 1;
  options.log_garbage_ = 0;
  zenFS->GetZenFSSnapshot(snapshot, options);

  // Find zones that need to be evacuated
  std::set<uint64_t> migrate_zones_start;
  for (int zone_id = start_zone_id; zone_id < end_zone_id; zone_id++) {
    uint64_t zone_start = static_cast<uint64_t>(zone_id) * zone_size;
    migrate_zones_start.insert(zone_start);
    fprintf(stdout, "  Target zone %d (start: 0x%lx)\n", zone_id, (unsigned long)zone_start);
  }

  // Find all extents in target zones
  std::vector<ZoneExtentSnapshot*> migrate_exts;
  for (auto& ext : snapshot.extents_) {
    if (migrate_zones_start.find(ext.zone_start) != migrate_zones_start.end()) {
      migrate_exts.push_back(&ext);
    }
  }

  if (migrate_exts.empty()) {
    fprintf(stdout, "ZenFS: No extents found in target zones, nothing to migrate\n");
    return 0;
  }

  fprintf(stdout, "ZenFS: Found %lu extents to migrate\n", migrate_exts.size());

  // Migrate extents
  uint64_t copied_data = 0;
  // Keep migrated data above the new ABA so the absorbed zones end up EMPTY.
  IOStatus ios = zenFS->MigrateExtents(migrate_exts, &copied_data,
                                       /*sst_only=*/false,
                                       (uint64_t)end_zone_id * zone_size);
  if (!ios.ok()) {
    fprintf(stderr, "ZenFS: Migration failed: %s\n", ios.ToString().c_str());
    return 1;
  }

  fprintf(stdout, "ZenFS: Successfully migrated %lu bytes from %lu extents\n",
          (unsigned long)copied_data, migrate_exts.size());

  // Note: Unused zones will be reset when ZenFS closes or on next write operation
  fprintf(stdout, "ZenFS: Zone migration completed successfully\n");
  return 0;
}

// Send F2FS gc_force for specific zone range
int f2fs_gc_force_zones(int start_zone_id, int end_zone_id) {
  int fd = open_aux_path_file();
  if (fd < 0) return 1;

  struct f2fs_gc_range range;
  int num_zones = end_zone_id - start_zone_id;

  range.sync = 1;  // Synchronous
  range.start = static_cast<__u64>(start_zone_id) * F2FS_ZONE_BLOCKS;
  range.len = static_cast<__u64>(num_zones - 1) * F2FS_ZONE_BLOCKS;
  range.force = 1;  // Force GC

  fprintf(stdout, "F2FS: Sending gc_force for zones [%d - %d) (start_block=%lu, len=%lu)\n",
          start_zone_id, end_zone_id,
          (unsigned long)range.start, (unsigned long)range.len);

  int ret = ioctl(fd, F2FS_IOC_GARBAGE_COLLECT_FORCE, &range);
  close(fd);

  if (ret < 0) {
    fprintf(stderr, "F2FS gc_force failed: %s\n", strerror(errno));
    return 1;
  }

  fprintf(stdout, "F2FS: gc_force completed for zones [%d - %d)\n",
          start_zone_id, end_zone_id);
  return 0;
}

// Send F2FS resize_cns ioctl
int f2fs_resize_cns_ioctl(int new_rzone) {
  int fd = open_aux_path_file();
  if (fd < 0) return 1;

  __u32 rzone = static_cast<__u32>(new_rzone);

  fprintf(stdout, "F2FS: Sending resize_cns (new_rzone=%u)\n", rzone);

  int ret = ioctl(fd, F2FS_IOC_RESIZE_CNS, &rzone);
  close(fd);

  if (ret < 0) {
    fprintf(stderr, "F2FS resize_cns failed: %s\n", strerror(errno));
    return 1;
  }

  fprintf(stdout, "F2FS: resize_cns completed (new_rzone=%u)\n", rzone);
  return 0;
}

int zenfs_tool_resize_cns() {
  if (FLAGS_new_rzone < 0) {
    fprintf(stderr, "You need to specify --new_rzone for resize-cns command\n");
    return 1;
  }

  if (FLAGS_zbd.empty() && FLAGS_zonefs.empty()) {
    fprintf(stderr, "You need to specify --zbd or --zonefs for resize-cns command\n");
    return 1;
  }

  // Open ZBD to get current reserved zone count
  std::unique_ptr<ZonedBlockDevice> zbd = zbd_open(true, false);  // readonly, non-exclusive
  if (!zbd) {
    fprintf(stderr, "Failed to open zoned block device\n");
    return 1;
  }

  uint64_t current_rzone = zbd->GetReservedZones();
  int new_rzone = FLAGS_new_rzone;

  fprintf(stdout, "Current R-zone count (from device): %lu\n", (unsigned long)current_rzone);

  if (new_rzone == (int)current_rzone) {
    fprintf(stdout, "new_rzone == current_rzone (%d), nothing to do\n", new_rzone);
    return 0;
  }

  fprintf(stdout, "=== ResizeCNS: %lu -> %d ===\n", (unsigned long)current_rzone, new_rzone);

  if (new_rzone > (int)current_rzone) {
    // R-zone INCREASE: S-zone -> R-zone (conventional)
    // ZenFS must evacuate data from zones [current_rzone, new_rzone) first
    fprintf(stdout, "\n[Step 1] R-zone increasing: ZenFS must evacuate zones [%lu - %d)\n",
            (unsigned long)current_rzone, new_rzone);

    // Close the readonly zbd before migration (migration will open its own)
    zbd.reset();

    int ret = zenfs_migrate_zones((int)current_rzone, new_rzone);
    if (ret != 0) {
      fprintf(stderr, "Failed to migrate ZenFS data\n");
      return ret;
    }

    // Now call F2FS resize_cns
    fprintf(stdout, "\n[Step 2] Calling F2FS resize_cns\n");
    ret = f2fs_resize_cns_ioctl(new_rzone);
    if (ret != 0) {
      fprintf(stderr, "Failed to modify F2FS R-zone\n");
      return ret;
    }

  } else {
    // R-zone DECREASE: R-zone (conventional) -> S-zone.
    // Single resize_cns ioctl: the kernel gates the aux F2FS usable area down
    // (free_segment_range drain + MAIN_SECS gate + freeze_super + checkpoint)
    // BEFORE the device ABA change, so no separate gc_force is needed.
    fprintf(stdout, "\n[Shrink] R-zone decreasing: F2FS gate-down + device ABA in one ioctl\n");

    // Close the zbd before F2FS operations
    zbd.reset();

    int ret = f2fs_resize_cns_ioctl(new_rzone);
    if (ret != 0) {
      fprintf(stderr, "Failed to modify F2FS R-zone\n");
      return ret;
    }
  }

  fprintf(stdout, "\n=== ResizeCNS completed: %lu -> %d ===\n", (unsigned long)current_rzone, new_rzone);
  return 0;
}

int zenfs_tool_gc_force() {
  if (FLAGS_gc_num_zones <= 0) {
    fprintf(stderr, "gc_num_zones must be > 0\n");
    return 1;
  }

  int fd = open_aux_path_file();
  if (fd < 0) return 1;

  struct f2fs_gc_range range;
  __u64 start_zone = static_cast<__u64>(FLAGS_gc_start_zone);
  __u64 num_zones = static_cast<__u64>(FLAGS_gc_num_zones) - 1;
  __u64 end_zone = start_zone + num_zones;

  range.sync = 1;  // Synchronous
  range.start = start_zone * F2FS_ZONE_BLOCKS;
  range.len = num_zones * F2FS_ZONE_BLOCKS;
  range.force = 1;  // Force GC

  fprintf(stdout, "Sending F2FS gc_force: zones[%lu - %lu] (start_block=%lu, len=%lu) to %s\n",
          (unsigned long)start_zone, (unsigned long)end_zone,
          (unsigned long)range.start, (unsigned long)range.len,
          FLAGS_aux_path.c_str());

  int ret = ioctl(fd, F2FS_IOC_GARBAGE_COLLECT_FORCE, &range);
  close(fd);

  if (ret < 0) {
    fprintf(stderr, "F2FS_IOC_GARBAGE_COLLECT_FORCE failed: %s\n", strerror(errno));
    return 1;
  }

  fprintf(stdout, "Success: gc_force completed for zones[%lu - %lu]\n",
          (unsigned long)start_zone, (unsigned long)end_zone);
  return 0;
}

int zenfs_tool_gc_n_modify() {
  if (FLAGS_gc_num_zones <= 0) {
    fprintf(stderr, "gc_num_zones must be > 0\n");
    return 1;
  }

  int fd = open_aux_path_file();
  if (fd < 0) return 1;

  struct f2fs_gc_range range;
  __u64 start_zone = static_cast<__u64>(FLAGS_gc_start_zone);
  __u64 num_zones = static_cast<__u64>(FLAGS_gc_num_zones) - 1;
  __u64 end_zone = start_zone + num_zones;

  range.sync = 1;  // Synchronous
  range.start = start_zone * F2FS_ZONE_BLOCKS;
  range.len = num_zones * F2FS_ZONE_BLOCKS;
  range.force = 1;  // Force GC

  fprintf(stdout, "Sending F2FS gc_n_modify: zones[%lu - %lu] (start_block=%lu, len=%lu) to %s\n",
          (unsigned long)start_zone, (unsigned long)end_zone,
          (unsigned long)range.start, (unsigned long)range.len,
          FLAGS_aux_path.c_str());
  fprintf(stdout, "  (new_rzone will be increased by %d zones)\n", FLAGS_gc_num_zones);

  int ret = ioctl(fd, F2FS_IOC_GC_FORCE_N_MODIFY, &range);
  close(fd);

  if (ret < 0) {
    fprintf(stderr, "F2FS_IOC_GC_FORCE_N_MODIFY failed: %s\n", strerror(errno));
    return 1;
  }

  fprintf(stdout, "Success: gc_n_modify completed for zones[%lu - %lu]\n",
          (unsigned long)start_zone, (unsigned long)end_zone);
  return 0;
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char **argv) {
  gflags::SetUsageMessage(
      std::string("\nUSAGE:\n") + argv[0] +
      +" <command> [OPTIONS]...\nCommands: mkfs, list, ls-uuid, " +
      +"df, backup, restore, dump, fs-info, link, delete, rename, rmdir");
  if (argc < 2) {
    fprintf(stderr, "You need to specify a command:\n");
    fprintf(stderr,
            "\t./zenfs [list | ls-uuid | df | backup | restore | dump | "
            "fs-info | link | delete | rename | rmdir]\n");
    return 1;
  }

  gflags::SetVersionString(ZENFS_VERSION);
  std::string subcmd(argv[1]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // F2FS/HYSSD modify commands operate via the aux mount; gc-* need only
  // --aux_path, resize-cns also needs --zbd (it migrates ZenFS extents).
  bool is_f2fs_cmd = (subcmd == "resize-cns" || subcmd == "gc-force" ||
                      subcmd == "gc-n-modify");
  if (FLAGS_zonefs.empty() && FLAGS_zbd.empty() && subcmd != "ls-uuid" &&
      !is_f2fs_cmd) {
    fprintf(
        stderr,
        "You need to specify a zoned block device using --zbd or --zonefs\n");
    return 1;
  }
  if (!FLAGS_zonefs.empty() && !FLAGS_zbd.empty()) {
    fprintf(stderr,
            "You need to specify a zoned block device using either "
            "--zbd or --zonefs - not both\n");
    return 1;
  }
  if (subcmd == "mkfs") {
    return ROCKSDB_NAMESPACE::zenfs_tool_mkfs();
  } else if (subcmd == "list") {
    return ROCKSDB_NAMESPACE::zenfs_tool_list();
  } else if (subcmd == "ls-uuid") {
    return ROCKSDB_NAMESPACE::zenfs_tool_lsuuid();
  } else if (subcmd == "df") {
    return ROCKSDB_NAMESPACE::zenfs_tool_df();
  } else if (subcmd == "backup") {
    return ROCKSDB_NAMESPACE::zenfs_tool_backup();
  } else if (subcmd == "restore") {
    return ROCKSDB_NAMESPACE::zenfs_tool_restore();
  } else if (subcmd == "dump") {
    return ROCKSDB_NAMESPACE::zenfs_tool_dump();
  } else if (subcmd == "fs-info") {
    return ROCKSDB_NAMESPACE::zenfs_tool_fsinfo();
  } else if (subcmd == "link") {
    return ROCKSDB_NAMESPACE::zenfs_tool_link();
  } else if (subcmd == "delete") {
    return ROCKSDB_NAMESPACE::zenfs_tool_delete_file();
  } else if (subcmd == "rename") {
    return ROCKSDB_NAMESPACE::zenfs_tool_rename_file();
  } else if (subcmd == "rmdir") {
    return ROCKSDB_NAMESPACE::zenfs_tool_remove_directory();
  } else if (subcmd == "resize-cns") {
    return ROCKSDB_NAMESPACE::zenfs_tool_resize_cns();
  } else if (subcmd == "gc-force") {
    return ROCKSDB_NAMESPACE::zenfs_tool_gc_force();
  } else if (subcmd == "gc-n-modify") {
    return ROCKSDB_NAMESPACE::zenfs_tool_gc_n_modify();
  } else {
    fprintf(stderr, "Subcommand not recognized: %s\n", subcmd.c_str());
    return 1;
  }

  return 0;
}
