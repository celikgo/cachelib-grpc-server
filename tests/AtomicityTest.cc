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

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../CacheManager.h"

namespace cachelib {
namespace grpc_server {
namespace test {

namespace {

// Releases every worker thread at (as close as possible to) the same instant so
// the operations under test actually contend instead of running back to back.
class AtomicityStartGate {
 public:
  void wait() const {
    while (!open_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  void open() { open_.store(true, std::memory_order_release); }

 private:
  std::atomic<bool> open_{false};
};

// Strict decimal parse of a stored counter value.
bool ParseCounter(const std::string& value, int64_t& out) {
  auto result =
      std::from_chars(value.data(), value.data() + value.size(), out);
  return result.ec == std::errc() && result.ptr == value.data() + value.size();
}

// True if `values` is exactly the sequence 1, 2, ... values.size().
bool IsContiguousFromOne(std::vector<int64_t> values, size_t expectedSize) {
  if (values.size() != expectedSize) {
    return false;
  }
  std::sort(values.begin(), values.end());
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i] != static_cast<int64_t>(i) + 1) {
      return false;
    }
  }
  return true;
}

}  // namespace

// =============================================================================
// Contended atomic operations
// =============================================================================

class AtomicityConcurrencyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "atomicity-concurrency-cache";
    config.cacheSize = 64 * 1024 * 1024;  // 64MB
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

// 16 threads race to claim the same key. Exactly one must win, every loser must
// observe the winner's value, and the key must end up holding it.
TEST_F(AtomicityConcurrencyTest, SetNxHasExactlyOneWinnerPerKey) {
  const int kThreads = 16;
  const int kRounds = 6;

  for (int round = 0; round < kRounds; ++round) {
    const std::string key = "setnx-race-" + std::to_string(round);

    AtomicityStartGate gate;
    std::vector<SetNXResult> results(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([this, &gate, &results, &key, round, t]() {
        const std::string value =
            "claim-" + std::to_string(round) + "-" + std::to_string(t);
        gate.wait();
        results[t] = cacheManager_->setNX(key, value, 300);
      });
    }
    gate.open();
    for (auto& thread : threads) {
      thread.join();
    }

    int winners = 0;
    int winnerIndex = -1;
    for (int t = 0; t < kThreads; ++t) {
      if (results[t].wasSet) {
        ++winners;
        winnerIndex = t;
      }
    }
    ASSERT_EQ(winners, 1) << "round " << round << " had " << winners
                          << " SetNX winners";

    const std::string winnerValue =
        "claim-" + std::to_string(round) + "-" + std::to_string(winnerIndex);

    // The winner never reports an existing value; every loser reports exactly
    // the value the winner installed - never its own, never a stale empty.
    EXPECT_TRUE(results[winnerIndex].existingValue.empty());
    for (int t = 0; t < kThreads; ++t) {
      if (t == winnerIndex) {
        continue;
      }
      EXPECT_FALSE(results[t].wasSet);
      EXPECT_EQ(results[t].existingValue, winnerValue)
          << "loser " << t << " in round " << round;
    }

    auto stored = cacheManager_->get(key);
    EXPECT_TRUE(stored.found);
    EXPECT_EQ(stored.value, winnerValue);
  }
}

// 8 threads x 200 increments through incr() must land every single update, and
// the TTL must be stamped exactly once - the "window sealed at creation"
// contract from proto/cache.proto.
TEST_F(AtomicityConcurrencyTest, IncrLosesNoUpdatesAndStampsTtlOnce) {
  const int kThreads = 8;
  const int kIncrementsPerThread = 200;
  const size_t kExpectedTotal =
      static_cast<size_t>(kThreads) * kIncrementsPerThread;
  const std::string key = "incr-race";

  AtomicityStartGate gate;
  std::atomic<int> ttlSetCount{0};
  std::atomic<int> failureCount{0};
  std::vector<std::vector<int64_t>> observed(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, &gate, &ttlSetCount, &failureCount, &observed,
                          &key, t, kIncrementsPerThread]() {
      observed[t].reserve(kIncrementsPerThread);
      gate.wait();
      for (int i = 0; i < kIncrementsPerThread; ++i) {
        auto result = cacheManager_->incr(key, 1, 120);
        if (!result.success) {
          failureCount.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        if (result.ttlSet) {
          ttlSetCount.fetch_add(1, std::memory_order_relaxed);
        }
        observed[t].push_back(result.value);
      }
    });
  }
  gate.open();
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(failureCount.load(), 0);

  // Exactly one caller created the bucket and stamped the window.
  EXPECT_EQ(ttlSetCount.load(), 1);

  // Every increment produced a distinct value and none was lost: the returned
  // values are exactly 1..1600.
  std::vector<int64_t> allValues;
  allValues.reserve(kExpectedTotal);
  for (const auto& perThread : observed) {
    allValues.insert(allValues.end(), perThread.begin(), perThread.end());
  }
  EXPECT_TRUE(IsContiguousFromOne(allValues, kExpectedTotal))
      << "incr returned " << allValues.size()
      << " values that are not the exact sequence 1.." << kExpectedTotal;

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  int64_t finalValue = 0;
  ASSERT_TRUE(ParseCounter(stored.value, finalValue)) << stored.value;
  EXPECT_EQ(finalValue, static_cast<int64_t>(kExpectedTotal));

  // The window was stamped once at 120s and never restamped from scratch.
  EXPECT_GT(stored.ttlRemaining, 100);
  EXPECT_LE(stored.ttlRemaining, 123);
}

// Same shape for increment(): no update may be lost under contention.
TEST_F(AtomicityConcurrencyTest, IncrementLosesNoUpdates) {
  const int kThreads = 8;
  const int kIncrementsPerThread = 200;
  const size_t kExpectedTotal =
      static_cast<size_t>(kThreads) * kIncrementsPerThread;
  const std::string key = "increment-race";

  AtomicityStartGate gate;
  std::atomic<int> failureCount{0};
  std::vector<std::vector<int64_t>> observed(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([this, &gate, &failureCount, &observed, &key, t,
                          kIncrementsPerThread]() {
      observed[t].reserve(kIncrementsPerThread);
      gate.wait();
      for (int i = 0; i < kIncrementsPerThread; ++i) {
        auto result = cacheManager_->increment(key, 1, 0);
        if (!result.success) {
          failureCount.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        observed[t].push_back(result.newValue);
      }
    });
  }
  gate.open();
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(failureCount.load(), 0);

  std::vector<int64_t> allValues;
  allValues.reserve(kExpectedTotal);
  for (const auto& perThread : observed) {
    allValues.insert(allValues.end(), perThread.begin(), perThread.end());
  }
  EXPECT_TRUE(IsContiguousFromOne(allValues, kExpectedTotal))
      << "increment returned " << allValues.size()
      << " values that are not the exact sequence 1.." << kExpectedTotal;

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  int64_t finalValue = 0;
  ASSERT_TRUE(ParseCounter(stored.value, finalValue)) << stored.value;
  EXPECT_EQ(finalValue, static_cast<int64_t>(kExpectedTotal));
}

// Half the threads add 1, half subtract 1, the same number of times each. If a
// single read-modify-write were lost the net result would not be exactly 0.
TEST_F(AtomicityConcurrencyTest, IncrementAndDecrementNetToZero) {
  const int kThreads = 8;  // 4 incrementers, 4 decrementers
  const int kOpsPerThread = 200;
  const std::string key = "incr-decr-race";

  AtomicityStartGate gate;
  std::atomic<int> failureCount{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    const bool adds = (t % 2 == 0);
    threads.emplace_back(
        [this, &gate, &failureCount, &key, adds, kOpsPerThread]() {
          gate.wait();
          for (int i = 0; i < kOpsPerThread; ++i) {
            auto result = adds ? cacheManager_->increment(key, 1, 0)
                               : cacheManager_->decrement(key, 1, 0);
            if (!result.success) {
              failureCount.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
  }
  gate.open();
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(failureCount.load(), 0);

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  int64_t finalValue = 0;
  ASSERT_TRUE(ParseCounter(stored.value, finalValue)) << stored.value;
  EXPECT_EQ(finalValue, 0);
}

// Every thread proposes a different new value for the same expected value.
// Compare-and-swap must admit exactly one of them per generation, and the
// losers must be told the value that actually won - never a value that no
// caller ever proposed.
TEST_F(AtomicityConcurrencyTest,
       CompareAndSwapHasExactlyOneWinnerPerGeneration) {
  const int kThreads = 16;
  const int kRounds = 4;

  for (int round = 0; round < kRounds; ++round) {
    const std::string key = "cas-race-" + std::to_string(round);
    ASSERT_TRUE(cacheManager_->set(key, "v0", 300));

    AtomicityStartGate gate;
    std::vector<CASResult> results(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([this, &gate, &results, &key, round, t]() {
        const std::string proposal =
            "cas-new-" + std::to_string(round) + "-" + std::to_string(t);
        gate.wait();
        results[t] = cacheManager_->compareAndSwap(key, "v0", proposal, 300,
                                                   false);
      });
    }
    gate.open();
    for (auto& thread : threads) {
      thread.join();
    }

    int winners = 0;
    int winnerIndex = -1;
    for (int t = 0; t < kThreads; ++t) {
      if (results[t].success) {
        ++winners;
        winnerIndex = t;
      }
    }
    ASSERT_EQ(winners, 1) << "round " << round << " had " << winners
                          << " CAS winners";

    const std::string winnerValue =
        "cas-new-" + std::to_string(round) + "-" + std::to_string(winnerIndex);
    EXPECT_EQ(results[winnerIndex].actualValue, winnerValue);

    // The swap is serialised, so no loser can still be looking at "v0": the
    // read and the write happen together, and only the winner ever writes.
    for (int t = 0; t < kThreads; ++t) {
      if (t == winnerIndex) {
        continue;
      }
      EXPECT_FALSE(results[t].success);
      EXPECT_EQ(results[t].actualValue, winnerValue)
          << "loser " << t << " in round " << round
          << " saw a value nobody committed";
    }

    auto stored = cacheManager_->get(key);
    EXPECT_TRUE(stored.found);
    EXPECT_EQ(stored.value, winnerValue);
  }
}

// touch() rewrites the item to change its expiry. Under contention that must
// never corrupt, truncate or drop the value, and the surviving TTL must be one
// of the TTLs that was actually requested.
TEST_F(AtomicityConcurrencyTest, ConcurrentTouchNeverCorruptsTheValue) {
  const int kThreads = 8;
  const int kOpsPerThread = 50;
  const std::string key = "touch-race";
  const std::string value(4096, 'z');

  ASSERT_TRUE(cacheManager_->set(key, value, 50));

  AtomicityStartGate gate;
  std::atomic<int> failureCount{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    const uint32_t ttl = (t % 2 == 0) ? 100 : 300;
    threads.emplace_back(
        [this, &gate, &failureCount, &key, ttl, kOpsPerThread]() {
          gate.wait();
          for (int i = 0; i < kOpsPerThread; ++i) {
            auto result = cacheManager_->touch(key, ttl);
            if (!result.success) {
              failureCount.fetch_add(1, std::memory_order_relaxed);
            }
          }
        });
  }
  gate.open();
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(failureCount.load(), 0);

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  EXPECT_EQ(stored.value.size(), value.size());
  EXPECT_EQ(stored.value, value);

  // Last writer wins, and the two candidate windows are far enough apart that
  // the result is unambiguous.
  const int64_t ttlRemaining = cacheManager_->getTTL(key);
  const bool ttlIsOneOfTheRequested =
      (ttlRemaining >= 97 && ttlRemaining <= 100) ||
      (ttlRemaining >= 297 && ttlRemaining <= 300);
  EXPECT_TRUE(ttlIsOneOfTheRequested)
      << "unexpected surviving TTL: " << ttlRemaining;
}

// =============================================================================
// Single-threaded semantics of the atomic operations
// =============================================================================

class AtomicitySemanticsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "atomicity-semantics-cache";
    config.cacheSize = 64 * 1024 * 1024;  // 64MB
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

// A CAS holding a stale token must be a complete no-op: value untouched, TTL
// untouched, and the caller is handed the value it lost to.
TEST_F(AtomicitySemanticsTest, CompareAndSwapWithStaleExpectedDoesNotMutate) {
  const std::string key = "cas-stale";
  ASSERT_TRUE(cacheManager_->set(key, "a", 100));

  const int64_t ttlBefore = cacheManager_->getTTL(key);
  ASSERT_GT(ttlBefore, 90);

  auto result = cacheManager_->compareAndSwap(key, "WRONG", "b", 5, false);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.actualValue, "a");

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  EXPECT_EQ(stored.value, "a");

  // The rejected ttl_seconds=5 must not have been applied.
  const int64_t ttlAfter = cacheManager_->getTTL(key);
  EXPECT_LE(ttlAfter, ttlBefore);
  EXPECT_GE(ttlAfter, ttlBefore - 2);
}

TEST_F(AtomicitySemanticsTest, CompareAndSwapOnMissingKeyDoesNotCreateIt) {
  const std::string key = "cas-missing";
  ASSERT_FALSE(cacheManager_->exists(key));

  auto result = cacheManager_->compareAndSwap(key, "", "created", 60, false);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.actualValue.empty());

  EXPECT_FALSE(cacheManager_->exists(key));
  EXPECT_EQ(cacheManager_->getTTL(key), -2);  // -2 = key not found
}

// README: "CompareAndSwap carries keep_ttl, so optimistic-locking updates do
// not silently reset expiry."
TEST_F(AtomicitySemanticsTest, CompareAndSwapKeepTtlPreservesRemainingTtl) {
  const std::string key = "cas-keep-ttl";
  ASSERT_TRUE(cacheManager_->set(key, "a", 100));

  auto result = cacheManager_->compareAndSwap(key, "a", "b", 0, true);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.actualValue, "b");

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  EXPECT_EQ(stored.value, "b");

  // ttl_seconds=0 would mean "no expiry" without keep_ttl; with keep_ttl the
  // roughly 100s that were left must survive the swap.
  const int64_t ttlAfter = cacheManager_->getTTL(key);
  EXPECT_GE(ttlAfter, 95);
  EXPECT_LE(ttlAfter, 101);
}

TEST_F(AtomicitySemanticsTest, CompareAndSwapWithoutKeepTtlClearsExpiry) {
  const std::string key = "cas-drop-ttl";
  ASSERT_TRUE(cacheManager_->set(key, "a", 100));
  ASSERT_GT(cacheManager_->getTTL(key), 90);

  auto result = cacheManager_->compareAndSwap(key, "a", "b", 0, false);
  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.actualValue, "b");

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  EXPECT_EQ(stored.value, "b");

  // ttl_seconds=0 with keep_ttl=false means "no expiration", same as Set.
  EXPECT_EQ(cacheManager_->getTTL(key), -1);
  EXPECT_EQ(stored.ttlRemaining, -1);
}

TEST_F(AtomicitySemanticsTest, TouchOnMissingKeyFailsWithMessage) {
  auto result = cacheManager_->touch("touch-missing", 60);
  EXPECT_FALSE(result.success);
  EXPECT_FALSE(result.message.empty());

  EXPECT_FALSE(cacheManager_->exists("touch-missing"));
  EXPECT_EQ(cacheManager_->getTTL("touch-missing"), -2);
}

// proto/cache.proto TouchRequest: "New TTL in seconds (0 = remove expiration)".
TEST_F(AtomicitySemanticsTest, TouchWithZeroTtlRemovesExpiry) {
  const std::string key = "touch-clear-ttl";
  ASSERT_TRUE(cacheManager_->set(key, "session", 100));
  ASSERT_GT(cacheManager_->getTTL(key), 90);

  auto result = cacheManager_->touch(key, 0);
  EXPECT_TRUE(result.success);

  EXPECT_EQ(cacheManager_->getTTL(key), -1);  // -1 = no expiry

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  EXPECT_EQ(stored.value, "session");
  EXPECT_EQ(stored.ttlRemaining, -1);
}

// touch() rewrites the item, so the value has to survive byte for byte -
// including embedded NULs, which would be truncated by any C-string handling.
TEST_F(AtomicitySemanticsTest, TouchPreservesValueWithEmbeddedNuls) {
  const std::string key = "touch-binary";
  const std::string value("a\0b\0\0c\xFF", 7);
  ASSERT_EQ(value.size(), 7u);

  ASSERT_TRUE(cacheManager_->set(key, value, 30));

  auto result = cacheManager_->touch(key, 200);
  EXPECT_TRUE(result.success);

  auto stored = cacheManager_->get(key);
  ASSERT_TRUE(stored.found);
  EXPECT_EQ(stored.value.size(), value.size());
  EXPECT_EQ(stored.value, value);

  const int64_t ttlAfter = cacheManager_->getTTL(key);
  EXPECT_GE(ttlAfter, 195);
  EXPECT_LE(ttlAfter, 201);
}

// README: Incr "stamps the TTL only when it creates the key and leaves it alone
// afterwards". Sleep long enough that a sliding window would be obvious.
TEST_F(AtomicitySemanticsTest, IncrWindowDoesNotSlideOnSubsequentHits) {
  const std::string key = "incr-window";

  auto first = cacheManager_->incr(key, 1, 60);
  ASSERT_TRUE(first.success);
  EXPECT_EQ(first.value, 1);
  EXPECT_TRUE(first.ttlSet);

  const int64_t ttlAfterCreate = cacheManager_->getTTL(key);
  EXPECT_GE(ttlAfterCreate, 59);
  EXPECT_LE(ttlAfterCreate, 60);

  std::this_thread::sleep_for(std::chrono::milliseconds(3000));

  auto second = cacheManager_->incr(key, 1, 60);
  ASSERT_TRUE(second.success);
  EXPECT_EQ(second.value, 2);
  EXPECT_FALSE(second.ttlSet);

  // Roughly 57s must be left. If the window slid this would be back at 60.
  const int64_t ttlAfterHit = cacheManager_->getTTL(key);
  EXPECT_GE(ttlAfterHit, 52);
  EXPECT_LE(ttlAfterHit, 58);
}

// The contrast the README draws: Increment does restamp the TTL whenever one is
// supplied, and only preserves the existing window when ttl_seconds is 0.
TEST_F(AtomicitySemanticsTest, IncrementTtlSemanticsDifferFromIncr) {
  const std::string key = "increment-ttl";
  ASSERT_TRUE(cacheManager_->set(key, "5", 100));

  auto preserved = cacheManager_->increment(key, 1, 0);
  ASSERT_TRUE(preserved.success);
  EXPECT_EQ(preserved.newValue, 6);
  const int64_t ttlPreserved = cacheManager_->getTTL(key);
  EXPECT_GE(ttlPreserved, 95);
  EXPECT_LE(ttlPreserved, 101);

  auto restamped = cacheManager_->increment(key, 1, 10);
  ASSERT_TRUE(restamped.success);
  EXPECT_EQ(restamped.newValue, 7);
  const int64_t ttlRestamped = cacheManager_->getTTL(key);
  EXPECT_GE(ttlRestamped, 8);
  EXPECT_LE(ttlRestamped, 11);
}

// The documented atomicity boundary.
//
// CacheManager serialises setNX / increment / decrement / incr /
// compareAndSwap / touch under one mutex. Plain set() deliberately does NOT
// take that mutex and does not participate in the counter protocol, so a set()
// racing an incr() is not serialised and can swallow an increment. That
// interleaving cannot be forced deterministically from a test without a hook
// inside CacheManager, so it is reported as a documentation gap rather than
// pinned by a flaky test.
//
// What IS deterministic - and what this test pins - is the observable
// consequence of set() living outside the protocol: a plain set() on a bucket
// key silently discards the sealed rate-limit window, and incr() cannot
// re-seal it while the key lives, because from incr()'s point of view the key
// is simply a hit whose (absent) TTL must be preserved.
TEST_F(AtomicitySemanticsTest, PlainSetIsOutsideTheAtomicOpProtocol) {
  const std::string key = "boundary-bucket";

  auto sealed = cacheManager_->incr(key, 1, 60);
  ASSERT_TRUE(sealed.success);
  EXPECT_TRUE(sealed.ttlSet);
  EXPECT_GT(cacheManager_->getTTL(key), 50);

  // A plain set() overwrites the counter and its window with no coordination
  // and no error.
  ASSERT_TRUE(cacheManager_->set(key, "0"));
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  // incr() now sees a live key, so it adopts the foreign value and leaves the
  // (missing) TTL alone: the bucket is unsealed and stays that way.
  auto afterSet = cacheManager_->incr(key, 1, 60);
  ASSERT_TRUE(afterSet.success);
  EXPECT_EQ(afterSet.value, 1);
  EXPECT_FALSE(afterSet.ttlSet);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  // Only removing the key lets the next incr() seal a fresh window.
  EXPECT_TRUE(cacheManager_->remove(key));
  auto resealed = cacheManager_->incr(key, 1, 60);
  ASSERT_TRUE(resealed.success);
  EXPECT_EQ(resealed.value, 1);
  EXPECT_TRUE(resealed.ttlSet);
  EXPECT_GT(cacheManager_->getTTL(key), 50);
}

// =============================================================================
// Counter arithmetic must not overflow
// =============================================================================

// Signed overflow is undefined behaviour, and it was reachable from the wire.
// Measured on the published 1.6.0 image:
//
//   Incr      on  9223372036854775807  ->  -9223372036854775808
//   Decrement on -9223372036854775808  ->   9223372036854775807
//
// A rate-limit bucket that wraps negative lets every subsequent limit check
// pass, which is the failure that matters here.
TEST_F(AtomicitySemanticsTest, IncrRefusesToOverflowInsteadOfWrapping) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  const std::string key = "overflow:incr";
  const std::string atMax = std::to_string(kMax);

  ASSERT_TRUE(cacheManager_->set(key, atMax));

  auto result = cacheManager_->incr(key, 1, 60);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("overflow"), std::string::npos)
      << "message was: " << result.message;

  // The stored counter is untouched -- not wrapped, not rewritten.
  EXPECT_EQ(cacheManager_->get(key).value, atMax);

  // One below the ceiling still works, so the guard is not off by one.
  const std::string below = "overflow:incr-below";
  ASSERT_TRUE(cacheManager_->set(below, std::to_string(kMax - 1)));
  auto ok = cacheManager_->incr(below, 1, 60);
  EXPECT_TRUE(ok.success) << ok.message;
  EXPECT_EQ(ok.value, kMax);
}

TEST_F(AtomicitySemanticsTest, IncrementAndDecrementRefuseToOverflow) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();

  const std::string high = "overflow:increment";
  ASSERT_TRUE(cacheManager_->set(high, std::to_string(kMax)));
  auto incremented = cacheManager_->increment(high, 1, 0);
  EXPECT_FALSE(incremented.success);
  EXPECT_EQ(cacheManager_->get(high).value, std::to_string(kMax));

  const std::string low = "overflow:decrement";
  ASSERT_TRUE(cacheManager_->set(low, std::to_string(kMin)));
  auto decremented = cacheManager_->decrement(low, 1, 0);
  EXPECT_FALSE(decremented.success);
  EXPECT_EQ(cacheManager_->get(low).value, std::to_string(kMin));
}

// decrement() negates the caller's delta before adding it, and negating
// INT64_MIN is itself undefined. The delta comes straight off the wire.
TEST_F(AtomicitySemanticsTest, DecrementRejectsTheUnnegatableDelta) {
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  const std::string key = "overflow:decrement-delta";

  ASSERT_TRUE(cacheManager_->set(key, "0"));

  auto result = cacheManager_->decrement(key, kMin, 0);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(cacheManager_->get(key).value, "0");
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib
