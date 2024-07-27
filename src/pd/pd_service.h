/*
 * Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "praft.h"

namespace pikiwidb {

class PlacementDriverServiceImpl : public PDService {
 private:
  PRaft* praft_;
};

}  // namespace pikiwidb
