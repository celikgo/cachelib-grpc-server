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

// Tests for the bidirectional Pipeline stream: response correlation, in-band
// error reporting, and what happens to the server when a stream ends badly
// (client cancel, server shutdown with the stream still open).
//
// Compiled into cache_service_test alongside CacheServiceTest.cc, so this file
// defines no main() and uses fixture/suite names that do not collide.

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <future>
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

namespace {

using PipelineStream = ::grpc::ClientReaderWriter<
    ::cachelib::grpc::PipelineRequest,
    ::cachelib::grpc::PipelineResponse>;

cachelib::grpc::PipelineRequest makeSet(
    uint64_t sequenceId,
    const std::string& key,
    const std::string& value,
    int64_t ttlSeconds = 0) {
  cachelib::grpc::PipelineRequest request;
  request.set_sequence_id(sequenceId);
  auto* op = request.mutable_set();
  op->set_key(key);
  op->set_value(value);
  op->set_ttl_seconds(ttlSeconds);
  return request;
}

cachelib::grpc::PipelineRequest makeGet(
    uint64_t sequenceId, const std::string& key) {
  cachelib::grpc::PipelineRequest request;
  request.set_sequence_id(sequenceId);
  request.mutable_get()->set_key(key);
  return request;
}

cachelib::grpc::PipelineRequest makeDelete(
    uint64_t sequenceId, const std::string& key) {
  cachelib::grpc::PipelineRequest request;
  request.set_sequence_id(sequenceId);
  request.mutable_delete_()->set_key(key);
  return request;
}

cachelib::grpc::PipelineRequest makeExists(
    uint64_t sequenceId, const std::string& key) {
  cachelib::grpc::PipelineRequest request;
  request.set_sequence_id(sequenceId);
  request.mutable_exists()->set_key(key);
  return request;
}

// Reads responses until the server half-closes the stream.
void drainResponses(
    PipelineStream* stream,
    std::vector<cachelib::grpc::PipelineResponse>* out) {
  cachelib::grpc::PipelineResponse response;
  while (stream->Read(&response)) {
    out->push_back(response);
  }
}

}  // namespace

