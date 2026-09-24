# cachelib-grpc-server

A standalone C++ gRPC server that puts Meta's [CacheLib](https://github.com/facebook/CacheLib)
behind a network API: 19 RPCs over a Redis-flavoured key/value surface, hybrid DRAM+SSD,
Prometheus metrics, multi-arch images at `ghcr.io/celikgo/cachelib-grpc-server`.

CacheLib is a **build dependency, not vendored**: the container build clones upstream at a
pinned commit and applies `patches/`. History before 1.6.0 lives in the
[`celikgo/CacheLib`](https://github.com/celikgo/CacheLib) fork under `standalone_server/`;
this repo was extracted from it at 1.7.0.

## Architecture

`server.cc` parses gflags, fills a `CacheConfig`, constructs a `shared_ptr<CacheManager>`,
registers a `CacheServiceImpl` on a `grpc::ServerBuilder`, and starts a `MetricsServer` on
`:9090`. **`CacheServiceImpl`** is a thin translation layer — it validates (`isReady()` →
`UNAVAILABLE`, empty key → `INVALID_ARGUMENT`), normalises arguments, delegates, and maps
result structs to proto fields. **`CacheManager`** holds all the real logic and owns the
`facebook::cachelib::LruAllocator`. Put behaviour in `CacheManager`, never in the handler.

`MetricsServer` derives every series from `CacheManager::getStats()`; it has no other input.

## Build reality

The **container build is the reference path**. It compiles gRPC v1.60.0, folly, fbthrift and
CacheLib from source. Cold builds are expensive; duration depends on available CPUs, cached
dependencies, network access, and whether the architecture is emulated.

```bash
docker build -t cachelib-grpc-server .                        # runtime image
docker build --target tester -t cachelib-grpc-server:test .   # build + run the unit tests
```

`./build.sh` is a Linux host-build wrapper. It expects CacheLib already installed at
`CACHELIB_PREFIX` (default `/opt/cachelib`) plus gRPC/Protobuf and CacheLib's dependencies.
It extends `CMAKE_PREFIX_PATH`; see [CONTRIBUTING.md](CONTRIBUTING.md). A host-build pass is
evidence for that host environment, not a substitute for container qualification.

In the `Dockerfile`, the `COPY` of this repo's sources sits *after* the dependency and CacheLib
builds, deliberately. Do not move it up — it puts an hour of compilation behind every
source-only edit.

## Tests

`tests/` builds into **two binaries** (`-DBUILD_TESTS=ON`), both registered with `add_test()`:

- `cache_manager_test` — `CacheManagerTest.cc`, `AtomicityTest.cc`, `TtlBoundaryTest.cc`,
  `ScanContractTest.cc`, `NvmHybridTest.cc`. Drives `CacheManager` directly, no gRPC.
- `cache_service_test` — `CacheServiceTest.cc`, `PipelineTest.cc`, `ServiceContractTest.cc`,
  and the fuzz body/corpus replay under `tests/fuzz/`. Service suites use in-process
  channels; Pipeline tests also exercise real streaming calls.

Add new suites as **source files in an existing executable**, not as new executables — each
extra executable repeats the large dependency link. `CacheManagerTest.cc` defines the manager
binary's `main()` and `CacheServiceTest.cc` defines the service binary's. Fixture classes,
`TEST_F` suite names, and `CacheConfig::cacheName` values
must be unique across a whole binary.

The Dockerfile `tester` stage runs `ctest`, so both binaries execute. (It used to invoke
`cache_manager_test` alone, silently skipping every gRPC-level test; fixed in `1a31bac`.)

Both `main()`s call `folly::Init`, as `server.cc` does. Do not remove it: CacheLib's flash tier
reaches folly's `Timekeeper` singleton and folly aborts on a singleton requested before
`registrationComplete()`, so without it the NVM path cannot be exercised from a test binary at
all. That is part of why the hybrid tier went untested long enough to break.

Iterating on tests without paying for a full image build: build `--target builder` once, then
mount the working tree into it and rebuild only the server and tests (a couple of minutes
instead of an hour).

Sanitizers: `docker build --target sanitize .`. A nightly workflow runs the same stage.
Record the actual executed test counts and outcome; a configured workflow is not evidence
that the current source passed. There is no qualified full-dependency TSan job — see
[docs/sanitizers.md](docs/sanitizers.md) before proposing one.

`tests/fuzz/` holds one fuzz body with two front ends: a corpus replay that runs inside
`cache_service_test` on every build, and an `LLVMFuzzerTestOneInput` behind
`-DCACHELIB_GRPC_LIBFUZZER`. New crashers go in the embedded corpus.

## Standing rules

- **A guarantee not covered by a test must not be claimed in `README.md`** or in the
  `proto/cache.proto` doc comments. Documented behaviour that the code does not have is a bug
  report, not a doc edit — the `MultiSet` empty-key caveat in the proto is the model for
  writing down a hole you are not fixing yet.
- Historical 1.6.0 tables are rendered from `bench/results-summary.json`;
  `python3 bench/render.py --check` fails CI if their generated blocks drift. Current
  September 23 candidate tables use `bench/strong/render_report_tables.py --check`;
  the fresh release report uses `bench/strong/render_release_report.py --check` after its
  complete campaign. Preserve separate versions, image identities, and environments. Regenerate
  tables from measured evidence; do not relabel old runs as current release measurements.
- `proto/cache.proto` is a published wire contract. Never renumber or reuse a field number,
  never change a field's type. Intentional incompatible semantics need a **new RPC** —
  `Incr` exists because replacing `Increment`'s TTL behavior would have broken callers.
  Correcting an implementation defect to restore its documented contract stays in the
  existing RPC and needs a regression test and changelog entry.
- Every `*.cc`, `*.h`, `proto/cache.proto`, `CMakeLists.txt` and `docker-compose.yml` must keep
  the Meta Apache-2.0 header in its first 20 lines; CI's `proto` job enforces it.
- C++20, `clang-format` with the repo `.clang-format` (Google, 2-space indent, 80 columns).
- Bumping `ARG CACHELIB_REF` is its own commit, never bundled with anything else.
- `Scan` and `Flush` iterate CacheLib's hash table, so their cost tracks the *configured
  cache size*, not the number of keys — and neither sees keys that live only on flash.
  Anything built on them inherits both.

## Commits

Conventional commits, scoped where a scope is meaningful. Real examples from this history:

```
fix(counters): refuse to overflow instead of wrapping
fix(nvm): set Navy's thread counts before enabling async I/O
fix(scan): replace the backtracking pattern matcher
perf(ci): copy the server sources after the dependency build
ci: run the whole test suite in the tester stage, not one binary
bench: generate the published tables from the committed measurement
```

## Version bumps

Bump `kServerVersion` in `CacheManager.h` (reported by `Stats.version`, `--version`, and the
`cachelib_info` metric) and `project(... VERSION ...)` in `CMakeLists.txt`, then update the
changelog and qualification report. The release workflow checks the source declarations,
report heading, requested version, and tag. Runtime smoke verifies the reported version.
Follow [docs/releasing.md](docs/releasing.md) for local Docker publication and its required
provenance. `latest` identifies a verified published image, not the current source checkout.
