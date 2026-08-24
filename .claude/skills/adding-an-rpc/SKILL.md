---
name: adding-an-rpc
description: Add a new RPC to cachelib-grpc-server — proto message and rpc, CacheServiceImpl handler, CacheManager method, tests, metrics, changelog and version bump. Use when adding, or being asked to expose, any new operation on cachelib.grpc.CacheService, and when changing an existing RPC's semantics (which requires a new RPC instead).
---

# Adding an RPC

Worked example throughout: `Incr`, added in 1.6.0 (fork commit `44d7088e`, five files,
+152/-1: `proto/cache.proto`, `CacheManager.h`, `CacheManager.cc`, `CacheServiceImpl.h`,
`CacheServiceImpl.cc`). Note what that commit did **not** contain: tests. `TtlBoundaryIncrTest`
arrived much later. Do not repeat that.

## 0. First decide: new RPC or changed RPC?

Changing what an existing RPC does is never allowed — clients are generated from the published
`.proto` and the container ships it. `Incr` exists solely because `Increment` re-arms the TTL on
every write and a fixed-window rate limiter needs the window sealed at creation. Two verbs, two
semantics. If your change alters observable behaviour of an existing RPC, you are adding an RPC.

## 1. Name it

Redis-flavoured verbs only: `Set`, `Get`, `Del`, `Incr`, `MGet`. Never HTTP verbs — no `Put`,
`Post`, `Patch`, `Fetch`. Match the neighbours already in `service CacheService`.

## 2. `proto/cache.proto`

Two edits, far apart in the file.

**The rpc**, inside `service CacheService`, under the right section banner
(`Basic Operations` / `Batch Operations` / `Atomic Operations` / `TTL Operations` /
`Key Scanning` / `Streaming Operations` / `Administration`). Comment it — the proto comments are
the reference documentation:

```proto
  // Incr atomically increments a counter and stamps a TTL on first creation.
  // On miss: creates the key with value=delta and TTL=ttl_seconds (ttl_set=true).
  // On hit: increments by delta and leaves the existing TTL untouched
  // (ttl_set=false). Designed for fixed-window rate-limit buckets where the
  // window must be sealed at creation and never extended by subsequent hits.
  rpc Incr(IncrRequest) returns (IncrResponse) {}
```

**The messages**, in the same order as the rpcs. House style: a header comment naming the
message, a blank comment line, an `Example:` line, usually a `Use case:` line, then a `//`
comment above **every** field:

```proto
// IncrRequest atomically increments a counter; on creation, stamps a TTL.
//
// Example: Incr(key="rl:user:42:tier:gold", delta=1, ttl_seconds=60)
// Use case: Tier-aware fixed-window rate-limit buckets where the window length
// is set once (at bucket creation) and must not slide on subsequent hits.
message IncrRequest {
  // Key to increment
  string key = 1;
  // Amount to add (must be > 0 for rate-limit semantics; server treats 0 as 1)
  int64 delta = 2;
  // TTL in seconds applied only when the key is newly created (0 = no expiry)
  int64 ttl_seconds = 3;
}
```

Field numbers start at 1 **within the new message**. Values are `bytes`, TTLs are `int64` on
the wire and get cast to `uint32_t` in the handler. Documented behaviour must be behaviour the
code actually has; if you are knowingly leaving a hole, write it down explicitly the way the
`MultiSetResponse` `CAVEAT:` block does rather than quietly overclaiming.

Check it compiles before anything else — this is what CI's fast `proto` job runs:

```bash
protoc --proto_path=proto --cpp_out=/tmp proto/cache.proto
```

## 3. `CacheServiceImpl.h`

Declare it under the matching section banner, with a one-line comment, `override`, and the
fully-qualified `::cachelib::grpc::` types:

```cpp
  // Incr atomically increments a counter and stamps TTL on first creation
  ::grpc::Status Incr(
      ::grpc::ServerContext* context,
      const ::cachelib::grpc::IncrRequest* request,
      ::cachelib::grpc::IncrResponse* response) override;
```

## 4. `CacheServiceImpl.cc`

The handler is a translation layer. No cache logic here. The established shape, in order:

```cpp
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

  // normalise arguments (0 delta means 1; only positive TTLs are applied)
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
```

Rules that fall out of that shape: `isReady()` is checked **first**, before argument validation;
the two error strings are exactly `"Cache not initialized"` and `"Key cannot be empty"`; a
failure carrying a `message` from `CacheManager` surfaces as `INTERNAL` with that message and a
non-empty fallback literal. Batch handlers validate per item, not up front.

## 5. `CacheManager.h` / `CacheManager.cc`

Add a result struct beside the others (`GetResult`, `SetNXResult`, `IncrDecrResult`,
`IncrResult`, `CASResult`, `TouchResult`) — `bool success`, the payload, and a `std::string
message` for the `INTERNAL` path. Declare the method under its section banner with the same doc
comment the proto carries.

**If it is a read-modify-write, take `atomicOpMutex_`:**

```cpp
  std::lock_guard<std::mutex> lock(atomicOpMutex_);
```

Methods that take it today: `setNX`, `atomicAddValue` (the shared body of `increment` and
`decrement`), `incr`, `compareAndSwap`, `touch`. Every one of them reads the current item, then
writes a value derived from it; without the lock two concurrent callers interleave and one
increment is lost.