class PipelineStreamTest : public ::testing::Test {
 protected:
  void SetUp() override {
    CacheConfig config;
    config.cacheName = "pipeline-stream-test-cache";
    config.cacheSize = 100 * 1024 * 1024;  // 100MB
    config.enableNvm = false;

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

  // Unary Get, used to verify that pipelined writes really landed in the
  // cache after the stream is gone.
  bool unaryGet(const std::string& key, std::string* value) {
    ::grpc::ClientContext context;
    cachelib::grpc::GetRequest request;
    cachelib::grpc::GetResponse response;

    request.set_key(key);

    auto status = stub_->Get(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    if (value != nullptr && response.found()) {
      *value = response.value();
    }
    return response.found();
  }

  // Unary Set, used to prove the server still serves traffic after a stream
  // was torn down abruptly.
  bool unarySet(const std::string& key, const std::string& value) {
    ::grpc::ClientContext context;
    cachelib::grpc::SetRequest request;
    cachelib::grpc::SetResponse response;

    request.set_key(key);
    request.set_value(value);

    auto status = stub_->Set(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response.success();
  }

  std::shared_ptr<CacheManager> cacheManager_;
  std::unique_ptr<CacheServiceImpl> service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<cachelib::grpc::CacheService::Stub> stub_;
};

// -----------------------------------------------------------------------------
// Round-trip correlation
// -----------------------------------------------------------------------------

TEST_F(PipelineStreamTest, MixedOperationsCorrelateBySequenceId) {
  constexpr int kOps = 100;  // 25 groups of Set / Get / Exists / Delete

  ::grpc::ClientContext context;
  auto stream = stub_->Pipeline(&context);
  ASSERT_NE(stream, nullptr);

  // The handler writes each response before it reads the next request, so
  // whether "write everything, then drain" works at all depends on how much
  // the transport buffers. A dedicated reader thread makes the correlation
  // assertions independent of that, and cannot deadlock.
  std::vector<cachelib::grpc::PipelineResponse> responses;
  PipelineStream* raw = stream.get();
  std::thread reader([raw, &responses]() { drainResponses(raw, &responses); });

  // No ASSERT_* between here and reader.join(): an early return would leave a
  // joinable thread behind and terminate the process.
  int writeFailures = 0;
  for (int i = 0; i < kOps; ++i) {
    const uint64_t sequenceId = static_cast<uint64_t>(i) + 1;
    const int group = i - (i % 4);
    const std::string key = "pipe:" + std::to_string(group);
    const std::string value = "value-" + std::to_string(group);

    bool written = false;
    switch (i % 4) {
      case 0:
        written = stream->Write(makeSet(sequenceId, key, value));
        break;
      case 1:
        written = stream->Write(makeGet(sequenceId, key));
        break;
      case 2:
        written = stream->Write(makeExists(sequenceId, key));
        break;
      default:
        written = stream->Write(makeDelete(sequenceId, key));
        break;
    }
    if (!written) {
      ++writeFailures;
      break;
    }
  }

  const bool writesDone = stream->WritesDone();
  reader.join();
  auto status = stream->Finish();

  EXPECT_EQ(writeFailures, 0);
  EXPECT_TRUE(writesDone);
  EXPECT_TRUE(status.ok()) << status.error_message();

  // Exactly one response per request, in the order the requests were sent.
  ASSERT_EQ(responses.size(), static_cast<size_t>(kOps));
  for (int i = 0; i < kOps; ++i) {
    const auto& response = responses[static_cast<size_t>(i)];
    const int group = i - (i % 4);
    const std::string value = "value-" + std::to_string(group);

    EXPECT_EQ(response.sequence_id(), static_cast<uint64_t>(i) + 1)
        << "response " << i << " carries the wrong sequence_id";
    EXPECT_TRUE(response.error().empty())
        << "response " << i << " error=" << response.error();

    switch (i % 4) {
      case 0:
        ASSERT_EQ(response.result_case(), cachelib::grpc::PipelineResponse::kSet)
            << "response " << i;
        EXPECT_TRUE(response.set().success());
        EXPECT_EQ(response.set().size_bytes(),
                  static_cast<int64_t>(value.size()));
        break;
      case 1:
        ASSERT_EQ(response.result_case(), cachelib::grpc::PipelineResponse::kGet)
            << "response " << i;
        EXPECT_TRUE(response.get().found());
        EXPECT_EQ(response.get().value(), value);
        break;
      case 2:
        ASSERT_EQ(response.result_case(),
                  cachelib::grpc::PipelineResponse::kExists)
            << "response " << i;
        EXPECT_TRUE(response.exists().exists());
        break;
      default:
        ASSERT_EQ(response.result_case(),
                  cachelib::grpc::PipelineResponse::kDelete)
            << "response " << i;
        EXPECT_TRUE(response.delete_().success());
        EXPECT_TRUE(response.delete_().key_existed());
        break;
    }
  }

  // The last op of every group deleted the group's key.
  EXPECT_FALSE(unaryGet("pipe:96", nullptr));
}

TEST_F(PipelineStreamTest, InterleavedWriteAndReadStaysCorrelated) {
  constexpr int kRounds = 20;

  ::grpc::ClientContext context;
  auto stream = stub_->Pipeline(&context);
  ASSERT_NE(stream, nullptr);

  for (int i = 0; i < kRounds; ++i) {
    const uint64_t setSequenceId = static_cast<uint64_t>(i) * 2 + 1;
    const uint64_t getSequenceId = setSequenceId + 1;
    const std::string key = "interleaved:" + std::to_string(i);
    const std::string value = "payload-" + std::to_string(i);

    ASSERT_TRUE(stream->Write(makeSet(setSequenceId, key, value)))
        << "round " << i;
    cachelib::grpc::PipelineResponse setResponse;
    ASSERT_TRUE(stream->Read(&setResponse)) << "round " << i;
    EXPECT_EQ(setResponse.sequence_id(), setSequenceId);
    ASSERT_EQ(setResponse.result_case(),
              cachelib::grpc::PipelineResponse::kSet);
    EXPECT_TRUE(setResponse.set().success());
    EXPECT_TRUE(setResponse.error().empty());

    ASSERT_TRUE(stream->Write(makeGet(getSequenceId, key))) << "round " << i;
    cachelib::grpc::PipelineResponse getResponse;
    ASSERT_TRUE(stream->Read(&getResponse)) << "round " << i;
    EXPECT_EQ(getResponse.sequence_id(), getSequenceId);
    ASSERT_EQ(getResponse.result_case(),
              cachelib::grpc::PipelineResponse::kGet);
    EXPECT_TRUE(getResponse.get().found());
    EXPECT_EQ(getResponse.get().value(), value);
  }

  EXPECT_TRUE(stream->WritesDone());

  // Every request produced exactly one response: nothing is left to read.
  cachelib::grpc::PipelineResponse extra;
  EXPECT_FALSE(stream->Read(&extra));

  auto status = stream->Finish();
  EXPECT_TRUE(status.ok()) << status.error_message();
}

TEST_F(PipelineStreamTest, GetCarriesTtlRemainingThroughTheStream) {
  const std::string key = "pipeline-ttl:key";
  const std::string value = "ttl-value";

  ::grpc::ClientContext context;
  auto stream = stub_->Pipeline(&context);
  ASSERT_NE(stream, nullptr);

  ASSERT_TRUE(stream->Write(makeSet(1, key, value, 120)));
  cachelib::grpc::PipelineResponse setResponse;
  ASSERT_TRUE(stream->Read(&setResponse));
  ASSERT_EQ(setResponse.result_case(), cachelib::grpc::PipelineResponse::kSet);
  EXPECT_TRUE(setResponse.set().success());

  ASSERT_TRUE(stream->Write(makeGet(2, key)));
  cachelib::grpc::PipelineResponse getResponse;
  ASSERT_TRUE(stream->Read(&getResponse));
  EXPECT_EQ(getResponse.sequence_id(), 2u);
  ASSERT_EQ(getResponse.result_case(), cachelib::grpc::PipelineResponse::kGet);
  EXPECT_TRUE(getResponse.get().found());
  EXPECT_EQ(getResponse.get().value(), value);
  EXPECT_GT(getResponse.get().ttl_remaining(), 100);
  EXPECT_LE(getResponse.get().ttl_remaining(), 120);

  EXPECT_TRUE(stream->WritesDone());
  auto status = stream->Finish();
  EXPECT_TRUE(status.ok()) << status.error_message();
}

// -----------------------------------------------------------------------------
// Malformed and rejected operations
// -----------------------------------------------------------------------------

TEST_F(PipelineStreamTest, UnknownOperationIsReportedAndStreamSurvives) {
  const std::string firstKey = "unknown-op:first";
  const std::string thirdKey = "unknown-op:third";

  {
    ::grpc::ClientContext context;
    auto stream = stub_->Pipeline(&context);
    ASSERT_NE(stream, nullptr);

    ASSERT_TRUE(stream->Write(makeSet(1, firstKey, "first")));
    cachelib::grpc::PipelineResponse firstResponse;
    ASSERT_TRUE(stream->Read(&firstResponse));
    EXPECT_EQ(firstResponse.sequence_id(), 1u);
    ASSERT_EQ(firstResponse.result_case(),
              cachelib::grpc::PipelineResponse::kSet);
    EXPECT_TRUE(firstResponse.set().success());

    // A request with the operation oneof left unset.
    cachelib::grpc::PipelineRequest malformed;
    malformed.set_sequence_id(2);
    ASSERT_EQ(malformed.operation_case(),
              cachelib::grpc::PipelineRequest::OPERATION_NOT_SET);
    ASSERT_TRUE(stream->Write(malformed));

    cachelib::grpc::PipelineResponse malformedResponse;
    ASSERT_TRUE(stream->Read(&malformedResponse));
    EXPECT_EQ(malformedResponse.sequence_id(), 2u);
    EXPECT_EQ(malformedResponse.error(), "Unknown operation");
    EXPECT_EQ(malformedResponse.result_case(),
              cachelib::grpc::PipelineResponse::RESULT_NOT_SET);

    // The stream is still usable after the malformed message.
    ASSERT_TRUE(stream->Write(makeSet(3, thirdKey, "third")));
    cachelib::grpc::PipelineResponse thirdResponse;
    ASSERT_TRUE(stream->Read(&thirdResponse));
    EXPECT_EQ(thirdResponse.sequence_id(), 3u);
    ASSERT_EQ(thirdResponse.result_case(),
              cachelib::grpc::PipelineResponse::kSet);
    EXPECT_TRUE(thirdResponse.set().success());
    EXPECT_TRUE(thirdResponse.error().empty());

    EXPECT_TRUE(stream->WritesDone());
    auto status = stream->Finish();
    EXPECT_TRUE(status.ok()) << status.error_message();
  }

  // The Set that followed the malformed message really took effect.
  std::string value;
  EXPECT_TRUE(unaryGet(thirdKey, &value));
  EXPECT_EQ(value, "third");
  EXPECT_TRUE(unaryGet(firstKey, nullptr));
}

TEST_F(PipelineStreamTest, EmptyKeyIsRejectedInBandAndStreamSurvives) {
  const std::string goodKey = "empty-key:after";

  {
    ::grpc::ClientContext context;
    auto stream = stub_->Pipeline(&context);
    ASSERT_NE(stream, nullptr);

    // Empty key inside a Get. The handler breaks out of the switch before it
    // touches the result oneof, so only `error` is populated.
    ASSERT_TRUE(stream->Write(makeGet(11, "")));
    cachelib::grpc::PipelineResponse getResponse;
    ASSERT_TRUE(stream->Read(&getResponse));
    EXPECT_EQ(getResponse.sequence_id(), 11u);
    EXPECT_EQ(getResponse.error(), "Key cannot be empty");
    EXPECT_EQ(getResponse.result_case(),
              cachelib::grpc::PipelineResponse::RESULT_NOT_SET);

    // Empty key inside a Set: same shape.
    ASSERT_TRUE(stream->Write(makeSet(12, "", "orphan-value")));
    cachelib::grpc::PipelineResponse setResponse;
    ASSERT_TRUE(stream->Read(&setResponse));
    EXPECT_EQ(setResponse.sequence_id(), 12u);
    EXPECT_EQ(setResponse.error(), "Key cannot be empty");
    EXPECT_EQ(setResponse.result_case(),
              cachelib::grpc::PipelineResponse::RESULT_NOT_SET);

    // Empty key inside a Delete and an Exists: same shape again.
    ASSERT_TRUE(stream->Write(makeDelete(13, "")));
    cachelib::grpc::PipelineResponse deleteResponse;
    ASSERT_TRUE(stream->Read(&deleteResponse));
    EXPECT_EQ(deleteResponse.sequence_id(), 13u);
    EXPECT_EQ(deleteResponse.error(), "Key cannot be empty");
    EXPECT_EQ(deleteResponse.result_case(),
              cachelib::grpc::PipelineResponse::RESULT_NOT_SET);

    ASSERT_TRUE(stream->Write(makeExists(14, "")));
    cachelib::grpc::PipelineResponse existsResponse;
    ASSERT_TRUE(stream->Read(&existsResponse));
    EXPECT_EQ(existsResponse.sequence_id(), 14u);
    EXPECT_EQ(existsResponse.error(), "Key cannot be empty");
    EXPECT_EQ(existsResponse.result_case(),
              cachelib::grpc::PipelineResponse::RESULT_NOT_SET);

    // The stream survives all four rejections.
    ASSERT_TRUE(stream->Write(makeSet(15, goodKey, "after")));
    cachelib::grpc::PipelineResponse goodResponse;
    ASSERT_TRUE(stream->Read(&goodResponse));
    EXPECT_EQ(goodResponse.sequence_id(), 15u);
    ASSERT_EQ(goodResponse.result_case(),
              cachelib::grpc::PipelineResponse::kSet);
    EXPECT_TRUE(goodResponse.set().success());
    EXPECT_TRUE(goodResponse.error().empty());

    EXPECT_TRUE(stream->WritesDone());
    auto status = stream->Finish();
    EXPECT_TRUE(status.ok()) << status.error_message();
  }

  std::string value;
  EXPECT_TRUE(unaryGet(goodKey, &value));
  EXPECT_EQ(value, "after");
}

TEST_F(PipelineStreamTest, FailedSetIsReportedWithoutBreakingStream) {
  // CacheLib is configured with allowLargeKeys=false, so a key longer than
  // KAllocation::kKeyMaxLenSmall (255) makes allocate() throw and
  // CacheManager::set return false.
  const std::string oversizedKey(300, 'k');
  const std::string goodKey = "oversized:after";

  {
    ::grpc::ClientContext context;
    auto stream = stub_->Pipeline(&context);
    ASSERT_NE(stream, nullptr);

    ASSERT_TRUE(stream->Write(makeSet(21, oversizedKey, "too-long-key")));
    cachelib::grpc::PipelineResponse failedResponse;
    ASSERT_TRUE(stream->Read(&failedResponse));
    EXPECT_EQ(failedResponse.sequence_id(), 21u);
    // The failure is reported inside SetResponse, not in the oneof-level
    // error field. See the divergence note about PipelineResponse.error.
    ASSERT_EQ(failedResponse.result_case(),
              cachelib::grpc::PipelineResponse::kSet);
    EXPECT_FALSE(failedResponse.set().success());

    ASSERT_TRUE(stream->Write(makeSet(22, goodKey, "after")));
    cachelib::grpc::PipelineResponse goodResponse;
    ASSERT_TRUE(stream->Read(&goodResponse));
    EXPECT_EQ(goodResponse.sequence_id(), 22u);
    ASSERT_EQ(goodResponse.result_case(),
              cachelib::grpc::PipelineResponse::kSet);
    EXPECT_TRUE(goodResponse.set().success());

    EXPECT_TRUE(stream->WritesDone());
    auto status = stream->Finish();
    EXPECT_TRUE(status.ok()) << status.error_message();
  }

  EXPECT_FALSE(unaryGet(oversizedKey, nullptr));
  std::string value;
  EXPECT_TRUE(unaryGet(goodKey, &value));
  EXPECT_EQ(value, "after");
}

// -----------------------------------------------------------------------------
// Abrupt teardown
// -----------------------------------------------------------------------------

TEST_F(PipelineStreamTest, ClientCancelMidStreamLeavesServerHealthy) {
  constexpr int kIterations = 20;

  for (int i = 0; i < kIterations; ++i) {
    ::grpc::ClientContext context;
    auto stream = stub_->Pipeline(&context);
    ASSERT_NE(stream, nullptr) << "iteration " << i;

    const std::string key = "cancelled:" + std::to_string(i);

    ASSERT_TRUE(stream->Write(makeSet(1, key, "before-cancel")))
        << "iteration " << i;
    cachelib::grpc::PipelineResponse response;
    ASSERT_TRUE(stream->Read(&response)) << "iteration " << i;
    EXPECT_EQ(response.sequence_id(), 1u);
    ASSERT_EQ(response.result_case(), cachelib::grpc::PipelineResponse::kSet);
    EXPECT_TRUE(response.set().success());

    // Leave a request in flight and walk away: no WritesDone, no drain, just
    // a cancel while the handler is mid-loop.
    EXPECT_TRUE(stream->Write(makeGet(2, key))) << "iteration " << i;
    context.TryCancel();

    auto status = stream->Finish();
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::CANCELLED)
        << "iteration " << i << " status=" << status.error_message();
  }

  // No leak, no crash: unary traffic still works.
  {
    ::grpc::ClientContext context;
    cachelib::grpc::PingRequest request;
    cachelib::grpc::PingResponse response;

    auto status = stub_->Ping(&context, request, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(response.message(), "PONG");
  }

  EXPECT_TRUE(unarySet("after-cancels", "still-alive"));
  std::string value;
  EXPECT_TRUE(unaryGet("after-cancels", &value));
  EXPECT_EQ(value, "still-alive");

  // ... and so does a brand new Pipeline stream.
  {
    ::grpc::ClientContext context;
    auto stream = stub_->Pipeline(&context);
    ASSERT_NE(stream, nullptr);

    ASSERT_TRUE(stream->Write(makeGet(99, "after-cancels")));
    cachelib::grpc::PipelineResponse response;
    ASSERT_TRUE(stream->Read(&response));
    EXPECT_EQ(response.sequence_id(), 99u);
    ASSERT_EQ(response.result_case(), cachelib::grpc::PipelineResponse::kGet);
    EXPECT_TRUE(response.get().found());
    EXPECT_EQ(response.get().value(), "still-alive");

    EXPECT_TRUE(stream->WritesDone());
    auto status = stream->Finish();
    EXPECT_TRUE(status.ok()) << status.error_message();
  }

  // Every cancelled stream still ran its Set through the cache.
  EXPECT_TRUE(unaryGet("cancelled:0", nullptr));
  EXPECT_TRUE(unaryGet("cancelled:19", nullptr));
}

TEST_F(PipelineStreamTest, ServerShutdownWithOpenStreamDoesNotHang) {
  // A second server over the same CacheManager, so shutting it down leaves the
  // fixture's server untouched. Everything the watchdog thread touches is held
  // by shared_ptr, so the objects outlive an abandoned thread on the failure
  // path.
  auto service = std::make_shared<CacheServiceImpl>(cacheManager_);

  ::grpc::ServerBuilder builder;
  builder.RegisterService(service.get());
  std::shared_ptr<::grpc::Server> server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);

  auto channel = server->InProcessChannel(::grpc::ChannelArguments());
  std::shared_ptr<cachelib::grpc::CacheService::Stub> stub =
      cachelib::grpc::CacheService::NewStub(channel);

  auto context = std::make_shared<::grpc::ClientContext>();
  std::shared_ptr<PipelineStream> stream = stub->Pipeline(context.get());
  ASSERT_NE(stream, nullptr);

  // Round-trip one operation so the handler is parked inside stream->Read()
  // with the stream still open, then shut the server down under it.
  ASSERT_TRUE(stream->Write(makeSet(1, "shutdown:key", "value")));
  cachelib::grpc::PipelineResponse response;
  ASSERT_TRUE(stream->Read(&response));
  EXPECT_EQ(response.sequence_id(), 1u);
  ASSERT_EQ(response.result_case(), cachelib::grpc::PipelineResponse::kSet);
  EXPECT_TRUE(response.set().success());

  auto done = std::make_shared<std::promise<::grpc::Status>>();
  auto future = done->get_future();

  std::thread worker([server, stub, stream, context, service, done]() {
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::seconds(2));
    done->set_value(stream->Finish());
    // stub, context and service are captured only to keep them alive if this
    // thread has to be abandoned.
    (void)stub;
    (void)context;
    (void)service;
  });

  // Hard bound: a regression that hangs must fail the test, not the CI job.
  if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
    worker.detach();
    FAIL() << "Shutdown() with an open Pipeline stream did not complete, or "
              "Finish() never returned, within 30s";
  }
  worker.join();

