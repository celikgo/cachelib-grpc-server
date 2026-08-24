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

// Replays the fuzz corpus through FuzzOneInput on every CI run.
//
// This is the cheap half of fuzzing and it needs no clang, no libFuzzer, no
// extra Docker stage and no nightly: it costs about a second inside a test
// binary that already exists. Its value is regression, not discovery -- once a
// crasher is found, it goes in here and can never come back silently.
//
// The seeds are embedded rather than read from disk so the test cannot
// silently replay zero inputs because a directory failed to copy into the
// image. Point CACHELIB_FUZZ_CORPUS at a directory to replay additional files
// (libFuzzer writes crashers as files); anything found there is replayed too.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "FuzzCacheService.h"

namespace cachelib {
namespace grpc_server {
namespace fuzz {
namespace {

using Bytes = std::vector<uint8_t>;

Bytes B(std::initializer_list<int> v) {
  Bytes out;
  out.reserve(v.size());
  for (int x : v) {
    out.push_back(static_cast<uint8_t>(x));
  }
  return out;
}

Bytes Repeat(const std::string& unit, size_t times) {
  Bytes out;
  for (size_t i = 0; i < times; ++i) {
    out.insert(out.end(), unit.begin(), unit.end());
  }
  return out;
}

// Field 1 (sequence_id) is varint; fields 2..5 (get/set/delete/exists) are
// length-delimited submessages. Tags below are (field << 3) | wiretype.
Bytes Submessage(int field, const Bytes& body) {
  Bytes out;
  out.push_back(static_cast<uint8_t>((field << 3) | 2));
  out.push_back(static_cast<uint8_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes StringField(int field, const std::string& value) {
  Bytes out;
  out.push_back(static_cast<uint8_t>((field << 3) | 2));
  out.push_back(static_cast<uint8_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

std::vector<std::pair<std::string, Bytes>> Seeds() {
  std::vector<std::pair<std::string, Bytes>> seeds;

  // --- degenerate buffers -------------------------------------------------
  seeds.emplace_back("empty", Bytes{});
  seeds.emplace_back("single-zero", B({0x00}));
  seeds.emplace_back("all-ones", Bytes(64, 0xFF));
  seeds.emplace_back("text", Bytes{'h', 'e', 'l', 'l', 'o'});

  // --- malformed encodings ------------------------------------------------
  // A length-delimited field claiming far more bytes than are present.
  seeds.emplace_back("truncated-submessage", B({0x12, 0x7F, 0x0A, 0x01}));
  // A varint that never terminates.
  seeds.emplace_back("unterminated-varint",
                     B({0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                        0xFF, 0xFF, 0xFF}));
  // Wire type 7 does not exist.
  seeds.emplace_back("bad-wiretype", B({0x0F, 0x01}));
  // A field number the schema does not define.
  seeds.emplace_back("unknown-field", B({0xF8, 0x7F, 0x01}));

  // --- well-formed operations ---------------------------------------------
  seeds.emplace_back("get", Submessage(2, StringField(1, "fuzz:key")));
  {
    Bytes set = StringField(1, "fuzz:key");
    Bytes value = StringField(2, "fuzz-value");
    set.insert(set.end(), value.begin(), value.end());
    seeds.emplace_back("set", Submessage(3, set));
  }
  seeds.emplace_back("delete", Submessage(4, StringField(1, "fuzz:key")));
  seeds.emplace_back("exists", Submessage(5, StringField(1, "fuzz:key")));
  seeds.emplace_back("empty-key-get", Submessage(2, StringField(1, "")));

  // Keys at and past CacheLib's 255-byte small-key ceiling.
  seeds.emplace_back("key-255",
                     Submessage(2, StringField(1, std::string(255, 'k'))));
  {
    // 256 bytes needs a two-byte length prefix, so build it by hand.
    const std::string key(256, 'k');
    Bytes inner;
    inner.push_back(0x0A);
    inner.push_back(0x80);
    inner.push_back(0x02);
    inner.insert(inner.end(), key.begin(), key.end());
    Bytes out;
    out.push_back(0x12);
    out.push_back(0x83);
    out.push_back(0x02);
    out.insert(out.end(), inner.begin(), inner.end());
    seeds.emplace_back("key-256", out);
  }
  seeds.emplace_back("key-with-nul",
                     Submessage(2, StringField(1, std::string("a\0b", 3))));

  // --- scan-matcher stressors ---------------------------------------------
  // These are the reason this target exists. Every buffer is also used as a
  // Scan pattern, and alternating wildcards with literals is what made the old
  // regex-based matcher backtrack exponentially.
  seeds.emplace_back("stars", Repeat("*", 60));
  seeds.emplace_back("star-a", Repeat("*a", 30));
  seeds.emplace_back("star-question", Repeat("*?", 30));
  seeds.emplace_back("question", Repeat("?", 60));
  seeds.emplace_back("star-a-tail", [] {
    Bytes b = Repeat("*a", 29);
    b.push_back('b');
    return b;
  }());
  // Regex metacharacters, which the old implementation escaped by hand and
  // which must now be matched literally.
  seeds.emplace_back("metacharacters",
                     Bytes{'(', '(', '(', '.', '*', ')', '+', ')', '+', '$'});
  seeds.emplace_back("bracket-unclosed", Bytes{'[', 'a', '-'});
  seeds.emplace_back("backslash-tail", Bytes{'a', '\\'});

  return seeds;
}

// Optional extra corpus, e.g. crashers written by libFuzzer.
std::vector<std::pair<std::string, Bytes>> ExtraCorpus() {
  std::vector<std::pair<std::string, Bytes>> out;
  const char* dir = std::getenv("CACHELIB_FUZZ_CORPUS");
  if (dir == nullptr || *dir == '\0') {
    return out;
  }
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec || !entry.is_regular_file(ec)) {
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    Bytes bytes((std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
    out.emplace_back(entry.path().filename().string(), std::move(bytes));
  }
  return out;
}

TEST(FuzzCorpusReplay, EveryInputIsHandledWithoutCrashing) {
  ASSERT_TRUE(CacheIsUsable())
      << "the fuzz cache could not be constructed; the replay would prove "
         "nothing";

  auto corpus = Seeds();
  const size_t embedded = corpus.size();
  for (auto& extra : ExtraCorpus()) {
    corpus.push_back(std::move(extra));
  }

  // A replay of zero inputs passes trivially, which is the one way this test
  // could lie.
  ASSERT_GT(embedded, 20u);

  for (const auto& [name, bytes] : corpus) {
    SCOPED_TRACE("corpus input: " + name);
    FuzzOneInput(bytes.data(), bytes.size());
  }

  std::cerr << "replayed " << corpus.size() << " fuzz inputs ("
            << embedded << " embedded, " << corpus.size() - embedded
            << " from CACHELIB_FUZZ_CORPUS)\n";
}

// The matcher is linear now. A corpus input that takes seconds is the
// signature of the exponential behaviour coming back.
TEST(FuzzCorpusReplay, NoSingleInputTakesAbsurdlyLong) {
  ASSERT_TRUE(CacheIsUsable());

  for (const auto& [name, bytes] : Seeds()) {
    const auto start = std::chrono::steady_clock::now();
    FuzzOneInput(bytes.data(), bytes.size());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    EXPECT_LT(ms, 1000) << "corpus input '" << name << "' took " << ms
                        << " ms; a linear matcher needs microseconds";
  }
}

}  // namespace
}  // namespace fuzz
}  // namespace grpc_server
}  // namespace cachelib
