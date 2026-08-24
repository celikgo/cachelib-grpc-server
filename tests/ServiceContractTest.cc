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
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../CacheManager.h"
#include "../CacheServiceImpl.h"
#include "cache.grpc.pb.h"

namespace cachelib {
namespace grpc_server {
namespace test {

// The fixture caps item size well below CacheLib's own ceiling so the
// value-size boundary can be exercised cheaply. CacheLib's hard per-allocation
// limit is one slab (Slab::kSize == 4 MiB) minus the item header, so any
// maxItemSize comfortably under 4 MiB is genuinely allocatable and the only
// thing the boundary test observes is CacheManager::set's own size check.
// 64 KiB keeps the whole boundary test under 200 KiB of traffic instead of the
// ~8 MiB the 4 MiB default would cost.
constexpr size_t kContractMaxItemSize = 64 * 1024;

// CacheLib rejects keys longer than KAllocation::kKeyMaxLenSmall (255) when
// allowLargeKeys is false, which is the configuration this server uses.
constexpr size_t kContractMaxKeyLen = 255;

class ServiceContractTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "service-contract-test-cache";
    config.cacheSize = 100 * 1024 * 1024;  // 100MB
    config.enableNvm = false;
    config.maxItemSize = kContractMaxItemSize;

    cacheManager_ = std::make_shared<CacheManager>(config);
    ASSERT_TRUE(cacheManager_->initialize());

    service_ = std::make_unique<CacheServiceImpl>(cacheManager_);

    ::grpc::ServerBuilder builder;
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);

    auto channel = server_->InProcessChannel(::grpc::ChannelArguments());
    stub_ = cachelib::grpc::CacheService::NewStub(channel);
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
    if (cacheManager_) {
      cacheManager_->shutdown();
    }
  }

  // ---------------------------------------------------------------------------
  // Small wire-level helpers. These only arrange state or read it back; every
  // contract assertion lives in the tests themselves.
  // ---------------------------------------------------------------------------

  void MustSet(const std::string& key,
               const std::string& value,
               int64_t ttlSeconds) {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);
    request.set_ttl_seconds(ttlSeconds);

    auto status = stub_->Set(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_TRUE(response.success()) << response.message();
  }

