#==============================================================================
#  Copyright (c) Qualcomm Technologies, Inc.
#  All Rights Reserved.
#  Confidential and Proprietary - Qualcomm Technologies, Inc.
#==============================================================================

cmake_minimum_required(VERSION 3.18.2)
project(llamachat LANGUAGES CXX)

# -----------------------------
# Build settings
# -----------------------------
set(APP "llamachat")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Threads (needed for mutex/condition_variable wait)
find_package(Threads REQUIRED)

# -----------------------------
# QAIRT / QNN SDK root
# -----------------------------
set(QNN_SDK_ROOT "" CACHE PATH "Path to QAIRT/QNN SDK root. If empty, uses env QNN_SDK_ROOT.")
if(NOT QNN_SDK_ROOT)
  if(DEFINED ENV{QNN_SDK_ROOT})
    set(QNN_SDK_ROOT "$ENV{QNN_SDK_ROOT}")
  else()
    message(FATAL_ERROR "QNN_SDK_ROOT is not set. Provide -DQNN_SDK_ROOT=... or export QNN_SDK_ROOT.")
  endif()
endif()

set(QNN_LIB_SUBDIR "aarch64-oe-linux-gcc11.2" CACHE STRING
    "QAIRT lib subdir under <QNN_SDK_ROOT>/lib (e.g., aarch64-oe-linux-gcc11.2)")

set(QNN_INCLUDE_ROOT      "${QNN_SDK_ROOT}/include")
set(QNN_GENIE_INCLUDE_DIR "${QNN_SDK_ROOT}/include/Genie")
set(QNN_LIB_DIR           "${QNN_SDK_ROOT}/lib/${QNN_LIB_SUBDIR}")

if(NOT EXISTS "${QNN_INCLUDE_ROOT}")
  message(FATAL_ERROR "QNN include root not found: ${QNN_INCLUDE_ROOT}")
endif()
if(NOT EXISTS "${QNN_LIB_DIR}")
  message(FATAL_ERROR "QNN lib dir not found: ${QNN_LIB_DIR}")
endif()

# -----------------------------
# Find required libraries
# -----------------------------
find_library(GENIE_LIB
  NAMES Genie libGenie
  PATHS "${QNN_LIB_DIR}"
  NO_DEFAULT_PATH
)
if(NOT GENIE_LIB)
  message(FATAL_ERROR "Could not find libGenie in: ${QNN_LIB_DIR}")
endif()

option(LINK_QNN_RUNTIME_LIBS "Link QnnSystem/QnnHtp explicitly (optional)" ON)

if(LINK_QNN_RUNTIME_LIBS)
  find_library(QNN_SYSTEM_LIB NAMES QnnSystem libQnnSystem PATHS "${QNN_LIB_DIR}" NO_DEFAULT_PATH)
  find_library(QNN_HTP_LIB    NAMES QnnHtp    libQnnHtp    PATHS "${QNN_LIB_DIR}" NO_DEFAULT_PATH)

  if(NOT QNN_SYSTEM_LIB)
    message(WARNING "QnnSystem not found in ${QNN_LIB_DIR}. Runtime may still work if available via loader paths.")
  endif()
  if(NOT QNN_HTP_LIB)
    message(WARNING "QnnHtp not found in ${QNN_LIB_DIR}. HTP/NPU execution may fail without it.")
  endif()
endif()

# -----------------------------
# Sources
# -----------------------------
set(SOURCES
  server_sse.cpp
  GeniePipelineRunner/GeniePipelineRunner.cpp
  VlmService/VlmService.cpp
  VlmService/GenieWorker.cpp
  ImageEncoder/ImageEncoderNode.cpp
  LutEncoder/LutEncoderNode.cpp
  TextGenerator/TextGeneratorNode.cpp
)

# -----------------------------
# Third-party header-only libraries
# -----------------------------
# cpp-httplib: single-header HTTP server (replaces Crow WebSocket)
set(HTTPLIB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib)

# nlohmann/json: single-header JSON library (replaces Crow JSON)
set(NLOHMANN_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann)

if(NOT EXISTS "${HTTPLIB_ROOT}/httplib.h")
  message(FATAL_ERROR
    "httplib.h not found at ${HTTPLIB_ROOT}/httplib.h\n"
    "Download it with:\n"
    "  mkdir -p ${HTTPLIB_ROOT}\n"
    "  curl -L https://github.com/yhirose/cpp-httplib/releases/download/v0.15.3/httplib.h "
    "-o ${HTTPLIB_ROOT}/httplib.h")
endif()

if(NOT EXISTS "${NLOHMANN_ROOT}/json.hpp")
  message(FATAL_ERROR
    "json.hpp not found at ${NLOHMANN_ROOT}/json.hpp\n"
    "Download it with:\n"
    "  mkdir -p ${NLOHMANN_ROOT}\n"
    "  curl -L https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp "
    "-o ${NLOHMANN_ROOT}/json.hpp")
endif()

add_executable(${APP} ${SOURCES})

# -----------------------------
# Includes (local modules + QAIRT + third-party)
# -----------------------------
target_include_directories(${APP} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${HTTPLIB_ROOT}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/GeniePipelineRunner
  ${CMAKE_CURRENT_SOURCE_DIR}/ImageEncoder
  ${CMAKE_CURRENT_SOURCE_DIR}/LutEncoder
  ${CMAKE_CURRENT_SOURCE_DIR}/TextGenerator
  ${CMAKE_CURRENT_SOURCE_DIR}/Utils
  ${CMAKE_CURRENT_SOURCE_DIR}/VlmService
  ${QNN_INCLUDE_ROOT}
  ${QNN_GENIE_INCLUDE_DIR}
)

# -----------------------------
# Link libraries
# -----------------------------
target_link_libraries(${APP} PRIVATE
  Threads::Threads
  ${GENIE_LIB}
)

if(LINK_QNN_RUNTIME_LIBS)
  if(QNN_SYSTEM_LIB)
    target_link_libraries(${APP} PRIVATE ${QNN_SYSTEM_LIB})
  endif()
  if(QNN_HTP_LIB)
    target_link_libraries(${APP} PRIVATE ${QNN_HTP_LIB})
  endif()
endif()

# -----------------------------
# Runtime path so the executable finds QAIRT libs without LD_LIBRARY_PATH
# -----------------------------
set_target_properties(${APP} PROPERTIES
  BUILD_RPATH "${QNN_LIB_DIR}"
  INSTALL_RPATH "${QNN_LIB_DIR}"
)

# -----------------------------
# Helpful build summary
# -----------------------------
message(STATUS "Building:           ${APP}")
message(STATUS "QNN_SDK_ROOT:       ${QNN_SDK_ROOT}")
message(STATUS "QNN_INCLUDE_ROOT:   ${QNN_INCLUDE_ROOT}")
message(STATUS "QNN_GENIE_INCLUDE:  ${QNN_GENIE_INCLUDE_DIR}")
message(STATUS "QNN_LIB_DIR:        ${QNN_LIB_DIR}")
message(STATUS "GENIE_LIB:          ${GENIE_LIB}")
message(STATUS "HTTPLIB_ROOT:       ${HTTPLIB_ROOT}")
message(STATUS "NLOHMANN_ROOT:      ${NLOHMANN_ROOT}")
if(LINK_QNN_RUNTIME_LIBS)
  message(STATUS "QNN_SYSTEM_LIB:     ${QNN_SYSTEM_LIB}")
  message(STATUS "QNN_HTP_LIB:        ${QNN_HTP_LIB}")
endif()
