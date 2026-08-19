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

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cachelib/allocator/CacheAllocator.h>
#include <folly/logging/xlog.h>

namespace cachelib {
namespace grpc_server {

// Server version
constexpr const char* kServerVersion = "1.6.0";

// Configuration for the cache manager
struct CacheConfig {
  // Cache name for identification
  std::string cacheName = "grpc-cachelib";

  // DRAM cache size in bytes (default: 1GB)
  size_t cacheSize = 1ULL * 1024 * 1024 * 1024;

  // Default pool name
  std::string defaultPoolName = "default";

  // Enable NVM (flash) cache
  bool enableNvm = false;

  // Path to NVM device/file
  std::string nvmCachePath;

  // NVM cache size in bytes (default: 10GB)
  size_t nvmCacheSize = 10ULL * 1024 * 1024 * 1024;

  // NVM block size
  uint32_t nvmBlockSize = 4096;

  // Number of NVM reader threads
  uint32_t nvmReaderThreads = 32;

  // Number of NVM writer threads
  uint32_t nvmWriterThreads = 32;

  // Enable io_uring for NVM operations
  bool enableIoUring = true;

  // LRU refresh time in seconds
  uint32_t lruRefreshTime = 60;

  // Enable access stats tracking
  bool enableStats = true;

  // Maximum item size (default: 4MB)
  size_t maxItemSize = 4 * 1024 * 1024;
};

// Result of a Get operation
struct GetResult {
  bool found = false;
  std::string value;
  int64_t ttlRemaining = 0;  // Remaining TTL in seconds, -1 if no expiry
};

// Result of SetNX operation
struct SetNXResult {
  bool wasSet = false;
  std::string existingValue;
};

// Result of Increment/Decrement operation
struct IncrDecrResult {
  bool success = false;
  int64_t newValue = 0;
  std::string message;
};

// Result of Incr operation (rate-limit-bucket semantics)
struct IncrResult {
  bool success = false;
  int64_t value = 0;
  // True when this call created the key and stamped the TTL; false when the
  // key already existed and the existing TTL was left in place.
  bool ttlSet = false;
  std::string message;
};

// Result of CompareAndSwap operation
struct CASResult {
  bool success = false;
  std::string actualValue;
};

// Result of Touch operation
struct TouchResult {
  bool success = false;
  std::string message;
};

// Per-key metadata returned by scan with details
struct KeyInfo {
  std::string key;
  int64_t ttlRemaining = 0;  // -1 = no expiry, 0 = expired
  int64_t sizeBytes = 0;
};

// Result of Scan operation
struct ScanResult {
  std::vector<std::string> keys;
  std::vector<KeyInfo> keyDetails;  // Populated when includeDetails=true
  std::string nextCursor;
  bool hasMore = false;
};

// Statistics for the cache
struct CacheStats {
  // Memory stats
  int64_t totalSize = 0;
  int64_t usedSize = 0;
  int64_t itemCount = 0;

  // Operation stats
  double hitRate = 0.0;
  int64_t getCount = 0;
  int64_t hitCount = 0;
  int64_t missCount = 0;
  int64_t setCount = 0;
  int64_t deleteCount = 0;
  int64_t evictionCount = 0;
  int64_t expiredCount = 0;

  // NVM stats
  bool nvmEnabled = false;
  int64_t nvmSize = 0;
  int64_t nvmUsed = 0;
  int64_t nvmHitCount = 0;
  int64_t nvmMissCount = 0;

  // Server stats
  int64_t uptimeSeconds = 0;
  std::string version;
};

// CacheManager wraps CacheLib and provides a simple key-value interface
class CacheManager {
 public:
  using Cache = facebook::cachelib::LruAllocator;
  using CacheAllocatorConfig = Cache::Config;
  using PoolId = facebook::cachelib::PoolId;
  using WriteHandle = Cache::WriteHandle;
  using ReadHandle = Cache::ReadHandle;

  explicit CacheManager(const CacheConfig& config);
  ~CacheManager();

  // Disable copy and move
  CacheManager(const CacheManager&) = delete;
  CacheManager& operator=(const CacheManager&) = delete;
  CacheManager(CacheManager&&) = delete;
  CacheManager& operator=(CacheManager&&) = delete;

  // Initialize the cache - must be called before any operations
  bool initialize();

  // Shutdown the cache gracefully
  void shutdown();

  // ---------------------------------------------------------------------------
  // Basic Operations
  // ---------------------------------------------------------------------------

  // Get a value by key
  GetResult get(std::string_view key);

  // Set a value with optional TTL (0 = no expiration)
  bool set(std::string_view key, std::string_view value, uint32_t ttlSeconds = 0);

  // Delete a key, returns true if the key existed
  bool remove(std::string_view key);

  // Check if a key exists
  bool exists(std::string_view key);

  // ---------------------------------------------------------------------------
  // Batch Operations
  // ---------------------------------------------------------------------------

  // Get multiple values at once
  std::vector<GetResult> multiGet(const std::vector<std::string>& keys);

  // Set multiple values at once, returns list of failed keys
  std::vector<std::string> multiSet(
      const std::vector<std::tuple<std::string, std::string, uint32_t>>& items);

  // Delete multiple keys, returns number of keys that existed and were deleted
  int32_t multiDelete(const std::vector<std::string>& keys);

  // ---------------------------------------------------------------------------
  // Atomic Operations
  // ---------------------------------------------------------------------------

  // Set if not exists, returns whether set was performed
  SetNXResult setNX(std::string_view key, std::string_view value, uint32_t ttlSeconds = 0);

  // Atomic increment
  IncrDecrResult increment(std::string_view key, int64_t delta, uint32_t ttlSeconds = 0);

  // Atomic decrement
  IncrDecrResult decrement(std::string_view key, int64_t delta, uint32_t ttlSeconds = 0);

  // Atomic increment for fixed-window rate-limit buckets.
  // On miss: creates the key with value=delta and TTL=ttlSeconds (ttlSet=true).
  // On hit: increments by delta and preserves the existing TTL (ttlSet=false).
  IncrResult incr(std::string_view key, int64_t delta, uint32_t ttlSeconds);

  // Compare and swap
  CASResult compareAndSwap(
      std::string_view key,
      std::string_view expectedValue,
      std::string_view newValue,
      uint32_t ttlSeconds = 0,
      bool keepTtl = false);

  // ---------------------------------------------------------------------------
  // TTL Operations
  // ---------------------------------------------------------------------------

  // Get TTL for a key (-2 = not found, -1 = no expiry, 0+ = seconds remaining)
  int64_t getTTL(std::string_view key);

  // Update TTL without changing value
  TouchResult touch(std::string_view key, uint32_t ttlSeconds);

  // ---------------------------------------------------------------------------
  // Key Scanning
  // ---------------------------------------------------------------------------

  // Scan keys matching a pattern (includeDetails adds TTL/size per key)
  ScanResult scan(const std::string& pattern, const std::string& cursor, int32_t count, bool includeDetails = false);

  // ---------------------------------------------------------------------------
  // Administration
  // ---------------------------------------------------------------------------

  // Get cache statistics
  CacheStats getStats() const;

  // Flush all keys
  int64_t flush();

  // Check if cache is initialized and ready
  bool isReady() const { return cache_ != nullptr; }

 private:
  // Configure NVM cache if enabled
  void configureNvmCache(CacheAllocatorConfig& cacheConfig);

  // Helper for atomic numeric operations
  IncrDecrResult atomicAddValue(std::string_view key, int64_t delta, uint32_t ttlSeconds);

  // Check if a pattern matches a key
  bool matchesPattern(const std::string& key, const std::string& pattern) const;

  CacheConfig config_;
  std::unique_ptr<Cache> cache_;
  PoolId defaultPoolId_;
  std::chrono::steady_clock::time_point startTime_;

  // Mutex for atomic operations that need read-modify-write
  mutable std::mutex atomicOpMutex_;

  // Stats counters
  mutable std::atomic<int64_t> getCount_{0};
  mutable std::atomic<int64_t> hitCount_{0};
  mutable std::atomic<int64_t> missCount_{0};
  mutable std::atomic<int64_t> setCount_{0};
  mutable std::atomic<int64_t> deleteCount_{0};
};

}  // namespace grpc_server
}  // namespace cachelib
