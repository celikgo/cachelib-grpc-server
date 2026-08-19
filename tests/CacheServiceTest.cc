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
#include <memory>
#include <thread>

#include "../CacheManager.h"
#include "../CacheServiceImpl.h"
#include "cache.grpc.pb.h"

namespace cachelib {
namespace grpc_server {
namespace test {

class CacheServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create cache manager
    CacheConfig config;
    config.cacheName = "service-test-cache";
    config.cacheSize = 100 * 1024 * 1024;  // 100MB
    config.enableNvm = false;

    cacheManager_ = std::make_shared<CacheManager>(config);
    ASSERT_TRUE(cacheManager_->initialize());

    // Create service
    service_ = std::make_unique<CacheServiceImpl>(cacheManager_);

    // Build and start in-process server
    ::grpc::ServerBuilder builder;
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);

    // Create in-process channel
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

  std::shared_ptr<CacheManager> cacheManager_;
  std::unique_ptr<CacheServiceImpl> service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<cachelib::grpc::CacheService::Stub> stub_;
};

TEST_F(CacheServiceTest, Ping) {
  ::grpc::ClientContext context;
  cachelib::grpc::PingRequest request;
  cachelib::grpc::PingResponse response;

  auto status = stub_->Ping(&context, request, &response);

  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.message(), "PONG");
  EXPECT_GT(response.timestamp(), 0);
}

TEST_F(CacheServiceTest, SetAndGet) {
  const std::string key = "grpc-test-key";
  const std::string value = "grpc-test-value";

  // Set
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
  }

  // Get
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key(key);

    auto status = stub_->Get(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.found());
    EXPECT_EQ(response.value(), value);
  }
}

TEST_F(CacheServiceTest, GetNotFound) {
  ::grpc::ClientContext context;
  cachelib::grpc::GetRequest request;
  cachelib::grpc::GetResponse response;

  request.set_key("non-existent-key");

  auto status = stub_->Get(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_FALSE(response.found());
}

TEST_F(CacheServiceTest, SetWithTtl) {
  const std::string key = "ttl-test-key";
  const std::string value = "ttl-test-value";

  // Set with TTL
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);
    request.set_ttl_seconds(120);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
  }

  // Get and check TTL
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key(key);

    auto status = stub_->Get(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.found());
    EXPECT_GT(response.ttl_remaining(), 100);  // Should be around 120
    EXPECT_LE(response.ttl_remaining(), 120);
  }
}

TEST_F(CacheServiceTest, Delete) {
  const std::string key = "delete-test-key";
  const std::string value = "delete-test-value";

  // Set first
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);

    stub_->Set(&context, request, &response);
  }

  // Delete
  {
    ::grpc::ClientContext context;
    cachelib::grpc::DeleteRequest request;
    cachelib::grpc::DeleteResponse response;

    request.set_key(key);

    auto status = stub_->Delete(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
    EXPECT_TRUE(response.key_existed());
  }

  // Verify deleted
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key(key);

    auto status = stub_->Get(&context, request, &response);
    EXPECT_TRUE(status.ok());
    EXPECT_FALSE(response.found());
  }
}

TEST_F(CacheServiceTest, DeleteNonExistent) {
  ::grpc::ClientContext context;
  cachelib::grpc::DeleteRequest request;
  cachelib::grpc::DeleteResponse response;

  request.set_key("never-existed-key");

  auto status = stub_->Delete(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_FALSE(response.key_existed());
}

TEST_F(CacheServiceTest, Exists) {
  const std::string key = "exists-test-key";
  const std::string value = "exists-test-value";

  // Check non-existent
  {
    ::grpc::ClientContext context;
    cachelib::grpc::ExistsRequest request;
    cachelib::grpc::ExistsResponse response;

    request.set_key(key);

    auto status = stub_->Exists(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_FALSE(response.exists());
  }

  // Set
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);

    stub_->Set(&context, request, &response);
  }

  // Check exists
  {
    ::grpc::ClientContext context;
    cachelib::grpc::ExistsRequest request;
    cachelib::grpc::ExistsResponse response;

    request.set_key(key);

    auto status = stub_->Exists(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.exists());
  }
}

TEST_F(CacheServiceTest, MultiGet) {
  // Set some keys
  for (int i = 0; i < 5; ++i) {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key("multi-" + std::to_string(i));
    request.set_value("value-" + std::to_string(i));

    stub_->Set(&context, request, &response);
  }

  // MultiGet
  ::grpc::ClientContext context;
  cachelib::grpc::MultiGetRequest request;
  cachelib::grpc::MultiGetResponse response;

  request.add_keys("multi-0");
  request.add_keys("multi-2");
  request.add_keys("multi-4");
  request.add_keys("non-existent");

  auto status = stub_->MultiGet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.results_size(), 4);

  EXPECT_TRUE(response.results(0).found());
  EXPECT_EQ(response.results(0).value(), "value-0");

  EXPECT_TRUE(response.results(1).found());
  EXPECT_EQ(response.results(1).value(), "value-2");

  EXPECT_TRUE(response.results(2).found());
  EXPECT_EQ(response.results(2).value(), "value-4");

  EXPECT_FALSE(response.results(3).found());
}

