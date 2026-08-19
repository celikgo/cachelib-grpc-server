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

#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "../CacheManager.h"

namespace cachelib {
namespace grpc_server {
namespace test {

class CacheManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "test-cache";
    config.cacheSize = 100 * 1024 * 1024;  // 100MB for testing
    config.enableNvm = false;

    cacheManager_ = std::make_unique<CacheManager>(config);
    ASSERT_TRUE(cacheManager_->initialize());
  }

  void TearDown() override {
    if (cacheManager_) {
      cacheManager_->shutdown();
    }
  }

  std::unique_ptr<CacheManager> cacheManager_;
};

TEST_F(CacheManagerTest, BasicSetGet) {
  const std::string key = "test-key";
  const std::string value = "test-value";

  EXPECT_TRUE(cacheManager_->set(key, value));

  auto result = cacheManager_->get(key);
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.value, value);
}

TEST_F(CacheManagerTest, GetNonExistent) {
  auto result = cacheManager_->get("non-existent-key");
  EXPECT_FALSE(result.found);
  EXPECT_TRUE(result.value.empty());
}

TEST_F(CacheManagerTest, SetOverwrite) {
  const std::string key = "overwrite-key";
  const std::string value1 = "first-value";
  const std::string value2 = "second-value";

  EXPECT_TRUE(cacheManager_->set(key, value1));
  auto result1 = cacheManager_->get(key);
  EXPECT_TRUE(result1.found);
  EXPECT_EQ(result1.value, value1);

  EXPECT_TRUE(cacheManager_->set(key, value2));
  auto result2 = cacheManager_->get(key);
  EXPECT_TRUE(result2.found);
  EXPECT_EQ(result2.value, value2);
}

TEST_F(CacheManagerTest, Remove) {
  const std::string key = "remove-key";
  const std::string value = "remove-value";

  EXPECT_TRUE(cacheManager_->set(key, value));
  EXPECT_TRUE(cacheManager_->exists(key));

  EXPECT_TRUE(cacheManager_->remove(key));
  EXPECT_FALSE(cacheManager_->exists(key));
}

TEST_F(CacheManagerTest, RemoveNonExistent) {
  EXPECT_FALSE(cacheManager_->remove("never-existed"));
}

TEST_F(CacheManagerTest, Exists) {
  const std::string key = "exists-key";
  const std::string value = "exists-value";

  EXPECT_FALSE(cacheManager_->exists(key));
  EXPECT_TRUE(cacheManager_->set(key, value));
  EXPECT_TRUE(cacheManager_->exists(key));
}

TEST_F(CacheManagerTest, EmptyValue) {
  const std::string key = "empty-value-key";
  const std::string value = "";

  EXPECT_TRUE(cacheManager_->set(key, value));

  auto result = cacheManager_->get(key);
  EXPECT_TRUE(result.found);
  EXPECT_TRUE(result.value.empty());
}

TEST_F(CacheManagerTest, BinaryValue) {
  const std::string key = "binary-key";
  std::string value;
  value.resize(256);
  for (int i = 0; i < 256; ++i) {
    value[i] = static_cast<char>(i);
  }

  EXPECT_TRUE(cacheManager_->set(key, value));

  auto result = cacheManager_->get(key);
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.value, value);
}

TEST_F(CacheManagerTest, LargeValue) {
  const std::string key = "large-key";
  std::string value(1024 * 1024, 'x');  // 1MB value

  EXPECT_TRUE(cacheManager_->set(key, value));

  auto result = cacheManager_->get(key);
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.value.size(), value.size());
  EXPECT_EQ(result.value, value);
}

TEST_F(CacheManagerTest, MultiGet) {
  // Set multiple keys
  for (int i = 0; i < 5; ++i) {
    std::string key = "multi-key-" + std::to_string(i);
    std::string value = "multi-value-" + std::to_string(i);
    EXPECT_TRUE(cacheManager_->set(key, value));
  }

  // Get multiple keys including non-existent
  std::vector<std::string> keys = {
      "multi-key-0", "multi-key-2", "multi-key-4", "non-existent"};
  auto results = cacheManager_->multiGet(keys);

  EXPECT_EQ(results.size(), 4);
  EXPECT_TRUE(results[0].found);
  EXPECT_EQ(results[0].value, "multi-value-0");
  EXPECT_TRUE(results[1].found);
  EXPECT_EQ(results[1].value, "multi-value-2");
  EXPECT_TRUE(results[2].found);
  EXPECT_EQ(results[2].value, "multi-value-4");
  EXPECT_FALSE(results[3].found);
}

TEST_F(CacheManagerTest, Stats) {
  // Perform some operations
  cacheManager_->set("stats-key-1", "value1");
  cacheManager_->set("stats-key-2", "value2");
  cacheManager_->get("stats-key-1");
  cacheManager_->get("stats-key-1");
  cacheManager_->get("non-existent");

  auto stats = cacheManager_->getStats();

  EXPECT_GT(stats.totalSize, 0);
  EXPECT_EQ(stats.setCount, 2);
  EXPECT_EQ(stats.getCount, 3);
  EXPECT_EQ(stats.hitCount, 2);
  EXPECT_EQ(stats.missCount, 1);
  EXPECT_GT(stats.hitRate, 0.0);
  EXPECT_LT(stats.hitRate, 1.0);
}

TEST_F(CacheManagerTest, ConcurrentAccess) {
  const int numThreads = 10;
  const int numOperationsPerThread = 100;
  std::vector<std::thread> threads;

  for (int t = 0; t < numThreads; ++t) {
    threads.emplace_back([this, t, numOperationsPerThread]() {
      for (int i = 0; i < numOperationsPerThread; ++i) {
        std::string key = "thread-" + std::to_string(t) + "-key-" + std::to_string(i);
        std::string value = "value-" + std::to_string(i);

        EXPECT_TRUE(cacheManager_->set(key, value));

        auto result = cacheManager_->get(key);
        EXPECT_TRUE(result.found);
        EXPECT_EQ(result.value, value);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  auto stats = cacheManager_->getStats();
  EXPECT_EQ(stats.setCount, numThreads * numOperationsPerThread);
  EXPECT_EQ(stats.getCount, numThreads * numOperationsPerThread);
  EXPECT_EQ(stats.hitCount, numThreads * numOperationsPerThread);
}

TEST_F(CacheManagerTest, SpecialCharactersInKey) {
  std::vector<std::string> specialKeys = {
      "key with spaces",
      "key\twith\ttabs",
      "key:with:colons",
      "key/with/slashes",
      "key\\with\\backslashes",
      "key\"with\"quotes",
      "emoji-key-\xF0\x9F\x98\x80",  // UTF-8 emoji
  };

  for (const auto& key : specialKeys) {
    std::string value = "value-for-" + key;
    EXPECT_TRUE(cacheManager_->set(key, value)) << "Failed to set key: " << key;

    auto result = cacheManager_->get(key);
    EXPECT_TRUE(result.found) << "Failed to get key: " << key;
    EXPECT_EQ(result.value, value) << "Value mismatch for key: " << key;
  }
}

TEST_F(CacheManagerTest, ScanWithDetails) {
  // Set some keys with known prefixes and TTLs
  EXPECT_TRUE(cacheManager_->set("scan:a", "value-a", 300));
  EXPECT_TRUE(cacheManager_->set("scan:b", "value-bb", 600));
  EXPECT_TRUE(cacheManager_->set("scan:c", "value-ccc", 0));  // no expiry
  EXPECT_TRUE(cacheManager_->set("other:x", "value-x", 60));

  // Scan without details (backward compat)
  auto result1 = cacheManager_->scan("scan:*", "", 100, false);
  EXPECT_EQ(result1.keys.size(), 3);
  EXPECT_TRUE(result1.keyDetails.empty());

  // Scan with details
  auto result2 = cacheManager_->scan("scan:*", "", 100, true);
  EXPECT_EQ(result2.keys.size(), 3);
  EXPECT_EQ(result2.keyDetails.size(), 3);

  // Verify each key has correct metadata
  for (const auto& info : result2.keyDetails) {
    EXPECT_FALSE(info.key.empty());
    EXPECT_GT(info.sizeBytes, 0);

    if (info.key == "scan:a") {
      EXPECT_EQ(info.sizeBytes, 7);  // "value-a"
      EXPECT_GT(info.ttlRemaining, 250);
      EXPECT_LE(info.ttlRemaining, 300);
    } else if (info.key == "scan:b") {
      EXPECT_EQ(info.sizeBytes, 8);  // "value-bb"
      EXPECT_GT(info.ttlRemaining, 550);
      EXPECT_LE(info.ttlRemaining, 600);
    } else if (info.key == "scan:c") {
      EXPECT_EQ(info.sizeBytes, 9);  // "value-ccc"
      EXPECT_EQ(info.ttlRemaining, -1);  // no expiry
    }
  }
}

TEST_F(CacheManagerTest, IsReady) {
  EXPECT_TRUE(cacheManager_->isReady());

  cacheManager_->shutdown();
  EXPECT_FALSE(cacheManager_->isReady());
}

class CacheManagerTtlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "ttl-test-cache";
    config.cacheSize = 50 * 1024 * 1024;  // 50MB
    config.enableNvm = false;

    cacheManager_ = std::make_unique<CacheManager>(config);
    ASSERT_TRUE(cacheManager_->initialize());
  }

  void TearDown() override {
    if (cacheManager_) {
      cacheManager_->shutdown();
    }
  }

  std::unique_ptr<CacheManager> cacheManager_;
};

TEST_F(CacheManagerTtlTest, SetWithTtl) {
  const std::string key = "ttl-key";
  const std::string value = "ttl-value";
  const uint32_t ttlSeconds = 60;

  EXPECT_TRUE(cacheManager_->set(key, value, ttlSeconds));

  auto result = cacheManager_->get(key);
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.value, value);
  // TTL should be approximately 60 seconds (might be slightly less due to processing)
  EXPECT_GT(result.ttlRemaining, 50);
  EXPECT_LE(result.ttlRemaining, 60);
}

TEST_F(CacheManagerTtlTest, SetWithoutTtl) {
  const std::string key = "no-ttl-key";
  const std::string value = "no-ttl-value";

  EXPECT_TRUE(cacheManager_->set(key, value, 0));

  auto result = cacheManager_->get(key);
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.ttlRemaining, -1);  // No expiration (-1 = no expiry)
}

// Note: Testing actual expiration would require sleeping, which is not ideal
// for unit tests. In production, you might use mock time or a fast-forward mechanism.

class CacheManagerInitializationTest : public ::testing::Test {};

TEST_F(CacheManagerInitializationTest, DefaultConfig) {
  CacheConfig config;
  auto cacheManager = std::make_unique<CacheManager>(config);

  EXPECT_TRUE(cacheManager->initialize());
  EXPECT_TRUE(cacheManager->isReady());

  cacheManager->shutdown();
}

TEST_F(CacheManagerInitializationTest, CustomConfig) {
  CacheConfig config;
  config.cacheName = "custom-cache";
  config.cacheSize = 256 * 1024 * 1024;  // 256MB
  config.lruRefreshTime = 30;
  config.maxItemSize = 8 * 1024 * 1024;  // 8MB

  auto cacheManager = std::make_unique<CacheManager>(config);
  EXPECT_TRUE(cacheManager->initialize());

  auto stats = cacheManager->getStats();
  EXPECT_GT(stats.totalSize, 0);

  cacheManager->shutdown();
}

TEST_F(CacheManagerInitializationTest, OperationsBeforeInit) {
  CacheConfig config;
  auto cacheManager = std::make_unique<CacheManager>(config);

  // Operations before initialization should handle gracefully
  EXPECT_FALSE(cacheManager->isReady());
  EXPECT_FALSE(cacheManager->set("key", "value"));

  auto result = cacheManager->get("key");
  EXPECT_FALSE(result.found);
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
