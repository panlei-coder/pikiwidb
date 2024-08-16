/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "base_cmd.h"

#include <fmt/format.h>

#include "common.h"
#include "config.h"
#include "log.h"
#include "pikiwidb.h"
#include "praft/praft.h"
#include "store.h"
#include "pstd_string.h"

namespace pikiwidb {

BaseCmd::BaseCmd(std::string name, int16_t arity, uint32_t flag, uint32_t aclCategory) {
  name_ = std::move(name);
  arity_ = arity;
  flag_ = flag;
  acl_category_ = aclCategory;
  cmd_id_ = g_pikiwidb->GetCmdID();
}

bool BaseCmd::CheckArg(size_t num) const {
  if (arity_ > 0) {
    return num == arity_;
  }
  return num >= -arity_;
}

std::vector<std::string> BaseCmd::CurrentKey(PClient* client) const { return std::vector<std::string>{client->Key()}; }

void BaseCmd::Execute(PClient* client) {
  DEBUG("execute command: {}", client->CmdName());

  if (g_config.use_raft.load()) {
    auto praft = PSTORE.GetBackend(client->GetCurrentDB())->GetPRaft();
    // 1. If PRAFT is not initialized yet, return an error message to the client for both read and write commands.
    if (!praft->IsInitialized() && (HasFlag(kCmdFlagsReadonly) || HasFlag(kCmdFlagsWrite))) {
      DEBUG("drop command: {}", client->CmdName());
      return client->SetRes(CmdRes::kErrOther, "PRAFT is not initialized");
    }

    // 2. If PRAFT is initialized and the current node is not the leader, return a redirection message for write
    // commands.
    if (HasFlag(kCmdFlagsWrite) && !praft->IsLeader()) {
      return client->SetRes(CmdRes::kErrOther, fmt::format("MOVED {}", praft->GetLeaderAddress()));
    }
  }

  auto db_id = client->GetCurrentDB();
  auto db = PSTORE.GetBackend(db_id);
  if (db == nullptr) {
    /*
    @todo
    Since the creation of shards through pd is not currently supported, if the shard to be accessed does not exist, the
    operation of the shard only supports the raft command, and the creation of shards is temporarily initialized using
    the raft.cluster init command. After pd is supported to create shards, it is logical that this command should not be
    allowed to create shards.
    */
    auto cmd_name = client->CmdName();
    pstd::StringToLower(cmd_name);
    if (cmd_name != kCmdNameRaftCluster) {
      return client->SetRes(CmdRes::kErrOther,
                            fmt::format("The db of {} that the client wants to access does not exist", db_id));
    }
  } else {
    if (!HasFlag(kCmdFlagsExclusive)) {
      db->LockShared();
    }

    DEFER {
      if (!HasFlag(kCmdFlagsExclusive)) {
        db->UnLockShared();
      }
    };
  }

  if (!DoInitial(client)) {
    return;
  }
  DoCmd(client);
}

std::string BaseCmd::ToBinlog(uint32_t exec_time, uint32_t term_id, uint64_t logic_id, uint32_t filenum,
                              uint64_t offset) {
  return "";
}

void BaseCmd::DoBinlog() {}
bool BaseCmd::HasFlag(uint32_t flag) const { return flag_ & flag; }
void BaseCmd::SetFlag(uint32_t flag) { flag_ |= flag; }
void BaseCmd::ResetFlag(uint32_t flag) { flag_ &= ~flag; }
bool BaseCmd::HasSubCommand() const { return false; }
BaseCmd* BaseCmd::GetSubCmd(const std::string& cmdName) { return nullptr; }
uint32_t BaseCmd::AclCategory() const { return acl_category_; }
void BaseCmd::AddAclCategory(uint32_t aclCategory) { acl_category_ |= aclCategory; }
std::string BaseCmd::Name() const { return name_; }
// CmdRes& BaseCommand::Res() { return res_; }
// void BaseCommand::SetResp(const std::shared_ptr<std::string>& resp) { resp_ = resp; }
// std::shared_ptr<std::string> BaseCommand::GetResp() { return resp_.lock(); }
uint32_t BaseCmd::GetCmdID() const { return cmd_id_; }

// BaseCmdGroup
BaseCmdGroup::BaseCmdGroup(const std::string& name, uint32_t flag) : BaseCmdGroup(name, -2, flag) {}
BaseCmdGroup::BaseCmdGroup(const std::string& name, int16_t arity, uint32_t flag) : BaseCmd(name, arity, flag, 0) {}

void BaseCmdGroup::AddSubCmd(std::unique_ptr<BaseCmd> cmd) { subCmds_[cmd->Name()] = std::move(cmd); }

BaseCmd* BaseCmdGroup::GetSubCmd(const std::string& cmdName) {
  auto subCmd = subCmds_.find(cmdName);
  if (subCmd == subCmds_.end()) {
    return nullptr;
  }
  return subCmd->second.get();
}

bool BaseCmdGroup::DoInitial(PClient* client) {
  client->SetSubCmdName(client->argv_[1]);
  if (!subCmds_.contains(client->SubCmdName())) {
    client->SetRes(CmdRes::kSyntaxErr, client->argv_[0] + " unknown subcommand for '" + client->SubCmdName() + "'");
    return false;
  }
  return true;
}

}  // namespace pikiwidb
