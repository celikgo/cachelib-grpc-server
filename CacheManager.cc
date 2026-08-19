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

#include "CacheManager.h"

#include <charconv>
#include <cstring>
#include <regex>

#include <cachelib/allocator/nvmcache/NavyConfig.h>
#include <folly/logging/xlog.h>

namespace cachelib {
namespace grpc_server {

CacheManager::CacheManager(const CacheConfig& config) : config_(config) {
  XLOG(INFO) << "CacheManager created with config:"
             << " cacheName=" << config_.cacheName
             << " cacheSize=" << config_.cacheSize
             << " enableNvm=" << config_.enableNvm;
}

CacheManager::~CacheManager() {
  shutdown();
}

bool CacheManager::initialize() {
  try {
    CacheAllocatorConfig cacheConfig;

    // Basic configuration
    cacheConfig.setCacheName(config_.cacheName)
        .setCacheSize(config_.cacheSize)
        .setAccessConfig({25, 10})  // Hash table config
        .validate();

    // Configure NVM if enabled
    if (config_.enableNvm && !config_.nvmCachePath.empty()) {
      configureNvmCache(cacheConfig);
    }

    // Create the cache
    cache_ = std::make_unique<Cache>(cacheConfig);

    // Add the default pool using all available memory
    defaultPoolId_ = cache_->addPool(
        config_.defaultPoolName,
        cache_->getCacheMemoryStats().ramCacheSize);

    startTime_ = std::chrono::steady_clock::now();

    XLOG(INFO) << "CacheManager initialized successfully"
               << " poolId=" << static_cast<int>(defaultPoolId_)
               << " poolSize=" << cache_->getCacheMemoryStats().ramCacheSize;

    return true;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Failed to initialize CacheManager: " << ex.what();
    return false;
  }
}

void CacheManager::configureNvmCache(CacheAllocatorConfig& cacheConfig) {
  XLOG(INFO) << "Configuring NVM cache: path=" << config_.nvmCachePath
             << " size=" << config_.nvmCacheSize;

  facebook::cachelib::navy::NavyConfig navyConfig;

  // Set up the NVM device
  navyConfig.setSimpleFile(
      config_.nvmCachePath,
      config_.nvmCacheSize,
      true /* truncateFile */);

  navyConfig.setBlockSize(config_.nvmBlockSize);
  navyConfig.setDeviceMaxWriteSize(1024 * 1024);  // 1MB max write

  // Configure async I/O
  navyConfig.enableAsyncIo(
      config_.nvmReaderThreads,
      config_.nvmWriterThreads,
      config_.enableIoUring,
      256 /* stackSizeKB */);

  // Configure BigHash for small objects (items < 2KB)
  auto& bigHashConfig = navyConfig.bigHash();
  bigHashConfig.setSizePctAndMaxItemSize(10, 2048);  // 10% of NVM for BigHash, max 2KB items
  bigHashConfig.setBucketSize(4096);
  bigHashConfig.setBucketBfSize(8);

  // Configure BlockCache for larger objects
  auto& blockCacheConfig = navyConfig.blockCache();
  blockCacheConfig.setRegionSize(16 * 1024 * 1024);  // 16MB regions
  blockCacheConfig.setDataChecksum(true);

  // Enable random admission policy
  navyConfig.enableRandomAdmPolicy().setAdmProbability(1.0);

  // Enable NVM cache in the allocator config
  typename Cache::NvmCacheConfig nvmCacheConfig;
  nvmCacheConfig.navyConfig = navyConfig;
  cacheConfig.enableNvmCache(nvmCacheConfig);

  XLOG(INFO) << "NVM cache configured successfully";
}

void CacheManager::shutdown() {
  if (cache_) {
    XLOG(INFO) << "Shutting down CacheManager...";
    cache_.reset();
    XLOG(INFO) << "CacheManager shutdown complete";
  }
}

// =============================================================================
// Basic Operations
// =============================================================================

GetResult CacheManager::get(std::string_view key) {
  GetResult result;

  // Increment get count using sequential consistency for guaranteed visibility
  getCount_.fetch_add(1, std::memory_order_seq_cst);

  if (!cache_) {
    XLOG(WARN) << "Cache not initialized";
    missCount_.fetch_add(1, std::memory_order_seq_cst);
    return result;
  }

  try {
    auto handle = cache_->find(folly::StringPiece(key.data(), key.size()));

    if (handle) {
      hitCount_.fetch_add(1, std::memory_order_seq_cst);
      result.found = true;

      // Copy the value
      const char* data = reinterpret_cast<const char*>(handle->getMemory());
      size_t size = handle->getSize();
      result.value.assign(data, size);

      // Calculate remaining TTL
      uint32_t expiryTime = handle->getExpiryTime();
      if (expiryTime == 0) {
        // No expiration set
        result.ttlRemaining = -1;
      } else {
        uint32_t now = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        if (expiryTime > now) {
          result.ttlRemaining = static_cast<int64_t>(expiryTime - now);
        } else {
          result.ttlRemaining = 0;
        }
      }
    } else {
      missCount_.fetch_add(1, std::memory_order_seq_cst);
    }
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error during get for key=" << key << ": " << ex.what();
    missCount_.fetch_add(1, std::memory_order_seq_cst);
  }

  return result;
}

bool CacheManager::set(std::string_view key,
                       std::string_view value,
                       uint32_t ttlSeconds) {
  // Increment set count using sequential consistency for guaranteed visibility
  setCount_.fetch_add(1, std::memory_order_seq_cst);

  if (!cache_) {
    XLOG(WARN) << "Cache not initialized";
    return false;
  }

  if (value.size() > config_.maxItemSize) {
    XLOG(WARN) << "Value too large: " << value.size()
               << " > " << config_.maxItemSize;
    return false;
  }

  try {
    // Allocate space for the item
    auto handle = cache_->allocate(
        defaultPoolId_,
        folly::StringPiece(key.data(), key.size()),
        static_cast<uint32_t>(value.size()),
        ttlSeconds);

    if (!handle) {
      XLOG(WARN) << "Failed to allocate cache item for key=" << key
                 << " size=" << value.size();
      return false;
    }

    // Copy the value into the allocated space
    std::memcpy(handle->getMemory(), value.data(), value.size());

    // Insert into cache (replaces existing item if present)
    cache_->insertOrReplace(handle);

    return true;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error during set for key=" << key << ": " << ex.what();
    return false;
  }
}

bool CacheManager::remove(std::string_view key) {
  // Increment delete count using sequential consistency for guaranteed visibility
  deleteCount_.fetch_add(1, std::memory_order_seq_cst);

  if (!cache_) {
    XLOG(WARN) << "Cache not initialized";
    return false;
  }

  try {
    auto result = cache_->remove(folly::StringPiece(key.data(), key.size()));
    return result == Cache::RemoveRes::kSuccess;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error during remove for key=" << key << ": " << ex.what();
    return false;
  }
}

bool CacheManager::exists(std::string_view key) {
  if (!cache_) {
    return false;
  }

  try {
    auto handle = cache_->find(folly::StringPiece(key.data(), key.size()));
    return handle != nullptr;
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error during exists check for key=" << key << ": "
              << ex.what();
    return false;
  }
}

// =============================================================================
// Batch Operations
// =============================================================================

std::vector<GetResult> CacheManager::multiGet(
    const std::vector<std::string>& keys) {
  std::vector<GetResult> results;
  results.reserve(keys.size());

  for (const auto& key : keys) {
    results.push_back(get(key));
  }

  return results;
}

std::vector<std::string> CacheManager::multiSet(
    const std::vector<std::tuple<std::string, std::string, uint32_t>>& items) {
  std::vector<std::string> failedKeys;

  for (const auto& [key, value, ttl] : items) {
    if (!set(key, value, ttl)) {
      failedKeys.push_back(key);
    }
  }

  return failedKeys;
}

int32_t CacheManager::multiDelete(const std::vector<std::string>& keys) {
  int32_t deletedCount = 0;

  for (const auto& key : keys) {
    if (remove(key)) {
      ++deletedCount;
    }
  }

  return deletedCount;
}

// =============================================================================
// Atomic Operations
// =============================================================================

SetNXResult CacheManager::setNX(std::string_view key,
                                 std::string_view value,
                                 uint32_t ttlSeconds) {
  SetNXResult result;

  if (!cache_) {
    XLOG(WARN) << "Cache not initialized";
    return result;
  }

  std::lock_guard<std::mutex> lock(atomicOpMutex_);

  // Check if key exists
  auto existingHandle = cache_->find(folly::StringPiece(key.data(), key.size()));
  if (existingHandle) {
    result.wasSet = false;
    const char* data = reinterpret_cast<const char*>(existingHandle->getMemory());
    size_t size = existingHandle->getSize();
    result.existingValue.assign(data, size);
    return result;
  }

  // Key doesn't exist, set it
  if (set(key, value, ttlSeconds)) {
    result.wasSet = true;
  }

  return result;
}

IncrDecrResult CacheManager::atomicAddValue(std::string_view key,
                                             int64_t delta,
                                             uint32_t ttlSeconds) {
  IncrDecrResult result;

  if (!cache_) {
    result.message = "Cache not initialized";
    return result;
  }

  std::lock_guard<std::mutex> lock(atomicOpMutex_);

  int64_t currentValue = 0;
  uint32_t existingTtl = ttlSeconds;

  // Try to get existing value
  auto existingHandle = cache_->find(folly::StringPiece(key.data(), key.size()));
  if (existingHandle) {
    const char* data = reinterpret_cast<const char*>(existingHandle->getMemory());
    size_t size = existingHandle->getSize();
    std::string valueStr(data, size);

    // Parse as integer
    auto parseResult = std::from_chars(
        valueStr.data(), valueStr.data() + valueStr.size(), currentValue);
    if (parseResult.ec != std::errc()) {
      result.message = "Value is not a valid integer";
      return result;
    }

    // Preserve existing TTL if not specified
    if (ttlSeconds == 0) {
      uint32_t expiryTime = existingHandle->getExpiryTime();
      if (expiryTime > 0) {
        uint32_t now = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        if (expiryTime > now) {
          existingTtl = expiryTime - now;
        }
      }
    }
  }

  // Calculate new value
  int64_t newValue = currentValue + delta;
  std::string newValueStr = std::to_string(newValue);

  // Store the new value
  if (set(key, newValueStr, existingTtl)) {
    result.success = true;
    result.newValue = newValue;
  } else {
    result.message = "Failed to store new value";
  }

  return result;
}

IncrDecrResult CacheManager::increment(std::string_view key,
                                        int64_t delta,
                                        uint32_t ttlSeconds) {
  if (delta == 0) {
    delta = 1;
  }
  return atomicAddValue(key, delta, ttlSeconds);
}

IncrDecrResult CacheManager::decrement(std::string_view key,
                                        int64_t delta,
                                        uint32_t ttlSeconds) {
  if (delta == 0) {
    delta = 1;
  }
  return atomicAddValue(key, -delta, ttlSeconds);
}

IncrResult CacheManager::incr(std::string_view key,
                              int64_t delta,
                              uint32_t ttlSeconds) {
  IncrResult result;

  if (!cache_) {
    result.message = "Cache not initialized";
    return result;
  }

  if (delta == 0) {
    delta = 1;
  }

  std::lock_guard<std::mutex> lock(atomicOpMutex_);

  int64_t baseValue = 0;
  uint32_t writeTtl = ttlSeconds;
  bool createdNew = true;

  auto existingHandle = cache_->find(folly::StringPiece(key.data(), key.size()));
  if (existingHandle) {
    // Treat logically-expired entries as miss so the window gets re-stamped.
    uint32_t expiryTime = existingHandle->getExpiryTime();
    uint32_t now = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    bool stillLive = (expiryTime == 0) || (expiryTime > now);

    if (stillLive) {
      const char* data =
          reinterpret_cast<const char*>(existingHandle->getMemory());
      size_t size = existingHandle->getSize();
      std::string valueStr(data, size);

      auto parseResult = std::from_chars(
          valueStr.data(), valueStr.data() + valueStr.size(), baseValue);
      if (parseResult.ec != std::errc()) {
        result.message = "Value is not a valid integer";
        return result;
      }

      // Preserve the existing TTL: the rate-limit window must not slide.
      writeTtl = (expiryTime == 0) ? 0 : (expiryTime - now);
      createdNew = false;
    }
  }

  int64_t newValue = baseValue + delta;
  std::string newValueStr = std::to_string(newValue);

  if (set(key, newValueStr, writeTtl)) {
    result.success = true;
    result.value = newValue;
    result.ttlSet = createdNew;
  } else {
    result.message = "Failed to store new value";
  }

  return result;
}

CASResult CacheManager::compareAndSwap(std::string_view key,
                                        std::string_view expectedValue,
                                        std::string_view newValue,
                                        uint32_t ttlSeconds,
                                        bool keepTtl) {
  CASResult result;

  if (!cache_) {
    return result;
  }

  std::lock_guard<std::mutex> lock(atomicOpMutex_);

  // Get current value
  auto existingHandle = cache_->find(folly::StringPiece(key.data(), key.size()));
  if (!existingHandle) {
    result.success = false;
    return result;
  }

  const char* data = reinterpret_cast<const char*>(existingHandle->getMemory());
  size_t size = existingHandle->getSize();
  result.actualValue.assign(data, size);

  // Compare
  if (result.actualValue != expectedValue) {
    result.success = false;
    return result;
  }

  // Determine effective TTL
  uint32_t effectiveTtl = ttlSeconds;
  if (keepTtl) {
    // Preserve existing TTL: calculate remaining time from expiry
    uint32_t expiryTime = existingHandle->getExpiryTime();
    if (expiryTime > 0) {
      uint32_t now = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      if (expiryTime > now) {
        effectiveTtl = expiryTime - now;
      } else {
        effectiveTtl = 0;
      }
    } else {
      // No expiration was set, keep it that way
      effectiveTtl = 0;
    }
  }
  // Otherwise use ttlSeconds directly (0 = no expiration, matching Set)

  // Values match, perform swap
  if (set(key, newValue, effectiveTtl)) {
    result.success = true;
    result.actualValue = std::string(newValue);
  }

  return result;
}

// =============================================================================
// TTL Operations
// =============================================================================

int64_t CacheManager::getTTL(std::string_view key) {
  if (!cache_) {
    return -2;  // Not found
  }

  try {
    auto handle = cache_->find(folly::StringPiece(key.data(), key.size()));
    if (!handle) {
      return -2;  // Key not found
    }

    uint32_t expiryTime = handle->getExpiryTime();
    if (expiryTime == 0) {
      return -1;  // No expiration
    }

    uint32_t now = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    if (expiryTime > now) {
      return static_cast<int64_t>(expiryTime - now);
    }
    return 0;  // Expired
  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error during getTTL for key=" << key << ": " << ex.what();
    return -2;
  }
}

TouchResult CacheManager::touch(std::string_view key, uint32_t ttlSeconds) {
  TouchResult result;

  if (!cache_) {
    result.message = "Cache not initialized";
    return result;
  }

  std::lock_guard<std::mutex> lock(atomicOpMutex_);

  // Get current value
  auto existingHandle = cache_->find(folly::StringPiece(key.data(), key.size()));
  if (!existingHandle) {
    result.message = "Key not found";
    return result;
  }

  // Get the value
  const char* data = reinterpret_cast<const char*>(existingHandle->getMemory());
  size_t size = existingHandle->getSize();
  std::string value(data, size);

  // Re-set with new TTL (this is the only way to update TTL in CacheLib)
  if (set(key, value, ttlSeconds)) {
    result.success = true;
    result.message = "TTL updated";
  } else {
    result.message = "Failed to update TTL";
  }

  return result;
}

// =============================================================================
// Key Scanning
// =============================================================================

bool CacheManager::matchesPattern(const std::string& key,
                                   const std::string& pattern) const {
  if (pattern.empty() || pattern == "*") {
    return true;
  }

  // Simple wildcard matching
  // Convert glob pattern to regex
  std::string regexPattern;
  for (char c : pattern) {
    switch (c) {
      case '*':
        regexPattern += ".*";
        break;
      case '?':
        regexPattern += ".";
        break;
      case '.':
      case '[':
      case ']':
      case '(':
      case ')':
      case '{':
      case '}':
      case '+':
      case '^':
      case '$':
      case '|':
      case '\\':
        regexPattern += '\\';
        regexPattern += c;
        break;
      default:
        regexPattern += c;
    }
  }

  try {
    std::regex re(regexPattern);
    return std::regex_match(key, re);
  } catch (const std::regex_error&) {
    // If regex fails, fall back to simple prefix matching
    if (pattern.back() == '*') {
      std::string prefix = pattern.substr(0, pattern.size() - 1);
      return key.substr(0, prefix.size()) == prefix;
    }
    return key == pattern;
  }
}

ScanResult CacheManager::scan(const std::string& pattern,
                               const std::string& cursor,
                               int32_t count,
                               bool includeDetails) {
  ScanResult result;

  if (!cache_) {
    return result;
  }

  if (count <= 0) {
    count = 100;
  }
  if (count > 10000) {
    count = 10000;
  }

  // Cursor is the last key seen (empty = start from beginning)
  bool pastCursor = cursor.empty();
  int32_t matched = 0;

  auto config = facebook::cachelib::util::Throttler::Config::makeNoThrottleConfig();

  for (auto it = cache_->begin(config); it != cache_->end(); ++it) {
    auto key = it->getKey();
    std::string keyStr(key.data(), key.size());

    // Skip until we pass the cursor position
    if (!pastCursor) {
      if (keyStr == cursor) {
        pastCursor = true;
      }
      continue;
    }

    // Match against pattern
    if (matchesPattern(keyStr, pattern)) {
      result.keys.push_back(keyStr);

      if (includeDetails) {
        KeyInfo info;
        info.key = keyStr;
        info.sizeBytes = static_cast<int64_t>(it->getSize());

        uint32_t expiryTime = it->getExpiryTime();
        if (expiryTime == 0) {
          info.ttlRemaining = -1;  // No expiration
        } else {
          uint32_t now = static_cast<uint32_t>(
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
          if (expiryTime > now) {
            info.ttlRemaining = static_cast<int64_t>(expiryTime - now);
          } else {
            info.ttlRemaining = 0;
          }
        }

        result.keyDetails.push_back(std::move(info));
      }

      matched++;

      if (matched >= count) {
        // Check if there are more items
        ++it;
        if (it != cache_->end()) {
          result.hasMore = true;
          result.nextCursor = keyStr;  // Last returned key as cursor
        }
        break;
      }
    }
  }

  return result;
}

// =============================================================================
// Administration
// =============================================================================

CacheStats CacheManager::getStats() const {
  CacheStats stats;
  stats.version = kServerVersion;

  if (!cache_) {
    return stats;
  }

  try {
    auto memStats = cache_->getCacheMemoryStats();
    auto globalStats = cache_->getGlobalCacheStats();

    // Memory stats
    stats.totalSize = static_cast<int64_t>(memStats.ramCacheSize);

    // Calculate actual used size from pool stats
    auto poolStats = cache_->getPoolStats(defaultPoolId_);
    stats.usedSize = static_cast<int64_t>(poolStats.poolUsableSize - poolStats.freeMemoryBytes());
    if (stats.usedSize < 0) {
      stats.usedSize = 0;
    }

    stats.itemCount = static_cast<int64_t>(globalStats.numItems);
    stats.evictionCount = static_cast<int64_t>(globalStats.numEvictions);
    stats.expiredCount = static_cast<int64_t>(globalStats.numCacheGetExpiries);

    // Our tracked counters - use sequential consistency for guaranteed visibility
    stats.getCount = getCount_.load(std::memory_order_seq_cst);
    stats.hitCount = hitCount_.load(std::memory_order_seq_cst);
    stats.missCount = missCount_.load(std::memory_order_seq_cst);
    stats.setCount = setCount_.load(std::memory_order_seq_cst);
    stats.deleteCount = deleteCount_.load(std::memory_order_seq_cst);

    if (stats.getCount > 0) {
      stats.hitRate =
          static_cast<double>(stats.hitCount) / static_cast<double>(stats.getCount);
    }

    // NVM stats if enabled
    stats.nvmEnabled = config_.enableNvm;
    if (config_.enableNvm) {
      auto nvmStatsMap = cache_->getNvmCacheStatsMap();
      auto nvmStats = nvmStatsMap.toMap();
      stats.nvmSize = static_cast<int64_t>(config_.nvmCacheSize);

      auto bytesWrittenIt = nvmStats.find("navy_device_bytes_written");
      if (bytesWrittenIt != nvmStats.end()) {
        stats.nvmUsed = static_cast<int64_t>(bytesWrittenIt->second);
      }

      auto nvmGetsIt = nvmStats.find("navy_gets");
      auto nvmHitsIt = nvmStats.find("navy_hits");
      if (nvmGetsIt != nvmStats.end()) {
        int64_t nvmGets = static_cast<int64_t>(nvmGetsIt->second);
        if (nvmHitsIt != nvmStats.end()) {
          stats.nvmHitCount = static_cast<int64_t>(nvmHitsIt->second);
          stats.nvmMissCount = nvmGets - stats.nvmHitCount;
        }
      }
    }

    // Uptime
    auto now = std::chrono::steady_clock::now();
    stats.uptimeSeconds = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - startTime_)
            .count());

  } catch (const std::exception& ex) {
    XLOG(ERR) << "Error getting stats: " << ex.what();
  }

  return stats;
}

int64_t CacheManager::flush() {
  if (!cache_) {
    return 0;
  }

  int64_t removedCount = 0;
  // Use throttled iterator with no throttling for fast flush
  auto config = facebook::cachelib::util::Throttler::Config::makeNoThrottleConfig();

  for (auto it = cache_->begin(config); it != cache_->end(); ++it) {
    auto key = it->getKey();
    auto removeResult = cache_->remove(key);
    if (removeResult == Cache::RemoveRes::kSuccess) {
      removedCount++;
    }
  }

  XLOG(INFO) << "Flush completed: removed " << removedCount << " items";
  return removedCount;
}

}  // namespace grpc_server
}  // namespace cachelib