  // The exact status depends on whether the forced cancellation or the
  // handler's own return reached the client first, so the assertion that
  // matters is the bounded wait above: Finish() came back.
  future.get();

  // Deliberately NOT probing the shut-down server with another RPC.
  //
  // On a real listening socket a post-shutdown call returns UNAVAILABLE, but
  // over an in-process channel it dereferences a null transport and takes the
  // process down with SIGSEGV. That is a property of gRPC's in-process
  // transport, not of this server, and asserting it here would be testing
  // gRPC. What this test is for is the two facts above: Shutdown() returned
  // with a stream open, and Finish() came back -- neither hung.

  // The fixture's server is a separate instance and is still serving.
  {
    ::grpc::ClientContext pingContext;
    cachelib::grpc::PingRequest request;
    cachelib::grpc::PingResponse pingResponse;

    auto pingStatus = stub_->Ping(&pingContext, request, &pingResponse);
    EXPECT_TRUE(pingStatus.ok()) << pingStatus.error_message();
    EXPECT_EQ(pingResponse.message(), "PONG");
  }

  // The write that went through before shutdown is in the shared cache.
  std::string value;
  EXPECT_TRUE(unaryGet("shutdown:key", &value));
  EXPECT_EQ(value, "value");
}

// -----------------------------------------------------------------------------
// Volume and degenerate streams
// -----------------------------------------------------------------------------