  cachelib::grpc::GetResponse DoGet(const std::string& key) {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key(key);

    auto status = stub_->Get(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response;
  }

  bool DoExists(const std::string& key) {
    ::grpc::ClientContext context;
    cachelib::grpc::ExistsRequest request;
    cachelib::grpc::ExistsResponse response;

    request.set_key(key);

    auto status = stub_->Exists(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response.exists();
  }

  // Returns GetTTLResponse.ttl_seconds and checks the found/sentinel
  // relationship the proto documents (-2 is the only "not found" sentinel).
  int64_t DoGetTTL(const std::string& key) {
    ::grpc::ClientContext context;
    cachelib::grpc::GetTTLRequest request;
    cachelib::grpc::GetTTLResponse response;

    request.set_key(key);

    auto status = stub_->GetTTL(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.found(), response.ttl_seconds() != -2);
    return response.ttl_seconds();
  }

  // A value that is not valid UTF-8: it carries NUL bytes, 0xFF bytes, a
  // truncated two-byte sequence (0xC3 0x28) and a stray continuation byte
  // (0x80). Values are a proto3 `bytes` field, so this must survive intact.
  static std::string BinaryBlob() {
    static const unsigned char kBytes[] = {0x00, 0xFF, 0xFE, 'm',
                                           'i',  'd',  0x00, 0xC3,
                                           0x28, 0x80, 0xFF, 0x7F};
    return std::string(reinterpret_cast<const char*>(kBytes), sizeof(kBytes));
  }

  std::shared_ptr<CacheManager> cacheManager_;
  std::unique_ptr<CacheServiceImpl> service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<cachelib::grpc::CacheService::Stub> stub_;
};

// =============================================================================
// 1. Empty keys
// =============================================================================

TEST_F(ServiceContractTest, EmptyKeyIsInvalidArgumentOnEveryValidatingRpc) {
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;
    request.set_key("");
    auto status = stub_->Get(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Get";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;
    request.set_key("");
    request.set_value("v");
    auto status = stub_->Set(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Set";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::DeleteRequest request;
    cachelib::grpc::DeleteResponse response;
    request.set_key("");
    auto status = stub_->Delete(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Delete";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::ExistsRequest request;
    cachelib::grpc::ExistsResponse response;
    request.set_key("");
    auto status = stub_->Exists(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Exists";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetNXRequest request;
    cachelib::grpc::SetNXResponse response;
    request.set_key("");
    request.set_value("v");
    auto status = stub_->SetNX(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "SetNX";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrementRequest request;
    cachelib::grpc::IncrementResponse response;
    request.set_key("");
    request.set_delta(1);
    auto status = stub_->Increment(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Increment";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::DecrementRequest request;
    cachelib::grpc::DecrementResponse response;
    request.set_key("");
    request.set_delta(1);
    auto status = stub_->Decrement(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Decrement";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;
    request.set_key("");
    request.set_delta(1);
    request.set_ttl_seconds(60);
    auto status = stub_->Incr(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Incr";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::CompareAndSwapRequest request;
    cachelib::grpc::CompareAndSwapResponse response;
    request.set_key("");
    request.set_expected_value("a");
    request.set_new_value("b");
    auto status = stub_->CompareAndSwap(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "CompareAndSwap";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetTTLRequest request;
    cachelib::grpc::GetTTLResponse response;
    request.set_key("");
    auto status = stub_->GetTTL(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "GetTTL";
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::TouchRequest request;
    cachelib::grpc::TouchResponse response;
    request.set_key("");
    request.set_ttl_seconds(60);
    auto status = stub_->Touch(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT)
        << "Touch";
  }
}

TEST_F(ServiceContractTest, MultiGetTreatsAnEmptyKeyAsAPlainMiss) {
  MustSet("mg:empty:present", "present-value", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::MultiGetRequest request;
  cachelib::grpc::MultiGetResponse response;

  request.add_keys("");
  request.add_keys("mg:empty:present");

  // MultiGet does not validate keys the way Get does: it answers rather than
  // failing the whole batch.
  auto status = stub_->MultiGet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.results_size(), 2);

  EXPECT_TRUE(response.results(0).key().empty());
  EXPECT_FALSE(response.results(0).found());
  EXPECT_TRUE(response.results(0).value().empty());

  EXPECT_EQ(response.results(1).key(), "mg:empty:present");
  EXPECT_TRUE(response.results(1).found());
  EXPECT_EQ(response.results(1).value(), "present-value");
}

TEST_F(ServiceContractTest, MultiSetDropsEmptyKeyItemsBeforeAttemptingThem) {
  ::grpc::ClientContext context;
  cachelib::grpc::MultiSetRequest request;
  cachelib::grpc::MultiSetResponse response;

  auto* first = request.add_items();
  first->set_key("ms:empty:first");
  first->set_value("one");

  auto* blank = request.add_items();
  blank->set_key("");
  blank->set_value("dropped");

  auto* last = request.add_items();
  last->set_key("ms:empty:last");
  last->set_value("two");

  auto status = stub_->MultiSet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();

  // The two well-formed items are stored and reported as successes.
  EXPECT_EQ(response.succeeded_count(), 2);

  // NOTE: response.success(), failed_count() and failed_keys() are
  // deliberately not asserted here. CacheServiceImpl::MultiSet currently drops
  // empty-key items before they reach CacheManager, so the dropped item is
  // reported neither as a success nor as a failure and success() stays true
  // even though a requested item was never stored. That conflicts with the
  // proto documentation for MultiSetResponse and is reported as a divergence;
  // pinning it here would lock in the behaviour a fix would have to change.

  auto firstGet = DoGet("ms:empty:first");
  EXPECT_TRUE(firstGet.found());
  EXPECT_EQ(firstGet.value(), "one");

  auto lastGet = DoGet("ms:empty:last");
  EXPECT_TRUE(lastGet.found());
  EXPECT_EQ(lastGet.value(), "two");

  // Whatever the counts say, the empty key is definitely not stored.
  ::grpc::ClientContext scanContext;
  cachelib::grpc::ScanRequest scanRequest;
  cachelib::grpc::ScanResponse scanResponse;
  scanRequest.set_pattern("*");
  auto scanStatus = stub_->Scan(&scanContext, scanRequest, &scanResponse);
  EXPECT_TRUE(scanStatus.ok()) << scanStatus.error_message();
  EXPECT_EQ(scanResponse.keys_size(), 2);
  for (int i = 0; i < scanResponse.keys_size(); ++i) {
    EXPECT_FALSE(scanResponse.keys(i).empty());
  }
}

TEST_F(ServiceContractTest, MultiDeleteCountsAnEmptyKeyAsNotFound) {
  MustSet("md:empty:present", "v", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::MultiDeleteRequest request;
  cachelib::grpc::MultiDeleteResponse response;

  request.add_keys("");
  request.add_keys("md:empty:present");

  auto status = stub_->MultiDelete(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.deleted_count(), 1);
  EXPECT_EQ(response.not_found_count(), 1);
  EXPECT_EQ(response.deleted_count() + response.not_found_count(),
            request.keys_size());

  EXPECT_FALSE(DoExists("md:empty:present"));
}

TEST_F(ServiceContractTest, ScanWithAnEmptyPatternMatchesEveryKey) {
  MustSet("scanall:a", "1", 0);
  MustSet("scanall:b", "2", 0);
  MustSet("unrelated", "3", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::ScanRequest request;
  cachelib::grpc::ScanResponse response;

  request.set_pattern("");  // empty pattern is treated as "*"
  request.set_cursor("");
  request.set_count(0);  // 0 means "use the default of 100"

  auto status = stub_->Scan(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.keys_size(), 3);
  EXPECT_FALSE(response.has_more());
  EXPECT_TRUE(response.next_cursor().empty());
}

// =============================================================================
// 2. Key size boundary
// =============================================================================

TEST_F(ServiceContractTest, KeySizeBoundaryIsExactlyTwoHundredFiftyFiveBytes) {
  const std::string atLimit(kContractMaxKeyLen, 'k');
  const std::string overLimit(kContractMaxKeyLen + 1, 'k');

  // 255 bytes: stored and readable end to end.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(atLimit);
    request.set_value("fits");

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.size_bytes(), 4);
  }

  auto atLimitGet = DoGet(atLimit);
  EXPECT_TRUE(atLimitGet.found());
  EXPECT_EQ(atLimitGet.value(), "fits");
  EXPECT_TRUE(DoExists(atLimit));

  // 256 bytes: CacheLib's allocate() throws (allowLargeKeys is off),
  // CacheManager catches it, and the RPC reports a soft failure.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(overLimit);
    request.set_value("does-not-fit");

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.success());
    EXPECT_FALSE(response.message().empty());
    EXPECT_EQ(response.size_bytes(), 0);
  }

  auto overLimitGet = DoGet(overLimit);
  EXPECT_FALSE(overLimitGet.found());
  EXPECT_FALSE(DoExists(overLimit));
  EXPECT_EQ(DoGetTTL(overLimit), -2);
}

// =============================================================================
// 3. Value size boundary
// =============================================================================

TEST_F(ServiceContractTest, ValueSizeBoundaryIsExactlyMaxItemSize) {
  const std::string atLimitKey = "value:at-limit";
  const std::string overLimitKey = "value:over-limit";
  const std::string atLimit(kContractMaxItemSize, 'v');
  const std::string overLimit(kContractMaxItemSize + 1, 'v');

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(atLimitKey);
    request.set_value(atLimit);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.size_bytes(),
              static_cast<int64_t>(kContractMaxItemSize));
  }

  auto atLimitGet = DoGet(atLimitKey);
  EXPECT_TRUE(atLimitGet.found());
  EXPECT_EQ(atLimitGet.value().size(), kContractMaxItemSize);
  EXPECT_EQ(atLimitGet.value(), atLimit);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(overLimitKey);
    request.set_value(overLimit);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.success());
    EXPECT_FALSE(response.message().empty());
    EXPECT_EQ(response.size_bytes(), 0);
  }

  auto overLimitGet = DoGet(overLimitKey);
  EXPECT_FALSE(overLimitGet.found());
  EXPECT_FALSE(DoExists(overLimitKey));
}

// =============================================================================
// 4. Binary and empty values
// =============================================================================

TEST_F(ServiceContractTest, BinaryValueRoundTripsThroughSetAndGet) {
  const std::string key = "binary:set-get";
  const std::string blob = BinaryBlob();

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(blob);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.size_bytes(), static_cast<int64_t>(blob.size()));
  }

  auto response = DoGet(key);
  EXPECT_TRUE(response.found());
  EXPECT_EQ(response.value().size(), blob.size());
  EXPECT_EQ(response.value(), blob);
}

TEST_F(ServiceContractTest, BinaryValueRoundTripsThroughMultiSetAndMultiGet) {
  const std::string key = "binary:multi";
  const std::string blob = BinaryBlob();

  {
    ::grpc::ClientContext context;
    cachelib::grpc::MultiSetRequest request;
    cachelib::grpc::MultiSetResponse response;

    auto* item = request.add_items();
    item->set_key(key);
    item->set_value(blob);

    auto status = stub_->MultiSet(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.succeeded_count(), 1);
  }

  ::grpc::ClientContext context;
  cachelib::grpc::MultiGetRequest request;
  cachelib::grpc::MultiGetResponse response;

  request.add_keys(key);

  auto status = stub_->MultiGet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.results_size(), 1);
  EXPECT_TRUE(response.results(0).found());
  EXPECT_EQ(response.results(0).value().size(), blob.size());
  EXPECT_EQ(response.results(0).value(), blob);
}

TEST_F(ServiceContractTest, BinaryValueRoundTripsThroughSetNXExistingValue) {
  const std::string key = "binary:setnx";
  const std::string blob = BinaryBlob();

  MustSet(key, blob, 0);

  ::grpc::ClientContext context;
  cachelib::grpc::SetNXRequest request;
  cachelib::grpc::SetNXResponse response;

  request.set_key(key);
  request.set_value("replacement");

  auto status = stub_->SetNX(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(response.was_set());
  EXPECT_EQ(response.existing_value().size(), blob.size());
  EXPECT_EQ(response.existing_value(), blob);

  // And the incumbent value is untouched.
  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), blob);
}

TEST_F(ServiceContractTest, EmptyValueIsStoredAndIsDistinctFromAMiss) {
  const std::string key = "empty:value";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value("");

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.size_bytes(), 0);
  }

  auto stored = DoGet(key);
  EXPECT_TRUE(stored.found());
  EXPECT_TRUE(stored.value().empty());
  EXPECT_EQ(stored.value().size(), 0u);
  EXPECT_EQ(stored.ttl_remaining(), -1);
  EXPECT_TRUE(DoExists(key));
  EXPECT_EQ(DoGetTTL(key), -1);

  // The miss looks the same on the wire except for found/ttl.
  auto missing = DoGet("empty:never-written");
  EXPECT_FALSE(missing.found());
  EXPECT_TRUE(missing.value().empty());
  EXPECT_FALSE(DoExists("empty:never-written"));
  EXPECT_EQ(DoGetTTL("empty:never-written"), -2);
}

