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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#include "../CacheManager.h"

namespace cachelib {
namespace grpc_server {
namespace test {
namespace {

// =============================================================================
// Helpers shared by the hybrid DRAM -> SSD fixtures below
// =============================================================================

// Size of the flash tier used by every NVM test in this file.
//
// Navy lays the device out as |metadata|BlockCache|BigHash|. Metadata is 0.5%
// of the device (~1.3 MiB here), BigHash takes the 10% configured in
// CacheManager::configureNvmCache off the end (~25 MiB), and BlockCache gets
// what is left rounded *down* to whole regions - and configureNvmCache sets a
// 16 MiB region size. At 256 MiB that leaves 14 whole BlockCache regions,
// comfortably above Navy's "not enough space on device" floor of one clean
// region, while still being cheap to create as a sparse temp file.
constexpr size_t kNvmSize = 256ULL * 1024 * 1024;

// configureNvmCache calls setSizePctAndMaxItemSize(10, 2048), so items up to
// 2 KiB are routed to BigHash and anything larger to BlockCache. These two
// sizes straddle that cutoff on purpose.
constexpr size_t kSmallValueSize = 512;
constexpr size_t kLargeValueSize = 8192;

// Polls |predicate| every 25 ms until it returns true or |timeoutMs| elapses.
// NVM admission and eviction are asynchronous: an item leaves DRAM before the
// Navy insert has completed on a writer thread, so nothing about the flash
// tier may be asserted without a bounded wait. Adds a test failure (rather
// than hanging or silently passing) on timeout.
template <typename Predicate>
bool pollUntil(Predicate predicate, int timeoutMs, const char* what) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (predicate()) {
    return true;
  }
  ADD_FAILURE() << "Timed out after " << timeoutMs << " ms waiting for: "
                << what;
  return false;
}

// Creates a fresh directory under the system temp directory to hold one NVM
// backing file. create_directory() fails when the name already exists, so two
// CI processes running this suite in parallel can never end up truncating the
// same backing file. Returns an empty path when no directory could be created
// (a runner without a usable temp filesystem); callers turn that into a skip.
std::filesystem::path makeNvmDir(const std::string& tag) {
  static std::atomic<uint64_t> counter{0};

  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return {};
  }

  for (int attempt = 0; attempt < 32; ++attempt) {
    const auto stamp = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const std::filesystem::path dir =
        base / ("cachelib_nvm_" + tag + "_" + std::to_string(stamp) + "_" +
                std::to_string(counter.fetch_add(1)));
    ec.clear();
    if (std::filesystem::create_directory(dir, ec) && !ec) {
      return dir;
    }
  }
  return {};
}

void removeNvmDir(const std::filesystem::path& dir) {
  if (dir.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// Fixed-width keys, so that every item written by a fill loop lands in the
// same CacheLib allocation class as the probe key. Eviction is per allocation
// class: a probe whose item size fell into a different class would never be
// evicted no matter how much filler was written past it.
std::string makeKey(int index) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "nvm-hybrid-key-%08d", index);
  return std::string(buf);
}

// A value that cycles through all 256 byte values (31 is coprime with 256),
// so NUL bytes and high-bit bytes are always present and any truncation or
// re-encoding on the flash round trip shows up as an inequality.
std::string makePatternValue(size_t size, unsigned seed) {
  std::string value;
  value.resize(size);
  for (size_t i = 0; i < size; ++i) {
    value[i] = static_cast<char>((i * 31u + seed) & 0xFFu);
  }
  return value;
}

}  // namespace

// =============================================================================
// 1. Configuration: what the tier reports about itself when it is off
// =============================================================================

class NvmHybridConfigTest : public ::testing::Test {};

TEST_F(NvmHybridConfigTest, NvmDisabledLeavesFlashCountersZero) {
  CacheConfig config;
  config.cacheName = "nvm-hybrid-disabled-cache";
  config.cacheSize = 64 * 1024 * 1024;
  config.enableNvm = false;
  config.nvmCacheSize = kNvmSize;  // Must be ignored while the tier is off.
  config.enableIoUring = false;

  CacheManager manager(config);
  ASSERT_TRUE(manager.initialize());
  ASSERT_TRUE(manager.isReady());

  const std::string value = makePatternValue(kSmallValueSize, 5);
  ASSERT_TRUE(manager.set("nvm-off-key", value));
  auto result = manager.get("nvm-off-key");
  ASSERT_TRUE(result.found);
  EXPECT_TRUE(result.value == value);

  auto stats = manager.getStats();
  EXPECT_FALSE(stats.nvmEnabled);
  EXPECT_EQ(stats.nvmSize, 0);
  EXPECT_EQ(stats.nvmUsed, 0);
  EXPECT_EQ(stats.nvmHitCount, 0);
  EXPECT_EQ(stats.nvmMissCount, 0);
  EXPECT_GT(stats.totalSize, 0);

  manager.shutdown();
  EXPECT_FALSE(manager.isReady());
}

TEST_F(NvmHybridConfigTest, EnableNvmWithEmptyPathStillStartsDramOnly) {
  CacheConfig config;
  config.cacheName = "nvm-hybrid-emptypath-cache";
  config.cacheSize = 64 * 1024 * 1024;
  config.enableNvm = true;
  config.nvmCachePath = "";  // No device: initialize()'s guard must hold.
  config.nvmCacheSize = kNvmSize;
  config.enableIoUring = false;

  CacheManager manager(config);
  // initialize() only calls configureNvmCache() when the path is non-empty.
  // If that guard were dropped, Navy would be handed an empty device path and
  // startup would fail outright instead of quietly running DRAM-only.
  ASSERT_TRUE(manager.initialize());
  ASSERT_TRUE(manager.isReady());

  const std::string value = makePatternValue(kSmallValueSize, 3);
  ASSERT_TRUE(manager.set("empty-path-key", value));
  auto result = manager.get("empty-path-key");
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.value.size(), value.size());
  EXPECT_TRUE(result.value == value);

