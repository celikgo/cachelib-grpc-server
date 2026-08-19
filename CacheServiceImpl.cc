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

#include "CacheServiceImpl.h"

#include <chrono>

#include <folly/logging/xlog.h>

namespace cachelib {
namespace grpc_server {

CacheServiceImpl::CacheServiceImpl(std::shared_ptr<CacheManager> cacheManager)
    : cacheManager_(std::move(cacheManager)) {
  XLOG(INFO) << "CacheServiceImpl created";
}

// =============================================================================
// Basic Operations
// =============================================================================

::grpc::Status CacheServiceImpl::Get(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::GetRequest* request,
    ::cachelib::grpc::GetResponse* response) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  auto result = cacheManager_->get(request->key());

  response->set_found(result.found);
  if (result.found) {
    response->set_value(result.value);
    response->set_ttl_remaining(result.ttlRemaining);
  }

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Set(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::SetRequest* request,
    ::cachelib::grpc::SetResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_success(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  uint32_t ttlSeconds = 0;
  if (request->ttl_seconds() > 0) {
    ttlSeconds = static_cast<uint32_t>(request->ttl_seconds());
  }

  bool success = cacheManager_->set(
      request->key(),
      request->value(),
      ttlSeconds);

  response->set_success(success);
  if (success) {
    response->set_message("OK");
    response->set_size_bytes(static_cast<int64_t>(request->value().size()));
  } else {
    response->set_message("Failed to set value");
  }

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Delete(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::DeleteRequest* request,
    ::cachelib::grpc::DeleteResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_key_existed(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_success(false);
    response->set_key_existed(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  bool existed = cacheManager_->remove(request->key());

  response->set_success(true);
  response->set_key_existed(existed);
  response->set_message(existed ? "Key deleted" : "Key not found");

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Exists(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::ExistsRequest* request,
    ::cachelib::grpc::ExistsResponse* response) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  response->set_exists(cacheManager_->exists(request->key()));

  return ::grpc::Status::OK;
}

// =============================================================================
// Batch Operations
// =============================================================================

::grpc::Status CacheServiceImpl::MultiGet(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::MultiGetRequest* request,
    ::cachelib::grpc::MultiGetResponse* response) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  std::vector<std::string> keys;
  keys.reserve(request->keys_size());
  for (const auto& key : request->keys()) {
    keys.push_back(key);
  }

  auto results = cacheManager_->multiGet(keys);

  for (size_t i = 0; i < results.size(); ++i) {
    auto* kv = response->add_results();
    kv->set_key(keys[i]);
    kv->set_found(results[i].found);
    if (results[i].found) {
      kv->set_value(results[i].value);
      kv->set_ttl_remaining(results[i].ttlRemaining);
    }
  }

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::MultiSet(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::MultiSetRequest* request,
    ::cachelib::grpc::MultiSetResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  std::vector<std::tuple<std::string, std::string, uint32_t>> items;
  items.reserve(request->items_size());

  for (const auto& item : request->items()) {
    if (item.key().empty()) {
      continue;
    }
    uint32_t ttl = item.ttl_seconds() > 0 ? static_cast<uint32_t>(item.ttl_seconds()) : 0;
    items.emplace_back(item.key(), item.value(), ttl);
  }

  auto failedKeys = cacheManager_->multiSet(items);

  int32_t succeeded = static_cast<int32_t>(items.size() - failedKeys.size());
  int32_t failed = static_cast<int32_t>(failedKeys.size());

  response->set_success(failed == 0);
  response->set_succeeded_count(succeeded);
  response->set_failed_count(failed);

  for (const auto& key : failedKeys) {
    response->add_failed_keys(key);
  }

  if (failed == 0) {
    response->set_message("All items set successfully");
  } else {
    response->set_message(
        "Some items failed: " + std::to_string(failed) + " failures");
  }

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::MultiDelete(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::MultiDeleteRequest* request,
    ::cachelib::grpc::MultiDeleteResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  std::vector<std::string> keys;
  keys.reserve(request->keys_size());
  for (const auto& key : request->keys()) {
    keys.push_back(key);
  }

  int32_t deletedCount = cacheManager_->multiDelete(keys);
  int32_t notFoundCount = static_cast<int32_t>(keys.size()) - deletedCount;

  response->set_success(true);
  response->set_deleted_count(deletedCount);
  response->set_not_found_count(notFoundCount);
  response->set_message(
      "Deleted " + std::to_string(deletedCount) + " keys, " +
      std::to_string(notFoundCount) + " not found");

  return ::grpc::Status::OK;
}

// =============================================================================
// Atomic Operations
// =============================================================================

::grpc::Status CacheServiceImpl::SetNX(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::SetNXRequest* request,
    ::cachelib::grpc::SetNXResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_was_set(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_was_set(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  uint32_t ttlSeconds = 0;
  if (request->ttl_seconds() > 0) {
    ttlSeconds = static_cast<uint32_t>(request->ttl_seconds());
  }

  auto result = cacheManager_->setNX(request->key(), request->value(), ttlSeconds);

  response->set_was_set(result.wasSet);
  if (result.wasSet) {
    response->set_message("Key set successfully");
  } else {
    response->set_message("Key already exists");
    response->set_existing_value(result.existingValue);
  }

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Increment(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::IncrementRequest* request,
    ::cachelib::grpc::IncrementResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_success(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  int64_t delta = request->delta();
  if (delta == 0) {
    delta = 1;
  }

  uint32_t ttlSeconds = 0;
  if (request->ttl_seconds() > 0) {
    ttlSeconds = static_cast<uint32_t>(request->ttl_seconds());
  }

  auto result = cacheManager_->increment(request->key(), delta, ttlSeconds);

  response->set_success(result.success);
  response->set_new_value(result.newValue);
  response->set_message(result.message);

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Decrement(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::DecrementRequest* request,
    ::cachelib::grpc::DecrementResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_success(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  int64_t delta = request->delta();
  if (delta == 0) {
    delta = 1;
  }

  uint32_t ttlSeconds = 0;
  if (request->ttl_seconds() > 0) {
    ttlSeconds = static_cast<uint32_t>(request->ttl_seconds());
  }

  auto result = cacheManager_->decrement(request->key(), delta, ttlSeconds);

  response->set_success(result.success);
  response->set_new_value(result.newValue);
  response->set_message(result.message);

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Incr(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::IncrRequest* request,
    ::cachelib::grpc::IncrResponse* response) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  int64_t delta = request->delta();
  if (delta == 0) {
    delta = 1;
  }

  uint32_t ttlSeconds = 0;
  if (request->ttl_seconds() > 0) {
    ttlSeconds = static_cast<uint32_t>(request->ttl_seconds());
  }

  auto result = cacheManager_->incr(request->key(), delta, ttlSeconds);

  if (!result.success) {
    return ::grpc::Status(
        ::grpc::StatusCode::INTERNAL,
        result.message.empty() ? "Incr failed" : result.message);
  }

  response->set_value(result.value);
  response->set_ttl_set(result.ttlSet);
  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::CompareAndSwap(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::CompareAndSwapRequest* request,
    ::cachelib::grpc::CompareAndSwapResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_success(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  auto result = cacheManager_->compareAndSwap(
      request->key(),
      request->expected_value(),
      request->new_value(),
      static_cast<uint32_t>(request->ttl_seconds()),
      request->keep_ttl());

  response->set_success(result.success);
  response->set_actual_value(result.actualValue);

  if (result.success) {
    response->set_message("Value swapped successfully");
  } else {
    response->set_message("Value mismatch or key not found");
  }

  return ::grpc::Status::OK;
}

// =============================================================================
// TTL Operations
// =============================================================================

::grpc::Status CacheServiceImpl::GetTTL(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::GetTTLRequest* request,
    ::cachelib::grpc::GetTTLResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_found(false);
    response->set_ttl_seconds(-2);
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_found(false);
    response->set_ttl_seconds(-2);
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  int64_t ttl = cacheManager_->getTTL(request->key());

  response->set_found(ttl != -2);
  response->set_ttl_seconds(ttl);

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Touch(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::TouchRequest* request,
    ::cachelib::grpc::TouchResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  if (request->key().empty()) {
    response->set_success(false);
    response->set_message("Key cannot be empty");
    return ::grpc::Status(
        ::grpc::StatusCode::INVALID_ARGUMENT, "Key cannot be empty");
  }

  uint32_t ttlSeconds = 0;
  if (request->ttl_seconds() > 0) {
    ttlSeconds = static_cast<uint32_t>(request->ttl_seconds());
  }

  auto result = cacheManager_->touch(request->key(), ttlSeconds);

  response->set_success(result.success);
  response->set_message(result.message);

  return ::grpc::Status::OK;
}

// =============================================================================
// Streaming Operations
// =============================================================================

::grpc::Status CacheServiceImpl::Pipeline(
    ::grpc::ServerContext* /*context*/,
    ::grpc::ServerReaderWriter<
        ::cachelib::grpc::PipelineResponse,
        ::cachelib::grpc::PipelineRequest>* stream) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  ::cachelib::grpc::PipelineRequest request;
  while (stream->Read(&request)) {
    ::cachelib::grpc::PipelineResponse response;
    response.set_sequence_id(request.sequence_id());

    switch (request.operation_case()) {
      case ::cachelib::grpc::PipelineRequest::kGet: {
        const auto& getReq = request.get();
        if (getReq.key().empty()) {
          response.set_error("Key cannot be empty");
          break;
        }
        auto result = cacheManager_->get(getReq.key());
        auto* getResp = response.mutable_get();
        getResp->set_found(result.found);
        if (result.found) {
          getResp->set_value(result.value);
          getResp->set_ttl_remaining(result.ttlRemaining);
        }
        break;
      }
      case ::cachelib::grpc::PipelineRequest::kSet: {
        const auto& setReq = request.set();
        if (setReq.key().empty()) {
          response.set_error("Key cannot be empty");
          break;
        }
        uint32_t ttl = setReq.ttl_seconds() > 0
            ? static_cast<uint32_t>(setReq.ttl_seconds()) : 0;
        bool success = cacheManager_->set(setReq.key(), setReq.value(), ttl);
        auto* setResp = response.mutable_set();
        setResp->set_success(success);
        setResp->set_message(success ? "OK" : "Failed to set value");
        if (success) {
          setResp->set_size_bytes(static_cast<int64_t>(setReq.value().size()));
        }
        break;
      }
      case ::cachelib::grpc::PipelineRequest::kDelete: {
        const auto& delReq = request.delete_();
        if (delReq.key().empty()) {
          response.set_error("Key cannot be empty");
          break;
        }
        bool existed = cacheManager_->remove(delReq.key());
        auto* delResp = response.mutable_delete_();
        delResp->set_success(true);
        delResp->set_key_existed(existed);
        delResp->set_message(existed ? "Key deleted" : "Key not found");
        break;
      }
      case ::cachelib::grpc::PipelineRequest::kExists: {
        const auto& existsReq = request.exists();
        if (existsReq.key().empty()) {
          response.set_error("Key cannot be empty");
          break;
        }
        bool found = cacheManager_->exists(existsReq.key());
        auto* existsResp = response.mutable_exists();
        existsResp->set_exists(found);
        break;
      }
      default:
        response.set_error("Unknown operation");
        break;
    }

    stream->Write(response);
  }

  return ::grpc::Status::OK;
}

// =============================================================================
// Key Scanning
// =============================================================================

::grpc::Status CacheServiceImpl::Scan(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::ScanRequest* request,
    ::cachelib::grpc::ScanResponse* response) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  auto result = cacheManager_->scan(
      request->pattern(),
      request->cursor(),
      request->count(),
      request->include_details());

  for (const auto& key : result.keys) {
    response->add_keys(key);
  }

  for (const auto& info : result.keyDetails) {
    auto* ki = response->add_key_details();
    ki->set_key(info.key);
    ki->set_ttl_remaining(info.ttlRemaining);
    ki->set_size_bytes(info.sizeBytes);
  }

  response->set_next_cursor(result.nextCursor);
  response->set_has_more(result.hasMore);

  return ::grpc::Status::OK;
}

// =============================================================================
// Administration
// =============================================================================

::grpc::Status CacheServiceImpl::Stats(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::StatsRequest* /*request*/,
    ::cachelib::grpc::StatsResponse* response) {
  if (!cacheManager_->isReady()) {
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  auto stats = cacheManager_->getStats();

  // Memory stats
  response->set_total_size(stats.totalSize);
  response->set_used_size(stats.usedSize);
  response->set_item_count(stats.itemCount);

  // Operation stats
  response->set_hit_rate(stats.hitRate);
  response->set_get_count(stats.getCount);
  response->set_hit_count(stats.hitCount);
  response->set_miss_count(stats.missCount);
  response->set_set_count(stats.setCount);
  response->set_delete_count(stats.deleteCount);
  response->set_eviction_count(stats.evictionCount);
  response->set_expired_count(stats.expiredCount);

  // NVM stats
  response->set_nvm_enabled(stats.nvmEnabled);
  response->set_nvm_size(stats.nvmSize);
  response->set_nvm_used(stats.nvmUsed);
  response->set_nvm_hit_count(stats.nvmHitCount);
  response->set_nvm_miss_count(stats.nvmMissCount);

  // Server stats
  response->set_uptime_seconds(stats.uptimeSeconds);
  response->set_version(stats.version);

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Ping(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::PingRequest* /*request*/,
    ::cachelib::grpc::PingResponse* response) {
  response->set_message("PONG");
  response->set_timestamp(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  return ::grpc::Status::OK;
}

::grpc::Status CacheServiceImpl::Flush(
    ::grpc::ServerContext* /*context*/,
    const ::cachelib::grpc::FlushRequest* /*request*/,
    ::cachelib::grpc::FlushResponse* response) {
  if (!cacheManager_->isReady()) {
    response->set_success(false);
    response->set_message("Cache not initialized");
    return ::grpc::Status(
        ::grpc::StatusCode::UNAVAILABLE, "Cache not initialized");
  }

  int64_t removed = cacheManager_->flush();

  response->set_success(true);
  response->set_items_removed(removed);
  response->set_message("Flush operation completed (note: may require restart for full effect)");

  return ::grpc::Status::OK;
}

}  // namespace grpc_server
}  // namespace cachelib
