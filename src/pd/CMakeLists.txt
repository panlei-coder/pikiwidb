# Copyright (c) 2024-present, Qihoo, Inc.  All rights reserved.
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

ADD_CUSTOM_COMMAND(
  OUTPUT "${PROTO_OUTPUT_DIR}/pd.pb.cc"
  DEPENDS protobuf
  COMMAND ${PROTOBUF_PROTOC}
  ARGS -I ${PROJECT_SOURCE_DIR}/src
    -I ${PROJECT_SOURCE_DIR}/src/pd
    --cpp_out ${PROTO_OUTPUT_DIR}
    pd.proto
)
ADD_LIBRARY(pd_pb STATIC "${PROTO_OUTPUT_DIR}/pd.pb.cc")
SET(LIBRARY_OUTPUT_PATH ${PLIB_INSTALL_DIR})
TARGET_INCLUDE_DIRECTORIES(pd_pb PRIVATE ${PROTOBUF_INCLUDE_DIR})

FILE(GLOB PD_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

SET(LIBRARY_OUTPUT_PATH ${PLIB_INSTALL_DIR})

ADD_LIBRARY(pd ${PD_SRC})

TARGET_INCLUDE_DIRECTORIES(pd
        PRIVATE ${PROJECT_SOURCE_DIR}/src
        PRIVATE ${rocksdb_SOURCE_DIR}/include
        PRIVATE ${BRAFT_INCLUDE_DIR}
        PRIVATE ${BRPC_INCLUDE_DIR}
        PRIVATE ${PROTO_OUTPUT_DIR}
)

IF (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    SET(PD_LIB ${PD_LIB} rt)
ENDIF ()

SET(LIBRARY_OUTPUT_PATH ${PLIB_INSTALL_DIR})
ADD_DEPENDENCIES(pd protobuf pd_pb)
TARGET_LINK_LIBRARIES(pd praft dl fmt storage pstd braft brpc ssl crypto zlib protobuf leveldb gflags rocksdb z ${PD_LIB})
SET_TARGET_PROPERTIES(pd PROPERTIES LINKER_LANGUAGE CXX)