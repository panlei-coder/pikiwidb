/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "pd.pb.h"
#include "pd_server.h"
#include "praft.h"
#include "store.pb.h"

namespace pikiwidb {

class PlacementDriverServiceImpl : public PlacementDriverService {
 public:
  PlacementDriverServiceImpl() = default;
  ~PlacementDriverServiceImpl() = default;

  void CreateAllRegions(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                        const ::pikiwidb::CreateAllRegionsRequest* request,
                        ::pikiwidb::CreateAllRegionsResponse* response, ::google::protobuf::Closure* done) override;
  void DeleteAllRegions(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                        const ::pikiwidb::DeleteAllRegionsRequest* request,
                        ::pikiwidb::DeleteAllRegionsResponse* response, ::google::protobuf::Closure* done) override;
  void AddStore(::PROTOBUF_NAMESPACE_ID::RpcController* controller, const ::pikiwidb::AddStoreRequest* request,
                ::pikiwidb::AddStoreResponse* response, ::google::protobuf::Closure* done) override;
  void RemoveStore(::PROTOBUF_NAMESPACE_ID::RpcController* controller, const ::pikiwidb::RemoveStoreRequest* request,
                   ::pikiwidb::RemoveStoreResponse* response, ::google::protobuf::Closure* done) override;
  void GetClusterInfo(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                      const ::pikiwidb::GetClusterInfoRequest* request, ::pikiwidb::GetClusterInfoResponse* response,
                      ::google::protobuf::Closure* done) override;
  void OpenPDScheduling(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                        const ::pikiwidb::OpenPDSchedulingRequest* request,
                        ::pikiwidb::OpenPDSchedulingResponse* response, ::google::protobuf::Closure* done) override;
  void ClosePDScheduling(::PROTOBUF_NAMESPACE_ID::RpcController* controller,
                         const ::pikiwidb::ClosePDSchedulingRequest* request,
                         ::pikiwidb::ClosePDSchedulingResponse* response, ::google::protobuf::Closure* done) override;
};

}  // namespace pikiwidb
