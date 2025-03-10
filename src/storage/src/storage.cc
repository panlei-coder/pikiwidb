//  Copyright (c) 2017-present, Qihoo, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include <algorithm>
#include <filesystem>
#include <future>
#include <string_view>
#include <utility>
#include <vector>

#include "binlog.pb.h"
#include "config.h"
#include "pstd/log.h"
#include "pstd/pikiwidb_slot.h"
#include "pstd/pstd_string.h"
#include "rocksdb/utilities/checkpoint.h"
#include "scope_snapshot.h"
#include "src/lru_cache.h"
#include "src/mutex_impl.h"
#include "src/options_helper.h"
#include "src/redis.h"
#include "src/redis_hyperloglog.h"
#include "src/type_iterator.h"
#include "storage/slot_indexer.h"
#include "storage/storage.h"
#include "storage/util.h"

#define PRAFT_SNAPSHOT_META_FILE "__raft_snapshot_meta"
#define SST_FILE_EXTENSION ".sst"

namespace storage {
extern std::string BitOpOperate(BitOpType op, const std::vector<std::string>& src_values, int64_t max_len);
class Redis;

Status StorageOptions::ResetOptions(const OptionType& option_type,
                                    const std::unordered_map<std::string, std::string>& options_map) {
  std::unordered_map<std::string, MemberTypeInfo>& options_member_type_info = mutable_cf_options_member_type_info;
  char* opt = reinterpret_cast<char*>(static_cast<rocksdb::ColumnFamilyOptions*>(&options));
  if (option_type == OptionType::kDB) {
    options_member_type_info = mutable_db_options_member_type_info;
    opt = reinterpret_cast<char*>(static_cast<rocksdb::DBOptions*>(&options));
  }
  for (const auto& option_member : options_map) {
    try {
      auto iter = options_member_type_info.find(option_member.first);
      if (iter == options_member_type_info.end()) {
        return Status::InvalidArgument("Unsupport option member: " + option_member.first);
      }
      const auto& member_info = iter->second;
      if (!ParseOptionMember(member_info.type, option_member.second, opt + member_info.offset)) {
        return Status::InvalidArgument("Error parsing option member " + option_member.first);
      }
    } catch (std::exception& e) {
      return Status::InvalidArgument("Error parsing option member " + option_member.first + ":" +
                                     std::string(e.what()));
    }
  }
  return Status::OK();
}

Storage::Storage() {
  cursors_store_ = std::make_unique<LRUCache<std::string, std::string>>();
  cursors_store_->SetCapacity(5000);

  Status s = StartBGThread();
  if (!s.ok()) {
    ERROR("start bg thread failed, ", s.ToString());
  }
}

Storage::~Storage() {
  INFO("Storage begin to clear storage!");
  bg_tasks_should_exit_.store(true);
  bg_tasks_cond_var_.notify_one();
  if (is_opened_.load()) {
    INFO("Storage begin to clear all instances!");
    int ret = 0;
    if (ret = pthread_join(bg_tasks_thread_id_, nullptr); ret != 0) {
      ERROR("pthread_join failed with bgtask thread error : {}", ret);
    }
    insts_.clear();
  }
}

Status Storage::Close() {
  if (!is_opened_.load()) {
    return Status::OK();
  }
  is_opened_.store(false);
  for (auto& inst : insts_) {
    inst->SetNeedClose(true);
  }
  return Status::OK();
}

static std::string AppendSubDirectory(const std::string& db_path, int index) {
  if (db_path.back() == '/') {
    return db_path + std::to_string(index);
  } else {
    return db_path + "/" + std::to_string(index);
  }
}

static int RecursiveLinkAndCopy(const std::filesystem::path& source, const std::filesystem::path& destination) {
  if (std::filesystem::is_regular_file(source)) {
    if (source.filename() == PRAFT_SNAPSHOT_META_FILE) {
      return 0;
    } else if (source.extension() == SST_FILE_EXTENSION) {
      // Create a hard link
      if (::link(source.c_str(), destination.c_str()) != 0) {
        WARN("hard link file {} fail", source.string());
        return -1;
      }
      DEBUG("hard link success! source_file = {} , destination_file = {}", source.string(), destination.string());
    } else {
      // Copy the file
      if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing)) {
        WARN("copy file {} fail", source.string());
        return -1;
      }
      DEBUG("copy success! source_file = {} , destination_file = {}", source.string(), destination.string());
    }
  } else {
    if (!pstd::FileExists(destination)) {
      if (pstd::CreateDir(destination) != 0) {
        WARN("create dir {} fail", destination.string());
        return -1;
      }
    }

    for (const auto& entry : std::filesystem::directory_iterator(source)) {
      if (RecursiveLinkAndCopy(entry.path(), destination / entry.path().filename()) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

Status Storage::Open(const StorageOptions& storage_options, const std::string& db_path) {
  mkpath(db_path.c_str(), 0755);
  db_instance_num_ = storage_options.db_instance_num;
  // Temporarily set to 100000
  LogIndexAndSequenceCollector::max_gap_.store(storage_options.max_gap);
  storage_options.options.write_buffer_manager =
      std::make_shared<rocksdb::WriteBufferManager>(storage_options.mem_manager_size);
  for (size_t index = 0; index < db_instance_num_; index++) {
    insts_.emplace_back(std::make_unique<Redis>(this, index));
    Status s = insts_.back()->Open(storage_options, AppendSubDirectory(db_path, index));
    if (!s.ok()) {
      ERROR("open RocksDB{} failed {}", index, s.ToString());
      return Status::IOError();
    }
    INFO("open RocksDB{} success!", index);
  }

  slot_indexer_ = std::make_unique<SlotIndexer>(db_instance_num_);
  db_id_ = storage_options.db_id;

  is_opened_.store(true);
  return Status::OK();
}

std::vector<std::future<Status>> Storage::CreateCheckpoint(const std::string& checkpoint_path) {
  INFO("DB{} begin to generate a checkpoint to {}", db_id_, checkpoint_path);
  //  auto source_dir = AppendSubDirectory(checkpoint_path, db_id_);

  std::vector<std::future<Status>> result;
  result.reserve(db_instance_num_);
  for (int i = 0; i < db_instance_num_; ++i) {
    // In a new thread, create a checkpoint for the specified rocksdb i.
    auto res = std::async(std::launch::async, &Storage::CreateCheckpointInternal, this, checkpoint_path, i);
    result.push_back(std::move(res));
  }
  return result;
}

Status Storage::CreateCheckpointInternal(const std::string& checkpoint_path, int index) {
  auto source_dir = AppendSubDirectory(checkpoint_path, index);

  auto tmp_dir = source_dir + ".tmp";
  // 1) Make sure the temporary directory does not exist
  if (!pstd::DeleteDirIfExist(tmp_dir)) {
    WARN("DB{}'s RocksDB {} delete directory fail!", db_id_, index);
    return Status::IOError("DeleteDirIfExist() fail! dir_name : {} ", tmp_dir);
  }

  // 2) Create checkpoint object of this RocksDB
  rocksdb::Checkpoint* checkpoint = nullptr;
  auto db = insts_[index]->GetDB();
  rocksdb::Status s = rocksdb::Checkpoint::Create(db, &checkpoint);
  if (!s.ok()) {
    WARN("DB{}'s RocksDB {} create checkpoint object failed!. Error: ", db_id_, index, s.ToString());
    return s;
  }

  // 3) Create a checkpoint
  std::unique_ptr<rocksdb::Checkpoint> checkpoint_guard(checkpoint);
  s = checkpoint->CreateCheckpoint(tmp_dir, kFlush, nullptr);
  if (!s.ok()) {
    WARN("DB{}'s RocksDB {} create checkpoint failed!. Error: {}", db_id_, index, s.ToString());
    return s;
  }

  // 4) Make sure the source directory does not exist
  if (!pstd::DeleteDirIfExist(source_dir)) {
    WARN("DB{}'s RocksDB {} delete directory {} fail!", db_id_, index, source_dir);
    if (!pstd::DeleteDirIfExist(tmp_dir)) {
      WARN("DB{}'s RocksDB {} fail to delete the temporary directory {} ", db_id_, index, tmp_dir);
    }
    return Status::IOError("DeleteDirIfExist() fail! dir_name : {} ", source_dir);
  }

  // 5) Rename the temporary directory to source directory
  if (auto status = pstd::RenameFile(tmp_dir, source_dir); status != 0) {
    WARN("DB{}'s RocksDB {} rename temporary directory {} to source directory {} fail!", db_id_, index, tmp_dir,
         source_dir);
    if (!pstd::DeleteDirIfExist(tmp_dir)) {
      WARN("DB{}'s RocksDB {} fail to delete the rename failed directory {} ", db_id_, index, tmp_dir);
    }
    return Status::IOError("Rename directory {} fail!", tmp_dir);
  }

  INFO("DB{}'s RocksDB {} create checkpoint {} success!", db_id_, index, source_dir);
  return Status::OK();
}

std::vector<std::future<Status>> Storage::LoadCheckpoint(const std::string& checkpoint_sub_path,
                                                         const std::string& db_sub_path) {
  INFO("DB{} begin to load a checkpoint from {} to {}", db_id_, checkpoint_sub_path, db_sub_path);
  std::vector<std::future<Status>> result;
  result.reserve(db_instance_num_);
  for (int i = 0; i < db_instance_num_; ++i) {
    // In a new thread, Load a checkpoint for the specified rocksdb i
    auto res =
        std::async(std::launch::async, &Storage::LoadCheckpointInternal, this, checkpoint_sub_path, db_sub_path, i);
    result.push_back(std::move(res));
  }
  return result;
}

Status Storage::LoadCheckpointInternal(const std::string& checkpoint_sub_path, const std::string& db_sub_path,
                                       int index) {
  auto rocksdb_path = AppendSubDirectory(db_sub_path, index);  // ./db/db_id/index
  auto tmp_rocksdb_path = rocksdb_path + ".tmp";               // ./db/db_id/index.tmp

  auto source_dir = AppendSubDirectory(checkpoint_sub_path, index);
  // 1) Rename the original db to db.tmp, and only perform the maximum possible recovery of data
  // when loading the checkpoint fails.
  if (auto status = pstd::RenameFile(rocksdb_path, tmp_rocksdb_path); status != 0) {
    WARN("DB{}'s RocksDB {} rename db directory {} to temporary directory {} fail!", db_id_, index, rocksdb_path,
         tmp_rocksdb_path);
    return Status::IOError("Rename directory {} fail!", rocksdb_path);
  }

  // 2) Create a db directory to save the checkpoint.
  if (0 != pstd::CreatePath(rocksdb_path)) {
    pstd::RenameFile(tmp_rocksdb_path, rocksdb_path);
    WARN("DB{}'s RocksDB {} load a checkpoint from {} fail!", db_id_, index, checkpoint_sub_path);
    return Status::IOError("Create directory {} fail!", rocksdb_path);
  }
  if (RecursiveLinkAndCopy(source_dir, rocksdb_path) != 0) {
    pstd::DeleteDir(rocksdb_path);
    pstd::RenameFile(tmp_rocksdb_path, rocksdb_path);
    WARN("DB{}'s RocksDB {} load a checkpoint from {} fail!", db_id_, index, source_dir);
    return Status::IOError("recursive link and copy directory {} fail!", rocksdb_path);
  }

  // 3) Destroy the db.tmp directory.
  if (auto s = rocksdb::DestroyDB(tmp_rocksdb_path, rocksdb::Options()); !s.ok()) {
    WARN("Failure to destroy the old DB, path = {}", tmp_rocksdb_path);
  }
  return Status::OK();
}

Status Storage::LoadCursorStartKey(const DataType& dtype, int64_t cursor, char* type, std::string* start_key) {
  std::string index_key = DataTypeTag[dtype] + std::to_string(cursor);
  std::string index_value;
  Status s = cursors_store_->Lookup(index_key, &index_value);
  if (!s.ok() || index_value.size() < 3) {
    return s;
  }
  *type = index_value[0];
  *start_key = index_value.substr(1);
  return s;
}

Status Storage::StoreCursorStartKey(const DataType& dtype, int64_t cursor, char type, const std::string& next_key) {
  std::string index_key = DataTypeTag[dtype] + std::to_string(cursor);
  // format: data_type tag(1B) | start_key
  std::string index_value(1, type);
  index_value.append(next_key);
  return cursors_store_->Insert(index_key, index_value);
}

std::unique_ptr<Redis>& Storage::GetDBInstance(const Slice& key) { return GetDBInstance(key.ToString()); }

std::unique_ptr<Redis>& Storage::GetDBInstance(const std::string& key) {
  auto inst_index = slot_indexer_->GetInstanceID(GetSlotID(key));
  return insts_[inst_index];
}

// Strings Commands
Status Storage::Set(const Slice& key, const Slice& value) {
  auto& inst = GetDBInstance(key);
  return inst->Set(key, value);
}

Status Storage::Setxx(const Slice& key, const Slice& value, int32_t* ret, const uint64_t ttl) {
  auto& inst = GetDBInstance(key);
  return inst->Setxx(key, value, ret, ttl);
}

Status Storage::Get(const Slice& key, std::string* value) {
  auto& inst = GetDBInstance(key);
  return inst->Get(key, value);
}

Status Storage::GetWithTTL(const Slice& key, std::string* value, uint64_t* ttl) {
  auto& inst = GetDBInstance(key);
  return inst->GetWithTTL(key, value, ttl);
}

Status Storage::GetSet(const Slice& key, const Slice& value, std::string* old_value) {
  auto& inst = GetDBInstance(key);
  return inst->GetSet(key, value, old_value);
}

Status Storage::SetBit(const Slice& key, int64_t offset, int32_t value, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->SetBit(key, offset, value, ret);
}

Status Storage::GetBit(const Slice& key, int64_t offset, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->GetBit(key, offset, ret);
}

Status Storage::MSet(const std::vector<KeyValue>& kvs) {
  Status s;
  for (const auto& kv : kvs) {
    auto& inst = GetDBInstance(kv.key);
    s = inst->Set(Slice(kv.key), Slice(kv.value));
    if (!s.ok()) {
      return s;
    }
  }
  return s;
}

Status Storage::MGet(const std::vector<std::string>& keys, std::vector<ValueStatus>* vss) {
  vss->clear();
  Status s;
  for (const auto& key : keys) {
    auto& inst = GetDBInstance(key);
    std::string value;
    s = inst->Get(key, &value);
    if (s.ok()) {
      vss->push_back({value, Status::OK()});
    } else if (s.IsNotFound()) {
      vss->push_back({std::string(), Status::NotFound()});
    } else {
      vss->clear();
      return s;
    }
  }
  return Status::OK();
}

Status Storage::MGetWithTTL(const std::vector<std::string>& keys, std::vector<ValueStatus>* vss) {
  vss->clear();
  Status s;
  for (const auto& key : keys) {
    auto& inst = GetDBInstance(key);
    std::string value;
    uint64_t ttl;
    s = inst->GetWithTTL(key, &value, &ttl);
    if (s.ok()) {
      vss->push_back({value, Status::OK(), ttl});
    } else if (s.IsNotFound()) {
      vss->push_back({std::string(), Status::NotFound(), ttl});
    } else {
      vss->clear();
      return s;
    }
  }
  return Status::OK();
}

Status Storage::Setnx(const Slice& key, const Slice& value, int32_t* ret, const uint64_t ttl) {
  auto& inst = GetDBInstance(key);
  return inst->Setnx(key, value, ret, ttl);
}

// TODO: Not concurrent safe now, merge wuxianrong's bugfix after floyd's PR review finishes.
Status Storage::MSetnx(const std::vector<KeyValue>& kvs, int32_t* ret) {
  Status s;
  for (const auto& kv : kvs) {
    auto& inst = GetDBInstance(kv.key);
    std::string value;
    s = inst->Get(Slice(kv.key), &value);
    if (s.ok() || !s.IsNotFound()) {
      return s;
    }
  }

  for (const auto& kv : kvs) {
    auto& inst = GetDBInstance(kv.key);
    s = inst->Set(Slice(kv.key), Slice(kv.value));
    if (!s.ok()) {
      return s;
    }
  }
  if (s.ok()) {
    *ret = 1;
  }
  return s;
}

Status Storage::Setvx(const Slice& key, const Slice& value, const Slice& new_value, int32_t* ret, const uint64_t ttl) {
  auto& inst = GetDBInstance(key);
  return inst->Setvx(key, value, new_value, ret, ttl);
}

Status Storage::Delvx(const Slice& key, const Slice& value, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Delvx(key, value, ret);
}

Status Storage::Setrange(const Slice& key, int64_t start_offset, const Slice& value, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Setrange(key, start_offset, value, ret);
}

Status Storage::Getrange(const Slice& key, int64_t start_offset, int64_t end_offset, std::string* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Getrange(key, start_offset, end_offset, ret);
}

Status Storage::GetrangeWithValue(const Slice& key, int64_t start_offset, int64_t end_offset, std::string* ret,
                                  std::string* value, uint64_t* ttl) {
  auto& inst = GetDBInstance(key);
  return inst->GetrangeWithValue(key, start_offset, end_offset, ret, value, ttl);
}

Status Storage::Append(const Slice& key, const Slice& value, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Append(key, value, ret);
}

Status Storage::BitCount(const Slice& key, int64_t start_offset, int64_t end_offset, int32_t* ret, bool have_range) {
  auto& inst = GetDBInstance(key);
  return inst->BitCount(key, start_offset, end_offset, ret, have_range);
}

// disallowed in codis proxy, only runs in classic mode
Status Storage::BitOp(BitOpType op, const std::string& dest_key, const std::vector<std::string>& src_keys,
                      std::string& value_to_dest, int64_t* ret) {
  Status s;
  int64_t max_len = 0;
  int64_t value_len = 0;
  std::vector<std::string> src_vlaues;
  for (const auto& src_key : src_keys) {
    auto& inst = GetDBInstance(src_key);
    std::string value;
    s = inst->Get(Slice(src_key), &value);
    if (s.ok()) {
      src_vlaues.push_back(value);
      value_len = value.size();
    } else if (s.IsNotFound()) {
      src_vlaues.push_back("");
      value_len = 0;
    } else {
      return s;
    }
    max_len = std::max(max_len, value_len);
  }

  std::string dest_value = BitOpOperate(op, src_vlaues, max_len);
  value_to_dest = dest_value;
  *ret = dest_value.size();

  auto& dest_inst = GetDBInstance(dest_key);
  return dest_inst->Set(Slice(dest_key), Slice(dest_value));
}

Status Storage::BitPos(const Slice& key, int32_t bit, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->BitPos(key, bit, ret);
}

Status Storage::BitPos(const Slice& key, int32_t bit, int64_t start_offset, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->BitPos(key, bit, start_offset, ret);
}

Status Storage::BitPos(const Slice& key, int32_t bit, int64_t start_offset, int64_t end_offset, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->BitPos(key, bit, start_offset, end_offset, ret);
}

Status Storage::Decrby(const Slice& key, int64_t value, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Decrby(key, value, ret);
}

Status Storage::Incrby(const Slice& key, int64_t value, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Incrby(key, value, ret);
}

Status Storage::Incrbyfloat(const Slice& key, const Slice& value, std::string* ret) {
  auto& inst = GetDBInstance(key);
  return inst->Incrbyfloat(key, value, ret);
}

Status Storage::Setex(const Slice& key, const Slice& value, uint64_t ttl) {
  auto& inst = GetDBInstance(key);
  return inst->Setex(key, value, ttl);
}

Status Storage::Strlen(const Slice& key, int32_t* len) {
  auto& inst = GetDBInstance(key);
  return inst->Strlen(key, len);
}

Status Storage::PKSetexAt(const Slice& key, const Slice& value, uint64_t timestamp) {
  auto& inst = GetDBInstance(key);
  return inst->PKSetexAt(key, value, timestamp);
}

// Hashes Commands
Status Storage::HSet(const Slice& key, const Slice& field, const Slice& value, int32_t* res) {
  auto& inst = GetDBInstance(key);
  return inst->HSet(key, field, value, res);
}

Status Storage::HGet(const Slice& key, const Slice& field, std::string* value) {
  auto& inst = GetDBInstance(key);
  return inst->HGet(key, field, value);
}

Status Storage::HMSet(const Slice& key, const std::vector<FieldValue>& fvs) {
  auto& inst = GetDBInstance(key);
  return inst->HMSet(key, fvs);
}

Status Storage::HMGet(const Slice& key, const std::vector<std::string>& fields, std::vector<ValueStatus>* vss) {
  auto& inst = GetDBInstance(key);
  return inst->HMGet(key, fields, vss);
}

Status Storage::HGetall(const Slice& key, std::vector<FieldValue>* fvs) {
  auto& inst = GetDBInstance(key);
  return inst->HGetall(key, fvs);
}

Status Storage::HGetallWithTTL(const Slice& key, std::vector<FieldValue>* fvs, uint64_t* ttl) {
  auto& inst = GetDBInstance(key);
  return inst->HGetallWithTTL(key, fvs, ttl);
}

Status Storage::HKeys(const Slice& key, std::vector<std::string>* fields) {
  auto& inst = GetDBInstance(key);
  return inst->HKeys(key, fields);
}

Status Storage::HVals(const Slice& key, std::vector<std::string>* values) {
  auto& inst = GetDBInstance(key);
  return inst->HVals(key, values);
}

Status Storage::HSetnx(const Slice& key, const Slice& field, const Slice& value, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->HSetnx(key, field, value, ret);
}

Status Storage::HLen(const Slice& key, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->HLen(key, ret);
}

Status Storage::HStrlen(const Slice& key, const Slice& field, int32_t* len) {
  auto& inst = GetDBInstance(key);
  return inst->HStrlen(key, field, len);
}

Status Storage::HExists(const Slice& key, const Slice& field) {
  auto& inst = GetDBInstance(key);
  return inst->HExists(key, field);
}

Status Storage::HIncrby(const Slice& key, const Slice& field, int64_t value, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->HIncrby(key, field, value, ret);
}

Status Storage::HIncrbyfloat(const Slice& key, const Slice& field, const Slice& by, std::string* new_value) {
  auto& inst = GetDBInstance(key);
  return inst->HIncrbyfloat(key, field, by, new_value);
}

Status Storage::HDel(const Slice& key, const std::vector<std::string>& fields, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->HDel(key, fields, ret);
}

Status Storage::HScan(const Slice& key, int64_t cursor, const std::string& pattern, int64_t count,
                      std::vector<FieldValue>* field_values, int64_t* next_cursor) {
  auto& inst = GetDBInstance(key);
  return inst->HScan(key, cursor, pattern, count, field_values, next_cursor);
}

Status Storage::HScanx(const Slice& key, const std::string& start_field, const std::string& pattern, int64_t count,
                       std::vector<FieldValue>* field_values, std::string* next_field) {
  auto& inst = GetDBInstance(key);
  return inst->HScanx(key, start_field, pattern, count, field_values, next_field);
}

Status Storage::HRandField(const Slice& key, int64_t count, bool with_values, std::vector<std::string>* res) {
  auto& inst = GetDBInstance(key);
  return inst->HRandField(key, count, with_values, res);
}

Status Storage::PKHScanRange(const Slice& key, const Slice& field_start, const std::string& field_end,
                             const Slice& pattern, int32_t limit, std::vector<FieldValue>* field_values,
                             std::string* next_field) {
  auto& inst = GetDBInstance(key);
  return inst->PKHScanRange(key, field_start, field_end, pattern, limit, field_values, next_field);
}

Status Storage::PKHRScanRange(const Slice& key, const Slice& field_start, const std::string& field_end,
                              const Slice& pattern, int32_t limit, std::vector<FieldValue>* field_values,
                              std::string* next_field) {
  auto& inst = GetDBInstance(key);
  return inst->PKHRScanRange(key, field_start, field_end, pattern, limit, field_values, next_field);
}

// Sets Commands
Status Storage::SAdd(const Slice& key, const std::vector<std::string>& members, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->SAdd(key, members, ret);
}

Status Storage::SCard(const Slice& key, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->SCard(key, ret);
}

Status Storage::SDiff(const std::vector<std::string>& keys, std::vector<std::string>* members) {
  if (keys.empty()) {
    return rocksdb::Status::Corruption("SDiff invalid parameter, no keys");
  }
  members->clear();

  Status s;
  auto& inst = GetDBInstance(keys[0]);
  std::vector<std::string> keys0_members;
  s = inst->SMembers(Slice(keys[0]), &keys0_members);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  for (const auto& member : keys0_members) {
    int32_t exist = 0;
    for (int idx = 1; idx < keys.size(); idx++) {
      Slice pkey = Slice(keys[idx]);
      auto& inst = GetDBInstance(pkey);
      s = inst->SIsmember(pkey, Slice(member), &exist);
      if (!s.ok() && !s.IsNotFound()) {
        return s;
      }
      if (exist) break;
    }
    if (!exist) {
      members->push_back(member);
    }
  }
  return Status::OK();
}

Status Storage::SDiffstore(const Slice& destination, const std::vector<std::string>& keys,
                           std::vector<std::string>& value_to_dest, int32_t* ret) {
  Status s;

  s = SDiff(keys, &value_to_dest);
  if (!s.ok()) {
    return s;
  }

  auto& inst = GetDBInstance(destination);
  s = inst->SetsDel(destination);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  s = inst->SAdd(destination, value_to_dest, ret);
  return s;
}

Status Storage::SInter(const std::vector<std::string>& keys, std::vector<std::string>* members) {
  Status s;
  members->clear();

  std::vector<std::string> key0_members;
  auto& inst = GetDBInstance(keys[0]);
  s = inst->SMembers(keys[0], &key0_members);
  if (s.IsNotFound()) {
    return Status::OK();
  }
  if (!s.ok()) {
    return s;
  }

  for (const auto member : key0_members) {
    int32_t exist = 1;
    for (int idx = 1; idx < keys.size(); idx++) {
      Slice pkey(keys[idx]);
      auto& inst = GetDBInstance(keys[idx]);
      s = inst->SIsmember(keys[idx], member, &exist);
      if (s.ok() && exist > 0) {
        continue;
      } else if (!s.IsNotFound()) {
        return s;
      } else {
        break;
      }
    }
    if (exist > 0) {
      members->push_back(member);
    }
  }
  return Status::OK();
}

Status Storage::SInterstore(const Slice& destination, const std::vector<std::string>& keys,
                            std::vector<std::string>& value_to_dest, int32_t* ret) {
  Status s;

  s = SInter(keys, &value_to_dest);
  if (!s.ok()) {
    return s;
  }

  auto& dest_inst = GetDBInstance(destination);
  s = dest_inst->SetsDel(destination);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  s = dest_inst->SAdd(destination, value_to_dest, ret);
  return s;
}

Status Storage::SIsmember(const Slice& key, const Slice& member, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->SIsmember(key, member, ret);
}

Status Storage::SMembers(const Slice& key, std::vector<std::string>* members) {
  auto& inst = GetDBInstance(key);
  return inst->SMembers(key, members);
}

Status Storage::SMembersWithTTL(const Slice& key, std::vector<std::string>* members, uint64_t* ttl) {
  auto& inst = GetDBInstance(key);
  return inst->SMembersWithTTL(key, members, ttl);
}

Status Storage::SMove(const Slice& source, const Slice& destination, const Slice& member, int32_t* ret) {
  Status s;

  auto& src_inst = GetDBInstance(source);
  s = src_inst->SIsmember(source, member, ret);
  if (s.IsNotFound()) {
    *ret = 0;
    return s;
  }
  if (!s.ok()) {
    return s;
  }

  s = src_inst->SRem(source, std::vector<std::string>{member.ToString()}, ret);
  if (!s.ok()) {
    return s;
  }
  auto& dest_inst = GetDBInstance(destination);
  int unused_ret;
  return dest_inst->SAdd(destination, std::vector<std::string>{member.ToString()}, &unused_ret);
}

Status Storage::SPop(const Slice& key, std::vector<std::string>* members, int64_t count) {
  auto& inst = GetDBInstance(key);
  Status status = inst->SPop(key, members, count);
  return status;
}

Status Storage::SRandmember(const Slice& key, int32_t count, std::vector<std::string>* members) {
  auto& inst = GetDBInstance(key);
  return inst->SRandmember(key, count, members);
}

Status Storage::SRem(const Slice& key, const std::vector<std::string>& members, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->SRem(key, members, ret);
}

Status Storage::SUnion(const std::vector<std::string>& keys, std::vector<std::string>* members) {
  Status s;
  members->clear();

  using Iter = std::vector<std::string>::iterator;
  using Uset = std::unordered_set<std::string>;
  Uset member_set;
  for (const auto& key : keys) {
    std::vector<std::string> vec;
    auto& inst = GetDBInstance(key);
    s = inst->SMembers(key, &vec);
    if (s.IsNotFound()) {
      continue;
    }
    if (!s.ok()) {
      return s;
    }
    std::copy(std::move_iterator<Iter>(vec.begin()), std::move_iterator<Iter>(vec.end()),
              std::insert_iterator<Uset>(member_set, member_set.begin()));
  }

  std::copy(member_set.begin(), member_set.end(), std::back_inserter(*members));
  return Status::OK();
}

Status Storage::SUnionstore(const Slice& destination, const std::vector<std::string>& keys,
                            std::vector<std::string>& value_to_dest, int32_t* ret) {
  Status s;
  value_to_dest.clear();

  s = SUnion(keys, &value_to_dest);
  if (!s.ok()) {
    return s;
  }
  *ret = value_to_dest.size();
  auto& dest_inst = GetDBInstance(destination);
  s = dest_inst->SetsDel(destination);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }
  int unused_ret;
  return dest_inst->SAdd(destination, value_to_dest, &unused_ret);
}

Status Storage::SScan(const Slice& key, int64_t cursor, const std::string& pattern, int64_t count,
                      std::vector<std::string>* members, int64_t* next_cursor) {
  auto& inst = GetDBInstance(key);
  return inst->SScan(key, cursor, pattern, count, members, next_cursor);
}

Status Storage::LPush(const Slice& key, const std::vector<std::string>& values, uint64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->LPush(key, values, ret);
}

Status Storage::RPush(const Slice& key, const std::vector<std::string>& values, uint64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->RPush(key, values, ret);
}

Status Storage::LRange(const Slice& key, int64_t start, int64_t stop, std::vector<std::string>* ret) {
  ret->clear();
  auto& inst = GetDBInstance(key);
  return inst->LRange(key, start, stop, ret);
}

Status Storage::LRangeWithTTL(const Slice& key, int64_t start, int64_t stop, std::vector<std::string>* ret,
                              uint64_t* ttl) {
  auto& inst = GetDBInstance(key);
  return inst->LRangeWithTTL(key, start, stop, ret, ttl);
}

Status Storage::LTrim(const Slice& key, int64_t start, int64_t stop) {
  auto& inst = GetDBInstance(key);
  return inst->LTrim(key, start, stop);
}

Status Storage::LLen(const Slice& key, uint64_t* len) {
  auto& inst = GetDBInstance(key);
  return inst->LLen(key, len);
}

Status Storage::LPop(const Slice& key, int64_t count, std::vector<std::string>* elements) {
  elements->clear();
  auto& inst = GetDBInstance(key);
  return inst->LPop(key, count, elements);
}

Status Storage::RPop(const Slice& key, int64_t count, std::vector<std::string>* elements) {
  elements->clear();
  auto& inst = GetDBInstance(key);
  return inst->RPop(key, count, elements);
}

Status Storage::LIndex(const Slice& key, int64_t index, std::string* element) {
  element->clear();
  auto& inst = GetDBInstance(key);
  return inst->LIndex(key, index, element);
}

Status Storage::LInsert(const Slice& key, const BeforeOrAfter& before_or_after, const std::string& pivot,
                        const std::string& value, int64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->LInsert(key, before_or_after, pivot, value, ret);
}

Status Storage::LPushx(const Slice& key, const std::vector<std::string>& values, uint64_t* len) {
  auto& inst = GetDBInstance(key);
  return inst->LPushx(key, values, len);
}

Status Storage::RPushx(const Slice& key, const std::vector<std::string>& values, uint64_t* len) {
  auto& inst = GetDBInstance(key);
  return inst->RPushx(key, values, len);
}

Status Storage::LRem(const Slice& key, int64_t count, const Slice& value, uint64_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->LRem(key, count, value, ret);
}

Status Storage::LSet(const Slice& key, int64_t index, const Slice& value) {
  auto& inst = GetDBInstance(key);
  return inst->LSet(key, index, value);
}

Status Storage::RPoplpush(const Slice& source, const Slice& destination, std::string* element) {
  Status s;
  element->clear();

  auto& source_inst = GetDBInstance(source);
  if (source.compare(destination) == 0) {
    s = source_inst->RPoplpush(source, destination, element);
    return s;
  }

  std::vector<std::string> elements;
  s = source_inst->RPop(source, 1, &elements);
  if (!s.ok()) {
    return s;
  }
  *element = elements.front();
  auto& dest_inst = GetDBInstance(destination);
  uint64_t ret;
  s = dest_inst->LPush(destination, elements, &ret);
  return s;
}

Status Storage::ZPopMax(const Slice& key, const int64_t count, std::vector<ScoreMember>* score_members) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZPopMax(key, count, score_members);
}

Status Storage::ZPopMin(const Slice& key, const int64_t count, std::vector<ScoreMember>* score_members) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZPopMin(key, count, score_members);
}