  // No flash device was ever opened, so nothing can have been written to or
  // read from one.
  auto stats = manager.getStats();
  EXPECT_EQ(stats.nvmUsed, 0);
  EXPECT_EQ(stats.nvmHitCount, 0);
  EXPECT_EQ(stats.nvmMissCount, 0);
  // NOTE: stats.nvmEnabled / stats.nvmSize are deliberately NOT asserted here.
  // They mirror the *request* (config.enableNvm, config.nvmCacheSize) rather
  // than whether a tier was actually configured, so with an empty path they
  // advertise a flash tier that does not exist. That is reported as a
  // divergence rather than pinned by a test.

  manager.shutdown();
  EXPECT_FALSE(manager.isReady());
}

// The one test in this file that must never be skipped.
//
// Every fixture below skips when initialize() fails, because a runner without
// a usable temp filesystem should not red-light CI. That is also exactly how
// the flash tier stayed broken: configureNvmCache() passed the reader/writer
// thread counts positionally into NavyConfig::enableAsyncIo(), whose first two
// parameters are maxNumReads and maxNumWrites, and never called
// setReaderAndWriterThreads() first. Navy threw
//
//   number of read/write threads should be set first as non-zero value
//
// initialize() caught it and returned false, and every --enable_nvm start
// died before serving a request. A skipping suite reports that as "skipped".
//
// So: given a writable temp directory -- checked separately, and the only
// legitimate reason to skip -- a hybrid CacheManager MUST initialise. If this
// fails, the flash tier is broken, and that is a failure, not an absence.
TEST(NvmHybridInitialization, HybridTierMustInitialiseWhenAFileCanBeCreated) {
  auto dir = makeNvmDir("must-init");
  if (dir.empty()) {
    GTEST_SKIP() << "No writable temp directory for an NVM backing file";
  }

  CacheConfig config;
  config.cacheName = "nvm-must-initialise-cache";
  config.cacheSize = 128 * 1024 * 1024;
  config.enableNvm = true;
  config.nvmCachePath = (dir / "nvm").string();
  config.nvmCacheSize = kNvmSize;
  config.enableIoUring = false;

  CacheManager manager(config);
  const bool initialised = manager.initialize();

  EXPECT_TRUE(initialised)
      << "CacheManager::initialize() rejected a hybrid configuration that a "
         "user following the README's Hybrid DRAM + SSD section would pass. "
         "The flash tier is the reason to run CacheLib; if this fails the "
         "server exits at startup rather than degrading to DRAM.";

  if (initialised) {
    EXPECT_TRUE(manager.isReady());
    // Prove the tier is actually usable, not merely constructed.
    EXPECT_TRUE(manager.set("nvm:must-init", "value"));
    auto got = manager.get("nvm:must-init");
    EXPECT_TRUE(got.found);
    EXPECT_EQ(got.value, "value");
    manager.shutdown();
  }

  removeNvmDir(dir);
}

