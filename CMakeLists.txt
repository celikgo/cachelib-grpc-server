# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.16)
project(cachelib-grpc-server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add our cmake modules directory first (for FindSodium.cmake)
list(INSERT CMAKE_MODULE_PATH 0 "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Add module path for finding other dependencies (from folly)
foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
  if(EXISTS "${prefix}/share/folly/cmake")
    list(APPEND CMAKE_MODULE_PATH "${prefix}/share/folly/cmake")
  endif()
  if(EXISTS "${prefix}/lib/cmake/folly")
    list(APPEND CMAKE_MODULE_PATH "${prefix}/lib/cmake/folly")
  endif()
endforeach()

message(STATUS "CMAKE_MODULE_PATH: ${CMAKE_MODULE_PATH}")

# Find required packages
find_package(Threads REQUIRED)
find_package(gflags REQUIRED)
find_package(glog REQUIRED)
find_package(GTest REQUIRED)  # Required by cachelib's cmake config
find_package(NUMA REQUIRED)    # Required by cachelib's cmake config
find_package(Folly REQUIRED)

# Try to find cachelib via find_package, otherwise set up manually
find_package(cachelib QUIET)
if(NOT cachelib_FOUND)
  message(STATUS "CacheLib not found via find_package, setting up manually")

  # Look for CacheLib in /opt/cachelib (Docker build) or CMAKE_PREFIX_PATH
  set(CACHELIB_ROOT "" CACHE PATH "Path to CacheLib installation")
  if(NOT CACHELIB_ROOT)
    if(EXISTS "/opt/cachelib/lib/libcachelib_allocator.a")
      set(CACHELIB_ROOT "/opt/cachelib")
    else()
      foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        if(EXISTS "${prefix}/lib/libcachelib_allocator.a")
          set(CACHELIB_ROOT "${prefix}")
          break()
        endif()
      endforeach()
    endif()
  endif()

  if(NOT CACHELIB_ROOT)
    message(FATAL_ERROR "Could not find CacheLib. Set CACHELIB_ROOT to the installation directory.")
  endif()

  message(STATUS "Found CacheLib at: ${CACHELIB_ROOT}")

  # Find FBThrift and its dependencies (needed for cachelib)
  find_package(wangle REQUIRED)
  find_package(FBThrift REQUIRED)

  # Create imported targets for CacheLib libraries
  add_library(cachelib_allocator STATIC IMPORTED)
  set_target_properties(cachelib_allocator PROPERTIES
    IMPORTED_LOCATION "${CACHELIB_ROOT}/lib/libcachelib_allocator.a"
  )

  add_library(cachelib_navy STATIC IMPORTED)
  set_target_properties(cachelib_navy PROPERTIES
    IMPORTED_LOCATION "${CACHELIB_ROOT}/lib/libcachelib_navy.a"
  )

  add_library(cachelib_common STATIC IMPORTED)
  set_target_properties(cachelib_common PROPERTIES
    IMPORTED_LOCATION "${CACHELIB_ROOT}/lib/libcachelib_common.a"
  )

  add_library(cachelib_shm STATIC IMPORTED)
  set_target_properties(cachelib_shm PROPERTIES
    IMPORTED_LOCATION "${CACHELIB_ROOT}/lib/libcachelib_shm.a"
  )

  add_library(cachelib_datatype STATIC IMPORTED)
  set_target_properties(cachelib_datatype PROPERTIES
    IMPORTED_LOCATION "${CACHELIB_ROOT}/lib/libcachelib_datatype.a"
  )

  add_library(cachelib_nvmitem STATIC IMPORTED)
  set_target_properties(cachelib_nvmitem PROPERTIES
    IMPORTED_LOCATION "${CACHELIB_ROOT}/lib/libcachelib_nvmitem.a"
  )

  # Create a combined cachelib interface target
  add_library(cachelib INTERFACE)
  target_include_directories(cachelib INTERFACE "${CACHELIB_ROOT}/include")
  target_link_libraries(cachelib INTERFACE
    cachelib_allocator
    cachelib_nvmitem
    cachelib_navy
    cachelib_common
    cachelib_shm
    cachelib_datatype
    Folly::folly
    FBThrift::thriftcpp2
    FBThrift::thriftprotocol
    NUMA::NUMA
    Threads::Threads
    GTest::gtest
    GTest::gmock
  )
endif()

# Find gRPC and Protobuf
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# Get the protoc and grpc_cpp_plugin executables
find_program(_PROTOBUF_PROTOC protoc)
find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)

# Proto file
set(cache_proto "${CMAKE_CURRENT_SOURCE_DIR}/proto/cache.proto")
get_filename_component(cache_proto_path "${cache_proto}" PATH)

# Generated sources
set(cache_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/cache.pb.cc")
set(cache_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/cache.pb.h")
set(cache_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/cache.grpc.pb.cc")
set(cache_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/cache.grpc.pb.h")

# Generate protobuf and grpc sources
add_custom_command(
  OUTPUT "${cache_proto_srcs}" "${cache_proto_hdrs}" "${cache_grpc_srcs}" "${cache_grpc_hdrs}"
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
       --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
       -I "${cache_proto_path}"
       --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
       "${cache_proto}"
  DEPENDS "${cache_proto}"
  COMMENT "Generating gRPC sources from ${cache_proto}"
)

# Include generated files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# Create a library for the generated proto files
add_library(cache_grpc_proto
  ${cache_proto_srcs}
  ${cache_proto_hdrs}
  ${cache_grpc_srcs}
  ${cache_grpc_hdrs}
)
target_link_libraries(cache_grpc_proto
  gRPC::grpc++
  protobuf::libprotobuf
)

# Get cachelib include directory from cachelib target or cmake prefix path
get_target_property(CACHELIB_INCLUDE_DIRS cachelib INTERFACE_INCLUDE_DIRECTORIES)
if(NOT CACHELIB_INCLUDE_DIRS)
  # Fallback: look in CMAKE_PREFIX_PATH
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/include/cachelib")
      set(CACHELIB_INCLUDE_DIRS "${prefix}/include")
      break()
    endif()
  endforeach()
endif()
message(STATUS "CacheLib include dirs: ${CACHELIB_INCLUDE_DIRS}")

# Create the CacheManager library
add_library(cache_manager
  CacheManager.cc
  CacheManager.h
)
target_include_directories(cache_manager PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CACHELIB_INCLUDE_DIRS}
)
target_link_libraries(cache_manager
  cachelib
  Folly::folly
  glog::glog
  gflags
)

# Create the CacheService library
add_library(cache_service
  CacheServiceImpl.cc
  CacheServiceImpl.h
)
target_include_directories(cache_service PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(cache_service
  cache_manager
  cache_grpc_proto
  gRPC::grpc++
  Folly::folly
  glog::glog
)

# Create the main server executable
add_executable(cachelib-grpc-server
  server.cc
  MetricsServer.cc
  MetricsServer.h
)
target_link_libraries(cachelib-grpc-server
  cache_service
  cache_manager
  cache_grpc_proto
  cachelib
  Folly::folly
  gRPC::grpc++
  gRPC::grpc++_reflection
  glog::glog
  gflags
  Threads::Threads
)

# Install targets
install(TARGETS cachelib-grpc-server
  RUNTIME DESTINATION bin
)

# Install proto file for clients
install(FILES ${cache_proto}
  DESTINATION share/cachelib/proto
)

# Optional: Build tests
option(BUILD_TESTS "Build tests" OFF)
if(BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)

  add_executable(cache_manager_test
    tests/CacheManagerTest.cc
  )
  target_link_libraries(cache_manager_test
    cache_manager
    GTest::gtest
    GTest::gtest_main
  )
  add_test(NAME CacheManagerTest COMMAND cache_manager_test)

  add_executable(cache_service_test
    tests/CacheServiceTest.cc
  )
  target_link_libraries(cache_service_test
    cache_service
    cache_manager
    cache_grpc_proto
    cachelib
    GTest::gtest
    GTest::gtest_main
    gRPC::grpc++
  )
  add_test(NAME CacheServiceTest COMMAND cache_service_test)
endif()

message(STATUS "CacheLib gRPC Server configured successfully")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
