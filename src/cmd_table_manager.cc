/*
 * Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "cmd_table_manager.h"

#include <memory>

#include "cmd_admin.h"
#include "cmd_hash.h"
#include "cmd_keys.h"
#include "cmd_kv.h"
#include "cmd_list.h"
#include "cmd_raft.h"
#include "cmd_set.h"
#include "cmd_zset.h"

namespace pikiwidb {

#define ADD_COMMAND(cmd, argc)                                                      \
  do {                                                                              \
    std::unique_ptr<BaseCmd> ptr = std::make_unique<cmd##Cmd>(kCmdName##cmd, argc); \
    cmds_->insert(std::make_pair(kCmdName##cmd, std::move(ptr)));                   \
  } while (0)

#define ADD_COMMAND_GROUP(cmd, argc)                                                \
  do {                                                                              \
    std::unique_ptr<BaseCmd> ptr = std::make_unique<Cmd##cmd>(kCmdName##cmd, argc); \
    cmds_->insert(std::make_pair(kCmdName##cmd, std::move(ptr)));                   \
  } while (0)

#define ADD_SUBCOMMAND(cmd, subcmd, argc)                                                                 \
  do {                                                                                                    \
    auto it##cmd = cmds_->find(kCmdName##cmd);                                                            \
    auto ptr##cmd = std::unique_ptr<BaseCmdGroup>(static_cast<BaseCmdGroup*>(it##cmd->second.release())); \
    ptr##cmd->AddSubCmd(std::make_unique<Cmd##cmd##subcmd>(kSubCmdName##cmd##subcmd, argc));              \
    it##cmd->second = std::move(ptr##cmd);                                                                \
  } while (0)

CmdTableManager::CmdTableManager() {
  cmds_ = std::make_unique<CmdTable>();
  cmds_->reserve(300);
}

void CmdTableManager::InitCmdTable() {
  std::unique_lock wl(mutex_);

  // admin
  ADD_COMMAND_GROUP(Config, -2);
  ADD_SUBCOMMAND(Config, Get, -3);
  ADD_SUBCOMMAND(Config, Set, -4);
  ADD_COMMAND(Ping, 0);
  ADD_COMMAND_GROUP(Debug, -2);
  ADD_SUBCOMMAND(Debug, Help, 2);
  ADD_SUBCOMMAND(Debug, OOM, 2);
  ADD_SUBCOMMAND(Debug, Segfault, 2);
  ADD_COMMAND(Sort, -2);

  // server
  ADD_COMMAND(Flushdb, 1);
  ADD_COMMAND(Flushall, 1);
  ADD_COMMAND(Select, 2);
  ADD_COMMAND(Shutdown, 1);

  // info
  ADD_COMMAND(Info, -1);

  // raft
  ADD_COMMAND(RaftCluster, -1);
  ADD_COMMAND(RaftNode, -2);

  // keyspace
  ADD_COMMAND(Del, -2);
  ADD_COMMAND(Exists, -2);
  ADD_COMMAND(Type, 2);
  ADD_COMMAND(Expire, 3);
  ADD_COMMAND(Ttl, 2);
  ADD_COMMAND(PExpire, 3);
  ADD_COMMAND(Expireat, 3);
  ADD_COMMAND(PExpireat, 3);
  ADD_COMMAND(Pttl, 2);
  ADD_COMMAND(Persist, 2);
  ADD_COMMAND(Keys, 2);
  ADD_COMMAND(Rename, 3);
  ADD_COMMAND(RenameNX, 3);

  // kv
  ADD_COMMAND(Get, 2);
  ADD_COMMAND(Set, -3);
  ADD_COMMAND(MGet, -2);
  ADD_COMMAND(MSet, -3);
  ADD_COMMAND(GetSet, 3);
  ADD_COMMAND(SetNX, 3);
  ADD_COMMAND(Append, 3);
  ADD_COMMAND(Strlen, 2);
  ADD_COMMAND(Incr, 2);
  ADD_COMMAND(Incrby, 3);
  ADD_COMMAND(Decrby, 3);
  ADD_COMMAND(IncrbyFloat, 3);
  ADD_COMMAND(SetEx, 4);
  ADD_COMMAND(PSetEx, 4);
  ADD_COMMAND(BitOp, -4);
  ADD_COMMAND(BitCount, -2);
  ADD_COMMAND(GetBit, 3);
  ADD_COMMAND(GetRange, 4);
  ADD_COMMAND(SetRange, 4);
  ADD_COMMAND(Decr, 2);
  ADD_COMMAND(SetBit, 4);
  ADD_COMMAND(MSetnx, -3);

  // hash
  ADD_COMMAND(HSet, -4);
  ADD_COMMAND(HGet, 3);
  ADD_COMMAND(HDel, -3);
  ADD_COMMAND(HMSet, -4);
  ADD_COMMAND(HMGet, -3);
  ADD_COMMAND(HGetAll, 2);
  ADD_COMMAND(HKeys, 2);
  ADD_COMMAND(HLen, 2);
  ADD_COMMAND(HStrLen, 3);
  ADD_COMMAND(HScan, -3);
  ADD_COMMAND(HVals, 2);
  ADD_COMMAND(HIncrbyFloat, 4);
  ADD_COMMAND(HSetNX, 4);
  ADD_COMMAND(HIncrby, 4);
  ADD_COMMAND(HRandField, -2);
  ADD_COMMAND(HExists, 3);

  // set
  ADD_COMMAND(SIsMember, 3);
  ADD_COMMAND(SAdd, -3);
  ADD_COMMAND(SUnionStore, -3);
  ADD_COMMAND(SRem, -3);
  ADD_COMMAND(SInter, -2);
  ADD_COMMAND(SUnion, -2);
  ADD_COMMAND(SInterStore, -3);
  ADD_COMMAND(SCard, 2);
  ADD_COMMAND(SMove, 4);
  ADD_COMMAND(SRandMember, -2);  // Added the count argument since Redis 3.2.0
  ADD_COMMAND(SPop, -2);
  ADD_COMMAND(SMembers, 2);
  ADD_COMMAND(SDiff, -2);
  ADD_COMMAND(SDiffstore, -3);
  ADD_COMMAND(SScan, -3);

  // list
  ADD_COMMAND(LPush, -3);
  ADD_COMMAND(RPush, -3);
  ADD_COMMAND(RPop, 2);
  ADD_COMMAND(LRem, 4);
  ADD_COMMAND(LRange, 4);
  ADD_COMMAND(LTrim, 4);
  ADD_COMMAND(LSet, 4);
  ADD_COMMAND(LInsert, 5);
  ADD_COMMAND(LPushx, -3);
  ADD_COMMAND(RPushx, -3);
  ADD_COMMAND(LPop, 2);
  ADD_COMMAND(LIndex, 3);
  ADD_COMMAND(LLen, 2);
  ADD_COMMAND(RPoplpush, 3);

  // zset
  ADD_COMMAND(ZAdd, -4);
  ADD_COMMAND(ZPopMin, -2);
  ADD_COMMAND(ZPopMax, -2);
  ADD_COMMAND(ZInterstore, -4);
  ADD_COMMAND(ZUnionstore, -4);
  ADD_COMMAND(ZRevrange, -4);
  ADD_COMMAND(ZRangebyscore, -4);
  ADD_COMMAND(ZRemrangebyscore, 4);
  ADD_COMMAND(ZRemrangebyrank, 4);
  ADD_COMMAND(ZRevrangebyscore, -4);
  ADD_COMMAND(ZCard, 2);
  ADD_COMMAND(ZScore, 3);
  ADD_COMMAND(ZRange, -4);
  ADD_COMMAND(ZRangebylex, -3);
  ADD_COMMAND(ZRevrangebylex, -3);
  ADD_COMMAND(ZRank, 3);
  ADD_COMMAND(ZRevrank, 3);
  ADD_COMMAND(ZRem, -3);
  ADD_COMMAND(ZIncrby, 4);
}

std::pair<BaseCmd*, CmdRes::CmdRet> CmdTableManager::GetCommand(const std::string& cmdName, PClient* client) {
  std::shared_lock rl(mutex_);

  auto cmd = cmds_->find(cmdName);

  if (cmd == cmds_->end()) {
    return std::pair(nullptr, CmdRes::kSyntaxErr);
  }

  if (cmd->second->HasSubCommand()) {
    if (client->argv_.size() < 2) {
      return std::pair(nullptr, CmdRes::kInvalidParameter);
    }
    return std::pair(cmd->second->GetSubCmd(client->argv_[1]), CmdRes::kSyntaxErr);
  }
  return std::pair(cmd->second.get(), CmdRes::kSyntaxErr);
}

bool CmdTableManager::CmdExist(const std::string& cmd) const {
  std::shared_lock rl(mutex_);
  return cmds_->find(cmd) != cmds_->end();
}

uint32_t CmdTableManager::GetCmdId() { return ++cmdId_; }
}  // namespace pikiwidb
