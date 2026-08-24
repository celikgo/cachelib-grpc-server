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
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../CacheManager.h"

namespace cachelib {
namespace grpc_server {
namespace test {

namespace {

// Zero padded key so every key in a series has the same length.
std::string NumberedKey(const std::string& prefix, int index) {
  std::string digits = std::to_string(index);
  while (digits.size() < 3) {
    digits.insert(digits.begin(), '0');
  }
  return prefix + digits;
}

// Every key used by the pattern matching fixture. Chosen so that each pattern
// under test has an exactly known answer over this corpus.
std::vector<std::string> PatternCorpus() {
  return {
      "user:1",
      "user:2",
      "alpha:end",
      "z:end",
      "az",
      "abz",
      "abcz",
      "kxy",
      "kxxy",
      "ky",
      "a.b",
      "axb",
      "a",
      "[abc]",
      "a-b",
      "\\star",  // literal backslash followed by "star"
      "star",
  };
}

// Scans the whole cache for `pattern` and compares the returned keys, order
// insensitively, against `expected`.
void ExpectPatternMatches(CacheManager& manager,
                          const std::string& pattern,
                          std::vector<std::string> expected) {
  auto result = manager.scan(pattern, "", 100);
  auto actual = result.keys;
  std::sort(actual.begin(), actual.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(actual, expected) << "pattern=" << pattern;
}

}  // namespace

// =============================================================================
// Pagination
// =============================================================================

class ScanPaginationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "scan-pagination-cache";
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

TEST_F(ScanPaginationTest, PagesThroughEveryKeyExactlyOnce) {
  const int kNumKeys = 50;
  for (int i = 0; i < kNumKeys; ++i) {
    ASSERT_TRUE(cacheManager_->set(NumberedKey("scan:", i), "v"));
  }

  // 50 keys at 7 per page is 8 pages. The cap turns an infinite-loop
  // regression into a fast failure instead of a hung CI job.
  const int kMaxPages = 20;
  std::set<std::string> seen;
  std::string cursor;
  int pages = 0;
  bool hasMore = true;

  while (hasMore) {
    ++pages;
    ASSERT_LE(pages, kMaxPages)
        << "scan did not terminate; stuck at cursor=" << cursor;

    auto page = cacheManager_->scan("scan:*", cursor, 7);
    ASSERT_LE(page.keys.size(), 7);

    for (const auto& key : page.keys) {
      EXPECT_TRUE(seen.insert(key).second)
          << "key returned by more than one page: " << key;
    }

    if (page.hasMore) {
      ASSERT_FALSE(page.keys.empty());
      // The cursor is the last key of the page, nothing more.
      EXPECT_EQ(page.nextCursor, page.keys.back());
    } else {
      EXPECT_TRUE(page.nextCursor.empty());
    }

    hasMore = page.hasMore;
    cursor = page.nextCursor;
  }

  EXPECT_EQ(pages, 8);
  EXPECT_EQ(seen.size(), kNumKeys);
  for (int i = 0; i < kNumKeys; ++i) {
    const std::string key = NumberedKey("scan:", i);
    EXPECT_EQ(seen.count(key), 1) << "key never returned by any page: " << key;
  }
}

TEST_F(ScanPaginationTest, ClampsNonPositiveCountToOneHundred) {
  const int kNumKeys = 150;
  for (int i = 0; i < kNumKeys; ++i) {
    ASSERT_TRUE(cacheManager_->set(NumberedKey("clamp:", i), "v"));
  }

  auto zero = cacheManager_->scan("clamp:*", "", 0);
  EXPECT_EQ(zero.keys.size(), 100);
  EXPECT_TRUE(zero.hasMore);
  ASSERT_FALSE(zero.keys.empty());
  EXPECT_EQ(zero.nextCursor, zero.keys.back());

  auto negative = cacheManager_->scan("clamp:*", "", -7);
  EXPECT_EQ(negative.keys.size(), 100);
  EXPECT_TRUE(negative.hasMore);
  // Same clamp, same starting point, same iteration order.
  EXPECT_EQ(negative.keys, zero.keys);
}

TEST_F(ScanPaginationTest, HasMoreTracksRemainingItemsNotRemainingMatches) {
  ASSERT_TRUE(cacheManager_->set("hm:only", "v"));
  for (int i = 0; i < 30; ++i) {
    ASSERT_TRUE(cacheManager_->set(NumberedKey("other:", i), "v"));
  }

  auto page1 = cacheManager_->scan("hm:*", "", 1);
  ASSERT_EQ(page1.keys.size(), 1);
  EXPECT_EQ(page1.keys[0], "hm:only");

  // has_more is computed from the underlying iteration, not from the pattern,
  // so unless the only matching key happened to hash last it is set even
  // though no matching key is left. The page that follows is then empty.
  if (page1.hasMore) {
    EXPECT_EQ(page1.nextCursor, "hm:only");

    auto page2 = cacheManager_->scan("hm:*", page1.nextCursor, 1);
    EXPECT_TRUE(page2.keys.empty());
    EXPECT_FALSE(page2.hasMore);
    EXPECT_TRUE(page2.nextCursor.empty());
  } else {
    EXPECT_TRUE(page1.nextCursor.empty());
  }
}

// =============================================================================
// Cursor behaviour under mutation
// =============================================================================

class ScanCursorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "scan-cursor-cache";
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

TEST_F(ScanCursorTest, DeletingTheCursorKeyTruncatesTheScan) {
  const int kNumKeys = 20;
  for (int i = 0; i < kNumKeys; ++i) {
    ASSERT_TRUE(cacheManager_->set(NumberedKey("cur:", i), "v"));
  }

  auto page1 = cacheManager_->scan("cur:*", "", 5);
  ASSERT_EQ(page1.keys.size(), 5);
  ASSERT_TRUE(page1.hasMore);
  ASSERT_EQ(page1.nextCursor, page1.keys.back());

  ASSERT_TRUE(cacheManager_->remove(page1.nextCursor));

  // Resumption works by re-iterating from the start and skipping until the
  // cursor key is seen. Once that key is gone the skip never completes, so the
  // resumed page is empty and the client's iteration silently ends early.
  auto page2 = cacheManager_->scan("cur:*", page1.nextCursor, 5);
  EXPECT_TRUE(page2.keys.empty());
  EXPECT_FALSE(page2.hasMore);
  EXPECT_TRUE(page2.nextCursor.empty());

  // The other keys are still there: the empty page is a cursor artefact, not
  // data loss.
  auto fromScratch = cacheManager_->scan("cur:*", "", 100);
  EXPECT_EQ(fromScratch.keys.size(), 19);
  EXPECT_FALSE(fromScratch.hasMore);
}

TEST_F(ScanCursorTest, KeysInsertedMidScanAreNotIsolated) {
  const int kOriginalKeys = 20;
  std::set<std::string> everInserted;
  for (int i = 0; i < kOriginalKeys; ++i) {
    const std::string key = NumberedKey("mut:", i);
    ASSERT_TRUE(cacheManager_->set(key, "v"));
    everInserted.insert(key);
  }

  auto page1 = cacheManager_->scan("mut:*", "", 5);
  ASSERT_EQ(page1.keys.size(), 5);
  ASSERT_TRUE(page1.hasMore);
  ASSERT_EQ(page1.nextCursor, page1.keys.back());

  // Mutate the cache between pages.
  for (int i = 100; i < 110; ++i) {
    const std::string key = NumberedKey("mut:", i);
    ASSERT_TRUE(cacheManager_->set(key, "v"));
    everInserted.insert(key);
  }

  // There is no snapshot: keys created after the scan started may or may not
  // show up, and this test deliberately does not assert either way. What is
  // guaranteed is that the page is well formed and drawn from keys that
  // really were inserted at some point.
  auto page2 = cacheManager_->scan("mut:*", page1.nextCursor, 5);
  EXPECT_EQ(page2.keys.size(), 5);

  std::set<std::string> keysInPage;
  for (const auto& key : page2.keys) {
    EXPECT_EQ(everInserted.count(key), 1)
        << "scan returned a key that was never inserted: " << key;
    EXPECT_TRUE(keysInPage.insert(key).second)
        << "key returned twice within a single page: " << key;
    EXPECT_NE(key, page1.nextCursor)
        << "cursor key returned again after resuming";
  }
}

// =============================================================================
// Pattern matching
// =============================================================================

class ScanPatternTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "scan-pattern-cache";
    config.cacheSize = 64 * 1024 * 1024;  // 64MB
    config.enableNvm = false;

