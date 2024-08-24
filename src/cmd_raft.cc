/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include "cmd_raft.h"

#include <fmt/format.h>
#include <cstdint>
#include <optional>
#include <string>

#include "brpc/channel.h"
#include "praft.pb.h"
#include "praft/praft.h"
#include "pstd/log.h"
#include "pstd/pstd_status.h"
#include "pstd/pstd_string.h"

#include "client.h"
#include "config.h"
#include "pikiwidb.h"
#include "replication.h"
#include "store.h"

namespace pikiwidb {

extern PConfig g_config;

RaftNodeCmd::RaftNodeCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsRaft, kAclCategoryRaft) {}

bool RaftNodeCmd::DoInitial(PClient* client) {
  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);

  if (cmd != kAddCmd && cmd != kRemoveCmd && cmd != kDoSnapshot) {
    client->SetRes(CmdRes::kErrOther, "RAFT.NODE supports ADD / REMOVE / DOSNAPSHOT only");
    return false;
  }
  group_id_ = client->argv_[2];

  return true;
}

void RaftNodeCmd::DoCmd(PClient* client) {
  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);
  if (cmd == kAddCmd) {
    DoCmdAdd(client);
  } else if (cmd == kRemoveCmd) {
    DoCmdRemove(client);
  } else if (cmd == kDoSnapshot) {
    assert(0);  // TODO(longfar): add group id in arguments
    DoCmdSnapshot(client);
  } else {
    client->SetRes(CmdRes::kErrOther, "RAFT.NODE supports ADD / REMOVE / DOSNAPSHOT only");
  }
}

void RaftNodeCmd::DoCmdAdd(PClient* client) {
  DEBUG("Received RAFT.NODE ADD cmd from {}", client->PeerIP());
  auto& praft = PSTORE.GetDBByGroupID(group_id_)->GetPRaft();
  // Check whether it is a leader. If it is not a leader, return the leader information
  if (!praft->IsLeader()) {
    client->SetRes(CmdRes::kWrongLeader, praft->GetLeaderID());
    return;
  }

  if (client->argv_.size() != 4) {
    client->SetRes(CmdRes::kWrongNum, client->CmdName());
    return;
  }

  // RedisRaft has nodeid, but in Braft, NodeId is IP:Port.
  // So we do not need to parse and use nodeid like redis;
  auto s = praft->AddPeer(client->argv_[3]);
  if (s.ok()) {
    client->SetRes(CmdRes::kOK);
  } else {
    client->SetRes(CmdRes::kErrOther, fmt::format("Failed to add peer: {}", s.error_str()));
  }
}

