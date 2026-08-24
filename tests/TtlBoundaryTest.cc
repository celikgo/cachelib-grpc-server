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

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "../CacheManager.h"

namespace cachelib {
namespace grpc_server {
namespace test {

// CacheLib stores expiry as an absolute uint32 unix second, and CacheManager
// derives every remaining TTL as (expiryTime - now) in whole seconds. So every
// assertion in this file is second-granular by construction: nothing here may
// depend on sub-second precision, and "expired" means now is strictly greater
// than the stamped expiry second (CacheItem::isExpired is expiryTime < now).
namespace {

// Same clock CacheManager and CacheLib use to stamp and compare expiry.
int64_t ttlNowSec() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void ttlSleepMs(int64_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Sleeps until offsetMs into the wall-clock second `sec`.
void ttlSleepUntil(int64_t sec, int64_t offsetMs) {
  std::this_thread::sleep_until(
      std::chrono::system_clock::time_point(std::chrono::seconds(sec)) +
      std::chrono::milliseconds(offsetMs));
}

}  // namespace

// Every fixture below differs only in its cache name.
class TtlBoundaryFixtureBase : public ::testing::Test {
 protected:
  void initCache(const std::string& cacheName) {
    CacheConfig config;
    config.cacheName = cacheName;
    config.cacheSize = 64 * 1024 * 1024;  // 64MB is plenty for these tests
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

// =============================================================================
// Expiry boundaries and getTTL sentinels
// =============================================================================

class TtlBoundaryTest : public TtlBoundaryFixtureBase {
 protected:
  void SetUp() override { initCache("ttl-boundary-cache"); }
};

TEST_F(TtlBoundaryTest, OneSecondTtlIsLiveBeforeBoundaryAndGoneAfter) {
  const std::string key = "boundary:one-second";
  const std::string value = "still-here";

  ASSERT_TRUE(cacheManager_->set(key, value, 1));

  // Expiry is stamped as (the second in which set ran) + 1, and 'now' can land
  // anywhere inside that second. So immediately after the set the remaining
  // TTL is 1 (same second) or 0 (the set straddled the second boundary);
  // anything outside {0, 1} means the stamp itself is wrong.
  auto immediate = cacheManager_->get(key);
  EXPECT_TRUE(immediate.found);
  EXPECT_EQ(immediate.value, value);
  EXPECT_GE(immediate.ttlRemaining, 0);
  EXPECT_LE(immediate.ttlRemaining, 1);

  const int64_t immediateTtl = cacheManager_->getTTL(key);
  EXPECT_GE(immediateTtl, 0);
  EXPECT_LE(immediateTtl, 1);
  EXPECT_TRUE(cacheManager_->exists(key));

  // ttl + 1 second plus margin: 'now' is then strictly greater than the
  // stamped expiry second, which is what CacheLib requires before it treats
  // a lookup as a miss.
  ttlSleepMs(2100);

  auto afterBoundary = cacheManager_->get(key);
  EXPECT_FALSE(afterBoundary.found);
  EXPECT_TRUE(afterBoundary.value.empty());
  EXPECT_FALSE(cacheManager_->exists(key));
  EXPECT_EQ(cacheManager_->getTTL(key), -2);
}

TEST_F(TtlBoundaryTest, ZeroTtlMeansNoExpirationAndSurvivesTheClock) {
  const std::string key = "boundary:no-expiry";
  const std::string value = "immortal";

  ASSERT_TRUE(cacheManager_->set(key, value, 0));

  auto immediate = cacheManager_->get(key);
  EXPECT_TRUE(immediate.found);
  EXPECT_EQ(immediate.ttlRemaining, -1);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  ttlSleepMs(1200);

  auto later = cacheManager_->get(key);
  EXPECT_TRUE(later.found);
  EXPECT_EQ(later.value, value);
  EXPECT_EQ(later.ttlRemaining, -1);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);
  EXPECT_TRUE(cacheManager_->exists(key));
}

TEST_F(TtlBoundaryTest, PlainSetReplacesTheExistingTtl) {
  const std::string key = "boundary:overwrite";

  ASSERT_TRUE(cacheManager_->set(key, "v1", 100));
  const int64_t ttlAfterFirst = cacheManager_->getTTL(key);
  ASSERT_GE(ttlAfterFirst, 95);
  ASSERT_LE(ttlAfterFirst, 100);

  // A plain Set allocates a brand new item, so the TTL argument of the second
  // Set wins outright: it does NOT inherit or preserve the previous expiry.
  // Overwriting with ttl=0 therefore makes an expiring key non-expiring.
  ASSERT_TRUE(cacheManager_->set(key, "v2"));

  auto overwritten = cacheManager_->get(key);
  EXPECT_TRUE(overwritten.found);
  EXPECT_EQ(overwritten.value, "v2");
  EXPECT_EQ(overwritten.ttlRemaining, -1);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  // And the same rule in the other direction: a Set with a TTL over a
  // non-expiring key stamps a window on it.
  ASSERT_TRUE(cacheManager_->set(key, "v3", 100));
  const int64_t ttlAfterThird = cacheManager_->getTTL(key);
  EXPECT_GE(ttlAfterThird, 95);
  EXPECT_LE(ttlAfterThird, 100);
  EXPECT_EQ(cacheManager_->get(key).value, "v3");
}

TEST_F(TtlBoundaryTest, GetTtlSentinels) {
  EXPECT_EQ(cacheManager_->getTTL("boundary:never-existed"), -2);

  const std::string forever = "boundary:sentinel-forever";
  ASSERT_TRUE(cacheManager_->set(forever, "v", 0));
  EXPECT_EQ(cacheManager_->getTTL(forever), -1);

  const std::string windowed = "boundary:sentinel-window";
  ASSERT_TRUE(cacheManager_->set(windowed, "v", 100));
  const int64_t windowedTtl = cacheManager_->getTTL(windowed);
  EXPECT_GE(windowedTtl, 95);
  EXPECT_LE(windowedTtl, 100);

  // A deleted key is indistinguishable from one that never existed.
  ASSERT_TRUE(cacheManager_->remove(windowed));
  EXPECT_EQ(cacheManager_->getTTL(windowed), -2);
}

TEST_F(TtlBoundaryTest, GetTtlIsZeroInsideTheExpirySecond) {
  const std::string key = "boundary:expiry-second";

  // CacheLib only calls an item expired once now is strictly past expiryTime,
  // so during the whole wall-clock second that equals expiryTime the item is
  // still reachable while (expiryTime - now) rounds to 0. Observing that
  // second requires knowing exactly which second stamped the item, so stamp
  // just after a second boundary and read back in the middle of the next
  // second. Retry a few times so a scheduling hiccup cannot fail the test.
  bool observed = false;
  for (int attempt = 0; attempt < 4 && !observed; ++attempt) {
    ttlSleepUntil(ttlNowSec() + 1, 50);
    const int64_t stampSecond = ttlNowSec();
    ASSERT_TRUE(cacheManager_->set(key, "5", 1));
    if (ttlNowSec() != stampSecond) {
      continue;  // the set straddled a second boundary; expiry is ambiguous
    }

    ttlSleepUntil(stampSecond + 1, 400);
    if (ttlNowSec() != stampSecond + 1) {
      continue;  // overslept the one-second observation window
    }

    auto result = cacheManager_->get(key);
    const int64_t ttl = cacheManager_->getTTL(key);
    // Incr deliberately disagrees with Get about this second: it treats an
    // item whose expiry second has arrived as a miss, so the bucket restarts
    // at delta instead of continuing from 5.
    auto restarted = cacheManager_->incr(key, 7, 5);
    if (ttlNowSec() != stampSecond + 1) {
      continue;  // the reads leaked out of the expiry second; retry
    }
    observed = true;

    EXPECT_TRUE(result.found);
    EXPECT_EQ(result.value, "5");
    EXPECT_EQ(result.ttlRemaining, 0);
    EXPECT_EQ(ttl, 0);

    EXPECT_TRUE(restarted.success);
    EXPECT_EQ(restarted.value, 7);
    EXPECT_TRUE(restarted.ttlSet);
  }

  ASSERT_TRUE(observed)
      << "could not observe the expiry second within four attempts";
}

// =============================================================================
// Touch
// =============================================================================

class TtlBoundaryTouchTest : public TtlBoundaryFixtureBase {
 protected:
  void SetUp() override { initCache("ttl-boundary-touch-cache"); }
};

TEST_F(TtlBoundaryTouchTest, TouchResetsTheWindowAndKeepsTheValue) {
  const std::string key = "touch:extend";
  const std::string value = "session-blob";

  ASSERT_TRUE(cacheManager_->set(key, value, 5));
  const int64_t before = cacheManager_->getTTL(key);
  ASSERT_GE(before, 1);
  ASSERT_LE(before, 5);

  auto result = cacheManager_->touch(key, 50);
  EXPECT_TRUE(result.success);

  const int64_t after = cacheManager_->getTTL(key);
  EXPECT_GT(after, before);  // the window really moved out
  EXPECT_GE(after, 45);
  EXPECT_LE(after, 50);

  auto stored = cacheManager_->get(key);
  EXPECT_TRUE(stored.found);
  EXPECT_EQ(stored.value, value);  // Touch must not change the value
}

TEST_F(TtlBoundaryTouchTest, TouchWithZeroTtlClearsExpiry) {
  const std::string key = "touch:clear";
  const std::string value = "keep-me";

  ASSERT_TRUE(cacheManager_->set(key, value, 30));
  ASSERT_NE(cacheManager_->getTTL(key), -1);

  auto result = cacheManager_->touch(key, 0);
  EXPECT_TRUE(result.success);

  EXPECT_EQ(cacheManager_->getTTL(key), -1);
  auto stored = cacheManager_->get(key);
  EXPECT_TRUE(stored.found);
  EXPECT_EQ(stored.value, value);
  EXPECT_EQ(stored.ttlRemaining, -1);
}

TEST_F(TtlBoundaryTouchTest, TouchOnExpiredOrMissingKeyIsAMiss) {
  const std::string expiredKey = "touch:expired";
  ASSERT_TRUE(cacheManager_->set(expiredKey, "gone-soon", 1));
  ttlSleepMs(2100);
  ASSERT_FALSE(cacheManager_->exists(expiredKey));

  auto expiredResult = cacheManager_->touch(expiredKey, 60);
  EXPECT_FALSE(expiredResult.success);
  EXPECT_NE(expiredResult.message.find("not found"), std::string::npos);
  // Touch must not resurrect an expired key.
  EXPECT_FALSE(cacheManager_->exists(expiredKey));
  EXPECT_EQ(cacheManager_->getTTL(expiredKey), -2);

  auto missingResult = cacheManager_->touch("touch:never-existed", 60);
  EXPECT_FALSE(missingResult.success);
  EXPECT_NE(missingResult.message.find("not found"), std::string::npos);
  EXPECT_EQ(cacheManager_->getTTL("touch:never-existed"), -2);
}

// =============================================================================
// Incr: the fixed-window contract
// =============================================================================

class TtlBoundaryIncrTest : public TtlBoundaryFixtureBase {
 protected:
  void SetUp() override { initCache("ttl-boundary-incr-cache"); }
};

TEST_F(TtlBoundaryIncrTest, FirstIncrStampsTheWindow) {
  const std::string key = "incr:first";

  auto result = cacheManager_->incr(key, 1, 100);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.value, 1);
  EXPECT_TRUE(result.ttlSet);
  EXPECT_TRUE(result.message.empty());