// =============================================================================
// 2. The tier switched on: round trips while nothing is evicted
// =============================================================================

// DRAM here is large enough that nothing is ever evicted: these tests are
// about the tier being configured and about ordinary operations behaving
// identically while it is on.
class NvmHybridBasicTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nvmDir_ = makeNvmDir("basic");
    if (nvmDir_.empty()) {
      GTEST_SKIP() << "No writable temp directory for an NVM backing file";
    }

    CacheConfig config;
    config.cacheName = "nvm-hybrid-basic-cache";
    config.cacheSize = 128 * 1024 * 1024;
    config.enableNvm = true;
    config.nvmCachePath = (nvmDir_ / "nvm").string();
    config.nvmCacheSize = kNvmSize;
    // The published images are built against a folly with io_uring disabled
    // and CI kernels may not support it; leaving this on would turn every
    // test below into an initialization failure unrelated to the behaviour
    // under test.
    config.enableIoUring = false;

    cacheManager_ = std::make_unique<CacheManager>(config);
    if (!cacheManager_->initialize()) {
      cacheManager_.reset();
      GTEST_SKIP() << "NVM (Navy) tier could not be initialised in this "
                      "environment; skipping hybrid-tier tests";
    }
  }

  void TearDown() override {
    if (cacheManager_) {
      cacheManager_->shutdown();
      cacheManager_.reset();
    }
    removeNvmDir(nvmDir_);
  }

  std::filesystem::path nvmDir_;
  std::unique_ptr<CacheManager> cacheManager_;
};

TEST_F(NvmHybridBasicTest, ReportsFlashTierInStats) {
  EXPECT_TRUE(cacheManager_->isReady());

  auto stats = cacheManager_->getStats();
  EXPECT_TRUE(stats.nvmEnabled);
  EXPECT_EQ(stats.nvmSize, static_cast<int64_t>(kNvmSize));
  EXPECT_GT(stats.totalSize, 0);
  EXPECT_EQ(stats.version, kServerVersion);
}

TEST_F(NvmHybridBasicTest, SmallValueRoundTripsWithTierOn) {
  const std::string key = "hybrid-small-value";
  const std::string value = makePatternValue(kSmallValueSize, 11);

  ASSERT_TRUE(cacheManager_->set(key, value));
  EXPECT_TRUE(cacheManager_->exists(key));

  auto result = cacheManager_->get(key);
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.value.size(), kSmallValueSize);
  EXPECT_TRUE(result.value == value) << "small value came back altered";
  EXPECT_EQ(result.ttlRemaining, -1);
  EXPECT_EQ(cacheManager_->getTTL(key), -1);

  EXPECT_TRUE(cacheManager_->remove(key));
  EXPECT_FALSE(cacheManager_->exists(key));
  EXPECT_FALSE(cacheManager_->get(key).found);
}

TEST_F(NvmHybridBasicTest, LargeValueRoundTripsWithTierOn) {
  const std::string key = "hybrid-large-value";
  const std::string value = makePatternValue(kLargeValueSize, 29);

  ASSERT_TRUE(cacheManager_->set(key, value, 300));

  auto result = cacheManager_->get(key);
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.value.size(), kLargeValueSize);
  EXPECT_TRUE(result.value == value) << "large value came back altered";
  EXPECT_GT(result.ttlRemaining, 240);
  EXPECT_LE(result.ttlRemaining, 300);

  EXPECT_TRUE(cacheManager_->remove(key));
  EXPECT_FALSE(cacheManager_->get(key).found);
}

TEST_F(NvmHybridBasicTest, FlushRemovesEveryDramResidentKey) {
  constexpr int kKeyCount = 50;
  for (int i = 0; i < kKeyCount; ++i) {
    ASSERT_TRUE(cacheManager_->set(makeKey(i), "flush-me"));
  }

  // Fifty tiny items cannot evict anything out of a 128 MiB DRAM tier, so
  // every key is still RAM-resident and flush() - which walks the RAM cache -
  // must account for all of them.
  EXPECT_EQ(cacheManager_->flush(), kKeyCount);

  for (int i = 0; i < kKeyCount; ++i) {
    EXPECT_FALSE(cacheManager_->exists(makeKey(i)))
        << "key " << i << " survived flush";
  }
  EXPECT_EQ(cacheManager_->flush(), 0);
}

// =============================================================================
// 3-5. Spilling DRAM onto flash and reading it back
// =============================================================================