    cacheManager_ = std::make_unique<CacheManager>(config);
    ASSERT_TRUE(cacheManager_->initialize());

    for (const auto& key : PatternCorpus()) {
      ASSERT_TRUE(cacheManager_->set(key, "v")) << "failed to set key: " << key;
    }
  }

  void TearDown() override {
    if (cacheManager_) {
      cacheManager_->shutdown();
    }
  }

  std::unique_ptr<CacheManager> cacheManager_;
};

TEST_F(ScanPatternTest, StarAndEmptyPatternMatchEverything) {
  ExpectPatternMatches(*cacheManager_, "*", PatternCorpus());
  ExpectPatternMatches(*cacheManager_, "", PatternCorpus());
}

TEST_F(ScanPatternTest, PrefixSuffixAndMiddleWildcards) {
  ExpectPatternMatches(*cacheManager_, "user:*", {"user:1", "user:2"});
  ExpectPatternMatches(*cacheManager_, "*:end", {"alpha:end", "z:end"});
  ExpectPatternMatches(*cacheManager_, "a*z", {"az", "abz", "abcz"});
}

TEST_F(ScanPatternTest, QuestionMarkMatchesExactlyOneCharacter) {
  // "kxxy" is too long and "ky" is too short.
  ExpectPatternMatches(*cacheManager_, "k?y", {"kxy"});
}

TEST_F(ScanPatternTest, RegexMetacharactersAreMatchedLiterally) {
  // '.' is escaped, so it is a dot and not "any character".
  ExpectPatternMatches(*cacheManager_, "a.b", {"a.b"});
  // Brackets are escaped, so this is the literal five character key "[abc]"
  // and not a character class that would match the key "a".
  ExpectPatternMatches(*cacheManager_, "[abc]", {"[abc]"});
  ExpectPatternMatches(*cacheManager_, "[a-c]", {});
}

TEST_F(ScanPatternTest, PatternsWithUnescapedCharactersDoNotThrow) {
  // '-' is not on the escape list. Outside a character class it is an ordinary
  // character, so the pattern must neither throw nor match anything else.
  EXPECT_NO_THROW({ cacheManager_->scan("a-b", "", 100); });
  ExpectPatternMatches(*cacheManager_, "a-b", {"a-b"});

  // A backslash is not an escape character in a pattern: it is matched
  // literally and the '*' that follows is still a wildcard. So this matches
  // the key that starts with a backslash and not the plain "star" key.
  EXPECT_NO_THROW({ cacheManager_->scan("\\*", "", 100); });
  ExpectPatternMatches(*cacheManager_, "\\*", {"\\star"});
}

// A pattern is attacker-controlled input on an unauthenticated port.
//
// The original matcher translated the pattern into a regular expression and
// ran the standard library's matcher over every key. Alternating wildcards and
// literals ("*a*a*a...b") against a key of repeated 'a's makes that backtrack
// exponentially. Measured on the published 1.6.0 image, a 17-byte pattern took
// 6.2 s and a 25-byte one had not finished after 40 s -- and because
// CacheServiceImpl::Scan never checks for cancellation, the server kept
// burning CPU after the client had disconnected.
//
// The bound below is generous by three orders of magnitude for a linear
// matcher and unreachable for a backtracking one, so this fails if anyone
// reintroduces a regex here.
TEST(ScanPatternRobustness, PathologicalPatternsDoNotBacktrack) {
  CacheConfig config;
  config.cacheName = "scan-redos-cache";
  config.cacheSize = 64 * 1024 * 1024;
  config.enableNvm = false;

  CacheManager manager(config);
  ASSERT_TRUE(manager.initialize());

  // Long runs of one character are what makes the old matcher explode.
  for (size_t length : {40u, 120u, 255u}) {
    ASSERT_TRUE(manager.set(std::string(length, 'a'), "v"));
  }

  std::string pattern;
  for (int i = 0; i < 24; ++i) {
    pattern += "*a";
  }
  pattern += "b";  // never matches, so the matcher explores every split

  const auto start = std::chrono::steady_clock::now();
  auto result = manager.scan(pattern, "", 100);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_TRUE(result.keys.empty());
  EXPECT_LT(elapsed.count(), 2000)
      << "scan with a " << pattern.size() << "-byte wildcard pattern took "
      << elapsed.count() << " ms over three keys. A linear matcher needs "
         "microseconds; this is the signature of catastrophic backtracking, "
         "which is remotely triggerable on an unauthenticated port.";

  manager.shutdown();
}

// =============================================================================
// include_details
// =============================================================================

class ScanDetailsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "scan-details-cache";
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

TEST_F(ScanDetailsTest, DetailsCarryTtlAndExactValueSizes) {
  const std::string valueWithNuls("a\0b\0c", 5);

  ASSERT_TRUE(cacheManager_->set("det:noexpiry", "abc", 0));
  ASSERT_TRUE(cacheManager_->set("det:ttl", "abcde", 120));
  ASSERT_TRUE(cacheManager_->set("det:empty", "", 0));
  ASSERT_TRUE(cacheManager_->set("det:nuls", valueWithNuls, 0));

  auto result = cacheManager_->scan("det:*", "", 100, true);
  ASSERT_EQ(result.keys.size(), 4);

  // key_details is parallel to keys: same length, same order.
  ASSERT_EQ(result.keyDetails.size(), result.keys.size());
  for (size_t i = 0; i < result.keys.size(); ++i) {
    EXPECT_EQ(result.keyDetails[i].key, result.keys[i]);
  }

  std::map<std::string, KeyInfo> byKey;
  for (const auto& info : result.keyDetails) {
    byKey[info.key] = info;
  }
  ASSERT_EQ(byKey.count("det:noexpiry"), 1);
  ASSERT_EQ(byKey.count("det:ttl"), 1);
  ASSERT_EQ(byKey.count("det:empty"), 1);
  ASSERT_EQ(byKey.count("det:nuls"), 1);

  EXPECT_EQ(byKey.at("det:noexpiry").ttlRemaining, -1);
  EXPECT_EQ(byKey.at("det:noexpiry").sizeBytes, 3);

  EXPECT_GT(byKey.at("det:ttl").ttlRemaining, 100);
  EXPECT_LE(byKey.at("det:ttl").ttlRemaining, 120);
  EXPECT_EQ(byKey.at("det:ttl").sizeBytes, 5);

  // An empty value is stored, and its recorded size is exactly zero.
  EXPECT_EQ(byKey.at("det:empty").sizeBytes, 0);
  EXPECT_EQ(byKey.at("det:empty").ttlRemaining, -1);

  // NUL bytes are counted, not treated as terminators.
  EXPECT_EQ(byKey.at("det:nuls").sizeBytes, 5);
  EXPECT_EQ(byKey.at("det:nuls").ttlRemaining, -1);
}

// =============================================================================
// Flush
// =============================================================================

class FlushContractTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "flush-contract-cache";
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

TEST_F(FlushContractTest, FlushRemovesEveryKeyAndReportsTheCount) {
  const int kNumKeys = 25;
  for (int i = 0; i < kNumKeys; ++i) {
    // Mix keys with and without an expiry.
    const uint32_t ttl = (i % 2 == 0) ? 0 : 300;
    ASSERT_TRUE(cacheManager_->set(NumberedKey("fl:", i), "value", ttl));
  }

  EXPECT_EQ(cacheManager_->flush(), kNumKeys);

  for (int i = 0; i < kNumKeys; ++i) {
    const std::string key = NumberedKey("fl:", i);
    EXPECT_FALSE(cacheManager_->get(key).found) << "survived flush: " << key;
  }

  auto afterFlush = cacheManager_->scan("*", "", 100);
  EXPECT_TRUE(afterFlush.keys.empty());
  EXPECT_FALSE(afterFlush.hasMore);

  // The cache is still usable afterwards.
  ASSERT_TRUE(cacheManager_->set("fl:reused", "back-in-business"));
  auto reused = cacheManager_->get("fl:reused");
  EXPECT_TRUE(reused.found);
  EXPECT_EQ(reused.value, "back-in-business");

  // ... and the next flush accounts for exactly the one key written since.
  EXPECT_EQ(cacheManager_->flush(), 1);
}

TEST_F(FlushContractTest, FlushOnEmptyCacheReturnsZero) {
  EXPECT_EQ(cacheManager_->flush(), 0);
  EXPECT_EQ(cacheManager_->flush(), 0);

  ASSERT_TRUE(cacheManager_->set("fl:after-empty-flush", "value"));
  EXPECT_TRUE(cacheManager_->get("fl:after-empty-flush").found);
}

// =============================================================================
// Expired keys
// =============================================================================

class ScanExpiryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "scan-expiry-cache";
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

TEST_F(ScanExpiryTest, ExpiredKeysAreNeverReportedAsLive) {
  ASSERT_TRUE(cacheManager_->set("exp:gone", "v", 1));
  ASSERT_TRUE(cacheManager_->set("exp:live", "v", 0));

  // An item expires once the wall clock second is strictly past its expiry
  // second, so a 1 second TTL needs a little over 2 seconds to be certain.
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));

  // Every read path treats the expired key as a miss.
  EXPECT_FALSE(cacheManager_->get("exp:gone").found);
  EXPECT_FALSE(cacheManager_->exists("exp:gone"));
  EXPECT_EQ(cacheManager_->getTTL("exp:gone"), -2);

  // Scan walks the hash table, which still holds logically expired items until
  // they are reaped, so the expired key may or may not be listed. If it is, it
  // must be reported as expired and never as live.
  auto result = cacheManager_->scan("exp:*", "", 100, true);
  ASSERT_EQ(result.keyDetails.size(), result.keys.size());
  EXPECT_GE(result.keys.size(), 1);
  EXPECT_LE(result.keys.size(), 2);

  bool sawLiveKey = false;
  for (size_t i = 0; i < result.keys.size(); ++i) {
    if (result.keys[i] == "exp:live") {
      sawLiveKey = true;
      EXPECT_EQ(result.keyDetails[i].ttlRemaining, -1);
    } else {
      EXPECT_EQ(result.keys[i], "exp:gone");
      EXPECT_EQ(result.keyDetails[i].ttlRemaining, 0);
    }
  }
  EXPECT_TRUE(sawLiveKey) << "the non-expiring key must still be listed";
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib
