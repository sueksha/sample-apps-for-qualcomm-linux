#==============================================================================
#
#  Copyright (c) Qualcomm Technologies, Inc.
#  All Rights Reserved.
#  Confidential and Proprietary - Qualcomm Technologies, Inc.
#
#==============================================================================

cmake_minimum_required(VERSION 3.18.2)
project(textgen_server)

# Application name
set(APP "textgen_server")

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
add_executable(${APP}
    Main.cpp
    ChatApp.cpp
    server/ModelRoutes.cpp
    server/CompletionRoutes.cpp
    server/CompletionService.cpp
    server/LegacyRoutes.cpp
    server/ServerUtils.cpp
    server/CompletionRuntime.cpp
    server/ModelCatalog.cpp
    Genie.cpp
    Logger.cpp
)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------

# JSON library (header-only)
find_package(nlohmann_json REQUIRED)

# Threads (required by cpp-httplib)
find_package(Threads REQUIRED)

# cpp-httplib (single header-only HTTP library – replaces Crow + Asio)
# Download with:
#   mkdir -p cpp-httplib
#   wget https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h \
#        -O cpp-httplib/httplib.h
set(HTTPLIB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib
    CACHE PATH "Path to the directory containing httplib.h")

# QNN SDK paths
set(CHIPSET "QCS9075")
# QNN_ROOT can be overridden at configure time:
#   cmake -DQNN_ROOT=/opt/qairt/current ..
set(QNN_ROOT "/opt/qairt/current"
    CACHE PATH "Path to the QAIRT SDK root (contains include/ and lib/)")
set(QNN_INCLUDE_DIR "${QNN_ROOT}/include/Genie")
set(QNN_LIB_PREFIX  "${QNN_ROOT}/lib")
set(_dtuple_POSTFIX "oe-linux-gcc11.2")

# Get QNN library path
get_filename_component(QNN_LIB_PATH
    "${QNN_LIB_PREFIX}/aarch64-${_dtuple_POSTFIX}/libGenie.so"
    REALPATH BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
)

# ------------------------------------------------------------------------------
# Imported QNN target
# ------------------------------------------------------------------------------
add_library(QNN SHARED IMPORTED)
set_target_properties(QNN PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${QNN_INCLUDE_DIR}
    IMPORTED_LOCATION ${QNN_LIB_PATH}
)

# ------------------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------------------
include_directories(${QNN_INCLUDE_DIR})
include_directories(${HTTPLIB_ROOT})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ------------------------------------------------------------------------------
# Link libraries
# ------------------------------------------------------------------------------
target_link_libraries(${APP}
    PUBLIC  QNN
    PRIVATE nlohmann_json::nlohmann_json
    PRIVATE Threads::Threads
)

# ------------------------------------------------------------------------------
# Optional: Print configuration summary
# ------------------------------------------------------------------------------
message(STATUS "Building ${APP}")
message(STATUS "QNN SDK Root  : ${QNN_ROOT}")
message(STATUS "QNN Include   : ${QNN_INCLUDE_DIR}")
message(STATUS "QNN Library   : ${QNN_LIB_PATH}")
message(STATUS "httplib root  : ${HTTPLIB_ROOT}")