// =============================================================================
// 5. Batch semantics
// =============================================================================

TEST_F(ServiceContractTest, MultiGetReturnsOneResultPerRequestedKeyInOrder) {
  MustSet("dup:key", "dup-value", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::MultiGetRequest request;
  cachelib::grpc::MultiGetResponse response;

  request.add_keys("dup:key");
  request.add_keys("dup:key");

  auto status = stub_->MultiGet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.results_size(), 2);

  for (int i = 0; i < response.results_size(); ++i) {
    EXPECT_EQ(response.results(i).key(), "dup:key") << "index " << i;
    EXPECT_TRUE(response.results(i).found()) << "index " << i;
    EXPECT_EQ(response.results(i).value(), "dup-value") << "index " << i;
  }
}

TEST_F(ServiceContractTest, MultiGetFoundFlagsLineUpPositionallyWithRequest) {
  MustSet("mix:hit-a", "a", 0);
  MustSet("mix:hit-b", "b", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::MultiGetRequest request;
  cachelib::grpc::MultiGetResponse response;

  request.add_keys("mix:miss-1");
  request.add_keys("mix:hit-a");
  request.add_keys("mix:miss-2");
  request.add_keys("mix:hit-b");

  auto status = stub_->MultiGet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.results_size(), 4);

  EXPECT_EQ(response.results(0).key(), "mix:miss-1");
  EXPECT_FALSE(response.results(0).found());
  EXPECT_TRUE(response.results(0).value().empty());

  EXPECT_EQ(response.results(1).key(), "mix:hit-a");
  EXPECT_TRUE(response.results(1).found());
  EXPECT_EQ(response.results(1).value(), "a");

  EXPECT_EQ(response.results(2).key(), "mix:miss-2");
  EXPECT_FALSE(response.results(2).found());
  EXPECT_TRUE(response.results(2).value().empty());

  EXPECT_EQ(response.results(3).key(), "mix:hit-b");
  EXPECT_TRUE(response.results(3).found());
  EXPECT_EQ(response.results(3).value(), "b");
}