Plain `set()`, `remove()`, `get()` and `exists()` deliberately do **not** take it, for two
reasons. They are single CacheLib operations, already safe in the allocator — the mutex exists
only to make read-then-write *pairs* indivisible, not to protect the cache. And `atomicOpMutex_`
is a non-recursive `std::mutex` that the RMW paths hold while calling `set()` (`incr` ends with
`if (set(key, newValueStr, writeTtl))`); making `set()` lock it would self-deadlock every atomic
op. Never add a lock to `set()`/`remove()`.

Corollary: `multiSet` and `multiDelete` are loops over `set()`/`remove()` and are **not** atomic
as a batch. Do not document a new batch RPC as atomic.

## 6. Metrics — usually nothing to do

`MetricsServer::generateMetrics()` reads `cacheManager_->getStats()` and nothing else. A new RPC
needs **no** MetricsServer change to be reflected in `/metrics`.

What it *does* change is which counters move, and those live in `CacheManager`, not the handler:
`getCount_`/`hitCount_`/`missCount_` are bumped only inside `get()`, `setCount_` only inside
`set()`, `deleteCount_` only inside `remove()`. Because `incr()` finishes by calling `set()`,
an `Incr` shows up in `cachelib_sets_total`. A new method that writes through
`cache_->insertOrReplace(...)` directly would be invisible to every counter — decide which is
correct and say so in the changelog.

Only if the RPC genuinely needs a new series: add a field to `CacheStats`, populate it in
`CacheManager::getStats()`, and emit it in `generateMetrics()` with matching `# HELP` and
`# TYPE` lines. That is a change to the published `/metrics` surface; treat it like a proto
change.

## 7. Tests — mandatory, in the same commit

Add source files to the **existing** executables in `CMakeLists.txt`; never add a new
executable (each one relinks CacheLib, folly and fbthrift statically). Only
`CacheManagerTest.cc` defines `main()`. Fixture class names, `TEST_F` suite names and
`CacheConfig::cacheName` must be unique across the whole binary.

- Manager-level behaviour → `cache_manager_test`
  (`CacheManagerTest.cc`, `AtomicityTest.cc`, `TtlBoundaryTest.cc`, `ScanContractTest.cc`,
  `NvmHybridTest.cc`).
- Wire-level behaviour → `cache_service_test`
  (`CacheServiceTest.cc`, `PipelineTest.cc`, `ServiceContractTest.cc`), driven through
  `server_->InProcessChannel(::grpc::ChannelArguments())` as `ServiceContractTest::SetUp` does.

Mandatory cases for any new RPC:

| Case | Where | Assert |
|---|---|---|
| Empty key | service-level | `INVALID_ARGUMENT` |
| Uninitialised cache | service-level | `UNAVAILABLE` |
| Oversized/invalid key (>255 bytes: `allowLargeKeys=false`) | manager-level | rejection, not a throw |
| Concurrency, if it mutates | `AtomicityTest.cc` | N threads via the `AtomicityStartGate`, exact final value |
| TTL, if it touches expiry | `TtlBoundaryTest.cc` | the window does or does not slide, as documented |
| Batch partial failure, if it is a batch | both | per-item outcome; failed keys reported, nothing silently dropped |

The last row is not hypothetical: `MultiSet` drops empty-keyed items without counting them as
either success or failure, which the proto documents as a `CAVEAT`. A new batch RPC must not
ship with that hole.

## 8. Backward compatibility

- Adding an rpc is backward compatible. Changing an existing message is not.
- Never renumber a field, never reuse a retired number, never change a field's type — old
  clients decode the new bytes as the old type and silently misread them.
- The `.proto` ships three ways: inside the runtime image at `/opt/cachelib/proto/cache.proto`,
  as a release asset, and over server reflection. Clients are generated from it and are not
  rebuilt when you edit it, so an incompatible change reaches users as corrupt data rather than
  a compile error.
- A client that has never heard of the new rpc is unaffected. A client that calls it against an
  older image gets `UNIMPLEMENTED`.

## 9. Ship it

1. `CHANGELOG.md` — new version section. Follow the 1.6.0 entry: what the RPC does, how it
   differs from the nearest existing one, and the compatibility line
   (*"Additive only; no proto field renumbering. v1.5.x clients keep working."*).
2. Bump `kServerVersion` in `CacheManager.h` **and** `project(... VERSION ...)` in
   `CMakeLists.txt`. Nothing in CI checks these agree with each other or with the tag.
3. Update the RPC count — it is currently 19 and is written down in three places:
   `README.md` (the intro paragraph and the `proto/cache.proto` line under Project layout) and
   the `org.opencontainers.image.description` label in the `Dockerfile`. Nothing derives it, so
   nothing will catch a stale count.
4. CI `smoke` job (`.github/workflows/ci.yml`) — add a `grpcurl` line under
   *"Exercise the RPC surface"*, modelled on the `Incr` pair that asserts `"ttl_set": true` then
   `"value": "2"`. **Land this after the release that publishes the RPC, not with the
   implementation**: the job runs against `ghcr.io/celikgo/cachelib-grpc-server:latest`, so a
   grpcurl line for an unreleased RPC fails with `Unimplemented` on every push until the image
   exists.
5. Commit as `feat(<area>): <what it does>`.

## Verify before pushing

```bash
protoc --proto_path=proto --cpp_out=/tmp proto/cache.proto   # seconds
docker build --target tester -t cachelib-grpc-server:test .  # ~1h cold, minutes warm
```

The `tester` stage runs `ctest`, so both test binaries execute. There is no faster local path
that actually compiles this code.