  const int64_t ttl = cacheManager_->getTTL(key);
  EXPECT_GE(ttl, 95);
  EXPECT_LE(ttl, 100);
  EXPECT_EQ(cacheManager_->get(key).value, "1");
}

TEST_F(TtlBoundaryIncrTest, RepeatedIncrNeverSlidesTheWindow) {
  const std::string key = "incr:no-slide";

  auto first = cacheManager_->incr(key, 1, 100);
  ASSERT_TRUE(first.success);
  EXPECT_TRUE(first.ttlSet);

  int64_t previousTtl = cacheManager_->getTTL(key);
  ASSERT_GE(previousTtl, 95);
  ASSERT_LE(previousTtl, 100);
  const int64_t stampedTtl = previousTtl;

  for (int i = 0; i < 2; ++i) {
    auto result = cacheManager_->incr(key, 1, 100);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.ttlSet) << "call " << i << " must not re-stamp";
    EXPECT_EQ(result.value, 2 + i);

    const int64_t ttl = cacheManager_->getTTL(key);
    EXPECT_LE(ttl, previousTtl) << "TTL grew on call " << i;
    previousTtl = ttl;
  }

  // A real gap in the middle of the traffic: a sliding implementation would
  // push the expiry back out to ~100 on the very next call.
  ttlSleepMs(1200);

  for (int i = 0; i < 3; ++i) {
    auto result = cacheManager_->incr(key, 1, 100);
    EXPECT_TRUE(result.success);
    EXPECT_FALSE(result.ttlSet) << "post-sleep call " << i;
    EXPECT_EQ(result.value, 4 + i);

    const int64_t ttl = cacheManager_->getTTL(key);
    EXPECT_LE(ttl, previousTtl) << "TTL grew on post-sleep call " << i;
    previousTtl = ttl;
  }

  // The headline guarantee: after more than a second of sustained traffic the
  // window has shrunk and was never re-stamped back to the configured length.
  EXPECT_LT(previousTtl, 100);
  EXPECT_LT(previousTtl, stampedTtl);
  EXPECT_GE(previousTtl, 90);  // ... and it did not collapse either
  EXPECT_EQ(cacheManager_->get(key).value, "6");
}

TEST_F(TtlBoundaryIncrTest, IncrRestartsTheBucketAfterTheWindowExpires) {
  const std::string key = "incr:restart";

  auto first = cacheManager_->incr(key, 5, 1);
  ASSERT_TRUE(first.success);
  EXPECT_EQ(first.value, 5);
  EXPECT_TRUE(first.ttlSet);

  ttlSleepMs(2100);
  ASSERT_FALSE(cacheManager_->exists(key));

  auto second = cacheManager_->incr(key, 5, 1);
  EXPECT_TRUE(second.success);
  EXPECT_EQ(second.value, 5) << "an expired bucket must restart at delta";
  EXPECT_TRUE(second.ttlSet) << "an expired bucket must be re-stamped";

  const int64_t ttl = cacheManager_->getTTL(key);
  EXPECT_GE(ttl, 0);
  EXPECT_LE(ttl, 1);
}

TEST_F(TtlBoundaryIncrTest, IncrWithZeroTtlCreatesANonExpiringCounter) {
  const std::string key = "incr:no-expiry";

  auto first = cacheManager_->incr(key, 3, 0);
  EXPECT_TRUE(first.success);
  EXPECT_EQ(first.value, 3);
  EXPECT_TRUE(first.ttlSet);  // created the key, even though no TTL was stamped
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  auto second = cacheManager_->incr(key, 3, 0);
  EXPECT_TRUE(second.success);
  EXPECT_EQ(second.value, 6);
  EXPECT_FALSE(second.ttlSet);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);
  EXPECT_EQ(cacheManager_->get(key).ttlRemaining, -1);
}

TEST_F(TtlBoundaryIncrTest, IncrOnNonNumericValueFailsAndKeepsTheValue) {
  const std::string key = "incr:not-a-number";

  ASSERT_TRUE(cacheManager_->set(key, "abc"));

  auto result = cacheManager_->incr(key, 1, 60);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.value, 0);
  EXPECT_FALSE(result.ttlSet);
  EXPECT_NE(result.message.find("not a valid integer"), std::string::npos);