TEST_F(ServiceContractTest, MultiSetWithADuplicateKeyLetsTheLastWriteWin) {
  ::grpc::ClientContext context;
  cachelib::grpc::MultiSetRequest request;
  cachelib::grpc::MultiSetResponse response;

  auto* first = request.add_items();
  first->set_key("ms:dup");
  first->set_value("first");

  auto* second = request.add_items();
  second->set_key("ms:dup");
  second->set_value("second");

  auto status = stub_->MultiSet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success()) << response.message();
  EXPECT_EQ(response.succeeded_count(), 2);
  EXPECT_EQ(response.failed_count(), 0);
  EXPECT_EQ(response.failed_keys_size(), 0);

  auto get = DoGet("ms:dup");
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "second");
}

TEST_F(ServiceContractTest, MultiSetPartialFailureReportsExactlyTheBadKey) {
  const std::string oversizedKey = "ms:partial:oversized";
  const std::string oversized(kContractMaxItemSize + 1, 'x');

  ::grpc::ClientContext context;
  cachelib::grpc::MultiSetRequest request;
  cachelib::grpc::MultiSetResponse response;

  auto* good1 = request.add_items();
  good1->set_key("ms:partial:good-1");
  good1->set_value("one");

  auto* bad = request.add_items();
  bad->set_key(oversizedKey);
  bad->set_value(oversized);

  auto* good2 = request.add_items();
  good2->set_key("ms:partial:good-2");
  good2->set_value("two");

  auto status = stub_->MultiSet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(response.success());
  EXPECT_EQ(response.succeeded_count(), 2);
  EXPECT_EQ(response.failed_count(), 1);
  EXPECT_EQ(response.succeeded_count() + response.failed_count(),
            request.items_size());
  ASSERT_EQ(response.failed_keys_size(), 1);
  EXPECT_EQ(response.failed_keys(0), oversizedKey);
  EXPECT_FALSE(response.message().empty());

  // The healthy items in the same batch were still written.
  auto good1Get = DoGet("ms:partial:good-1");
  EXPECT_TRUE(good1Get.found());
  EXPECT_EQ(good1Get.value(), "one");

  auto good2Get = DoGet("ms:partial:good-2");
  EXPECT_TRUE(good2Get.found());
  EXPECT_EQ(good2Get.value(), "two");

  EXPECT_FALSE(DoExists(oversizedKey));
}