Status Storage::ZAdd(const Slice& key, const std::vector<ScoreMember>& score_members, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZAdd(key, score_members, ret);
}

Status Storage::ZCard(const Slice& key, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZCard(key, ret);
}

Status Storage::ZCount(const Slice& key, double min, double max, bool left_close, bool right_close, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZCount(key, min, max, left_close, right_close, ret);
}

Status Storage::ZIncrby(const Slice& key, const Slice& member, double increment, double* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZIncrby(key, member, increment, ret);
}

Status Storage::ZRange(const Slice& key, int32_t start, int32_t stop, std::vector<ScoreMember>* score_members) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRange(key, start, stop, score_members);
}
Status Storage::ZRangeWithTTL(const Slice& key, int32_t start, int32_t stop, std::vector<ScoreMember>* score_members,
                              uint64_t* ttl) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRangeWithTTL(key, start, stop, score_members, ttl);
}

Status Storage::ZRangebyscore(const Slice& key, double min, double max, bool left_close, bool right_close,
                              std::vector<ScoreMember>* score_members) {
  // maximum number of zset is std::numeric_limits<int32_t>::max()
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRangebyscore(key, min, max, left_close, right_close, std::numeric_limits<int32_t>::max(), 0,
                             score_members);
}

Status Storage::ZRangebyscore(const Slice& key, double min, double max, bool left_close, bool right_close,
                              int64_t count, int64_t offset, std::vector<ScoreMember>* score_members) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRangebyscore(key, min, max, left_close, right_close, count, offset, score_members);
}

Status Storage::ZRank(const Slice& key, const Slice& member, int32_t* rank) {
  auto& inst = GetDBInstance(key);
  return inst->ZRank(key, member, rank);
}

Status Storage::ZRem(const Slice& key, const std::vector<std::string>& members, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZRem(key, members, ret);
}

Status Storage::ZRemrangebyrank(const Slice& key, int32_t start, int32_t stop, int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZRemrangebyrank(key, start, stop, ret);
}

Status Storage::ZRemrangebyscore(const Slice& key, double min, double max, bool left_close, bool right_close,
                                 int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZRemrangebyscore(key, min, max, left_close, right_close, ret);
}

Status Storage::ZRevrangebyscore(const Slice& key, double min, double max, bool left_close, bool right_close,
                                 int64_t count, int64_t offset, std::vector<ScoreMember>* score_members) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRevrangebyscore(key, min, max, left_close, right_close, count, offset, score_members);
}

Status Storage::ZRevrange(const Slice& key, int32_t start, int32_t stop, std::vector<ScoreMember>* score_members) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRevrange(key, start, stop, score_members);
}

Status Storage::ZRevrangebyscore(const Slice& key, double min, double max, bool left_close, bool right_close,
                                 std::vector<ScoreMember>* score_members) {
  // maximum number of zset is std::numeric_limits<int32_t>::max()
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRevrangebyscore(key, min, max, left_close, right_close, std::numeric_limits<int32_t>::max(), 0,
                                score_members);
}

