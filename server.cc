// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "CacheManager.h"
#include "CacheServiceImpl.h"
#include "MetricsServer.h"

// Server configuration flags
DEFINE_string(address, "0.0.0.0", "Server bind address");
DEFINE_int32(port, 50051, "Server port");
DEFINE_string(cache_name, "grpc-cachelib", "Name of the cache instance");
DEFINE_uint64(
    cache_size,
    1073741824,  // 1GB default
    "DRAM cache size in bytes");

// NVM (flash) configuration
DEFINE_bool(enable_nvm, false, "Enable NVM (flash) cache");
DEFINE_string(nvm_path, "/tmp/cachelib_nvm", "Path to NVM device/file");
DEFINE_uint64(
    nvm_size,
    10737418240,  // 10GB default
    "NVM cache size in bytes");
DEFINE_uint32(nvm_block_size, 4096, "NVM block size");
DEFINE_uint32(nvm_reader_threads, 32, "Number of NVM reader threads");
DEFINE_uint32(nvm_writer_threads, 32, "Number of NVM writer threads");
DEFINE_bool(enable_io_uring, true, "Enable io_uring for NVM operations");

// Cache behavior configuration
DEFINE_uint32(lru_refresh_time, 60, "LRU refresh time in seconds");
DEFINE_uint64(max_item_size, 4194304, "Maximum item size in bytes (4MB)");

// Logging configuration
DEFINE_string(
    log_level,
    "INFO",
    "Log level (DBG, INFO, WARN, ERR, CRITICAL)");

// Metrics configuration
DEFINE_int32(metrics_port, 9090, "Prometheus metrics HTTP port (0 = disabled)");

// Global server pointer for signal handling
std::unique_ptr<grpc::Server> g_server;

void signalHandler(int signum) {
  XLOG(INFO) << "Received signal " << signum << ", shutting down...";
  if (g_server) {
    g_server->Shutdown();
  }
}

void printBanner() {
  std::cout << R"(
   ____           _          _     _ _
  / ___|__ _  ___| |__   ___| |   (_) |__
 | |   / _` |/ __| '_ \ / _ \ |   | | '_ \
 | |__| (_| | (__| | | |  __/ |___| | |_) |
  \____\__,_|\___|_| |_|\___|_____|_|_.__/
         gRPC Server
)" << std::endl;
}

void printConfiguration(const cachelib::grpc_server::CacheConfig& config) {
  XLOG(INFO) << "=== CacheLib gRPC Server Configuration ===";
  XLOG(INFO) << "Cache Name: " << config.cacheName;
  XLOG(INFO) << "Cache Size: "
             << (config.cacheSize / (1024 * 1024)) << " MB";
  XLOG(INFO) << "Max Item Size: "
             << (config.maxItemSize / 1024) << " KB";
  XLOG(INFO) << "LRU Refresh Time: " << config.lruRefreshTime << " seconds";
  XLOG(INFO) << "NVM Enabled: " << (config.enableNvm ? "Yes" : "No");
  if (config.enableNvm) {
    XLOG(INFO) << "NVM Path: " << config.nvmCachePath;
    XLOG(INFO) << "NVM Size: "
               << (config.nvmCacheSize / (1024 * 1024)) << " MB";
    XLOG(INFO) << "NVM Block Size: " << config.nvmBlockSize << " bytes";
    XLOG(INFO) << "NVM Reader Threads: " << config.nvmReaderThreads;
    XLOG(INFO) << "NVM Writer Threads: " << config.nvmWriterThreads;
    XLOG(INFO) << "io_uring: " << (config.enableIoUring ? "Enabled" : "Disabled");
  }
  XLOG(INFO) << "==========================================";
}

int main(int argc, char** argv) {
  // Check --version before folly::Init (which may modify args)
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--version" || std::string(argv[i]) == "-version") {
      std::cout << "cachelib-grpc-server " << cachelib::grpc_server::kServerVersion << std::endl;
      return 0;
    }
  }

  // Initialize folly and gflags
  folly::Init init(&argc, &argv);

  // Set up logging
  folly::initLogging(
      ".:=" + FLAGS_log_level + "; default:async=true,sync_level=WARN");

  printBanner();

  // Build cache configuration from flags
  cachelib::grpc_server::CacheConfig cacheConfig;
  cacheConfig.cacheName = FLAGS_cache_name;
  cacheConfig.cacheSize = FLAGS_cache_size;
  cacheConfig.enableNvm = FLAGS_enable_nvm;
  cacheConfig.nvmCachePath = FLAGS_nvm_path;
  cacheConfig.nvmCacheSize = FLAGS_nvm_size;
  cacheConfig.nvmBlockSize = FLAGS_nvm_block_size;
  cacheConfig.nvmReaderThreads = FLAGS_nvm_reader_threads;
  cacheConfig.nvmWriterThreads = FLAGS_nvm_writer_threads;
  cacheConfig.enableIoUring = FLAGS_enable_io_uring;
  cacheConfig.lruRefreshTime = FLAGS_lru_refresh_time;
  cacheConfig.maxItemSize = FLAGS_max_item_size;

  printConfiguration(cacheConfig);

  // Create and initialize cache manager
  auto cacheManager =
      std::make_shared<cachelib::grpc_server::CacheManager>(cacheConfig);

  if (!cacheManager->initialize()) {
    XLOG(CRITICAL) << "Failed to initialize cache manager";
    return 1;
  }

  XLOG(INFO) << "Cache manager initialized successfully";

  // Create gRPC service
  cachelib::grpc_server::CacheServiceImpl service(cacheManager);

  // Build server
  std::string serverAddress = FLAGS_address + ":" + std::to_string(FLAGS_port);

  grpc::ServerBuilder builder;

  // Configure server options
  builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  // Enable built-in health checking
  grpc::EnableDefaultHealthCheckService(true);

  // Enable server reflection for service discovery (e.g., grpcurl)
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  // Set max message size (default 4MB, but configurable)
  builder.SetMaxReceiveMessageSize(static_cast<int>(FLAGS_max_item_size) + 1024);
  builder.SetMaxSendMessageSize(static_cast<int>(FLAGS_max_item_size) + 1024);

  // Build and start the server
  g_server = builder.BuildAndStart();

  if (!g_server) {
    XLOG(CRITICAL) << "Failed to start gRPC server on " << serverAddress;
    return 1;
  }

  XLOG(INFO) << "gRPC server listening on " << serverAddress;

  // Start Prometheus metrics server if enabled
  std::unique_ptr<cachelib::grpc_server::MetricsServer> metricsServer;
  if (FLAGS_metrics_port > 0) {
    metricsServer = std::make_unique<cachelib::grpc_server::MetricsServer>(
        cacheManager, static_cast<uint16_t>(FLAGS_metrics_port));
    if (!metricsServer->start()) {
      XLOG(WARN) << "Failed to start metrics server on port "
                 << FLAGS_metrics_port;
    }
  }

  // Set up signal handlers for graceful shutdown
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // Wait for the server to shutdown
  g_server->Wait();

  XLOG(INFO) << "Server stopped, cleaning up...";

  // Stop metrics server
  if (metricsServer) {
    metricsServer->stop();
  }

  // Cleanup
  cacheManager->shutdown();

  XLOG(INFO) << "Shutdown complete";

  return 0;
}