TEST_F(ServiceContractTest, MultiDeleteCountsAreExactAndSumToKeysSize) {
  MustSet("md:present-1", "1", 0);
  MustSet("md:present-2", "2", 0);
  MustSet("md:present-3", "3", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::MultiDeleteRequest request;
  cachelib::grpc::MultiDeleteResponse response;

  request.add_keys("md:present-1");
  request.add_keys("md:absent-1");
  request.add_keys("md:present-2");
  request.add_keys("md:absent-2");
  request.add_keys("md:present-3");

  auto status = stub_->MultiDelete(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.deleted_count(), 3);
  EXPECT_EQ(response.not_found_count(), 2);
  EXPECT_EQ(response.deleted_count() + response.not_found_count(),
            request.keys_size());
  EXPECT_FALSE(response.message().empty());

  EXPECT_FALSE(DoExists("md:present-1"));
  EXPECT_FALSE(DoExists("md:present-2"));
  EXPECT_FALSE(DoExists("md:present-3"));
}

TEST_F(ServiceContractTest, MultiDeleteCountsTheSecondCopyOfAKeyAsNotFound) {
  MustSet("md:dup", "v", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::MultiDeleteRequest request;
  cachelib::grpc::MultiDeleteResponse response;

  request.add_keys("md:dup");
  request.add_keys("md:dup");

  auto status = stub_->MultiDelete(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  // The key is deleted once; by the time the duplicate is processed it really
  // is absent, so it lands in not_found_count.
  EXPECT_EQ(response.deleted_count(), 1);
  EXPECT_EQ(response.not_found_count(), 1);
  EXPECT_EQ(response.deleted_count() + response.not_found_count(),
            request.keys_size());
}

TEST_F(ServiceContractTest, EmptyBatchesAreNotErrors) {
  {
    ::grpc::ClientContext context;
    cachelib::grpc::MultiGetRequest request;
    cachelib::grpc::MultiGetResponse response;

    auto status = stub_->MultiGet(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.results_size(), 0);
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::MultiSetRequest request;
    cachelib::grpc::MultiSetResponse response;

    auto status = stub_->MultiSet(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
    EXPECT_EQ(response.succeeded_count(), 0);
    EXPECT_EQ(response.failed_count(), 0);
    EXPECT_EQ(response.failed_keys_size(), 0);
  }
  {
    ::grpc::ClientContext context;
    cachelib::grpc::MultiDeleteRequest request;
    cachelib::grpc::MultiDeleteResponse response;

    auto status = stub_->MultiDelete(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
    EXPECT_EQ(response.deleted_count(), 0);
    EXPECT_EQ(response.not_found_count(), 0);
  }
}

// =============================================================================
// 6. The atomic RPCs at the wire level
// =============================================================================

TEST_F(ServiceContractTest, SetNXSetsOnlyWhenTheKeyIsAbsent) {
  const std::string key = "setnx:lock";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetNXRequest request;
    cachelib::grpc::SetNXResponse response;

    request.set_key(key);
    request.set_value("owner-a");

    auto status = stub_->SetNX(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.was_set());
    EXPECT_TRUE(response.existing_value().empty());
    EXPECT_FALSE(response.message().empty());
  }

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetNXRequest request;
    cachelib::grpc::SetNXResponse response;

    request.set_key(key);
    request.set_value("owner-b");

    auto status = stub_->SetNX(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.was_set());
    EXPECT_EQ(response.existing_value(), "owner-a");
    EXPECT_FALSE(response.message().empty());
  }

  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "owner-a");
}

TEST_F(ServiceContractTest, SetNXStampsTtlOnCreationAndNeverExtendsIt) {
  const std::string key = "setnx:ttl";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetNXRequest request;
    cachelib::grpc::SetNXResponse response;

    request.set_key(key);
    request.set_value("first");
    request.set_ttl_seconds(120);

    auto status = stub_->SetNX(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.was_set());
  }

  const int64_t stamped = DoGetTTL(key);
  EXPECT_GT(stamped, 110);
  EXPECT_LE(stamped, 120);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetNXRequest request;
    cachelib::grpc::SetNXResponse response;

    request.set_key(key);
    request.set_value("second");
    request.set_ttl_seconds(6000);

    auto status = stub_->SetNX(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.was_set());
  }

  // A rejected SetNX must not touch the incumbent's expiry.
  const int64_t afterReject = DoGetTTL(key);
  EXPECT_GT(afterReject, 110);
  EXPECT_LE(afterReject, 120);
}

TEST_F(ServiceContractTest, IncrementCreatesTheKeyAndTreatsDeltaZeroAsOne) {
  const std::string key = "incr:create";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrementRequest request;
    cachelib::grpc::IncrementResponse response;

    request.set_key(key);
    // delta left at the proto3 default of 0, which the server reads as 1.

    auto status = stub_->Increment(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.new_value(), 1);
  }

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrementRequest request;
    cachelib::grpc::IncrementResponse response;

    request.set_key(key);
    request.set_delta(41);

    auto status = stub_->Increment(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.new_value(), 42);
  }

  // The counter is stored as its decimal text representation.
  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "42");
}

TEST_F(ServiceContractTest, IncrementWithANegativeDeltaSubtracts) {
  const std::string key = "incr:negative";

  MustSet(key, "100", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::IncrementRequest request;
  cachelib::grpc::IncrementResponse response;

  request.set_key(key);
  request.set_delta(-30);

  auto status = stub_->Increment(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success()) << response.message();
  EXPECT_EQ(response.new_value(), 70);

  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "70");
}

TEST_F(ServiceContractTest, DecrementCreatesTheKeyAtNegativeDelta) {
  const std::string key = "decr:create";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::DecrementRequest request;
    cachelib::grpc::DecrementResponse response;

    request.set_key(key);
    // delta 0 is read as 1, so an absent key becomes -1.

    auto status = stub_->Decrement(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.new_value(), -1);
  }

  {
    ::grpc::ClientContext context;
    cachelib::grpc::DecrementRequest request;
    cachelib::grpc::DecrementResponse response;

    request.set_key(key);
    request.set_delta(9);

    auto status = stub_->Decrement(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.new_value(), -10);
  }

  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "-10");
}

