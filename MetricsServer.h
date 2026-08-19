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

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "CacheManager.h"

namespace cachelib {
namespace grpc_server {

// Minimal HTTP server that exposes Prometheus metrics from CacheManager stats.
// Runs on a separate port and serves GET /metrics in Prometheus text format.
class MetricsServer {
 public:
  MetricsServer(std::shared_ptr<CacheManager> cacheManager, uint16_t port);
  ~MetricsServer();

  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  // Start the metrics server in a background thread
  bool start();

  // Stop the metrics server
  void stop();

 private:
  // Main accept loop
  void run();

  // Handle a single HTTP connection
  void handleConnection(int clientFd);

  // Generate Prometheus text exposition format from cache stats
  std::string generateMetrics();

  std::shared_ptr<CacheManager> cacheManager_;
  uint16_t port_;
  int serverFd_ = -1;
  std::atomic<bool> running_{false};
  std::thread serverThread_;
};

}  // namespace grpc_server
}  // namespace cachelib
