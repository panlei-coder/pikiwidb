/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "store_service.h"

namespace pikiwidb {
void StoreServiceImpl::GetStoreStats(::google::protobuf::RpcController* controller,
                                     const ::pikiwidb::StoreStatsRequest* request,
                                     ::pikiwidb::StoreStatsResponse* response, ::google::protobuf::Closure* done) {}

void StoreServiceImpl::GetRegionStats(::google::protobuf::RpcController* controller,
                                      const ::pikiwidb::RegionStatsRequest* request,
                                      ::pikiwidb::RegionStatsResponse* response, ::google::protobuf::Closure* done) {}

void StoreServiceImpl::InitRegionPeer(::google::protobuf::RpcController* controller,
                                      const ::pikiwidb::InitRegionPeerRequest* request,
                                      ::pikiwidb::InitRegionPeerResponse* response, ::google::protobuf::Closure* done) {
}

void StoreServiceImpl::AddRegionPeer(::google::protobuf::RpcController* controller,
                                     const ::pikiwidb::AddRegionPeerRequest* request,
                                     ::pikiwidb::AddRegionResponse* response, ::google::protobuf::Closure* done) {}

void StoreServiceImpl::RemoveRegionPeer(::google::protobuf::RpcController* controller,
                                        const ::pikiwidb::RemoveRegionPeerRequest* request,
                                        ::pikiwidb::RemoveRegionPeerResponse* response,
                                        ::google::protobuf::Closure* done) {}

void StoreServiceImpl::TransferLeader(::google::protobuf::RpcController* controller,
                                      const ::pikiwidb::TransferLeaderRequest* request,
                                      ::pikiwidb::TransferLeaderResponse* response, ::google::protobuf::Closure* done) {
}
}  // namespace pikiwidb