TEST_F(ServiceContractTest, IncrementKeepsTtlWhenZeroAndResetsItWhenGiven) {
  const std::string key = "incr:ttl";

  MustSet(key, "10", 300);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrementRequest request;
    cachelib::grpc::IncrementResponse response;

    request.set_key(key);
    request.set_delta(5);
    request.set_ttl_seconds(0);

    auto status = stub_->Increment(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.new_value(), 15);
  }

  // ttl_seconds=0 means "carry the remaining expiry over", not "clear it".
  const int64_t preserved = DoGetTTL(key);
  EXPECT_GT(preserved, 290);
  EXPECT_LE(preserved, 300);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrementRequest request;
    cachelib::grpc::IncrementResponse response;

    request.set_key(key);
    request.set_delta(5);
    request.set_ttl_seconds(1000);

    auto status = stub_->Increment(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_EQ(response.new_value(), 20);
  }

  // An explicit ttl_seconds does slide the window forward.
  const int64_t refreshed = DoGetTTL(key);
  EXPECT_GT(refreshed, 990);
  EXPECT_LE(refreshed, 1000);
}

TEST_F(ServiceContractTest, IncrStampsTtlOnlyWhenItCreatesTheKey) {
  const std::string key = "rl:user:42";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;

    request.set_key(key);
    request.set_delta(1);
    request.set_ttl_seconds(60);

    auto status = stub_->Incr(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.value(), 1);
    EXPECT_TRUE(response.ttl_set());
  }

  const int64_t stamped = DoGetTTL(key);
  EXPECT_GT(stamped, 50);
  EXPECT_LE(stamped, 60);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;

    request.set_key(key);
    request.set_delta(2);
    request.set_ttl_seconds(6000);

    auto status = stub_->Incr(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.value(), 3);
    EXPECT_FALSE(response.ttl_set());
  }

  // The fixed window must not slide: the second call's much larger
  // ttl_seconds is ignored.
  const int64_t preserved = DoGetTTL(key);
  EXPECT_GT(preserved, 50);
  EXPECT_LE(preserved, 60);

  // Once the bucket is gone, the next Incr re-creates and re-stamps it.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::DeleteRequest request;
    cachelib::grpc::DeleteResponse response;
    request.set_key(key);
    auto status = stub_->Delete(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.key_existed());
  }

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;

    request.set_key(key);
    request.set_delta(1);
    request.set_ttl_seconds(600);

    auto status = stub_->Incr(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.value(), 1);
    EXPECT_TRUE(response.ttl_set());
  }

  const int64_t restamped = DoGetTTL(key);
  EXPECT_GT(restamped, 590);
  EXPECT_LE(restamped, 600);
}

TEST_F(ServiceContractTest, IncrTreatsDeltaZeroAsOneAndZeroTtlAsNoExpiry) {
  const std::string key = "rl:default-delta";

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;

    request.set_key(key);
    // delta and ttl_seconds both left at the proto3 default of 0.

    auto status = stub_->Incr(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.value(), 1);
    EXPECT_TRUE(response.ttl_set());
  }

  EXPECT_EQ(DoGetTTL(key), -1);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;

    request.set_key(key);

    auto status = stub_->Incr(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.value(), 2);
    EXPECT_FALSE(response.ttl_set());
  }

  EXPECT_EQ(DoGetTTL(key), -1);
}

TEST_F(ServiceContractTest, IncrOnANonNumericValueFailsAndLeavesItIntact) {
  const std::string key = "rl:not-a-counter";
  const std::string value = "not-a-number";

  MustSet(key, value, 0);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::IncrRequest request;
    cachelib::grpc::IncrResponse response;

    request.set_key(key);
    request.set_delta(1);
    request.set_ttl_seconds(60);

    auto status = stub_->Incr(&context, request, &response);
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INTERNAL);
    EXPECT_NE(status.error_message().find("integer"), std::string::npos)
        << status.error_message();
  }

  // A failed Incr must not clobber the value it could not parse.
  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), value);
  EXPECT_EQ(DoGetTTL(key), -1);
}

TEST_F(ServiceContractTest, CompareAndSwapSuccessReportsTheNewValue) {
  const std::string key = "cas:success";

  MustSet(key, "v1", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::CompareAndSwapRequest request;
  cachelib::grpc::CompareAndSwapResponse response;

  request.set_key(key);
  request.set_expected_value("v1");
  request.set_new_value("v2");

  auto status = stub_->CompareAndSwap(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.actual_value(), "v2");
  EXPECT_FALSE(response.message().empty());

  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "v2");
}

TEST_F(ServiceContractTest, CompareAndSwapMismatchReportsTheCurrentValue) {
  const std::string key = "cas:mismatch";

  MustSet(key, "actual", 0);

  ::grpc::ClientContext context;
  cachelib::grpc::CompareAndSwapRequest request;
  cachelib::grpc::CompareAndSwapResponse response;

  request.set_key(key);
  request.set_expected_value("stale");
  request.set_new_value("never-written");

  auto status = stub_->CompareAndSwap(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(response.success());
  EXPECT_EQ(response.actual_value(), "actual");

  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "actual");
}

