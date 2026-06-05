#==============================================================================
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#==============================================================================

cmake_minimum_required(VERSION 3.18.2)
project(image-generation-service)

set(APP "image-generation-service")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

add_compile_options(-O2 -Wall -fPIC)
add_link_options(-Wl,--allow-shlib-undefined)

# ------------------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------------------
add_executable(${APP}
    main.cpp
    ImageGenService.cpp
    StableDiffusionEngine.cpp
    ClipTokenizer.cpp
    DpmScheduler.cpp
)

# ------------------------------------------------------------------------------
# QNN SDK
#
# QNN_SDK_ROOT can be overridden at configure time:
#   cmake -DQNN_SDK_ROOT=/path/to/qnn_sdk ..
# ------------------------------------------------------------------------------
set(QNN_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../.deps/qairt"
    CACHE PATH "Path to the QNN SDK root (contains include/ and lib/)")

set(QNN_INCLUDE_DIR "${QNN_SDK_ROOT}/include/QNN")
set(QNN_LIB_DIR     "${QNN_SDK_ROOT}/lib/aarch64-oe-linux-gcc11.2")

# ------------------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------------------
include_directories(
    ${QNN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/crow
    ${CMAKE_CURRENT_SOURCE_DIR}/httplib
)

# ------------------------------------------------------------------------------
# QNN runtime libraries (loaded at runtime via dlopen, but needed for linking)
# ------------------------------------------------------------------------------
target_link_libraries(${APP}
    PRIVATE
        dl
        pthread
)

# ------------------------------------------------------------------------------
# Configuration summary
# ------------------------------------------------------------------------------
message(STATUS "Building        : ${APP}")
message(STATUS "QNN SDK         : ${QNN_SDK_ROOT}")
message(STATUS "QNN include     : ${QNN_INCLUDE_DIR}")
message(STATUS "QNN lib         : ${QNN_LIB_DIR}")
message(STATUS "Third-party dir : ${CMAKE_CURRENT_SOURCE_DIR}/third_party")
message(STATUS "Crow dir        : ${CMAKE_CURRENT_SOURCE_DIR}/crow")
message(STATUS "Httplib dir     : ${CMAKE_CURRENT_SOURCE_DIR}/httplib")