Status Storage::ZRevrank(const Slice& key, const Slice& member, int32_t* rank) {
  auto& inst = GetDBInstance(key);
  return inst->ZRevrank(key, member, rank);
}

Status Storage::ZScore(const Slice& key, const Slice& member, double* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZScore(key, member, ret);
}

Status Storage::ZUnionstore(const Slice& destination, const std::vector<std::string>& keys,
                            const std::vector<double>& weights, const AGGREGATE agg,
                            std::map<std::string, double>& value_to_dest, int32_t* ret) {
  value_to_dest.clear();
  Status s;

  for (int idx = 0; idx < keys.size(); idx++) {
    Slice key = Slice(keys[idx]);
    auto& inst = GetDBInstance(key);
    std::map<std::string, double> member_to_score;
    double weight = idx >= weights.size() ? 1 : weights[idx];
    s = inst->ZGetAll(key, weight, &member_to_score);
    if (!s.ok() && !s.IsNotFound()) {
      return s;
    }
    for (const auto& key_score : member_to_score) {
      const std::string& member = key_score.first;
      double score = key_score.second;
      if (value_to_dest.find(member) == value_to_dest.end()) {
        value_to_dest[member] = score;
        continue;
      }
      switch (agg) {
        case SUM:
          score += value_to_dest[member];
          break;
        case MIN:
          score = std::min(value_to_dest[member], score);
          break;
        case MAX:
          score = std::max(value_to_dest[member], score);
          break;
      }
      value_to_dest[member] = (score == -0.0) ? 0 : score;
    }
  }

  BaseMetaKey base_destination(destination);
  auto& inst = GetDBInstance(destination);
  s = inst->ZsetsDel(destination);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }
  std::vector<ScoreMember> score_members;
  std::for_each(value_to_dest.begin(), value_to_dest.end(),
                [&score_members](auto kv) { score_members.emplace_back(kv.second, kv.first); });
  *ret = score_members.size();
  int unused_ret;
  return inst->ZAdd(destination, score_members, &unused_ret);
}

Status Storage::ZInterstore(const Slice& destination, const std::vector<std::string>& keys,
                            const std::vector<double>& weights, const AGGREGATE agg,
                            std::vector<ScoreMember>& value_to_dest, int32_t* ret) {
  Status s;
  value_to_dest.clear();

  Slice key = Slice(keys[0]);
  auto& inst = GetDBInstance(key);
  std::map<std::string, double> member_to_score;
  double weight = weights.empty() ? 1 : weights[0];
  s = inst->ZGetAll(key, weight, &member_to_score);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }

  for (const auto member_score : member_to_score) {
    std::string member = member_score.first;
    double score = member_score.second;
    bool reliable = true;

    for (int idx = 1; idx < keys.size(); idx++) {
      double weight = idx >= weights.size() ? 1 : weights[idx];
      auto& inst = GetDBInstance(keys[idx]);
      double ret_score;
      s = inst->ZScore(keys[idx], member, &ret_score);
      if (!s.ok() && !s.IsNotFound()) {
        return s;
      }
      if (s.IsNotFound()) {
        reliable = false;
        break;
      }
      switch (agg) {
        case SUM:
          score += ret_score * weight;
          break;
        case MIN:
          score = std::min(score, ret_score * weight);
          break;
        case MAX:
          score = std::max(score, ret_score * weight);
          break;
      }
    }
    if (reliable) {
      value_to_dest.emplace_back(score, member);
    }
  }

  BaseMetaKey base_destination(destination);
  auto& dinst = GetDBInstance(destination);

  s = dinst->ZsetsDel(destination);
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }
  *ret = value_to_dest.size();
  int unused_ret;
  return dinst->ZAdd(destination, value_to_dest, &unused_ret);
}

Status Storage::ZRangebylex(const Slice& key, const Slice& min, const Slice& max, bool left_close, bool right_close,
                            std::vector<std::string>* members) {
  members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZRangebylex(key, min, max, left_close, right_close, members);
}

Status Storage::ZLexcount(const Slice& key, const Slice& min, const Slice& max, bool left_close, bool right_close,
                          int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZLexcount(key, min, max, left_close, right_close, ret);
}

Status Storage::ZRemrangebylex(const Slice& key, const Slice& min, const Slice& max, bool left_close, bool right_close,
                               int32_t* ret) {
  auto& inst = GetDBInstance(key);
  return inst->ZRemrangebylex(key, min, max, left_close, right_close, ret);
}

Status Storage::ZScan(const Slice& key, int64_t cursor, const std::string& pattern, int64_t count,
                      std::vector<ScoreMember>* score_members, int64_t* next_cursor) {
  score_members->clear();
  auto& inst = GetDBInstance(key);
  return inst->ZScan(key, cursor, pattern, count, score_members, next_cursor);
}

int32_t Storage::Expire(const Slice& key, uint64_t ttl) {
  int32_t ret = 0;
  bool is_corruption = false;

  auto& inst = GetDBInstance(key);
  // Strings
  Status s = inst->StringsExpire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  // Hash
  s = inst->HashesExpire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  // Sets
  s = inst->SetsExpire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  // Lists
  s = inst->ListsExpire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  // Zsets
  s = inst->ZsetsExpire(key, ttl);
  if (s.ok()) {
    ret++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  if (is_corruption) {
    return -1;
  } else {
    return ret;
  }
}

int64_t Storage::Del(const std::vector<std::string>& keys) {
  Status s;
  int64_t count = 0;
  bool is_corruption = false;

  for (const auto& key : keys) {
    auto& inst = GetDBInstance(key);
    // Strings
    Status s = inst->StringsDel(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    // Hashes
    s = inst->HashesDel(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    // Sets
    s = inst->SetsDel(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    // Lists
    s = inst->ListsDel(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    // ZSets
    s = inst->ZsetsDel(key);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

int64_t Storage::DelByType(const std::vector<std::string>& keys, const DataType& type) {
  Status s;
  int64_t count = 0;
  bool is_corruption = false;

  for (const auto& key : keys) {
    auto& inst = GetDBInstance(key);
    switch (type) {
      // Strings
      case DataType::kStrings: {
        s = inst->StringsDel(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // Hashes
      case DataType::kHashes: {
        s = inst->HashesDel(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // Sets
      case DataType::kSets: {
        s = inst->SetsDel(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // Lists
      case DataType::kLists: {
        s = inst->ListsDel(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      // ZSets
      case DataType::kZSets: {
        s = inst->ZsetsDel(key);
        if (s.ok()) {
          count++;
        } else if (!s.IsNotFound()) {
          is_corruption = true;
        }
        break;
      }
      case DataType::kAll: {
        return -1;
      }
    }
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

int64_t Storage::Exists(const std::vector<std::string>& keys) {
  int64_t count = 0;
  int32_t ret;
  uint64_t llen;
  std::string value;
  Status s;
  bool is_corruption = false;

  for (const auto& key : keys) {
    auto& inst = GetDBInstance(key);
    s = inst->Get(key, &value);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    s = inst->HLen(key, &ret);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    s = inst->SCard(key, &ret);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    s = inst->LLen(key, &llen);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }

    s = inst->ZCard(key, &ret);
    if (s.ok()) {
      count++;
    } else if (!s.IsNotFound()) {
      is_corruption = true;
    }
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

int64_t Storage::Scan(const DataType& dtype, int64_t cursor, const std::string& pattern, int64_t count,
                      std::vector<std::string>* keys) {
  keys->clear();
  bool is_finish;
  int64_t leftover_visits = count;
  int64_t step_length = count;
  int64_t cursor_ret = 0;
  std::string start_key;
  std::string next_key;
  std::string prefix;
  char key_type;

  // invalid cursor
  if (cursor < 0) {
    return cursor_ret;
  }

  // get seek by corsor
  prefix = isTailWildcard(pattern) ? pattern.substr(0, pattern.size() - 1) : "";
  Status s = LoadCursorStartKey(dtype, cursor, &key_type, &start_key);
  if (!s.ok()) {
    // If want to scan all the databases, we start with the strings database
    key_type = dtype == DataType::kAll ? DataTypeTag[DataType::kStrings] : DataTypeTag[dtype];
    start_key = prefix;
    cursor = 0;
  }

  // collect types to scan
  std::vector<char> types;
  if (DataType::kAll == dtype) {
    auto iter_end = std::end(DataTypeTag);
    auto pos = std::find(std::begin(DataTypeTag), iter_end, key_type);
    if (pos == iter_end) {
      WARN("Invalid key_type: ", key_type);
      return 0;
    }
    std::copy(pos, iter_end, std::back_inserter(types));
  } else {
    types.push_back(DataTypeTag[dtype]);
  }

  for (const auto& type : types) {
    std::vector<IterSptr> inst_iters;
    for (const auto& inst : insts_) {
      IterSptr iter_sptr;
      iter_sptr.reset(inst->CreateIterator(type, pattern, nullptr /*lower_bound*/, nullptr /*upper_bound*/));
      inst_iters.push_back(iter_sptr);
    }

    BaseMetaKey base_start_key(start_key);
    MergingIterator miter(inst_iters);
    miter.Seek(base_start_key.Encode().ToString());
    while (miter.Valid() && count > 0) {
      keys->push_back(miter.Key());
      miter.Next();
      count--;
    }

    bool is_finish = !miter.Valid();
    if (miter.Valid() && (miter.Key().compare(prefix) <= 0 || miter.Key().substr(0, prefix.size()) == prefix)) {
      is_finish = false;
    }

    // for specific type scan, reach the end
    if (is_finish && dtype != DataType::kAll) {
      return cursor_ret;
    }

    // already get count's element, while iterator is still valid,
    // store cursor
    if (!is_finish) {
      next_key = miter.Key();
      cursor_ret = cursor + step_length;
      StoreCursorStartKey(dtype, cursor_ret, type, next_key);
      return cursor_ret;
    }

    // for all type scan, move to next type, reset start_key
    start_key = prefix;
  }
  return cursor_ret;
}

Status Storage::PKScanRange(const DataType& data_type, const Slice& key_start, const Slice& key_end,
                            const Slice& pattern, int32_t limit, std::vector<std::string>* keys,
                            std::vector<KeyValue>* kvs, std::string* next_key) {
  next_key->clear();
  std::string key;
  std::string value;

  BaseMetaKey base_key_start(key_start);
  BaseMetaKey base_key_end(key_end);
  Slice base_key_end_slice(base_key_end.Encode());

  bool start_no_limit = key_start.empty();
  bool end_no_limit = key_end.empty();
  if (!start_no_limit && !end_no_limit && key_start.compare(key_end) > 0) {
    return Status::InvalidArgument("error in given range");
  }

  std::vector<IterSptr> inst_iters;
  for (const auto& inst : insts_) {
    IterSptr iter_sptr;
    iter_sptr.reset(
        inst->CreateIterator(data_type, pattern.ToString(), nullptr /*lower_bound*/, nullptr /*upper_bound*/));
    inst_iters.push_back(iter_sptr);
  }

  MergingIterator miter(inst_iters);
  if (start_no_limit) {
    miter.SeekToFirst();
  } else {
    std::string temp = base_key_start.Encode().ToString();
    miter.Seek(temp);
  }

  while (miter.Valid() && limit > 0 && (end_no_limit || miter.Key().compare(key_end.ToString()) <= 0)) {
    if (data_type == DataType::kStrings) {
      kvs->push_back({miter.Key(), miter.Value()});
    } else {
      keys->push_back(miter.Key());
    }
    limit--;
    miter.Next();
  }

  if (miter.Valid() && (end_no_limit || miter.Key().compare(key_end.ToString()) <= 0)) {
    *next_key = miter.Key();
  }
  return Status::OK();
}

Status Storage::PKRScanRange(const DataType& data_type, const Slice& key_start, const Slice& key_end,
                             const Slice& pattern, int32_t limit, std::vector<std::string>* keys,
                             std::vector<KeyValue>* kvs, std::string* next_key) {
  next_key->clear();
  std::string key, value;
  BaseMetaKey base_key_start(key_start);
  BaseMetaKey base_key_end(key_end);
  Slice base_key_start_slice = Slice(base_key_start.Encode());

  bool start_no_limit = key_start.empty();
  bool end_no_limit = key_end.empty();

  if (!start_no_limit && !end_no_limit && key_start.compare(key_end) < 0) {
    return Status::InvalidArgument("error in given range");
  }

  std::vector<IterSptr> inst_iters;
  for (const auto& inst : insts_) {
    IterSptr iter_sptr;
    iter_sptr.reset(
        inst->CreateIterator(data_type, pattern.ToString(), nullptr /*lower_bound*/, nullptr /*upper_bound*/));
    inst_iters.push_back(iter_sptr);
  }
  MergingIterator miter(inst_iters);
  if (start_no_limit) {
    miter.SeekToLast();
  } else {
    miter.SeekForPrev(base_key_start.Encode().ToString());
  }

  while (miter.Valid() && limit > 0 && (end_no_limit || miter.Key().compare(key_end.ToString()) >= 0)) {
    if (data_type == DataType::kStrings) {
      kvs->push_back({miter.Key(), miter.Value()});
    } else {
      keys->push_back(miter.Key());
    }
    limit--;
    miter.Prev();
  }

  if (miter.Valid() && (end_no_limit || miter.Key().compare(key_end.ToString()) >= 0)) {
    *next_key = miter.Key();
  }
  return Status::OK();
}

Status Storage::PKPatternMatchDel(const DataType& data_type, const std::string& pattern, int32_t* ret) {
  Status s;
  for (const auto& inst : insts_) {
    switch (data_type) {
      case DataType::kStrings: {
        s = inst->StringsPKPatternMatchDel(pattern, ret);
        if (!s.ok()) {
          return s;
        }
      }
      case DataType::kHashes: {
        s = inst->HashesPKPatternMatchDel(pattern, ret);
        if (!s.ok()) {
          return s;
        }
      }
      case DataType::kLists: {
        s = inst->ListsPKPatternMatchDel(pattern, ret);
        if (!s.ok()) {
          return s;
        }
      }
      case DataType::kZSets: {
        s = inst->ZsetsPKPatternMatchDel(pattern, ret);
        if (!s.ok()) {
          return s;
        }
      }
      case DataType::kSets: {
        s = inst->SetsPKPatternMatchDel(pattern, ret);
        if (!s.ok()) {
          return s;
        }
      }
      default:
        s = Status::Corruption("Unsupported data types");
        break;
    }
  }
  return s;
}

Status Storage::Scanx(const DataType& data_type, const std::string& start_key, const std::string& pattern,
                      int64_t count, std::vector<std::string>* keys, std::string* next_key) {
  Status s;
  keys->clear();
  next_key->clear();

  std::vector<IterSptr> inst_iters;
  for (const auto& inst : insts_) {
    IterSptr iter_sptr;
    iter_sptr.reset(inst->CreateIterator(data_type, pattern, nullptr /*lower_bound*/, nullptr /*upper_bound*/));
    inst_iters.push_back(iter_sptr);
  }

  BaseMetaKey base_start_key(start_key);
  MergingIterator miter(inst_iters);
  miter.Seek(base_start_key.Encode().ToString());
  while (miter.Valid() && count > 0) {
    keys->push_back(miter.Key());
    miter.Next();
    count--;
  }

  std::string prefix = isTailWildcard(pattern) ? pattern.substr(0, pattern.size() - 1) : "";
  if (miter.Valid() && (miter.Key().compare(prefix) <= 0 || miter.Key().substr(0, prefix.size()) == prefix)) {
    *next_key = miter.Key();
  } else {
    *next_key = "";
  }
  return Status::OK();
}

int32_t Storage::Expireat(const Slice& key, uint64_t timestamp) {
  Status s;
  int32_t count = 0;
  bool is_corruption = false;

  auto& inst = GetDBInstance(key);
  s = inst->StringsExpireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  s = inst->HashesExpireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  s = inst->SetsExpireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  s = inst->ListsExpireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  s = inst->ZsetsExpireat(key, timestamp);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
  }

  if (is_corruption) {
    return -1;
  }
  return count;
}

int32_t Storage::Persist(const Slice& key, std::map<DataType, Status>* type_status) {
  Status s;
  int32_t count = 0;
  bool is_corruption = false;

  auto& inst = GetDBInstance(key);
  s = inst->StringsPersist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kStrings] = s;
  }

  s = inst->HashesPersist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kHashes] = s;
  }

  s = inst->SetsPersist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kSets] = s;
  }

  s = inst->ListsPersist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kLists] = s;
  }

  s = inst->ZsetsPersist(key);
  if (s.ok()) {
    count++;
  } else if (!s.IsNotFound()) {
    is_corruption = true;
    (*type_status)[DataType::kZSets] = s;
  }

  if (is_corruption) {
    return -1;
  } else {
    return count;
  }
}

std::map<DataType, int64_t> Storage::TTL(const Slice& key, std::map<DataType, Status>* type_status) {
  Status s;
  std::map<DataType, int64_t> ret;
  uint64_t timestamp = 0;

  auto& inst = GetDBInstance(key);
  s = inst->StringsTTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kStrings] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kStrings] = -3;
    (*type_status)[DataType::kStrings] = s;
  }

  s = inst->HashesTTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kHashes] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kHashes] = -3;
    (*type_status)[DataType::kHashes] = s;
  }

  s = inst->ListsTTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kLists] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kLists] = -3;
    (*type_status)[DataType::kLists] = s;
  }

  s = inst->SetsTTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kSets] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kSets] = -3;
    (*type_status)[DataType::kSets] = s;
  }

  s = inst->ZsetsTTL(key, &timestamp);
  if (s.ok() || s.IsNotFound()) {
    ret[DataType::kZSets] = timestamp;
  } else if (!s.IsNotFound()) {
    ret[DataType::kZSets] = -3;
    (*type_status)[DataType::kZSets] = s;
  }
  return ret;
}

Status Storage::GetType(const std::string& key, bool single, std::vector<std::string>& types) {
  types.clear();

  Status s;
  std::string value;
  auto& inst = GetDBInstance(key);
  s = inst->Get(key, &value);
  if (s.ok()) {
    types.emplace_back("string");
  } else if (!s.IsNotFound()) {
    return s;
  }
  if (single && !types.empty()) {
    return s;
  }

  int32_t hashes_len = 0;
  s = inst->HLen(key, &hashes_len);
  if (s.ok() && hashes_len != 0) {
    types.emplace_back("hash");
  } else if (!s.IsNotFound()) {
    return s;
  }
  if (single && !types.empty()) {
    return s;
  }

  uint64_t lists_len = 0;
  s = inst->LLen(key, &lists_len);
  if (s.ok() && lists_len != 0) {
    types.emplace_back("list");
  } else if (!s.IsNotFound()) {
    return s;
  }
  if (single && !types.empty()) {
    return s;
  }

  int32_t zsets_size = 0;
  s = inst->ZCard(key, &zsets_size);
  if (s.ok() && zsets_size != 0) {
    types.emplace_back("zset");
  } else if (!s.IsNotFound()) {
    return s;
  }
  if (single && !types.empty()) {
    return s;
  }

  int32_t sets_size = 0;
  s = inst->SCard(key, &sets_size);
  if (s.ok() && sets_size != 0) {
    types.emplace_back("set");
  } else if (!s.IsNotFound()) {
    return s;
  }
  if (single && types.empty()) {
    types.emplace_back("none");
  }
  return Status::OK();
}

Status Storage::Keys(const DataType& data_type, const std::string& pattern, std::vector<std::string>* keys) {
  keys->clear();
  std::vector<DataType> types;
  if (data_type == DataType::kAll) {
    types.push_back(DataType::kStrings);
    types.push_back(DataType::kHashes);
    types.push_back(DataType::kLists);
    types.push_back(DataType::kZSets);
    types.push_back(DataType::kSets);
  } else {
    types.push_back(data_type);
  }

  for (const auto& type : types) {
    std::vector<IterSptr> inst_iters;
    for (const auto& inst : insts_) {
      IterSptr inst_iter;
      inst_iter.reset(inst->CreateIterator(type, pattern, nullptr /*lower_bound*/, nullptr /*upper_bound*/));
      inst_iters.push_back(inst_iter);
    }

    MergingIterator miter(inst_iters);
    miter.SeekToFirst();
    while (miter.Valid()) {
      keys->push_back(miter.Key());
      miter.Next();
    }
  }

  return Status::OK();
}

Status Storage::Rename(const std::string& key, const std::string& newkey) {
  Status ret = Status::NotFound();
  auto& inst = GetDBInstance(key);
  auto& new_inst = GetDBInstance(newkey);

  // Strings
  Status s = inst->StringsRename(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // Hashes
  s = inst->HashesRename(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // Sets
  s = inst->SetsRename(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // Lists
  s = inst->ListsRename(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // ZSets
  s = inst->ZsetsRename(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  return ret;
}

Status Storage::Renamenx(const std::string& key, const std::string& newkey) {
  Status ret = Status::NotFound();
  auto& inst = GetDBInstance(key);
  auto& new_inst = GetDBInstance(newkey);

  // Strings
  Status s = inst->StringsRenamenx(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // Hashes
  s = inst->HashesRenamenx(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // Sets
  s = inst->SetsRenamenx(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // Lists
  s = inst->ListsRenamenx(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  // ZSets
  s = inst->ZsetsRenamenx(key, new_inst.get(), newkey);
  if (s.ok()) {
    ret = Status::OK();
  } else if (!s.IsNotFound()) {
    return s;
  }

  return ret;
}

void Storage::ScanDatabase(const DataType& type) {
  for (const auto& inst : insts_) {
    switch (type) {
      case kStrings:
        inst->ScanStrings();
        break;
      case kHashes:
        inst->ScanHashes();
        break;
      case kSets:
        inst->ScanSets();
        break;
      case kZSets:
        inst->ScanZsets();
        break;
      case kLists:
        inst->ScanLists();
        break;
      case kAll:
        inst->ScanStrings();
        inst->ScanHashes();
        inst->ScanSets();
        inst->ScanZsets();
        inst->ScanLists();
        break;
    }
  }
}

// HyperLogLog
Status Storage::PfAdd(const Slice& key, const std::vector<std::string>& values, bool* update) {
  *update = false;
  if (values.size() >= kMaxKeys) {
    return Status::InvalidArgument("Invalid the number of key");
  }

  std::string value;
  std::string registers;
  std::string result;
  auto& inst = GetDBInstance(key);
  Status s = inst->Get(key, &value);
  if (s.ok()) {
    registers = value;
  } else if (s.IsNotFound()) {
    registers = "";
  } else {
    return s;
  }
  HyperLogLog log(kPrecision, registers);
  auto previous = static_cast<int32_t>(log.Estimate());
  for (const auto& value : values) {
    result = log.Add(value.data(), value.size());
  }
  HyperLogLog update_log(kPrecision, result);
  auto now = static_cast<int32_t>(update_log.Estimate());
  if (previous != now || (s.IsNotFound() && values.empty())) {
    *update = true;
  }
  s = inst->Set(key, result);
  return s;
}

Status Storage::PfCount(const std::vector<std::string>& keys, int64_t* result) {
  if (keys.size() >= kMaxKeys || keys.empty()) {
    return Status::InvalidArgument("Invalid the number of key");
  }

  std::string value;
  std::string first_registers;
  auto& inst = GetDBInstance(keys[0]);
  Status s = inst->Get(keys[0], &value);
  if (s.ok()) {
    first_registers = std::string(value.data(), value.size());
  } else if (s.IsNotFound()) {
    first_registers = "";
  }

  HyperLogLog first_log(kPrecision, first_registers);
  for (size_t i = 1; i < keys.size(); ++i) {
    std::string value;
    std::string registers;
    auto& inst = GetDBInstance(keys[i]);
    s = inst->Get(keys[i], &value);
    if (s.ok()) {
      registers = value;
    } else if (s.IsNotFound()) {
      continue;
    } else {
      return s;
    }
    HyperLogLog log(kPrecision, registers);
    first_log.Merge(log);
  }
  *result = static_cast<int32_t>(first_log.Estimate());
  return Status::OK();
}

Status Storage::PfMerge(const std::vector<std::string>& keys, std::string& value_to_dest) {
  if (keys.size() >= kMaxKeys || keys.empty()) {
    return Status::InvalidArgument("Invalid the number of key");
  }

  Status s;
  std::string value;
  std::string first_registers;
  std::string result;
  auto& inst = GetDBInstance(keys[0]);
  s = inst->Get(keys[0], &value);
  if (s.ok()) {
    first_registers = std::string(value.data(), value.size());
  } else if (s.IsNotFound()) {
    first_registers = "";
  }

  result = first_registers;
  HyperLogLog first_log(kPrecision, first_registers);
  for (size_t i = 1; i < keys.size(); ++i) {
    std::string value;
    std::string registers;
    auto& tmp_inst = GetDBInstance(keys[i]);
    s = tmp_inst->Get(keys[i], &value);
    if (s.ok()) {
      registers = std::string(value.data(), value.size());
    } else if (s.IsNotFound()) {
      continue;
    } else {
      return s;
    }
    HyperLogLog log(kPrecision, registers);
    result = first_log.Merge(log);
  }
  auto& ninst = GetDBInstance(keys[0]);
  s = ninst->Set(keys[0], result);
  value_to_dest = std::move(result);
  return s;
}

static void* StartBGThreadWrapper(void* arg) {
  auto s = reinterpret_cast<Storage*>(arg);
  s->RunBGTask();
  return nullptr;
}

Status Storage::StartBGThread() {
  int result = pthread_create(&bg_tasks_thread_id_, nullptr, StartBGThreadWrapper, this);
  if (result != 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "pthread create: %s", strerror(result));
    return Status::Corruption(msg);
  }
  return Status::OK();
}

Status Storage::AddBGTask(const BGTask& bg_task) {
  bg_tasks_mutex_.lock();
  if (bg_task.type == kAll) {
    // if current task it is global compact,
    // clear the bg_tasks_queue_;
    std::queue<BGTask> empty_queue;
    bg_tasks_queue_.swap(empty_queue);
  }
  bg_tasks_queue_.push(bg_task);
  bg_tasks_cond_var_.notify_one();
  bg_tasks_mutex_.unlock();
  return Status::OK();
}

Status Storage::RunBGTask() {
  BGTask task;
  while (!bg_tasks_should_exit_.load()) {
    std::unique_lock<std::mutex> lock(bg_tasks_mutex_);
    bg_tasks_cond_var_.wait(lock, [this]() { return !bg_tasks_queue_.empty() || bg_tasks_should_exit_.load(); });

    if (!bg_tasks_queue_.empty()) {
      task = bg_tasks_queue_.front();
      bg_tasks_queue_.pop();
    }
    lock.unlock();

    if (bg_tasks_should_exit_.load()) {
      return Status::Incomplete("bgtask return with bg_tasks_should_exit true");
    }

    if (task.operation == kCleanAll) {
      DoCompactRange(task.type, "", "");
    } else if (task.operation == kCompactRange) {
      if (task.argv.size() == 1) {
        DoCompactSpecificKey(task.type, task.argv[0]);
      }
      if (task.argv.size() == 2) {
        DoCompactRange(task.type, task.argv.front(), task.argv.back());
      }
    }
  }
  return Status::OK();
}

Status Storage::Compact(const DataType& type, bool sync) {
  if (sync) {
    return DoCompactRange(type, "", "");
  } else {
    AddBGTask({type, kCleanAll});
  }
  return Status::OK();
}

// run compactrange for all rocksdb instance
Status Storage::DoCompactRange(const DataType& type, const std::string& start, const std::string& end) {
  if (type != kAll && type != kStrings && type != kHashes && type != kSets && type != kZSets && type != kLists) {
    return Status::InvalidArgument("");
  }

  std::string start_key, end_key;
  CalculateStartAndEndKey(start, &start_key, nullptr);
  CalculateStartAndEndKey(end, nullptr, &end_key);
  Slice slice_start_key(start_key);
  Slice slice_end_key(end_key);
  Slice* start_ptr = slice_start_key.empty() ? nullptr : &slice_start_key;
  Slice* end_ptr = slice_end_key.empty() ? nullptr : &slice_end_key;

  Status s;
  for (const auto& inst : insts_) {
    switch (type) {
      case DataType::kStrings:
        current_task_type_ = Operation::kCleanStrings;
        s = inst->CompactRange(type, start_ptr, end_ptr);
        break;
      case DataType::kHashes:
        current_task_type_ = Operation::kCleanHashes;
        s = inst->CompactRange(type, start_ptr, end_ptr);
        break;
      case DataType::kLists:
        current_task_type_ = Operation::kCleanLists;
        s = inst->CompactRange(type, start_ptr, end_ptr);
        break;
      case DataType::kSets:
        current_task_type_ = Operation::kCleanSets;
        s = inst->CompactRange(type, start_ptr, end_ptr);
        break;
      case DataType::kZSets:
        current_task_type_ = Operation::kCleanZSets;
        s = inst->CompactRange(type, start_ptr, end_ptr);
        break;
      default:
        current_task_type_ = Operation::kCleanAll;
        s = inst->CompactRange(DataType::kStrings, start_ptr, end_ptr);
        s = inst->CompactRange(DataType::kHashes, start_ptr, end_ptr);
        s = inst->CompactRange(DataType::kLists, start_ptr, end_ptr);
        s = inst->CompactRange(DataType::kSets, start_ptr, end_ptr);
        s = inst->CompactRange(DataType::kZSets, start_ptr, end_ptr);
    }
  }
  current_task_type_ = Operation::kNone;
  return s;
}

Status Storage::CompactRange(const DataType& type, const std::string& start, const std::string& end, bool sync) {
  if (sync) {
    return DoCompactRange(type, start, end);
  } else {
    AddBGTask({type, kCompactRange, {start, end}});
  }
  return Status::OK();
}

Status Storage::DoCompactSpecificKey(const DataType& type, const std::string& key) {
  Status s;
  auto& inst = GetDBInstance(key);

  std::string start_key;
  std::string end_key;
  CalculateStartAndEndKey(key, &start_key, &end_key);
  Slice slice_begin(start_key);
  Slice slice_end(end_key);
  s = inst->CompactRange(type, &slice_begin, &slice_end, kMeta);
  return s;
}

Status Storage::SetMaxCacheStatisticKeys(uint32_t max_cache_statistic_keys) {
  for (const auto& inst : insts_) {
    inst->SetMaxCacheStatisticKeys(max_cache_statistic_keys);
  }
  return Status::OK();
}

Status Storage::SetSmallCompactionThreshold(uint32_t small_compaction_threshold) {
  for (const auto& inst : insts_) {
    inst->SetSmallCompactionThreshold(small_compaction_threshold);
  }
  return Status::OK();
}

Status Storage::SetSmallCompactionDurationThreshold(uint32_t small_compaction_duration_threshold) {
  for (const auto& inst : insts_) {
    inst->SetSmallCompactionDurationThreshold(small_compaction_duration_threshold);
  }
  return Status::OK();
}

std::string Storage::GetCurrentTaskType() {
  int type = current_task_type_;
  switch (type) {
    case kCleanAll:
      return "All";
    case kCleanStrings:
      return "String";
    case kCleanHashes:
      return "Hash";
    case kCleanZSets:
      return "ZSet";
    case kCleanSets:
      return "Set";
    case kCleanLists:
      return "List";
    case kNone:
    default:
      return "No";
  }
}

Status Storage::GetUsage(const std::string& property, uint64_t* const result) {
  std::map<int, uint64_t> inst_result;
  GetUsage(property, &inst_result);
  for (const auto& it : inst_result) {
    *result += it.second;
  }
  return Status::OK();
}

Status Storage::GetUsage(const std::string& property, std::map<int, uint64_t>* const inst_result) {
  inst_result->clear();
  for (const auto& inst : insts_) {
    uint64_t value;
    inst->GetProperty(property, &value);
    (*inst_result)[inst->GetIndex()] = value;
  }
  return Status::OK();
}

uint64_t Storage::GetProperty(const std::string& property) {
  uint64_t out = 0;
  uint64_t result = 0;
  Status s;
  for (const auto& inst : insts_) {
    s = inst->GetProperty(property, &out);
    result += out;
  }
  return result;
}

Status Storage::GetKeyNum(std::vector<KeyInfo>* key_infos) {
  KeyInfo key_info;
  key_infos->resize(5);
  for (const auto& db : insts_) {
    std::vector<KeyInfo> db_key_infos;
    // check the scanner was stopped or not, before scanning the next db
    if (scan_keynum_exit_) {
      break;
    }
    auto s = db->ScanKeyNum(&db_key_infos);
    if (!s.ok()) {
      return s;
    }
    std::transform(db_key_infos.begin(), db_key_infos.end(), key_infos->begin(), key_infos->begin(), std::plus<>{});
  }
  if (scan_keynum_exit_) {
    scan_keynum_exit_ = false;
    return Status::Corruption("exit");
  }
  return Status::OK();
}

Status Storage::StopScanKeyNum() {
  scan_keynum_exit_ = true;
  return Status::OK();
}

rocksdb::DB* Storage::GetDBByIndex(int index) {
  if (index < 0 || index >= db_instance_num_) {
    WARN("Invalid DB Index: {} total: {}", index, db_instance_num_);
    return nullptr;
  }
  return insts_[index]->GetDB();
}

Status Storage::SetOptions(const OptionType& option_type, const std::unordered_map<std::string, std::string>& options) {
  Status s;
  for (const auto& inst : insts_) {
    s = inst->SetOptions(option_type, options);
    if (!s.ok()) {
      return s;
    }
  }
  return s;
}

void Storage::GetRocksDBInfo(std::string& info) {
  char temp[12] = {0};
  for (const auto& inst : insts_) {
    sprintf(temp, "instance:%2d", inst->GetIndex());
    inst->GetRocksDBInfo(info, temp);
  }
}

int64_t Storage::IsExist(const Slice& key, std::map<DataType, Status>* type_status) {
  std::string value;
  int32_t ret = 0;
  int64_t type_count = 0;
  auto& inst = GetDBInstance(key);
  Status s = inst->Get(key, &value);
  (*type_status)[DataType::kStrings] = s;
  if (s.ok()) {
    type_count++;
  }
  s = inst->HLen(key, &ret);
  (*type_status)[DataType::kHashes] = s;
  if (s.ok()) {
    type_count++;
  }
  s = inst->SCard(key, &ret);
  (*type_status)[DataType::kSets] = s;
  if (s.ok()) {
    type_count++;
  }
  uint64_t llen = 0;
  s = inst->LLen(key, &llen);
  (*type_status)[DataType::kLists] = s;
  if (s.ok()) {
    type_count++;
  }

  s = inst->ZCard(key, &ret);
  (*type_status)[DataType::kZSets] = s;
  if (s.ok()) {
    type_count++;
  }
  return type_count;
}

void Storage::DisableWal(const bool is_wal_disable) {
  for (const auto& inst : insts_) {
    inst->SetWriteWalOptions(is_wal_disable);
  }
}

Status Storage::OnBinlogWrite(const pikiwidb::Binlog& log, LogIndex log_idx) {
  auto& inst = insts_[log.slot_idx()];

  rocksdb::WriteBatch batch;
  bool is_finished_start = true;
  auto seqno = inst->GetDB()->GetLatestSequenceNumber();
  for (const auto& entry : log.entries()) {
    if (inst->IsRestarting() && inst->IsApplied(entry.cf_idx(), log_idx)) [[unlikely]] {
      // If the starting phase is over, the log must not have been applied
      // If the starting phase is not over and the log has been applied, skip it.
      WARN("Log {} has been applied", log_idx);
      is_finished_start = false;
      continue;
    }

    switch (entry.op_type()) {
      case pikiwidb::OperateType::kPut: {
        assert(entry.has_value());
        batch.Put(inst->GetColumnFamilyHandles()[entry.cf_idx()], entry.key(), entry.value());
      } break;
      case pikiwidb::OperateType::kDelete: {
        assert(!entry.has_value());
        batch.Delete(inst->GetColumnFamilyHandles()[entry.cf_idx()], entry.key());
      } break;
      default:
        static constexpr std::string_view msg = "Unknown operate type in binlog";
        ERROR(msg);
        return Status::Incomplete(msg);
    }
    inst->UpdateAppliedLogIndexOfColumnFamily(entry.cf_idx(), log_idx, ++seqno);
  }
  if (inst->IsRestarting() && is_finished_start) [[unlikely]] {
    INFO("Redis {} finished start phase", inst->GetIndex());
    inst->StartingPhaseEnd();
  }
  auto first_seqno = inst->GetDB()->GetLatestSequenceNumber() + 1;
  auto s = inst->GetDB()->Write(inst->GetWriteOptions(), &batch);
  if (!s.ok()) {
    // TODO(longfar): What we should do if the write operation failed ? 💥
    return s;
  }
  inst->UpdateLogIndex(log_idx, first_seqno);
  return s;
}

LogIndex Storage::GetSmallestFlushedLogIndex() const {
  LogIndex smallest_flushed_log_index = INT64_MAX;
  for (auto& inst : insts_) {
    smallest_flushed_log_index = std::min(smallest_flushed_log_index, inst->GetSmallestFlushedLogIndex());
  }

  return smallest_flushed_log_index;
}

}  //  namespace storage