void RaftNodeCmd::DoCmdRemove(PClient* client) {
  auto& praft = PSTORE.GetDBByGroupID(group_id_)->GetPRaft();
  // If the node has been initialized, it needs to close the previous initialization and rejoin the other group
  if (!praft->IsInitialized()) {
    client->SetRes(CmdRes::kErrOther, "Don't already cluster member");
    return;
  }

  if (client->argv_.size() != 3) {
    client->SetRes(CmdRes::kWrongNum, client->CmdName());
    return;
  }

  // Check whether it is a leader. If it is not a leader, send remove request to leader
  if (!praft->IsLeader()) {
    // Get the leader information
    braft::PeerId leader_peer_id(praft->GetLeaderID());
    // @todo There will be an unreasonable address, need to consider how to deal with it
    if (leader_peer_id.is_empty()) {
      client->SetRes(CmdRes::kErrOther,
                     "The leader address of the cluster is incorrect, try again or delete the node from another node");
      return;
    }

    brpc::ChannelOptions options;
    options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
    options.max_retry = 0;
    options.connect_timeout_ms = kChannelTimeoutMS;

    NodeRemoveRequest request;
    NodeRemoveResponse response;

    request.set_group_id(praft->GetGroupID());
    request.set_endpoint(client->argv_[2]);
    request.set_index(client->GetCurrentDB());
    request.set_role(0);

    auto endpoint = leader_peer_id.addr;
    int retry_count = 0;
    do {
      brpc::Channel remove_node_channel;
      if (0 != remove_node_channel.Init(endpoint, &options)) {
        ERROR("Fail to init remove_node_channel to praft service!");
        client->SetRes(CmdRes::kErrOther, "Fail to init remove_node_channel.");
        return;
      }

      brpc::Controller cntl;
      PRaftService_Stub stub(&remove_node_channel);
      stub.RemoveNode(&cntl, &request, &response, NULL);

      if (cntl.Failed()) {
        ERROR("Fail to send remove node rpc to target server {}", butil::endpoint2str(endpoint).c_str());
        client->SetRes(CmdRes::kErrOther, "Failed to send remove node rpc");
        return;
      }

      if (response.success()) {
        client->SetRes(CmdRes::kOK, "Remove Node Success");
        return;
      }

      switch (response.error_code()) {
        case PRaftErrorCode::kErrorReDirect: {
          butil::str2endpoint(response.leader_endpoint().c_str(), &endpoint);
          endpoint.port += g_config.raft_port_offset;
          break;
        }
        default: {
          ERROR("Remove node request return false");
          client->SetRes(CmdRes::kErrOther, "Failed to Remove Node");
          return;
        }
      }
    } while (!response.success() && ++retry_count <= 3);

    ERROR("Remove node request return false");
    client->SetRes(CmdRes::kErrOther, "Failed to Remove Node");
    return;
  }

  auto s = praft->RemovePeer(client->argv_[2], client->GetCurrentDB());
  if (s.ok()) {
    client->SetRes(CmdRes::kOK);
  } else {
    client->SetRes(CmdRes::kErrOther, fmt::format("Failed to remove peer: {}", s.error_str()));
  }
}

void RaftNodeCmd::DoCmdSnapshot(PClient* client) {
  auto& praft = PSTORE.GetDBByGroupID(group_id_)->GetPRaft();
  auto s = praft->DoSnapshot();
  if (s.ok()) {
    client->SetRes(CmdRes::kOK);
  }
}

RaftClusterCmd::RaftClusterCmd(const std::string& name, int16_t arity)
    : BaseCmd(name, arity, kCmdFlagsRaft, kAclCategoryRaft) {}

bool RaftClusterCmd::DoInitial(PClient* client) {
  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);
  if (cmd != kInitCmd && cmd != kJoinCmd) {
    client->SetRes(CmdRes::kErrOther, "RAFT.CLUSTER supports INIT/JOIN only");
    return false;
  }

  return true;
}

void RaftClusterCmd::DoCmd(PClient* client) {
  auto db = PSTORE.GetBackend(client->GetCurrentDB());
  if (db) {
    auto& praft = db->GetPRaft();
    if (praft->IsInitialized()) {
      return client->SetRes(CmdRes::kErrOther, "Already cluster member");
    }
  }

  auto cmd = client->argv_[1];
  pstd::StringToUpper(cmd);
  if (cmd == kInitCmd) {
    DoCmdInit(client);
  } else {
    DoCmdJoin(client);
  }
}

void RaftClusterCmd::DoCmdInit(PClient* client) {
  if (client->argv_.size() != 2 && client->argv_.size() != 3) {
    return client->SetRes(CmdRes::kWrongNum, client->CmdName());
  }

  std::string group_id;
  if (client->argv_.size() == 3) {
    group_id = client->argv_[2];
    if (group_id.size() != RAFT_GROUPID_LEN) {
      return client->SetRes(CmdRes::kInvalidParameter,
                            "Cluster id must be " + std::to_string(RAFT_GROUPID_LEN) + " characters");
    }
  } else {
    group_id = pstd::RandomHexChars(RAFT_GROUPID_LEN);
  }

  std::string group_id_copy(group_id);
  auto s = PSTORE.AddBackend(client->GetCurrentDB(), std::move(group_id_copy));
  if (s.ok()) {
    client->SetLineString(fmt::format("+OK {}", group_id));
  } else {
    client->SetRes(CmdRes::kErrOther, fmt::format("The current GroupID {} already exists", group_id));
  }
}

static inline std::optional<std::pair<std::string, int32_t>> GetIpAndPortFromEndPoint(const std::string& endpoint) {
  auto pos = endpoint.find(':');
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  int32_t ret = 0;
  pstd::String2int(endpoint.substr(pos + 1), &ret);
  return {{endpoint.substr(0, pos), ret}};
}

void RaftClusterCmd::DoCmdJoin(PClient* client) {
  assert(client->argv_.size() == 4);
  auto group_id = client->argv_[2];
  auto addr = client->argv_[3];
  butil::EndPoint endpoint;
  if (0 != butil::str2endpoint(addr.c_str(), &endpoint)) {
    ERROR("Wrong endpoint format: {}", addr);
    return client->SetRes(CmdRes::kErrOther, "Wrong endpoint format");
  }
  endpoint.port += g_config.raft_port_offset;

  if (group_id.size() != RAFT_GROUPID_LEN) {
    return client->SetRes(CmdRes::kInvalidParameter,
                          "Cluster id must be " + std::to_string(RAFT_GROUPID_LEN) + " characters");
  }

  std::string group_id_copy(group_id);
  auto db_id = client->GetCurrentDB();
  auto s = PSTORE.AddBackend(db_id, std::move(group_id_copy));
  if (!s.ok()) {
    client->SetRes(CmdRes::kErrOther,
                   fmt::format("The current GroupID {} fails to create region because of {}", group_id, s.ToString()));
  }

  brpc::ChannelOptions options;
  options.connection_type = brpc::CONNECTION_TYPE_SINGLE;
  options.max_retry = 0;
  options.connect_timeout_ms = kChannelTimeoutMS;

  NodeAddRequest request;
  NodeAddResponse response;

  auto end_point = std::string(butil::endpoint2str(PSTORE.GetEndPoint()).c_str());
  request.set_group_id(group_id);
  request.set_endpoint(end_point);
  request.set_index(client->GetCurrentDB());
  request.set_role(0);

  int retry_count = 0;

  do {
    brpc::Channel add_node_channel;
    if (0 != add_node_channel.Init(endpoint, &options)) {
      PSTORE.RemoveBackend(db_id);
      ERROR("Fail to init add_node_channel to praft service!");
      client->SetRes(CmdRes::kErrOther, "Fail to init add_node_channel.");
      return;
    }

    brpc::Controller cntl;
    PRaftService_Stub stub(&add_node_channel);
    stub.AddNode(&cntl, &request, &response, NULL);

    if (cntl.Failed()) {
      PSTORE.RemoveBackend(db_id);
      ERROR("Fail to send add node rpc to target server {}", addr);
      client->SetRes(CmdRes::kErrOther, "Failed to send add node rpc");
      return;
    }

    if (response.success()) {
      client->SetRes(CmdRes::kOK, "Add Node Success");
      return;
    }

    switch (response.error_code()) {
      case PRaftErrorCode::kErrorReDirect: {
        butil::str2endpoint(response.leader_endpoint().c_str(), &endpoint);
        endpoint.port += g_config.raft_port_offset;
        break;
      }
      default: {
        ERROR("Add node request return false");
        PSTORE.RemoveBackend(db_id);
        client->SetRes(CmdRes::kErrOther, "Failed to Add Node");
        return;
      }
    }
  } while (!response.success() && ++retry_count <= 3);

  ERROR("Add node request return false");
  PSTORE.RemoveBackend(db_id);
  client->SetRes(CmdRes::kErrOther, "Failed to Add Node");
}

}  // namespace pikiwidb