class NvmHybridEvictionTest : public ::testing::Test {
 protected:
  // 64 MiB is 16 CacheLib slabs (Slab::kSize is 1 << 22). SlabAllocator
  // reserves the leading slab(s) for slab headers, so this sits comfortably
  // above its "not enough memory for slabs" floor while still being small
  // enough to overflow in a second or two.
  static constexpr size_t kDramSize = 64ULL * 1024 * 1024;

  // 4 KiB values are above BigHash's 2 KiB cutoff, so everything evicted here
  // lands in BlockCache, which owns ~224 MiB of the 256 MiB device.
  static constexpr size_t kFillValueSize = 4096;

  // 40960 * 4 KiB is 160 MiB - two and a half times the DRAM tier, so the
  // first key written is certainly gone from DRAM by the end. The ~100 MiB
  // that spills onto flash stays well inside BlockCache's ~224 MiB, so the
  // early keys are not reclaimed again before they are read back.
  static constexpr int kFillCount = 40960;

  void SetUp() override {
    nvmDir_ = makeNvmDir("eviction");
    if (nvmDir_.empty()) {
      GTEST_SKIP() << "No writable temp directory for an NVM backing file";
    }

    CacheConfig config;
    config.cacheName = "nvm-hybrid-eviction-cache";
    config.cacheSize = kDramSize;
    config.enableNvm = true;
    config.nvmCachePath = (nvmDir_ / "nvm").string();
    config.nvmCacheSize = kNvmSize;
    config.enableIoUring = false;

    cacheManager_ = std::make_unique<CacheManager>(config);
    if (!cacheManager_->initialize()) {
      cacheManager_.reset();
      GTEST_SKIP() << "NVM (Navy) tier could not be initialised in this "
                      "environment; skipping hybrid-tier tests";
    }
  }

  void TearDown() override {
    if (cacheManager_) {
      cacheManager_->shutdown();
      cacheManager_.reset();
    }
    removeNvmDir(nvmDir_);
  }

  // Writes kFillCount filler items, leaving |skipIndex| (the probe key)
  // untouched. Returns the number of sets that failed so the caller can
  // assert on it once instead of once per iteration.
  size_t writeFiller(int skipIndex) {
    const std::string filler(kFillValueSize, 'F');
    size_t failures = 0;
    for (int i = 0; i < kFillCount; ++i) {
      if (i == skipIndex) {
        continue;
      }
      if (!cacheManager_->set(makeKey(i), filler)) {
        ++failures;
      }
    }
    return failures;
  }

  std::filesystem::path nvmDir_;
  std::unique_ptr<CacheManager> cacheManager_;
};

TEST_F(NvmHybridEvictionTest, EvictedKeyIsStillServedAfterLeavingDram) {
  const std::string probeKey = makeKey(0);
  const std::string probeValue = makePatternValue(kFillValueSize, 7);
  ASSERT_TRUE(cacheManager_->set(probeKey, probeValue));

  ASSERT_EQ(writeFiller(0), 0u);

  auto stats = cacheManager_->getStats();
  EXPECT_GT(stats.evictionCount, 0)
      << "DRAM never evicted anything; the fill did not overflow the RAM tier";
  // numItems counts the RAM hash table only, so this pins that the DRAM tier
  // is physically incapable of still holding everything that was written.
  EXPECT_LT(stats.itemCount, static_cast<int64_t>(kFillCount));

  // Admission to flash is asynchronous: the item is copied out of DRAM when it
  // is evicted, but the Navy insert completes on a writer thread.
  std::string fetched;
  ASSERT_TRUE(pollUntil(
      [&]() {
        auto result = cacheManager_->get(probeKey);
        if (!result.found) {
          return false;
        }
        fetched = result.value;
        return true;
      },
      15000,
      "the first key written to become readable again after DRAM eviction"));

  EXPECT_EQ(fetched.size(), probeValue.size());
  EXPECT_TRUE(fetched == probeValue) << "value changed on the flash round trip";
}

// The three places where DRAM-only bookkeeping leaks into the wire contract.
//
// CacheLib's iterator walks the DRAM access container, and remove() reports
// kNotFoundInRam when the key was not in DRAM even though it does delete the
// flash copy. Scan, Flush and Delete all sit on top of that, so with the flash
// tier on they behave differently from what a reader would assume. These tests
// pin the actual behaviour, and proto/cache.proto and README.md now say so.
//
// Each uses a probe key that is NOT read back first, because a read promotes
// the item into DRAM and would destroy what is being measured.

