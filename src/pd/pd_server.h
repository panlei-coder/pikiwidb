/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <atomic>
#include <string>
#include <tuple>

#include "db.h"
#include "pstd/pstd_status.h"

namespace pikiwidb {

const std::string PD_STORE_INFO = "pd_store_info";
const std::string PD_STORE_ID = "pd_store_id";
const std::string PD_STORE_STATS = "pd_store_stats";
const std::string PD_MAX_STORE_ID = "pd_max_store_id";
const std::string PD_REGION_STATS = "pd_region_stats";
const std::string PD_MAX_REGION_ID = "pd_max_region_id";

class PDBackendLock final : public pstd::noncopyable {
 public:
  PDBackendLock(std::shared_ptr<DB> db, bool only_read = false) : db_(db), only_read_(only_read) {
    if (only_read_) {
      db_->LockShared();
    } else {
      db_->Lock();
    }
  }

  ~PDBackendLock() {
    if (only_read_) {
      db_->UnLockShared();
    } else {
      db_->UnLock();
    }
  }

 private:
  std::shared_ptr<DB> db_;
  bool only_read_ = false;
};

// PD options
class PlacementDriverOptions {
 public:
  PlacementDriverOptions(bool fake, std::string&& pd_group_id, std::string&& initial_pd_server_list)
      : fake_(fake), pd_group_id_(std::move(pd_group_id)), initial_pd_server_list_(std::move(initial_pd_server_list)) {}
  ~PlacementDriverOptions() = default;

  bool GetFake(bool fake) { return fake_; }
  std::string& GetPDGroupID() { return pd_group_id_; }
  std::string& GetInitialPDServerList() { return initial_pd_server_list_; }

 private:
  bool fake_ = false;                   // Standalone or hybrid deployment
  std::string pd_group_id_;             // PD Raft Group ID
  std::string initial_pd_server_list_;  // initial list of pd server
};

// PD
/*
Store and Region meta information is persisted to Floyd:
1.store_map_: <"pd_store_info", storeID, store> hash
2.store_id_map_: <"pd_store_id", store ip, storeID> hash
3.store_stats_map_: <"pd_store_stats", storeID, storeStats> hash
4.max_store_id_: <"pd_max_store_id", maxStoreID> string
5.region_stats_map_: <"pd_region_stats", regionID, regionStats> hash
6.max_region_id: <"pd_max_region_id", maxRegionID> string
*/
class PlacementDriverServer {
 public:
  static PlacementDriverServer& Instance();

  PlacementDriverServer(const PlacementDriverServer&) = delete;
  void operator=(const PlacementDriverServer&) = delete;
  ~PlacementDriverServer();

  pstd::Status Init(PlacementDriverOptions& pd_options);
  void Start();
  void Stop();

  std::tuple<bool, int64_t> GenerateStoreID();
  std::tuple<bool, int64_t> GenerateRegionID();
  std::tuple<bool, int64_t> CheckStoreExistByIP(const std::string& ip);
  std::tuple<bool, int64_t> AddStore(const std::string& ip, int32_t port);
  std::tuple<bool, GetClusterInfoResponse> GetClusterInfo();

 private:
  PlacementDriverServer() = default;

  int64_t db_id_ = 0;             // region id of pd
  std::atomic<bool> is_started_;  // mark whether the fragment is started
};

}  // namespace pikiwidb
