/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "praft/praft.h"
#include "praft/praft_service.h"

#define GLOG_NO_ABBREVIATED_SEVERITIES

#include <atomic>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "brpc/server.h"
#include "butil/endpoint.h"

#include "db.h"
#include "pd/pd_service.h"
#include "praft/praft_service.h"
#include "pstd/pstd_status.h"
#include "storage/storage.h"
#include "store_service.h"

namespace pikiwidb {
class RaftServiceImpl;

enum TaskType { kCheckpoint = 0, kLoadDBFromCheckpoint, kEmpty };

enum TaskArg {
  kCheckpointPath = 0,
};

struct TaskContext {
  TaskType type = kEmpty;
  int db = -1;
  std::map<TaskArg, std::string> args;
  bool sync = false;
  TaskContext() = delete;
  TaskContext(TaskType t, bool s = false) : type(t), sync(s) {}
  TaskContext(TaskType t, uint32_t d, bool s = false) : type(t), db(d), sync(s) {}
  TaskContext(TaskType t, uint32_t d, const std::map<TaskArg, std::string>& a, bool s = false)
      : type(t), db(d), args(a), sync(s) {}
};

using TasksVector = std::vector<TaskContext>;

enum PRaftErrorCode {
  kErrorDisMatch = 0,
  kErrorAddNode,
  kErrorRemoveNode,
  kErrorReDirect,
};

class PStore {
 public:
  static PStore& Instance();

  PStore(const PStore&) = delete;
  void operator=(const PStore&) = delete;
  ~PStore();

  void Init();
  bool InitRpcServer();
  bool RegisterStoreToPDServer();

  void SetStoreID(int64_t store_id) { store_id_.store(store_id, std::memory_order_relaxed); }

  int64_t GetStoreID() { return store_id_.load(std::memory_order_relaxed); }

  std::shared_ptr<DB> GetBackend(int64_t db_id);

  std::shared_ptr<DB> GetDBByGroupID(const std::string& group_id);

  pstd::Status AddBackend(int64_t db_id, std::string&& group_id);

  pstd::Status RemoveBackend(int64_t db_id);

  void HandleTaskSpecificDB(const TasksVector& tasks);

  const butil::EndPoint& GetEndPoint() const { return endpoint_; }

 private:
  PStore() = default;

  std::atomic<int64_t> store_id_ = {0};
  butil::EndPoint endpoint_;

  std::unique_ptr<brpc::Server> rpc_server_{nullptr};
  std::unique_ptr<PRaftServiceImpl> praft_service_{nullptr};         // praft service
  std::unique_ptr<PlacementDriverServiceImpl> pd_service_{nullptr};  // pd service
  std::unique_ptr<StoreServiceImpl> store_service_{nullptr};         // store service

  std::shared_mutex store_mutex_;
  std::unordered_map<int64_t, std::shared_ptr<DB>> backends_table_;  // <db_id, db> / <region_id, region_engine>
  std::unordered_map<std::string, int64_t> group_id_of_db_id_;       // <group_id, db_id> / <group_id, region_id>

  std::atomic<int64_t> is_started_ = {false};
};

#define PSTORE PStore::Instance()

}  // namespace pikiwidb