TEST_F(ServiceContractTest, CompareAndSwapOnAMissingKeyFails) {
  const std::string key = "cas:missing";

  ::grpc::ClientContext context;
  cachelib::grpc::CompareAndSwapRequest request;
  cachelib::grpc::CompareAndSwapResponse response;

  request.set_key(key);
  request.set_expected_value("anything");
  request.set_new_value("new");

  auto status = stub_->CompareAndSwap(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(response.success());
  EXPECT_TRUE(response.actual_value().empty());

  // A failed CAS must not create the key.
  EXPECT_FALSE(DoExists(key));
  EXPECT_EQ(DoGetTTL(key), -2);
}

TEST_F(ServiceContractTest, CompareAndSwapKeepTtlPreservesTheRemainingExpiry) {
  const std::string key = "cas:keep-ttl";

  MustSet(key, "v1", 300);

  ::grpc::ClientContext context;
  cachelib::grpc::CompareAndSwapRequest request;
  cachelib::grpc::CompareAndSwapResponse response;

  request.set_key(key);
  request.set_expected_value("v1");
  request.set_new_value("v2");
  request.set_ttl_seconds(0);
  request.set_keep_ttl(true);

  auto status = stub_->CompareAndSwap(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.actual_value(), "v2");

  const int64_t ttl = DoGetTTL(key);
  EXPECT_GT(ttl, 290);
  EXPECT_LE(ttl, 300);
}

TEST_F(ServiceContractTest, CompareAndSwapWithoutKeepTtlAppliesTtlSeconds) {
  const std::string clearKey = "cas:clear-ttl";
  const std::string resetKey = "cas:reset-ttl";

  MustSet(clearKey, "v1", 300);
  MustSet(resetKey, "v1", 300);

  // keep_ttl=false with ttl_seconds=0 means "no expiration", exactly like Set.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::CompareAndSwapRequest request;
    cachelib::grpc::CompareAndSwapResponse response;

    request.set_key(clearKey);
    request.set_expected_value("v1");
    request.set_new_value("v2");
    request.set_ttl_seconds(0);
    request.set_keep_ttl(false);

    auto status = stub_->CompareAndSwap(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
  }

  EXPECT_EQ(DoGetTTL(clearKey), -1);

  // A non-zero ttl_seconds replaces the old expiry outright.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::CompareAndSwapRequest request;
    cachelib::grpc::CompareAndSwapResponse response;

    request.set_key(resetKey);
    request.set_expected_value("v1");
    request.set_new_value("v2");
    request.set_ttl_seconds(1000);
    request.set_keep_ttl(false);

    auto status = stub_->CompareAndSwap(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
  }

  const int64_t ttl = DoGetTTL(resetKey);
  EXPECT_GT(ttl, 990);
  EXPECT_LE(ttl, 1000);
}

TEST_F(ServiceContractTest, GetTtlReportsTheDocumentedSentinels) {
  MustSet("ttl:no-expiry", "v", 0);
  MustSet("ttl:with-expiry", "v", 300);

  // -2: key not found.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetTTLRequest request;
    cachelib::grpc::GetTTLResponse response;

    request.set_key("ttl:missing");

    auto status = stub_->GetTTL(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.found());
    EXPECT_EQ(response.ttl_seconds(), -2);
  }

  // -1: key exists with no expiration.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetTTLRequest request;
    cachelib::grpc::GetTTLResponse response;

    request.set_key("ttl:no-expiry");

    auto status = stub_->GetTTL(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.found());
    EXPECT_EQ(response.ttl_seconds(), -1);
  }

  // 0+: seconds remaining.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetTTLRequest request;
    cachelib::grpc::GetTTLResponse response;

    request.set_key("ttl:with-expiry");

    auto status = stub_->GetTTL(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.found());
    EXPECT_GT(response.ttl_seconds(), 290);
    EXPECT_LE(response.ttl_seconds(), 300);
  }
}

TEST_F(ServiceContractTest, TouchUpdatesTtlOnAnExistingKeyOnly) {
  const std::string key = "touch:session";

  MustSet(key, "session-value", 0);
  EXPECT_EQ(DoGetTTL(key), -1);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::TouchRequest request;
    cachelib::grpc::TouchResponse response;

    request.set_key(key);
    request.set_ttl_seconds(600);

    auto status = stub_->Touch(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success()) << response.message();
    EXPECT_FALSE(response.message().empty());
  }

  const int64_t ttl = DoGetTTL(key);
  EXPECT_GT(ttl, 590);
  EXPECT_LE(ttl, 600);

  // Touch changes only the expiry, never the value.
  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "session-value");

  // A missing key is a soft failure, not a transport error, and Touch must not
  // conjure the key into existence.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::TouchRequest request;
    cachelib::grpc::TouchResponse response;

    request.set_key("touch:missing");
    request.set_ttl_seconds(600);

    auto status = stub_->Touch(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.success());
    EXPECT_FALSE(response.message().empty());
  }

  EXPECT_EQ(DoGetTTL("touch:missing"), -2);
  EXPECT_FALSE(DoExists("touch:missing"));
}

TEST_F(ServiceContractTest, TouchWithZeroTtlRemovesTheExpiration) {
  const std::string key = "touch:clear";

  MustSet(key, "v", 300);
  EXPECT_GT(DoGetTTL(key), 290);

  ::grpc::ClientContext context;
  cachelib::grpc::TouchRequest request;
  cachelib::grpc::TouchResponse response;

  request.set_key(key);
  request.set_ttl_seconds(0);

  auto status = stub_->Touch(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success()) << response.message();

  EXPECT_EQ(DoGetTTL(key), -1);

  auto get = DoGet(key);
  EXPECT_TRUE(get.found());
  EXPECT_EQ(get.value(), "v");
}

