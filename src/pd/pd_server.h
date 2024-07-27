/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <string>
#include <thread>
#include <vector>

namespace pikiwidb {

// PD options
class PlacementDriverOptions {
 private:
  bool fake_ = false;                 // Standalone or hybrid deployment
  std::string pd_group_id_;           // PD Raft Group ID
  std::string initial_pd_sever_list;  // the list of initial pd server
};

// PD
class PlacementDriverServer {
 private:
  PlacementDriverServiceImpl placementDriverService_;  // pd rpc service
  int db_id_;                                          // pd region, store meta
  std::vector<std::thread> threads_;                   // for exploration, information gathering functions
  bool is_started_;                                    // mark whether the fragment is started
};

}  // namespace pikiwidb
