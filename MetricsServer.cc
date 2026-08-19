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

#include "MetricsServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sstream>

#include <folly/logging/xlog.h>

namespace cachelib {
namespace grpc_server {

MetricsServer::MetricsServer(std::shared_ptr<CacheManager> cacheManager,
                             uint16_t port)
    : cacheManager_(std::move(cacheManager)), port_(port) {}

MetricsServer::~MetricsServer() {
  stop();
}

bool MetricsServer::start() {
  serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd_ < 0) {
    XLOG(ERR) << "MetricsServer: failed to create socket";
    return false;
  }

  int opt = 1;
  setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(serverFd_, reinterpret_cast<struct sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    XLOG(ERR) << "MetricsServer: failed to bind on port " << port_;
    close(serverFd_);
    serverFd_ = -1;
    return false;
  }

  if (listen(serverFd_, 8) < 0) {
    XLOG(ERR) << "MetricsServer: failed to listen";
    close(serverFd_);
    serverFd_ = -1;
    return false;
  }

  running_ = true;
  serverThread_ = std::thread(&MetricsServer::run, this);

  XLOG(INFO) << "MetricsServer listening on port " << port_;
  return true;
}

void MetricsServer::stop() {
  if (!running_) {
    return;
  }
  running_ = false;

  // Close the listening socket to unblock accept()
  if (serverFd_ >= 0) {
    ::shutdown(serverFd_, SHUT_RDWR);
    close(serverFd_);
    serverFd_ = -1;
  }

  if (serverThread_.joinable()) {
    serverThread_.join();
  }

  XLOG(INFO) << "MetricsServer stopped";
}

void MetricsServer::run() {
  while (running_) {
    struct sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int clientFd = accept(serverFd_,
                          reinterpret_cast<struct sockaddr*>(&clientAddr),
                          &clientLen);
    if (clientFd < 0) {
      if (running_) {
        XLOG(WARN) << "MetricsServer: accept failed";
      }
      continue;
    }

    handleConnection(clientFd);
    close(clientFd);
  }
}

void MetricsServer::handleConnection(int clientFd) {
  // Read the HTTP request (we only need enough to check the method/path)
  char buf[1024];
  ssize_t n = read(clientFd, buf, sizeof(buf) - 1);
  if (n <= 0) {
    return;
  }
  buf[n] = '\0';

  std::string request(buf, static_cast<size_t>(n));

  // Only handle GET /metrics
  if (request.find("GET /metrics") == 0 ||
      request.find("GET /metrics ") != std::string::npos) {
    std::string body = generateMetrics();
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << body;
    std::string respStr = resp.str();
    write(clientFd, respStr.data(), respStr.size());
  } else {
    const char* notFound =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    write(clientFd, notFound, strlen(notFound));
  }
}

std::string MetricsServer::generateMetrics() {
  auto stats = cacheManager_->getStats();

  std::ostringstream out;

  // Memory metrics
  out << "# HELP cachelib_cache_size_bytes Total configured cache size in bytes.\n"
      << "# TYPE cachelib_cache_size_bytes gauge\n"
      << "cachelib_cache_size_bytes " << stats.totalSize << "\n\n";

  out << "# HELP cachelib_cache_used_bytes Currently used cache size in bytes.\n"
      << "# TYPE cachelib_cache_used_bytes gauge\n"
      << "cachelib_cache_used_bytes " << stats.usedSize << "\n\n";

  out << "# HELP cachelib_items_total Number of items currently in cache.\n"
      << "# TYPE cachelib_items_total gauge\n"
      << "cachelib_items_total " << stats.itemCount << "\n\n";

  // Operation counters
  out << "# HELP cachelib_gets_total Total number of GET operations.\n"
      << "# TYPE cachelib_gets_total counter\n"
      << "cachelib_gets_total " << stats.getCount << "\n\n";

  out << "# HELP cachelib_hits_total Total number of cache hits.\n"
      << "# TYPE cachelib_hits_total counter\n"
      << "cachelib_hits_total " << stats.hitCount << "\n\n";

  out << "# HELP cachelib_misses_total Total number of cache misses.\n"
      << "# TYPE cachelib_misses_total counter\n"
      << "cachelib_misses_total " << stats.missCount << "\n\n";

  out << "# HELP cachelib_sets_total Total number of SET operations.\n"
      << "# TYPE cachelib_sets_total counter\n"
      << "cachelib_sets_total " << stats.setCount << "\n\n";

  out << "# HELP cachelib_deletes_total Total number of DELETE operations.\n"
      << "# TYPE cachelib_deletes_total counter\n"
      << "cachelib_deletes_total " << stats.deleteCount << "\n\n";

  out << "# HELP cachelib_evictions_total Total number of evictions.\n"
      << "# TYPE cachelib_evictions_total counter\n"
      << "cachelib_evictions_total " << stats.evictionCount << "\n\n";

  out << "# HELP cachelib_expired_total Total number of expired items found during gets.\n"
      << "# TYPE cachelib_expired_total counter\n"
      << "cachelib_expired_total " << stats.expiredCount << "\n\n";

  // Hit rate
  out << "# HELP cachelib_hit_rate Cache hit rate (0.0 - 1.0).\n"
      << "# TYPE cachelib_hit_rate gauge\n"
      << "cachelib_hit_rate " << stats.hitRate << "\n\n";

  // Uptime
  out << "# HELP cachelib_uptime_seconds Server uptime in seconds.\n"
      << "# TYPE cachelib_uptime_seconds gauge\n"
      << "cachelib_uptime_seconds " << stats.uptimeSeconds << "\n\n";

  // Version info
  out << "# HELP cachelib_info CacheLib gRPC server version info.\n"
      << "# TYPE cachelib_info gauge\n"
      << "cachelib_info{version=\"" << stats.version << "\"} 1\n\n";

  // NVM metrics (when enabled)
  if (stats.nvmEnabled) {
    out << "# HELP cachelib_nvm_size_bytes NVM cache size in bytes.\n"
        << "# TYPE cachelib_nvm_size_bytes gauge\n"
        << "cachelib_nvm_size_bytes " << stats.nvmSize << "\n\n";

    out << "# HELP cachelib_nvm_used_bytes NVM cache used in bytes.\n"
        << "# TYPE cachelib_nvm_used_bytes gauge\n"
        << "cachelib_nvm_used_bytes " << stats.nvmUsed << "\n\n";

    out << "# HELP cachelib_nvm_hits_total NVM cache hit count.\n"
        << "# TYPE cachelib_nvm_hits_total counter\n"
        << "cachelib_nvm_hits_total " << stats.nvmHitCount << "\n\n";

    out << "# HELP cachelib_nvm_misses_total NVM cache miss count.\n"
        << "# TYPE cachelib_nvm_misses_total counter\n"
        << "cachelib_nvm_misses_total " << stats.nvmMissCount << "\n\n";
  }

  return out.str();
}

}  // namespace grpc_server
}  // namespace cachelib
