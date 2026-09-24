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

#include <cstddef>
#include <cstdint>

namespace cachelib {
namespace grpc_server {
namespace fuzz {

// True once the process-wide fuzz cache has been constructed successfully.
bool CacheIsUsable();

struct FuzzInputResult {
  bool parsed = false;
  bool operationSucceeded = false;
  size_t scanMatches = 0;
};

// Reset value state and seed matcher keys outside the timed request body.
bool PrepareOneInput();

// Parse, dispatch, and scan with a cache already prepared for this input.
FuzzInputResult FuzzPreparedInput(const uint8_t* data, size_t size);

// Drive the server with an arbitrary byte buffer. Must never crash.
FuzzInputResult FuzzOneInput(const uint8_t* data, size_t size);

}  // namespace fuzz
}  // namespace grpc_server
}  // namespace cachelib