TEST_F(CacheServiceTest, MultiSet) {
  ::grpc::ClientContext context;
  cachelib::grpc::MultiSetRequest request;
  cachelib::grpc::MultiSetResponse response;

  for (int i = 0; i < 10; ++i) {
    auto* item = request.add_items();
    item->set_key("batch-" + std::to_string(i));
    item->set_value("batch-value-" + std::to_string(i));
    item->set_ttl_seconds(300);
  }

  auto status = stub_->MultiSet(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.succeeded_count(), 10);
  EXPECT_EQ(response.failed_count(), 0);

  // Verify some values
  {
    ::grpc::ClientContext ctx;
    cachelib::grpc::GetRequest req;
    cachelib::grpc::GetResponse resp;

    req.set_key("batch-5");
    stub_->Get(&ctx, req, &resp);

    EXPECT_TRUE(resp.found());
    EXPECT_EQ(resp.value(), "batch-value-5");
  }
}

TEST_F(CacheServiceTest, Stats) {
  // Perform some operations
  for (int i = 0; i < 10; ++i) {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key("stats-key-" + std::to_string(i));
    request.set_value("stats-value-" + std::to_string(i));

    stub_->Set(&context, request, &response);
  }

  for (int i = 0; i < 5; ++i) {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key("stats-key-" + std::to_string(i));
    stub_->Get(&context, request, &response);
  }

  // Get stats
  ::grpc::ClientContext context;
  cachelib::grpc::StatsRequest request;
  cachelib::grpc::StatsResponse response;

  request.set_detailed(true);

  auto status = stub_->Stats(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();

  EXPECT_GT(response.total_size(), 0);
  EXPECT_GE(response.set_count(), 10);
  EXPECT_GE(response.get_count(), 5);
  EXPECT_GE(response.hit_count(), 5);
  EXPECT_GT(response.uptime_seconds(), 0);
}

TEST_F(CacheServiceTest, SetReturnsSizeBytes) {
  const std::string key = "size-test-key";
  const std::string value = "hello world";  // 11 bytes

  ::grpc::ClientContext context;
  cachelib::grpc::SetRequest request;
  cachelib::grpc::SetResponse response;

  request.set_key(key);
  request.set_value(value);

  auto status = stub_->Set(&context, request, &response);
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.size_bytes(), 11);
}

TEST_F(CacheServiceTest, ScanWithDetails) {
  // Set some keys with known prefixes
  for (int i = 0; i < 3; ++i) {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key("detail:" + std::to_string(i));
    request.set_value(std::string(10 + i, 'x'));
    request.set_ttl_seconds(300);

    stub_->Set(&context, request, &response);
  }

  // Scan without details
  {
    ::grpc::ClientContext context;
    cachelib::grpc::ScanRequest request;
    cachelib::grpc::ScanResponse response;

    request.set_pattern("detail:*");
    request.set_include_details(false);

    auto status = stub_->Scan(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.keys_size(), 3);
    EXPECT_EQ(response.key_details_size(), 0);
  }

  // Scan with details
  {
    ::grpc::ClientContext context;
    cachelib::grpc::ScanRequest request;
    cachelib::grpc::ScanResponse response;

    request.set_pattern("detail:*");
    request.set_include_details(true);

    auto status = stub_->Scan(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.keys_size(), 3);
    EXPECT_EQ(response.key_details_size(), 3);

    for (int i = 0; i < response.key_details_size(); ++i) {
      const auto& info = response.key_details(i);
      EXPECT_FALSE(info.key().empty());
      EXPECT_GT(info.size_bytes(), 0);
      EXPECT_GT(info.ttl_remaining(), 250);
      EXPECT_LE(info.ttl_remaining(), 300);
    }
  }
}

TEST_F(CacheServiceTest, EmptyKeyError) {
  ::grpc::ClientContext context;
  cachelib::grpc::SetRequest request;
  cachelib::grpc::SetResponse response;

  request.set_key("");  // Empty key
  request.set_value("some-value");

  auto status = stub_->Set(&context, request, &response);
  EXPECT_EQ(status.error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(CacheServiceTest, BinaryData) {
  const std::string key = "binary-data-key";
  std::string value;
  value.resize(256);
  for (int i = 0; i < 256; ++i) {
    value[i] = static_cast<char>(i);
  }

  // Set binary data
  {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.success());
  }

  // Get and verify
  {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key(key);

    auto status = stub_->Get(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(response.found());
    EXPECT_EQ(response.value(), value);
  }
}

TEST_F(CacheServiceTest, ConcurrentRequests) {
  const int numThreads = 10;
  const int numOpsPerThread = 50;
  std::vector<std::thread> threads;

  for (int t = 0; t < numThreads; ++t) {
    threads.emplace_back([this, t, numOpsPerThread]() {
      for (int i = 0; i < numOpsPerThread; ++i) {
        std::string key = "concurrent-" + std::to_string(t) + "-" + std::to_string(i);
        std::string value = "value-" + std::to_string(i);

        // Set
        {
          ::grpc::ClientContext context;
          cachelib::grpc::SetRequest request;
          cachelib::grpc::SetResponse response;

          request.set_key(key);
          request.set_value(value);

          auto status = stub_->Set(&context, request, &response);
          EXPECT_TRUE(status.ok());
          EXPECT_TRUE(response.success());
        }

        // Get
        {
          ::grpc::ClientContext context;
          cachelib::grpc::GetRequest request;
          cachelib::grpc::GetResponse response;

          request.set_key(key);

          auto status = stub_->Get(&context, request, &response);
          EXPECT_TRUE(status.ok());
          EXPECT_TRUE(response.found());
          EXPECT_EQ(response.value(), value);
        }
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Verify with stats
  ::grpc::ClientContext context;
  cachelib::grpc::StatsRequest request;
  cachelib::grpc::StatsResponse response;

  stub_->Stats(&context, request, &response);

  EXPECT_GE(response.set_count(), numThreads * numOpsPerThread);
  EXPECT_GE(response.get_count(), numThreads * numOpsPerThread);
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