TEST_F(NvmHybridEvictionTest, ScanDoesNotSeeFlashResidentKeys) {
  const std::string probeKey = "nvm-probe-scan";
  const std::string probeValue = makePatternValue(kFillValueSize, 11);
  ASSERT_TRUE(cacheManager_->set(probeKey, probeValue));

  ASSERT_EQ(writeFiller(-1), 0u);
  ASSERT_GT(cacheManager_->getStats().evictionCount, 0);

  // Scan first: reading would promote the key back into DRAM.
  auto scanned = cacheManager_->scan("nvm-probe-scan", "", 100);
  const bool visibleToScan = !scanned.keys.empty();

  // Now prove the key is still in the cache, just not in DRAM.
  std::string fetched;
  ASSERT_TRUE(pollUntil(
      [&]() {
        auto result = cacheManager_->get(probeKey);
        if (!result.found) {
          return false;
        }
        fetched = result.value;
        return true;
      },
      15000,
      "the probe key to be served from flash"));
  EXPECT_TRUE(fetched == probeValue);

  EXPECT_FALSE(visibleToScan)
      << "Scan returned a key that had been evicted to flash. That would be an "
         "improvement, but proto/cache.proto and README.md document the "
         "opposite -- update them rather than deleting this test.";
}

TEST_F(NvmHybridEvictionTest, FlushLeavesFlashResidentKeysBehind) {
  const std::string probeKey = "nvm-probe-flush";
  const std::string probeValue = makePatternValue(kFillValueSize, 13);
  ASSERT_TRUE(cacheManager_->set(probeKey, probeValue));

  ASSERT_EQ(writeFiller(-1), 0u);
  ASSERT_GT(cacheManager_->getStats().evictionCount, 0);

  const int64_t removed = cacheManager_->flush();
  EXPECT_GT(removed, 0) << "flush removed nothing at all";

  // The DRAM-resident filler is gone.
  EXPECT_FALSE(cacheManager_->get(makeKey(kFillCount - 1)).found);

  // The flash-resident probe survived the flush.
  std::string fetched;
  const bool survived = pollUntil(
      [&]() {
        auto result = cacheManager_->get(probeKey);
        if (!result.found) {
          return false;
        }
        fetched = result.value;
        return true;
      },
      15000,
      "the probe key to be served from flash after a flush");

  EXPECT_TRUE(survived)
      << "Flush reached a flash-resident key. That would be an improvement, "
         "but FlushRequest.include_nvm is documented as not implemented and "
         "the README says flash entries are left in place -- update them "
         "rather than deleting this test.";
  if (survived) {
    EXPECT_TRUE(fetched == probeValue);
  }
}

TEST_F(NvmHybridEvictionTest, RemoveReportsDramResidencyNotExistence) {
  const std::string probeKey = "nvm-probe-remove";
  const std::string probeValue = makePatternValue(kFillValueSize, 17);
  ASSERT_TRUE(cacheManager_->set(probeKey, probeValue));

  // A control key that stays in DRAM, so the assertions below cannot be
  // explained by "nothing was ever stored".
  const std::string controlKey = "nvm-probe-remove-control";
  ASSERT_TRUE(cacheManager_->set(controlKey, "control"));

  ASSERT_EQ(writeFiller(-1), 0u);
  ASSERT_GT(cacheManager_->getStats().evictionCount, 0);

  // Delete WITHOUT reading first. CacheLib removes the flash copy but returns
  // kNotFoundInRam, which CacheManager::remove reports as false, which
  // CacheServiceImpl::Delete reports as key_existed=false.
  const bool reportedExisted = cacheManager_->remove(probeKey);

  // Whatever it reported, the key really is gone: give the asynchronous flash
  // delete a moment and confirm it never comes back.
  bool stillReadable = false;
  for (int i = 0; i < 30 && !stillReadable; ++i) {
    stillReadable = cacheManager_->get(probeKey).found;
    if (!stillReadable) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      stillReadable = cacheManager_->get(probeKey).found;
    }
  }
  EXPECT_FALSE(stillReadable) << "the flash copy was not deleted";

  EXPECT_FALSE(reportedExisted)
      << "remove() reported that a flash-resident key existed. That would be "
         "an improvement over kNotFoundInRam, but DeleteResponse.key_existed "
         "is documented as DRAM residency -- update the docs rather than "
         "deleting this test.";

  // The control key is still in DRAM, and there remove() reports correctly.
  EXPECT_TRUE(cacheManager_->remove(controlKey));
}