  // The failed Incr must neither clobber the value nor stamp a TTL.
  auto stored = cacheManager_->get(key);
  EXPECT_TRUE(stored.found);
  EXPECT_EQ(stored.value, "abc");
  EXPECT_EQ(cacheManager_->getTTL(key), -1);
}

TEST_F(TtlBoundaryIncrTest, IncrRejectsLeadingSpaceAndParseOverflow) {
  // std::from_chars does not skip whitespace, so a leading space is a hard
  // parse failure.
  const std::string leadingSpace = "incr:leading-space";
  ASSERT_TRUE(cacheManager_->set(leadingSpace, " 12"));
  auto leadingResult = cacheManager_->incr(leadingSpace, 1, 60);
  EXPECT_FALSE(leadingResult.success);
  EXPECT_NE(leadingResult.message.find("not a valid integer"),
            std::string::npos);
  EXPECT_EQ(cacheManager_->get(leadingSpace).value, " 12");

  // A value that does not fit in int64 is reported out of range and rejected.
  const std::string overflow = "incr:overflow";
  ASSERT_TRUE(cacheManager_->set(overflow, "9223372036854775808"));
  auto overflowResult = cacheManager_->incr(overflow, 1, 60);
  EXPECT_FALSE(overflowResult.success);
  EXPECT_NE(overflowResult.message.find("not a valid integer"),
            std::string::npos);
  EXPECT_EQ(cacheManager_->get(overflow).value, "9223372036854775808");
}

TEST_F(TtlBoundaryIncrTest, IncrRejectsTrailingGarbageInsteadOfDestroyingIt) {
  // std::from_chars stops at the first character it cannot consume and still
  // reports success for the prefix, so a parse that checks only `ec` accepted
  // "12abc" as 12 and then wrote "13" over the whole value -- silently
  // destroying the rest of it and reporting success. An Increment aimed at the
  // wrong key was a data-loss bug. The parse must consume the entire value.
  const struct {
    const char* key;
    const char* stored;
  } kNotCounters[] = {
      {"incr:trailing-space", "12 "},
      {"incr:numeric-prefix", "12abc"},
      {"incr:decimal", "7.9"},
      {"incr:sentence", "42 users"},
      {"incr:negative-suffix", "-2x"},
  };

  for (const auto& c : kNotCounters) {
    ASSERT_TRUE(cacheManager_->set(c.key, c.stored)) << c.key;

    auto result = cacheManager_->incr(c.key, 1, 60);
    EXPECT_FALSE(result.success) << c.key << " was accepted as a counter";
    EXPECT_NE(result.message.find("not a valid integer"), std::string::npos)
        << c.key;

    // The point of the fix: the value is still exactly what was stored.
    EXPECT_EQ(cacheManager_->get(c.key).value, c.stored) << c.key;
  }
}

TEST_F(TtlBoundaryIncrTest, IncrementRejectsTrailingGarbageToo) {
  // increment()/decrement() share the parse, so they share the bug and the fix.
  const std::string key = "increment:numeric-prefix";
  ASSERT_TRUE(cacheManager_->set(key, "12abc"));

  auto incremented = cacheManager_->increment(key, 1, 0);
  EXPECT_FALSE(incremented.success);
  EXPECT_EQ(cacheManager_->get(key).value, "12abc");

  auto decremented = cacheManager_->decrement(key, 1, 0);
  EXPECT_FALSE(decremented.success);
  EXPECT_EQ(cacheManager_->get(key).value, "12abc");
}

TEST_F(TtlBoundaryIncrTest, IncrStillAcceptsWellFormedCounters) {
  // The stricter parse must not reject the values a counter actually holds.
  for (const char* value : {"0", "1", "-1", "9223372036854775806"}) {
    const std::string key = std::string("incr:wellformed:") + value;
    ASSERT_TRUE(cacheManager_->set(key, value)) << value;
    auto result = cacheManager_->incr(key, 1, 60);
    EXPECT_TRUE(result.success) << value << ": " << result.message;
  }
}

// =============================================================================
// Increment / Decrement: the documented difference from Incr
// =============================================================================

class TtlBoundaryIncrementTest : public TtlBoundaryFixtureBase {
 protected:
  void SetUp() override { initCache("ttl-boundary-increment-cache"); }
};

TEST_F(TtlBoundaryIncrementTest, IncrementWithPositiveTtlRefreshesTheWindow) {
  const std::string key = "increment:refresh";

  auto first = cacheManager_->increment(key, 1, 100);
  ASSERT_TRUE(first.success);
  EXPECT_EQ(first.newValue, 1);

  const int64_t ttlAfterFirst = cacheManager_->getTTL(key);
  ASSERT_GE(ttlAfterFirst, 95);
  ASSERT_LE(ttlAfterFirst, 100);

  ttlSleepMs(3000);

  // This is the sliding window README describes. Three seconds of wall clock
  // have gone by, so an implementation that preserved the existing expiry
  // would report at most ttlAfterFirst - 2; a refresh puts the TTL back at
  // the configured window length.
  auto second = cacheManager_->increment(key, 1, 100);
  EXPECT_TRUE(second.success);
  EXPECT_EQ(second.newValue, 2);

  const int64_t ttlAfterSecond = cacheManager_->getTTL(key);
  EXPECT_GE(ttlAfterSecond, ttlAfterFirst - 1) << "the window was not refreshed";
  EXPECT_LE(ttlAfterSecond, 100);
}

TEST_F(TtlBoundaryIncrementTest, IncrementWithZeroTtlPreservesTheWindow) {
  const std::string key = "increment:preserve";

  auto first = cacheManager_->increment(key, 1, 100);
  ASSERT_TRUE(first.success);

  const int64_t ttlAfterFirst = cacheManager_->getTTL(key);
  ASSERT_GE(ttlAfterFirst, 95);
  ASSERT_LE(ttlAfterFirst, 100);

  ttlSleepMs(3000);

  // ttlSeconds == 0 on an existing key means "leave the expiry alone": it
  // neither refreshes the window nor clears it.
  auto second = cacheManager_->increment(key, 1, 0);
  EXPECT_TRUE(second.success);
  EXPECT_EQ(second.newValue, 2);

  const int64_t ttlAfterSecond = cacheManager_->getTTL(key);
  EXPECT_NE(ttlAfterSecond, -1) << "ttl=0 must not clear an existing expiry";
  EXPECT_LE(ttlAfterSecond, ttlAfterFirst - 2) << "the window was refreshed";
  EXPECT_GE(ttlAfterSecond, ttlAfterFirst - 6);
}

TEST_F(TtlBoundaryIncrementTest, IncrementWithZeroTtlCreatesNonExpiringKey) {
  const std::string key = "increment:create-no-expiry";

  auto result = cacheManager_->increment(key, 7, 0);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.newValue, 7);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);
  EXPECT_EQ(cacheManager_->get(key).value, "7");
}

