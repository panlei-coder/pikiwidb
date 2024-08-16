/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "store.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "braft/raft.h"
#include "braft/route_table.h"
#include "braft/util.h"
#include "brpc/channel.h"
#include "brpc/controller.h"

#include "config.h"
#include "db.h"
#include "pd.pb.h"
#include "pd/pd_server.h"
#include "praft/praft_service.h"
#include "pstd/log.h"
#include "pstd/pstd_string.h"

namespace pikiwidb {
PStore::~PStore() { INFO("STORE is closing..."); }

PStore& PStore::Instance() {
  static PStore store;
  return store;
}

void PStore::Init() {
  // 1. init rpc
  if (!InitRpcServer()) {
    ERROR("STORE Init failed!");
    return;
  }

  // 2. Currently, only pd independent deployment is supported if the current node is not a pd node,
  // the current node needs to report to the pd node that it is online as a common node.
  if (!RegisterStoreToPDServer()) {
    ERROR("STORE Init failed!");
    return;
  }

  // 3.If the node acts as a pd, then the initializer node can build a pd group based on the configuration.
  if (g_config.as_pd.load(std::memory_order_relaxed)) {
    PlacementDriverOptions pd_options(g_config.fake.load(std::memory_order_relaxed),
                                      std::move(g_config.pd_group_id.ToString()),
                                      std::move(g_config.pd_conf.ToString()));
    PDSERVER.Init(pd_options);
    PDSERVER.Start();
  }

  INFO("STORE Init success!");
}

bool PStore::InitRpcServer() {
  rpc_server_ = std::make_unique<brpc::Server>();
  auto port = g_config.port.load(std::memory_order_relaxed) +
              pikiwidb::g_config.raft_port_offset.load(std::memory_order_relaxed);

  if (braft::add_service(rpc_server_.get(), port) != 0) {
    rpc_server_.reset();
    ERROR("Failed to add raft service");
    return false;
  }

  // Add praft service into RPC server
  praft_service_ = std::make_unique<PRaftServiceImpl>();
  if (rpc_server_->AddService(praft_service_.get(), brpc::SERVER_OWNS_SERVICE) != 0) {
    rpc_server_.reset();
    ERROR("Failed to add service");
    return false;
  }

  // Add PDService if the node as the pd
  if (g_config.as_pd.load(std::memory_order_relaxed)) {
    pd_service_ = std::make_unique<PlacementDriverServiceImpl>();
    if (rpc_server_->AddService(pd_service_.get(), brpc::SERVER_OWNS_SERVICE) != 0) {
      rpc_server_.reset();
      ERROR("Failed to add service");
      return false;
    }
  }

  // Add StoreService if the node is deployed in mixed mode or is not currently used as a pd node
  if (!g_config.as_pd.load(std::memory_order_relaxed) || g_config.fake.load(std::memory_order_relaxed)) {
    store_service_ = std::make_unique<StoreServiceImpl>();
    if (rpc_server_->AddService(store_service_.get(), brpc::SERVER_OWNS_SERVICE) != 0) {
      rpc_server_.reset();
      ERROR("Failed to add service");
      return false;
    }
  }

  if (rpc_server_->Start(port, nullptr) != 0) {
    rpc_server_.reset();
    ERROR("Failed to start server");
    return false;
  }

  return true;
}

bool PStore::RegisterStoreToPDServer() {
  if (!g_config.as_pd.load(std::memory_order_relaxed)) {
    // Register configuration of target group to RouteTable
    if (braft::rtb::update_configuration(g_config.pd_group_id.ToString(), g_config.pd_conf.ToString()) != 0) {
      ERROR("Fail to register configuration {} of group {}", g_config.pd_conf.ToString(),
            g_config.pd_group_id.ToString());
      return false;
    }

    auto conf = g_config.pd_conf.ToString();
    int retry_times = std::count_if(conf.begin(), conf.end(), [](char& c) { return c == ','; }) + 2;
    for (int i = 0; i < retry_times; i++) {
      braft::PeerId leader;
      // select leader of the target group from RouteTable
      if (braft::rtb::select_leader(g_config.pd_group_id, &leader) != 0) {
        // leader is unknow in RouteTable. Ask RouteTable to refresh leader by sending RPCs.
        butil::Status st = braft::rtb::refresh_leader(g_config.pd_group_id, g_config.request_timeout_ms);
        if (!st.ok()) {
          // not sure about the leader, sleep for a while and the ask again.
          WARN("Fail to refresh_leader : {}", st.error_str());
          std::chrono::milliseconds duration(g_config.request_timeout_ms);
          std::this_thread::sleep_for(duration);
        }
        continue;
      }

      // Now we know who is the leader, construct Stub and then sending
      // rpc
      brpc::Channel channel;
      if (channel.Init(leader.addr, NULL) != 0) {
        WARN("Fail to init channel to {}", leader.to_string());
        std::chrono::milliseconds duration(g_config.request_timeout_ms);
        std::this_thread::sleep_for(duration);
        continue;
      }
      PlacementDriverService_Stub stub(&channel);

      brpc::Controller cntl;
      cntl.set_timeout_ms(g_config.request_timeout_ms);
      AddStoreResponse response;
      AddStoreRequest request;
      request.set_ip(g_config.ip.ToString());
      request.set_port(g_config.port.load(std::memory_order_relaxed));
      stub.AddStore(&cntl, &request, &response, NULL);

      if (cntl.Failed()) {
        WARN("Fail to send request to {} : {}", leader.to_string(), cntl.ErrorText());
        // clear leadership since this RPC failed.
        braft::rtb::update_leader(g_config.pd_group_id.ToString(), braft::PeerId());
        std::chrono::milliseconds duration(g_config.request_timeout_ms);
        std::this_thread::sleep_for(duration);
        continue;
      }

      if (!response.success()) {
        WARN("Fail to send request to {}, redirecting to {}", leader.to_string(),
             response.has_redirect() ? response.redirect() : "nowhere");
        // update route table since we have redirect information
        braft::rtb::update_leader(g_config.pd_group_id.ToString(), response.redirect());
        std::chrono::milliseconds duration(g_config.request_timeout_ms);
        std::this_thread::sleep_for(duration);
        continue;
      }

      SetStoreID(response.store_id());
      return true;
    }

    return false;
  }

  return true;
}

std::shared_ptr<DB> PStore::GetBackend(int64_t db_id) {
  std::shared_lock lock(store_mutex_);
  auto it = backends_table_.find(db_id);
  if (it != backends_table_.end()) {
    return it->second;
  }

  WARN("the db of {} is not exist!", db_id);
  return nullptr;
}

pstd::Status PStore::AddBackend(int64_t db_id, std::string&& group_id) {
  std::lock_guard<std::shared_mutex> lock(store_mutex_);
  auto it = backends_table_.find(db_id);
  if (it != backends_table_.end()) {
    return pstd::Status::OK();
  }

  backends_table_.insert({db_id, std::make_shared<DB>(db_id, g_config.db_path)});
  return backends_table_[db_id]->Init(std::move(group_id));
}

void PStore::HandleTaskSpecificDB(const TasksVector& tasks) {
  std::for_each(tasks.begin(), tasks.end(), [this](const auto& task) {
    auto db = GetBackend(task.db);
    if (db == nullptr) {
      WARN("The database of db_id is not exit.");
      return;
    }

    switch (task.type) {
      case kCheckpoint: {
        if (auto s = task.args.find(kCheckpointPath); s == task.args.end()) {
          WARN("The critical parameter 'path' is missing for do a checkpoint.");
          return;
        }
        auto path = task.args.find(kCheckpointPath)->second;
        pstd::TrimSlash(path);
        db->CreateCheckpoint(path, task.sync);
        break;
      }
      case kLoadDBFromCheckpoint: {
        if (auto s = task.args.find(kCheckpointPath); s == task.args.end()) {
          WARN("The critical parameter 'path' is missing for load a checkpoint.");
          return;
        }
        auto path = task.args.find(kCheckpointPath)->second;
        pstd::TrimSlash(path);
        db->LoadDBFromCheckpoint(path, task.sync);
        break;
      }
      case kEmpty: {
        WARN("A empty task was passed in, not doing anything.");
        break;
      }
      default:
        break;
    }
  });
}

}  // namespace pikiwidb
