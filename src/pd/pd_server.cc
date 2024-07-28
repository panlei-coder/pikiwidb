/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "pd_server.h"

#include "praft/praft.h"
#include "pstd/log.h"
#include "pstd/pstd_string.h"
#include "store.h"

namespace pikiwidb {

PlacementDriverServer& PlacementDriverServer::Instance() {
  static PlacementDriverServer pd_server;
  return pd_server;
}

PlacementDriverServer::~PlacementDriverServer() {
  INFO("PD Server is closing...");
  Stop();
}

pstd::Status PlacementDriverServer::Init(PlacementDriverOptions& pd_options) {
  // 1.Initializing PD's db (region)
  // Currently, only independent deployment is considered, so use default value 0.
  auto status = PSTORE.AddBackend(db_id_, std::move(pd_options.GetPDGroupID()));
  if (!status.ok()) {
    ERROR("Fail to initialize db : {}", db_id_);
    return pstd::Status::Error(status.ToString());
  }

  // 2.Deploy independently as pd for now, using the default PRAFT
  if (pd_options.GetInitialPDServerList() == "None") {
    WARN("The PD member configuration is empty and the raft build group needs to be manually initialized");
  } else {
    // @todo
    // Later consider supporting pd group initialization using pd's configuration file
    WARN("Later consider supporting pd group initialization using pd's configuration file");
  }
}

void PlacementDriverServer::Start() {
  is_started_.store(true, std::memory_order_release);
  // @todo
  // In the future, you need to create a worker thread pool to implement cluster node exploration and meta information
  // collection
}

void PlacementDriverServer::Stop() {
  if (!is_started_.load(std::memory_order_acquire)) {
    return;
  }
  is_started_.store(false, std::memory_order_release);

  // @todo
  // the related thread pool release task is to be added
}

std::tuple<bool, int64_t> PlacementDriverServer::GenerateStoreID() {
  auto db = PSTORE.GetBackend(db_id_);
  // Ensures atomicity when both read and write operations are performed
  PDBackendLock pd_backend_lock(db);

  int64_t max_store_id = 0;
  std::string max_store_id_str;
  auto status = db->GetStorage()->Get(PD_MAX_STORE_ID, &max_store_id_str);
  if (status.ok()) {
    if (pstd::String2int(max_store_id_str, &max_store_id) == 0) {
      ERROR("Fail to read the correct max store id value");
      return {false, -1};
    }

    max_store_id += 1;
    status = db->GetStorage()->Set(PD_MAX_STORE_ID, pstd::Int2string(max_store_id));
    if (status.ok()) {
      ERROR("Fail to write the max store id");
      return {false, -1};
    }
  } else if (status.IsNotFound()) {
    // Note If pd is created for the first time without any store, the initial value of max_store_id is 0.
    status = db->GetStorage()->Set(PD_MAX_STORE_ID, max_store_id);
    if (status.ok()) {
      ERROR("Fail to write the max store id");
      return {false, -1};
    }
  } else {
    return {false, -1};
  }

  return {true, max_store_id};
}

std::tuple<bool, int64_t> PlacementDriverServer::GenerateRegionID() {
  auto db = PSTORE.GetBackend(db_id_);
  // Ensures atomicity when both read and write operations are performed
  PDBackendLock pd_backend_lock(db);

  int64_t max_region_id = 0;
  std::string max_region_id_str;
  auto status = db->GetStorage()->Get(PD_MAX_REGION_ID, &max_region_id_str);
  if (status.ok()) {
    if (pstd::String2int(max_region_id_str, &max_region_id) == 0) {
      ERROR("Fail to read the correct max region id value");
      return {false, -1};
    }

    max_region_id += 1;
    status = db->GetStorage()->Set(PD_MAX_REGION_ID, pstd::Int2string(max_region_id));
    if (status.ok()) {
      ERROR("Fail to write the max region id");
      return {false, -1};
    }
  } else if (status.IsNotFound()) {
    // Note If pd is created for the first time without any store, the initial value of max_store_id is 0.
    status = db->GetStorage()->Set(PD_MAX_REGION_ID, max_region_id);
    if (status.ok()) {
      ERROR("Fail to write the max region id");
      return {false, -1};
    }
  } else {
    return {false, -1};
  }

  return {true, max_region_id};
}

std::tuple<bool, int64_t> PlacementDriverServer::CheckStoreExistByIP(const std::string& ip) {
  auto db = PSTORE.GetBackend(db_id_);
  PDBackendLock pd_backend_share_lock(db, true);

  std::string store_id_str;
  auto status = db->GetStorage()->HGet(PD_STORE_ID, ip, &store_id_str);
  if (status.ok()) {
    int64_t store_id = 0;
    if (pstd::String2int(store_id_str, &store_id) == 0) {
      ERROR("Fail to read the correct max region id value");
      return {false, -1};
    }

    return {true, store_id};
  }

  return {false, -1};
}

// @todo
// pikiwidb does not support transactions and will have to consider consistency
// when writing multiple pieces of data at once
std::tuple<bool, int64_t> PlacementDriverServer::AddStore(const std::string& ip, int32_t port) {
  // 1.check whether the store has register
  auto [exist, store_id] = CheckStoreExistByIP(ip);
  if (exist) {
    return {true, store_id};
  }

  // 2. generate store id
  auto [success, new_store_id] = GenerateStoreID();
  if (!success) {
    return {false, -1};
  }

  // 3. update
  // store_id_map_: <"pd_store_id", store ip, storeID>
  auto db = PSTORE.GetBackend(db_id_);
  PDBackendLock pd_backend_share_lock(db);
  int temp = 0;
  auto value = pstd::Int2string(new_store_id);
  auto status = db->GetStorage()->HSet(PD_STORE_ID, ip, value, &temp);
  if (!status.ok()) {
    return {false, -1};
  }

  // store_map_: <"pd_store_info", storeID, store> hash
  Store store;
  store.set_store_id(new_store_id);
  store.set_ip(ip);
  store.set_ip(port);
  store.set_state(StoreState::UP);
  std::string store_str;
  if (!store.SerializeToString(&store_str)) {
    return {false, -1};
  }
  status = db->GetStorage()->HSet(PD_STORE_INFO, new_store_id, store_str, &temp);
  if (!status.ok()) {
    return {false, -1};
  }

  // store_stats_map_: <"pd_store_stats", storeID, storeStats> hash
  StoreStatsResponse store_stats;
  store_stats.set_store_id(new_store_id);
  std::string store_stats_str;
  if (!store_stats.SerializeToString(&store_stats_str)) {
    return {false, -1};
  }
  status = db->GetStorage()->HSet(PD_STORE_STATS, new_store_id, store_stats_str, &temp);
  if (!status.ok()) {
    return {false, -1};
  }

  return {true, new_store_id};
}

}  // namespace pikiwidb