TEST_F(TtlBoundaryIncrementTest, DecrementStampsTtlOnCreation) {
  const std::string key = "decrement:create";

  auto result = cacheManager_->decrement(key, 3, 100);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.newValue, -3);
  EXPECT_EQ(cacheManager_->get(key).value, "-3");

  const int64_t ttl = cacheManager_->getTTL(key);
  EXPECT_GE(ttl, 95);
  EXPECT_LE(ttl, 100);
}

TEST_F(TtlBoundaryIncrementTest, IncrementOnNonNumericValueFailsAndKeepsIt) {
  const std::string key = "increment:not-a-number";

  ASSERT_TRUE(cacheManager_->set(key, "abc"));

  auto result = cacheManager_->increment(key, 1, 60);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.newValue, 0);
  EXPECT_NE(result.message.find("not a valid integer"), std::string::npos);

  auto stored = cacheManager_->get(key);
  EXPECT_TRUE(stored.found);
  EXPECT_EQ(stored.value, "abc");
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  // The same parse guards apply to values that overflow int64.
  const std::string overflow = "increment:overflow";
  ASSERT_TRUE(cacheManager_->set(overflow, "9223372036854775808"));
  auto overflowResult = cacheManager_->increment(overflow, 1, 60);
  EXPECT_FALSE(overflowResult.success);
  EXPECT_NE(overflowResult.message.find("not a valid integer"),
            std::string::npos);
  EXPECT_EQ(cacheManager_->get(overflow).value, "9223372036854775808");
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib
