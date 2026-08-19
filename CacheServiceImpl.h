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

#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "CacheManager.h"
#include "cache.grpc.pb.h"

namespace cachelib {
namespace grpc_server {

// gRPC service implementation for CacheService
class CacheServiceImpl final : public cachelib::grpc::CacheService::Service {
 public:
  explicit CacheServiceImpl(std::shared_ptr<CacheManager> cacheManager);

  // ---------------------------------------------------------------------------
  // Basic Operations
  // ---------------------------------------------------------------------------

  // Get retrieves a value from the cache by key
  ::grpc::Status Get(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::GetRequest* request,
      ::cachelib::grpc::GetResponse* response) override;

  // Set stores a key-value pair in the cache with optional TTL
  ::grpc::Status Set(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::SetRequest* request,
      ::cachelib::grpc::SetResponse* response) override;

  // Delete removes a key from the cache
  ::grpc::Status Delete(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::DeleteRequest* request,
      ::cachelib::grpc::DeleteResponse* response) override;

  // Exists checks if a key exists in the cache
  ::grpc::Status Exists(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::ExistsRequest* request,
      ::cachelib::grpc::ExistsResponse* response) override;

  // ---------------------------------------------------------------------------
  // Batch Operations
  // ---------------------------------------------------------------------------

  // MultiGet retrieves multiple values in a single call
  ::grpc::Status MultiGet(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::MultiGetRequest* request,
      ::cachelib::grpc::MultiGetResponse* response) override;

  // MultiSet stores multiple key-value pairs in a single call
  ::grpc::Status MultiSet(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::MultiSetRequest* request,
      ::cachelib::grpc::MultiSetResponse* response) override;

  // MultiDelete removes multiple keys in a single call
  ::grpc::Status MultiDelete(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::MultiDeleteRequest* request,
      ::cachelib::grpc::MultiDeleteResponse* response) override;

  // ---------------------------------------------------------------------------
  // Atomic Operations
  // ---------------------------------------------------------------------------

  // SetNX sets a value only if the key doesn't exist
  ::grpc::Status SetNX(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::SetNXRequest* request,
      ::cachelib::grpc::SetNXResponse* response) override;

  // Increment atomically increments a numeric value
  ::grpc::Status Increment(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::IncrementRequest* request,
      ::cachelib::grpc::IncrementResponse* response) override;

  // Decrement atomically decrements a numeric value
  ::grpc::Status Decrement(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::DecrementRequest* request,
      ::cachelib::grpc::DecrementResponse* response) override;

  // Incr atomically increments a counter and stamps TTL on first creation
  ::grpc::Status Incr(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::IncrRequest* request,
      ::cachelib::grpc::IncrResponse* response) override;

  // CompareAndSwap atomically updates a value if it matches expected
  ::grpc::Status CompareAndSwap(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::CompareAndSwapRequest* request,
      ::cachelib::grpc::CompareAndSwapResponse* response) override;

  // ---------------------------------------------------------------------------
  // TTL Operations
  // ---------------------------------------------------------------------------

  // GetTTL returns the remaining TTL for a key
  ::grpc::Status GetTTL(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::GetTTLRequest* request,
      ::cachelib::grpc::GetTTLResponse* response) override;

  // Touch updates the TTL without changing the value
  ::grpc::Status Touch(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::TouchRequest* request,
      ::cachelib::grpc::TouchResponse* response) override;

  // ---------------------------------------------------------------------------
  // Key Scanning
  // ---------------------------------------------------------------------------

  // Scan iterates over keys matching a pattern
  ::grpc::Status Scan(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::ScanRequest* request,
      ::cachelib::grpc::ScanResponse* response) override;

  // ---------------------------------------------------------------------------
  // Streaming Operations
  // ---------------------------------------------------------------------------

  // Pipeline enables bidirectional streaming for batching operations
  ::grpc::Status Pipeline(
      ::grpc::ServerContext* context,
      ::grpc::ServerReaderWriter<
          ::cachelib::grpc::PipelineResponse,
          ::cachelib::grpc::PipelineRequest>* stream) override;

  // ---------------------------------------------------------------------------
  // Administration
  // ---------------------------------------------------------------------------

  // Stats returns cache statistics
  ::grpc::Status Stats(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::StatsRequest* request,
      ::cachelib::grpc::StatsResponse* response) override;

  // Ping checks if the server is alive
  ::grpc::Status Ping(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::PingRequest* request,
      ::cachelib::grpc::PingResponse* response) override;

  // Flush removes all keys from the cache
  ::grpc::Status Flush(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::FlushRequest* request,
      ::cachelib::grpc::FlushResponse* response) override;

 private:
  std::shared_ptr<CacheManager> cacheManager_;
};

}  // namespace grpc_server
}  // namespace cachelib