TEST_F(PipelineStreamTest, ManySmallWritesAllCorrelate) {
  constexpr int kOps = 5000;

  ::grpc::ClientContext context;
  auto stream = stub_->Pipeline(&context);
  ASSERT_NE(stream, nullptr);

  std::vector<cachelib::grpc::PipelineResponse> responses;
  responses.reserve(static_cast<size_t>(kOps));
  PipelineStream* raw = stream.get();
  std::thread reader([raw, &responses]() { drainResponses(raw, &responses); });

  // No ASSERT_* while the reader thread is alive.
  int writeFailures = 0;
  for (int i = 0; i < kOps; ++i) {
    const std::string key = "bulk:" + std::to_string(i);
    const std::string value = "v" + std::to_string(i);
    if (!stream->Write(makeSet(static_cast<uint64_t>(i) + 1, key, value))) {
      ++writeFailures;
      break;
    }
  }

  const bool writesDone = stream->WritesDone();
  reader.join();
  auto status = stream->Finish();

  EXPECT_EQ(writeFailures, 0);
  EXPECT_TRUE(writesDone);
  EXPECT_TRUE(status.ok()) << status.error_message();

  ASSERT_EQ(responses.size(), static_cast<size_t>(kOps));
  for (int i = 0; i < kOps; ++i) {
    const auto& response = responses[static_cast<size_t>(i)];
    const std::string value = "v" + std::to_string(i);

    ASSERT_EQ(response.sequence_id(), static_cast<uint64_t>(i) + 1)
        << "response " << i << " is out of order";
    ASSERT_EQ(response.result_case(), cachelib::grpc::PipelineResponse::kSet)
        << "response " << i;
    EXPECT_TRUE(response.set().success()) << "response " << i;
    EXPECT_EQ(response.set().size_bytes(), static_cast<int64_t>(value.size()))
        << "response " << i;
  }

  // Spot-check that the bulk writes actually reached the cache.
  std::string value;
  EXPECT_TRUE(unaryGet("bulk:0", &value));
  EXPECT_EQ(value, "v0");
  EXPECT_TRUE(unaryGet("bulk:4999", &value));
  EXPECT_EQ(value, "v4999");
}

TEST_F(PipelineStreamTest, EmptyStreamFinishesOk) {
  ::grpc::ClientContext context;
  auto stream = stub_->Pipeline(&context);
  ASSERT_NE(stream, nullptr);

  EXPECT_TRUE(stream->WritesDone());

  cachelib::grpc::PipelineResponse response;
  EXPECT_FALSE(stream->Read(&response));

  auto status = stream->Finish();
  EXPECT_TRUE(status.ok()) << status.error_message();
}

}  // namespace test
}  // namespace grpc_server
}  // namespace cachelib