TEST_F(NvmHybridEvictionTest, BinaryValueSurvivesTheFlashRoundTripExactly) {
  const std::string probeKey = makeKey(0);
  // Seed 0 puts a NUL at offset 0 and covers every high-bit byte. BlockCache
  // is configured with setDataChecksum(true); this is the test that gives that
  // setting meaning.
  const std::string probeValue = makePatternValue(kFillValueSize, 0);
  ASSERT_NE(probeValue.find('\0'), std::string::npos);
  ASSERT_NE(probeValue.find(static_cast<char>(0xFF)), std::string::npos);

  ASSERT_TRUE(cacheManager_->set(probeKey, probeValue));
  ASSERT_EQ(writeFiller(0), 0u);
  EXPECT_GT(cacheManager_->getStats().evictionCount, 0);

  std::string fetched;
  ASSERT_TRUE(pollUntil(
      [&]() {
        auto result = cacheManager_->get(probeKey);
        if (!result.found) {
          return false;
        }
        fetched = result.value;
        return true;
      },
      15000,
      "the binary probe value to become readable again after DRAM eviction"));

  ASSERT_EQ(fetched.size(), probeValue.size());
  EXPECT_TRUE(fetched == probeValue)
      << "binary value was corrupted on the flash round trip";
}

TEST_F(NvmHybridEvictionTest, TtlSurvivesTheDramToFlashTrip) {
  const std::string probeKey = makeKey(0);
  const std::string probeValue = makePatternValue(kFillValueSize, 13);
  ASSERT_TRUE(cacheManager_->set(probeKey, probeValue, 120));

  ASSERT_EQ(writeFiller(0), 0u);
  EXPECT_GT(cacheManager_->getStats().evictionCount, 0);

  GetResult fetched;
  ASSERT_TRUE(pollUntil(
      [&]() {
        fetched = cacheManager_->get(probeKey);
        return fetched.found;
      },
      15000,
      "the TTL probe key to become readable again after DRAM eviction"));

  EXPECT_TRUE(fetched.value == probeValue);
  // The expiry has to travel with the item. Losing it would report -1 (no
  // expiry) and a mangled one would land outside the original window.
  EXPECT_GT(fetched.ttlRemaining, 0);
  EXPECT_LE(fetched.ttlRemaining, 120);

  const int64_t ttl = cacheManager_->getTTL(probeKey);
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 120);
}

// =============================================================================
// 7. Lifecycle: shutdown and destruction with the tier on
// =============================================================================

class NvmHybridLifecycleTest : public ::testing::Test {};

TEST_F(NvmHybridLifecycleTest, ShutdownAndDestroyRepeatedlyWithFlashTier) {
  constexpr int kRounds = 3;

  for (int round = 0; round < kRounds; ++round) {
    const auto dir = makeNvmDir("lifecycle");
    if (dir.empty()) {
      GTEST_SKIP() << "No writable temp directory for an NVM backing file";
    }

    CacheConfig config;
    config.cacheName = "nvm-hybrid-lifecycle-cache";
    config.cacheSize = 64 * 1024 * 1024;
    config.enableNvm = true;
    config.nvmCachePath = (dir / "nvm").string();
    config.nvmCacheSize = kNvmSize;
    config.enableIoUring = false;

    auto manager = std::make_unique<CacheManager>(config);
    if (!manager->initialize()) {
      manager.reset();
      removeNvmDir(dir);
      GTEST_SKIP() << "NVM (Navy) tier could not be initialised in this "
                      "environment; skipping hybrid-tier tests";
    }

    const std::string key = "lifecycle-key";
    const std::string value =
        makePatternValue(kLargeValueSize, static_cast<unsigned>(round));
    ASSERT_TRUE(manager->set(key, value));

    auto result = manager->get(key);
    ASSERT_TRUE(result.found);
    EXPECT_TRUE(result.value == value);
    EXPECT_TRUE(manager->getStats().nvmEnabled);

    manager->shutdown();
    EXPECT_FALSE(manager->isReady());

    // Everything has to degrade gracefully rather than crash once the
    // allocator - and with it the flash tier - is gone.
    EXPECT_FALSE(manager->set(key, value));
    EXPECT_FALSE(manager->get(key).found);
    EXPECT_FALSE(manager->exists(key));

    // The destructor runs shutdown() a second time; that must be a no-op and
    // must not leak or hang on the flash path.
    manager.reset();
    removeNvmDir(dir);
  }
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib
