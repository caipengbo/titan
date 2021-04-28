#include "titan_checkpoint_impl.h"
#include "util.h"
#include "version_edit.h"

#include <cinttypes>
#include "file/file_util.h"
#include "file/filename.h"
#include "port/port.h"
#include "rocksdb/transaction_log.h"
#include "test_util/sync_point.h"

namespace rocksdb {
namespace titandb {

Status Checkpoint::Create(TitanDB* db, Checkpoint** checkpoint_ptr) {
  *checkpoint_ptr = new TitanCheckpointImpl(db);
  return Status::OK();
}

Status Checkpoint::CreateCheckpoint(const std::string& /*checkpoint_dir*/,
                                    uint64_t /*log_size_for_flush*/) {
  return Status::NotSupported("TitanDB doesn't support this operation");
}

void TitanCheckpointImpl::CleanStagingDirectory(
    const std::string& full_private_path, Logger* info_log) {
  std::vector<std::string> subchildren;
  Status s = db_->GetEnv()->FileExists(full_private_path);
  if (s.IsNotFound()) {
    return;
  }
  ROCKS_LOG_INFO(info_log, "File exists %s -- %s",
                 full_private_path.c_str(), s.ToString().c_str());
  s = db_->GetEnv()->GetChildren(full_private_path, &subchildren);
  if (s.ok()) {
    for (auto& subchild : subchildren) {
      std::string subchild_path = full_private_path + "/" + subchild;
      if (subchild == "titandb") {
        CleanStagingDirectory(subchild_path, info_log);
        ROCKS_LOG_INFO(info_log, "Clean titandb directory %s", subchild_path.c_str());
      } else {
        s = db_->GetEnv()->DeleteFile(subchild_path);
        ROCKS_LOG_INFO(info_log, "Delete file %s -- %s", subchild_path.c_str(),
                       s.ToString().c_str());
      }
    }
  }
  // Finally delete the private dir
  s = db_->GetEnv()->DeleteDir(full_private_path);
  ROCKS_LOG_INFO(info_log, "Delete dir %s -- %s",
                 full_private_path.c_str(), s.ToString().c_str());
}

// Builds an openable checkpoint of TitanDB
Status TitanCheckpointImpl::CreateCheckpoint(const std::string& checkpoint_dir,
                                        uint64_t log_size_for_flush) {
  DBOptions db_options = db_->GetDBOptions();
  Status s = db_->GetEnv()->FileExists(checkpoint_dir);
  if (s.ok()) {
    return Status::InvalidArgument("Directory exists");
  } else if (!s.IsNotFound()) {
    assert(s.IsIOError());
    return s;
  }

  ROCKS_LOG_INFO(
          db_options.info_log,
          "Started the checkpoint process -- creating checkpoint in directory %s",
          checkpoint_dir.c_str());

  size_t final_nonslash_idx = checkpoint_dir.find_last_not_of('/');
  if (final_nonslash_idx == std::string::npos) {
    // npos means it's only slashes or empty. Non-empty means it's the root
    // directory, but it shouldn't be because we verified above the directory
    // doesn't exist.
    assert(checkpoint_dir.empty());
    return Status::InvalidArgument("Invalid checkpoint directory name");
  }

  std::string full_private_path =
          checkpoint_dir.substr(0, final_nonslash_idx + 1) + ".tmp";
  ROCKS_LOG_INFO(
          db_options.info_log,
          "Checkpoint process -- using temporary directory %s",
          full_private_path.c_str());
  CleanStagingDirectory(full_private_path, db_options.info_log.get());
  // Create checkpoint directory and subdirectory
  s = db_->GetEnv()->CreateDir(full_private_path);
  if (s.ok()) {
    s = db_->GetEnv()->CreateDir(full_private_path + "/titandb");
  }
  uint64_t sequence_number = 0;
  if (s.ok()) {
    // Disable file deletions
    s = db_->DisableFileDeletions();
    const bool disabled_file_deletions = s.ok();
    if (s.ok()) {
      s = CreateCustomCheckpoint(
              db_options,
              [&](const std::string& src_dirname, const std::string& fname,
                  FileType) {
                ROCKS_LOG_INFO(db_options.info_log, "Hard Linking %s", fname.c_str());
                return db_->GetEnv()->LinkFile(src_dirname + fname,
                                               full_private_path + fname);
              } /* link_file_cb */,
              [&](const std::string& src_dirname, const std::string& fname,
                  uint64_t size_limit_bytes, FileType) {
                ROCKS_LOG_INFO(db_options.info_log, "Copying %s", fname.c_str());
                return CopyFile(db_->GetEnv(), src_dirname + fname,
                                full_private_path + fname, size_limit_bytes,
                                db_options.use_fsync);
              } /* copy_file_cb */,
              [&](const std::string& fname, const std::string& contents, FileType) {
                ROCKS_LOG_INFO(db_options.info_log, "Creating %s", fname.c_str());
                return CreateFile(db_->GetEnv(), full_private_path + fname, contents,
                                  db_options.use_fsync);
              } /* create_file_cb */,
              &sequence_number, log_size_for_flush, full_private_path);
    }

    if (disabled_file_deletions) {
      // We copied all the files, enable file deletions
      Status ss = db_->EnableFileDeletions(false);
      assert(ss.ok());
    }

  }

  if (s.ok()) {
    // Move tmp private backup to real checkpoint directory
    s = db_->GetEnv()->RenameFile(full_private_path, checkpoint_dir);
  }
  if (s.ok()) {
    std::unique_ptr<Directory> checkpoint_directory;
    db_->GetEnv()->NewDirectory(checkpoint_dir, &checkpoint_directory);
    if (checkpoint_directory != nullptr) {
      s = checkpoint_directory->Fsync();
    }
  }

  if (s.ok()) {
    // Here we know that we succeeded and installed the new checkpoint
    ROCKS_LOG_INFO(db_options.info_log, "Checkpoint DONE. All is good");
    ROCKS_LOG_INFO(db_options.info_log, "Checkpoint sequence number: %" PRIu64,
                   sequence_number);
  } else {
    // Clean all the files we might have created
    ROCKS_LOG_INFO(db_options.info_log, "Checkpoint failed -- %s",
                   s.ToString().c_str());
    CleanStagingDirectory(full_private_path, db_options.info_log.get());
  }
  return s;
}

Status TitanCheckpointImpl::CreateCustomCheckpoint(
        const DBOptions& db_options,
        std::function<Status(const std::string& src_dirname,
                             const std::string& src_fname, FileType type)>
        link_file_cb,
        std::function<Status(const std::string& src_dirname,
                             const std::string& src_fname,
                             uint64_t size_limit_bytes, FileType type)>
        copy_file_cb,
        std::function<Status(const std::string& fname, const std::string& contents,
                             FileType type)>
        create_file_cb,
        uint64_t* sequence_number, uint64_t log_size_for_flush,
        const std::string full_private_path) {
  Status s;
  std::vector<std::string> live_files;
  std::vector<std::string> titan_live_files;
  std::vector<VersionEdit> version_edits;
  uint64_t base_manifest_file_size = 0;
  uint64_t min_log_num = port::kMaxUint64;
  *sequence_number = db_->GetLatestSequenceNumber();
  bool same_fs = true;
  VectorLogPtr live_wal_files;

  bool flush_memtable = true;
  if (!db_options.allow_2pc) {
    if (log_size_for_flush == port::kMaxUint64) {
      flush_memtable = false;
    } else if (log_size_for_flush > 0) {
      // if out standing log files are small, we skip the flush.
      s = db_->GetSortedWalFiles(live_wal_files);

      if (!s.ok()) {
        return s;
      }

      // Don't flush column families if total log size is smaller than
      // log_size_for_flush. We copy the log files instead.
      // We may be able to cover 2PC case too.
      uint64_t total_wal_size = 0;
      for (auto& wal : live_wal_files) {
        total_wal_size += wal->SizeFileBytes();
      }
      if (total_wal_size < log_size_for_flush) {
        flush_memtable = false;
      }
      live_wal_files.clear();
    }
  }

  // This will return live files prefixed with "/"
  s = db_->GetTitanLiveFiles(live_files, &base_manifest_file_size,
                              &version_edits, flush_memtable);
  
  if (s.ok() && db_options.allow_2pc) {
    // If 2PC is enabled, we need to get minimum log number after the flush.
    // Need to refetch the live files to recapture the checkpoint.
    if (!db_->GetIntProperty(DB::Properties::kMinLogNumberToKeep,
                              &min_log_num)) {
      return Status::InvalidArgument(
              "2PC enabled but cannot fine the min log number to keep.");
    }
    // GetTitanLiveFiles() calls the rocksdb::DB::GetLiveFiles() internally.
    // We need to refetch live files with flush to handle this case:
    // A previous 000001.log contains the prepare record of transaction tnx1.
    // The current log file is 000002.log, and sequence_number points to this
    // file.
    // After calling rocksdb::DB::GetLiveFiles(), 000003.log is created.
    // Then tnx1 is committed. The commit record is written to 000003.log.
    // Now we fetch min_log_num, which will be 3.
    // Then only 000002.log and 000003.log will be copied, and 000001.log will
    // be skipped. 000003.log contains commit message of tnx1, but we don't
    // have respective prepare record for it.
    // In order to avoid this situation, we need to force flush to make sure
    // all transactions committed before getting min_log_num will be flushed
    // to SST files.
    // We cannot get min_log_num before calling the rocksdb::DB::GetLiveFiles()
    // for the first time, because if we do that, all the logs files will be
    // included, far more than needed.
    s = db_->GetTitanLiveFiles(live_files, &base_manifest_file_size,
                              &version_edits, flush_memtable);
  }

  TEST_SYNC_POINT("TitanCheckpointImpl::CreateCheckpoint:SavedLiveFiles1");
  TEST_SYNC_POINT("TitanCheckpointImpl::CreateCheckpoint:SavedLiveFiles2");
  db_->FlushWAL(false /* sync */);
  
  // if we have more than one column family, we need to also get WAL files
  if (s.ok()) {
    s = db_->GetSortedWalFiles(live_wal_files);
  }
  if (!s.ok()) {
    return s;
  }

  size_t wal_size = live_wal_files.size();

  // copy/hard link live_files
  std::string base_manifest_fname, base_current_fname;
  std::string titan_manifest_fname, titan_current_fname;
  for (size_t i = 0; s.ok() && i < live_files.size(); ++i) {
    uint64_t number;
    FileType type;
    bool ok, in_titan_dir = false;
    // the filename prefix is '/titandb'
    if (live_files[i].rfind("/titandb", 0) == 0) {
      in_titan_dir = true;
      ok = ParseFileName(live_files[i].substr(8), &number, &type);
    } else {
      ok = ParseFileName(live_files[i], &number, &type);
    }

    if (!ok) {
      s = Status::Corruption("Can't parse file name. This is very bad");
      break;
    }
    // we should only get sst, blob, options, manifest and current files here
    assert(type == kTableFile || type == kBlobFile || type == kDescriptorFile ||
           type == kCurrentFile || type == kOptionsFile);
    assert(live_files[i].size() > 0 && live_files[i][0] == '/');
    if (type == kCurrentFile) {
      // We will craft the current file manually to ensure it's consistent with
      // the manifest number. This is necessary because current's file contents
      // can change during checkpoint creation.
      if (in_titan_dir) {
        titan_current_fname = live_files[i];
      } else {
        base_current_fname = live_files[i];
      }
      continue;
    } else if (type == kDescriptorFile) {
      if (in_titan_dir) {
        titan_manifest_fname = live_files[i];
      } else {
        base_manifest_fname = live_files[i];
      }
    }
    std::string src_fname = live_files[i];

    // Rules:
    // * If it's kTableFile/kBlobFile, then it's shared
    // * If it's kDescriptorFile of base db, limit the size to base_manifest_file_size
    // * If it's kDescriptorFile of titandb, craft the manifest based on all blob file
    // * Always copy if cross-device link
    if ((type == kTableFile || type == kBlobFile) && same_fs) {
      s = link_file_cb(db_->GetName(), src_fname, type);
      if (s.IsNotSupported()) {
        same_fs = false;
        s = Status::OK();
      }
    }
    if ((type != kTableFile && type != kBlobFile) || (!same_fs)) {
      if (type == kDescriptorFile) {
        if (in_titan_dir) {
          // Craft titan manifest file, ensure include all titan file.
          CreateTitanManifest(db_->GetEnv(), db_->GetDBOptions().use_fsync,
                             full_private_path+src_fname, &version_edits);
        } else {
          s = copy_file_cb(db_->GetName(), src_fname, base_manifest_file_size, type); 
        }
      } else {
        s = copy_file_cb(db_->GetName(), src_fname, 0, type);        
      }
    }
    
  }
  // Write manifest name to CURRENT file
  if (s.ok() && !base_current_fname.empty() && !base_manifest_fname.empty()) {
    create_file_cb(base_current_fname, base_manifest_fname.substr(1) + "\n",
                   kCurrentFile);
  }
  if (s.ok() && !titan_current_fname.empty() && !titan_manifest_fname.empty()) {
    create_file_cb(titan_current_fname, titan_manifest_fname.substr(9) + "\n",
                   kCurrentFile);
  }

  ROCKS_LOG_INFO(db_options.info_log, "Number of log files %" ROCKSDB_PRIszt,
                 live_wal_files.size());

  // Link WAL files. Copy exact size of last one because it is the only one
  // that has changes after the last flush.
  for (size_t i = 0; s.ok() && i < wal_size; ++i) {
    if ((live_wal_files[i]->Type() == kAliveLogFile) &&
        (!flush_memtable ||
         live_wal_files[i]->StartSequence() >= *sequence_number ||
         live_wal_files[i]->LogNumber() >= min_log_num)) {
      if (i + 1 == wal_size) {
        s = copy_file_cb(db_options.wal_dir, live_wal_files[i]->PathName(),
                         live_wal_files[i]->SizeFileBytes(), kLogFile);
        break;
      }
      if (same_fs) {
        // we only care about live log files
        s = link_file_cb(db_options.wal_dir, live_wal_files[i]->PathName(),
                         kLogFile);
        if (s.IsNotSupported()) {
          same_fs = false;
          s = Status::OK();
        }
      }
      if (!same_fs) {
        s = copy_file_cb(db_options.wal_dir, live_wal_files[i]->PathName(), 0,
                         kLogFile);
      }
    }
  }

  return s;
}

}  // namespace titandb
}  // namespace rocksdb