TEST_F(ServiceContractTest, FlushRemovesEveryItemAndReportsHowMany) {
  const int kInserted = 5;
  for (int i = 0; i < kInserted; ++i) {
    MustSet("flush:" + std::to_string(i), "value-" + std::to_string(i), 0);
  }

  for (int i = 0; i < kInserted; ++i) {
    EXPECT_TRUE(DoExists("flush:" + std::to_string(i)));
  }

  {
    ::grpc::ClientContext context;
    cachelib::grpc::FlushRequest request;
    cachelib::grpc::FlushResponse response;

    auto status = stub_->Flush(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
    EXPECT_EQ(response.items_removed(), kInserted);
  }

  for (int i = 0; i < kInserted; ++i) {
    auto get = DoGet("flush:" + std::to_string(i));
    EXPECT_FALSE(get.found()) << "flush:" << i;
    EXPECT_EQ(DoGetTTL("flush:" + std::to_string(i)), -2);
  }

  ::grpc::ClientContext context;
  cachelib::grpc::ScanRequest request;
  cachelib::grpc::ScanResponse response;

  request.set_pattern("*");

  auto status = stub_->Scan(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.keys_size(), 0);
  EXPECT_FALSE(response.has_more());

  // A second flush over an empty cache is a no-op, not an error.
  {
    ::grpc::ClientContext flushContext;
    cachelib::grpc::FlushRequest flushRequest;
    cachelib::grpc::FlushResponse flushResponse;

    auto flushStatus =
        stub_->Flush(&flushContext, flushRequest, &flushResponse);
    EXPECT_TRUE(flushStatus.ok()) << flushStatus.error_message();
    EXPECT_TRUE(flushResponse.success());
    EXPECT_EQ(flushResponse.items_removed(), 0);
  }
}

// =============================================================================
// 7. Unicode, control characters and embedded NULs in keys
// =============================================================================

TEST_F(ServiceContractTest, UnicodeAndControlCharacterKeysRoundTrip) {
  // All three are structurally valid UTF-8 (U+0000 included), which matters
  // because `key` is a proto3 `string`, not `bytes`.
  const std::vector<std::string> keys = {
      "unicode:caf\xC3\xA9:\xE6\x97\xA5\xE6\x9C\xAC:\xF0\x9F\x94\x91",
      "control:line-one\nline-two\ttabbed",
      std::string("nul:a\0b", 7),
  };

  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string value = "value-" + std::to_string(i);

    {
      ::grpc::ClientContext context;
      cachelib::grpc::SetRequest request;
      cachelib::grpc::SetResponse response;

      request.set_key(keys[i]);
      request.set_value(value);

      auto status = stub_->Set(&context, request, &response);
      EXPECT_TRUE(status.ok()) << "index " << i << ": "
                               << status.error_message();
      EXPECT_TRUE(response.success()) << "index " << i << ": "
                                      << response.message();
    }

    auto get = DoGet(keys[i]);
    EXPECT_TRUE(get.found()) << "index " << i;
    EXPECT_EQ(get.value(), value) << "index " << i;
    EXPECT_TRUE(DoExists(keys[i])) << "index " << i;
  }

  // The embedded NUL is part of the key, not a terminator: neither the
  // truncated nor the NUL-stripped spelling resolves to the same entry.
  EXPECT_FALSE(DoExists("nul:a"));
  EXPECT_FALSE(DoExists("nul:ab"));

  // Deleting by the exact byte sequence works.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::DeleteRequest request;
    cachelib::grpc::DeleteResponse response;

    request.set_key(std::string("nul:a\0b", 7));

    auto status = stub_->Delete(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.key_existed());
  }

  EXPECT_FALSE(DoExists(std::string("nul:a\0b", 7)));
}

// =============================================================================
// Expiry, observed through the wire contract
// =============================================================================

TEST_F(ServiceContractTest, ExpiredKeysDisappearFromEveryReadPath) {
  const std::string key = "expire:soon";

  MustSet(key, "gone-shortly", 2);
  EXPECT_TRUE(DoExists(key));

  // Expiry is second-granularity and the comparison is strictly
  // "expiry < now", so a 2s TTL needs the clock to reach creation+3. Sleeping
  // 3s clears that unconditionally and still leaves a two-second cushion for
  // the liveness check above on a loaded CI machine.
  std::this_thread::sleep_for(std::chrono::milliseconds(3000));

  auto get = DoGet(key);
  EXPECT_FALSE(get.found());
  EXPECT_TRUE(get.value().empty());
  EXPECT_FALSE(DoExists(key));
  EXPECT_EQ(DoGetTTL(key), -2);

  {
    ::grpc::ClientContext context;
    cachelib::grpc::MultiGetRequest request;
    cachelib::grpc::MultiGetResponse response;

    request.add_keys(key);

    auto status = stub_->MultiGet(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    ASSERT_EQ(response.results_size(), 1);
    EXPECT_FALSE(response.results(0).found());
  }

  // Scan walks raw items rather than the lookup path, so an expired-but-not-
  // yet-reclaimed key may still be listed. When it is, it must carry the
  // documented "0 = expired" sentinel rather than a positive TTL.
  ::grpc::ClientContext context;
  cachelib::grpc::ScanRequest request;
  cachelib::grpc::ScanResponse response;

  request.set_pattern("expire:*");
  request.set_include_details(true);

  auto status = stub_->Scan(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  for (int i = 0; i < response.key_details_size(); ++i) {
    if (response.key_details(i).key() == key) {
      EXPECT_EQ(response.key_details(i).ttl_remaining(), 0);
    }
  }
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib
