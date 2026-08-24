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

// One fuzz body, two front ends.
//
// FuzzOneInput() takes an arbitrary byte buffer, parses it as a PipelineRequest
// and, if that succeeds, drives CacheManager through the operation it encodes.
// It must never crash, whatever the bytes are.
//
//   * Built into cache_service_test, a gtest replays the committed corpus on
//     every CI run. That costs about a second and turns each past crasher into
//     a permanent regression test.
//   * Built with -DCACHELIB_GRPC_LIBFUZZER under clang, the same body becomes
//     LLVMFuzzerTestOneInput for continuous fuzzing.
//
// The protobuf parse itself is Google's code and is already fuzzed upstream.
// What is worth fuzzing here is everything after it: the server's own
// validation, the key and value handling, and above all the glob matcher --
// reachable from an unauthenticated Scan, applied to every key in the cache,
// and until recently implemented by building a regular expression that could
// be made to backtrack exponentially.

#include "FuzzCacheService.h"

#include <climits>
#include <cstdint>
#include <string>

#include "../../CacheManager.h"
#include "cache.pb.h"

namespace cachelib {
namespace grpc_server {
namespace fuzz {

namespace {

// One cache for the whole process. Constructing a CacheAllocator costs far more
// than an iteration, so it is built once and reused. That makes the target
// stateful, which is why the corpus is replayed in order rather than shuffled.
CacheManager* sharedCache() {
  static CacheManager* manager = []() -> CacheManager* {
    CacheConfig config;
    config.cacheName = "fuzz-cache";
    config.cacheSize = 64 * 1024 * 1024;
    config.enableNvm = false;
    config.maxItemSize = 64 * 1024;
    auto* m = new CacheManager(config);
    if (!m->initialize()) {
      delete m;
      return nullptr;
    }
    return m;
  }();
  return manager;
}

// Long enough to reach the matcher's interesting behaviour, short enough that
// one iteration stays fast.
constexpr size_t kMaxPatternBytes = 64;

}  // namespace

bool CacheIsUsable() {
  auto* cache = sharedCache();
  return cache != nullptr && cache->isReady();
}

void FuzzOneInput(const uint8_t* data, size_t size) {
  auto* cache = sharedCache();
  if (cache == nullptr || !cache->isReady()) {
    return;
  }

  ::cachelib::grpc::PipelineRequest request;
  if (request.ParseFromArray(data, static_cast<int>(size))) {
    switch (request.operation_case()) {
      case ::cachelib::grpc::PipelineRequest::kGet:
        if (!request.get().key().empty()) {
          cache->get(request.get().key());
        }
        break;
      case ::cachelib::grpc::PipelineRequest::kSet: {
        const auto& set = request.set();
        if (!set.key().empty()) {
          const int64_t ttl = set.ttl_seconds();
          const uint32_t ttlSeconds =
              (ttl > 0 && ttl <= UINT_MAX) ? static_cast<uint32_t>(ttl) : 0;
          cache->set(set.key(), set.value(), ttlSeconds);
        }
        break;
      }
      case ::cachelib::grpc::PipelineRequest::kDelete:
        if (!request.delete_().key().empty()) {
          cache->remove(request.delete_().key());
        }
        break;
      case ::cachelib::grpc::PipelineRequest::kExists:
        if (!request.exists().key().empty()) {
          cache->exists(request.exists().key());
        }
        break;
      default:
        break;
    }
  }
  // A buffer that is not a valid encoding is the common case, and rejecting it
  // must not crash either -- so the scan below runs regardless of the parse.

  const std::string pattern(
      reinterpret_cast<const char*>(data),
      size > kMaxPatternBytes ? kMaxPatternBytes : size);
  cache->scan(pattern, "", 16);
}

}  // namespace fuzz
}  // namespace grpc_server
}  // namespace cachelib

#ifdef CACHELIB_GRPC_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  ::cachelib::grpc_server::fuzz::FuzzOneInput(data, size);
  return 0;
}
#endif
