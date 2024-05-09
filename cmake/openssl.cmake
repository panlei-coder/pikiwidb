# Copyright (c) 2023-present, Qihoo, Inc.  All rights reserved.
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

INCLUDE(ExternalProject)

SET(OPENSSL_SOURCE_DIR ${THIRD_PARTY_PATH}/openssl)
SET(OPENSSL_INSTALL_DIR ${THIRD_PARTY_PATH}/install/openssl)
SET(OPENSSL_INCLUDE_DIR ${OPENSSL_INSTALL_DIR}/include)
SET(OPENSSL_CONFIGURE_COMMAND ${OPENSSL_SOURCE_DIR}/config)

IF (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    SET(OPENSSL_LIBRARY_SUFFIX "dylib")
ELSEIF (CMAKE_SYSTEM_NAME MATCHES "Linux")
    SET(OPENSSL_LIBRARY_SUFFIX "so")
ELSE ()
    MESSAGE(FATAL_ERROR "only support linux or macOS")
ENDIF ()

FILE(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})

ExternalProject_Add(
        OpenSSL
        SOURCE_DIR ${OPENSSL_SOURCE_DIR}
        GIT_REPOSITORY https://github.com/openssl/openssl.git
        GIT_TAG OpenSSL_1_1_1v
        USES_TERMINAL_DOWNLOAD TRUE
        CONFIGURE_COMMAND
        ${OPENSSL_CONFIGURE_COMMAND}
        --prefix=${OPENSSL_INSTALL_DIR}
        --openssldir=${OPENSSL_INSTALL_DIR}
        BUILD_COMMAND make
        TEST_COMMAND ""
        INSTALL_COMMAND make install
        INSTALL_DIR ${OPENSSL_INSTALL_DIR}
)

ADD_LIBRARY(ssl STATIC IMPORTED GLOBAL)
SET_PROPERTY(TARGET ssl PROPERTY IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib/libssl.${OPENSSL_LIBRARY_SUFFIX})
SET_PROPERTY(TARGET ssl PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
ADD_DEPENDENCIES(ssl OpenSSL)

ADD_LIBRARY(crypto STATIC IMPORTED GLOBAL)
SET_PROPERTY(TARGET crypto PROPERTY IMPORTED_LOCATION ${OPENSSL_INSTALL_DIR}/lib/libcrypto.${OPENSSL_LIBRARY_SUFFIX})
SET_PROPERTY(TARGET crypto PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${OPENSSL_INCLUDE_DIR})
ADD_DEPENDENCIES(crypto OpenSSL)