/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "store.pb.h"

namespace pikiwidb {

class StoreServiceImpl : public StoreService {
 public:
  StoreServiceImpl() = default;
  ~StoreServiceImpl() = default;

  void GetStoreStats(::PROTOBUF_NAMESPACE_ID::RpcController* controller, const ::pikiwidb::StoreStatsRequest* request,
                     ::pikiwidb::StoreStatsResponse* response, ::google::protobuf::Closure* done) override;

  void GetRegionStats(::PROTOBUF_NAMESPACE_ID::RpcController* controller, const ::pikiwidb::RegionStatsRequest* request,
                      ::pikiwidb::RegionStatsResponse* response, ::google::protobuf::Closure* done) override;

  void InitRegionPeer(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                      const ::pikiwidb::InitRegionPeerRequest* request, ::pikiwidb::InitRegionPeerResponse* response,
                      ::google::protobuf::Closure* done) override;

  void AddRegionPeer(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                     const ::pikiwidb::AddRegionPeerRequest* request, ::pikiwidb::AddRegionResponse* response,
                     ::google::protobuf::Closure* done) override;

  void RemoveRegionPeer(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                        const ::pikiwidb::RemoveRegionPeerRequest* request,
                        ::pikiwidb::RemoveRegionPeerResponse* response, ::google::protobuf::Closure* done) override;

  void TransferLeader(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                      const ::pikiwidb::TransferLeaderRequest* request, ::pikiwidb::TransferLeaderResponse* response,
                      ::google::protobuf::Closure* done) override;
};

}  // namespace pikiwidb
