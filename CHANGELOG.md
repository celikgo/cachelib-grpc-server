# Changelog

History prior to `1.6.0` was made in the [`celikgo/CacheLib`][fork] fork, where
this server lived as `standalone_server/` before being extracted into its own
repository. Container images for every version below are published to
[`ghcr.io/celikgo/cachelib-grpc-server`][pkg].

## [1.8.0] — unreleased

Correctness release. Three defects here could lose or corrupt a user's data or
take the server down, and none of them had a test that would have caught it.

**The hybrid DRAM+SSD tier has never worked.** Running the command in the
README's "Hybrid DRAM + SSD" section against any published image exits at
startup with `number of read/write threads should be set first as non-zero
value`. `configureNvmCache` called `NavyConfig::enableAsyncIo()` with the
reader/writer thread counts in the parameters that mean `maxNumReads` and
`maxNumWrites`, without the `setReaderAndWriterThreads()` call that overload
requires first.

**`Incr` and `Increment` destroyed data.** `std::from_chars` parses the longest
valid prefix and reports success; the code checked only the error code and
never the end pointer, so `"42 users"` incremented to `"43"` and the rest of
the value was gone, with the RPC reporting success. They also wrapped on
overflow — `Incr` on `INT64_MAX` returned `INT64_MIN`, which for the
fixed-window rate limiter `Incr` exists to serve means every later limit check
passes.

**`Scan` was a remotely-triggerable denial of service.** The glob pattern was
compiled into a regular expression, once per key; alternating wildcards with
literals made it backtrack exponentially. A 25-byte pattern pinned a core
indefinitely and kept running after the client disconnected — on a port with no
authentication.

### Fixed
- `--enable_nvm` starts. Navy's reader and writer thread counts are set before
  async I/O is enabled.
- `Incr`, `Increment` and `Decrement` reject a value that is not entirely an
  integer instead of overwriting it, and refuse to overflow instead of wrapping.
  `Decrement` also rejects the one delta whose negation overflows.
- `Scan`'s pattern matcher is a linear iterative wildcard matcher rather than a
  regular expression. Behaviour is unchanged — differential-tested against the
  old semantics over 1,563,485 (key, pattern) pairs with zero disagreements —
  and the pathological patterns now complete in microseconds.
- `--lru_refresh_time` reaches the LRU. It was parsed, logged, and read by
  nothing, because `addPool` was called without an MMLru config.
- The Dockerfile's `tester` stage runs `ctest`, so both test binaries execute.
  It built and ran `cache_manager_test` only, so 15 of the 35 existing tests had
  never run in CI — including one, `CacheServiceTest.Stats`, that could not have
  passed.

### Changed
- The hash table is sized from `--cache_size` instead of a hardcoded 2^25
  buckets. `Scan` and `Flush` iterate the table rather than the items, so their
  cost was the bucket count regardless of how much was cached: a `Scan` over a
  cache holding nine keys took 2.23 seconds. Override with
  `CacheConfig::hashBucketsPower`.
- The CI smoke job builds and tests the image from the checkout. It pulled
  `:latest`, so the fastest job in the pipeline proved nothing about the change
  under review, and it now also starts the server with `--enable_nvm`.

### Added
- 150 tests, up from 35. First coverage for `SetNX`, `Increment`, `Decrement`,
  `Incr`, `CompareAndSwap`, `Touch`, `GetTTL`, `MultiDelete`, `Flush`,
  `Pipeline` and the hybrid tier.
- A nightly ASan + UBSan run, and `docs/sanitizers.md` stating what it covers
  and why there is no ThreadSanitizer job.
- A fuzz target over the request-decode path, with its corpus replayed on every
  CI build.
- `bench/summarize.py` and `bench/all.sh`: the published numbers are now derived
  from the raw runs by committed commands, and CI fails if the markdown and the
  measurement disagree.
- `CLAUDE.md` and two skills, `adding-an-rpc` and `updating-the-cachelib-pin`.

### Documentation
- `proto/cache.proto` and `README.md` corrected where they promised more than
  the code delivers: the real value-size ceiling, `Scan`'s best-effort cursor,
  `Increment`'s TTL behaviour, `StatsRequest.detailed` and
  `FlushRequest.include_nvm` being unimplemented, and — with the flash tier on —
  `Scan` and `Flush` not seeing flash-resident keys and `Delete.key_existed`
  reporting DRAM residency rather than existence.
- `--enable_io_uring` is documented as inert in the published images. `liburing`
  is absent at build time, so CacheLib's CMake defines
  `CACHELIB_IOURING_DISABLE` and the io_uring paths are compiled out of Navy
  entirely; `nm` on `libcachelib_navy.a` finds no io_uring symbols. Flash I/O
  uses libaio whatever the flag says, and the flag defaults to `true`.
- `Scan` was described as "O(n) over the cache", which reads as O(keys). It is
  O(configured cache size), because it iterates the hash table.

## [1.7.0] — tagged 2026-08-19, never published

The `v1.7.0` tag exists but no image or GitHub release does: the Release
workflow failed three times at `denied: permission_denied: write_package`. The
package exists but has not granted this repository Actions write access, which
is a separate, UI-only setting. `ghcr.io/celikgo/cachelib-grpc-server:latest`
still resolves to 1.6.0.

Intended as the first release built and published by this repository's own CI,
on native `amd64` and `arm64` runners rather than under QEMU emulation.

No change to the RPC surface or the wire format: v1.6.x clients work unchanged.
`Stats.version` now reports `1.7.0`.

### Changed
- Extracted from the `celikgo/CacheLib` fork into a standalone repository.
  Upstream CacheLib is now cloned as a build dependency instead of the server
  living inside a fork of it. No functional or wire-format change.
- `Dockerfile` builds from this repository as its context; the two modified
  upstream CacheLib files moved to [`patches/`](patches/) with documentation.
- `docker-compose.yml`: publishes the Prometheus port, drops `CACHELIB_*`
  environment variables that the server never read, drops the obsolete
  `version:` key.

### Added
- `NOTICE`, `SECURITY.md`, `CONTRIBUTING.md`.
- [`bench/`](bench/) — reproducible benchmark harness, and
  [`BENCHMARKS.md`](BENCHMARKS.md) with measured throughput and tail latency.
- CI: build, unit tests, and multi-arch (`amd64` + `arm64`) container builds.
- `latest` container tag, tracking the newest release.
- Upstream CacheLib is pinned to an explicit commit (`ARG CACHELIB_REF`).
  Previously the build cloned `main`, so rebuilding a release tag pulled
  whatever upstream was that day and did not reproduce the release.
- `patches/` are now real diffs applied with `git apply`, replacing whole-file
  copies of two upstream files. The copies were taken from a tree ~450 commits
  behind upstream; once the pin was added, the stale folly manifest still
  declared a `double-conversion` dependency upstream had removed and the build
  failed with `ManifestNotFound`. A diff cannot drift silently. CI verifies
  they still apply, in ~20 seconds, on every push.
- The pin is upstream main as of 2026-05-02, the revision the last known-good
  image was built from, rather than the newest commit: later upstream revisions
  bump mvfst to a version that does not compile under GCC 13 on Ubuntu 24.04.
- OCI image labels, including `org.opencontainers.image.source`.
- The release workflow records the exact upstream CacheLib revision each
  release was built against, in the release notes and in a machine-readable
  `provenance.json` asset, alongside the image digests.
- A `pin` job that fails the release if `CACHELIB_REF` is not a full 40-character
  commit SHA, or does not resolve upstream. It runs in ~15 seconds, ahead of the
  ~50-minute compile, so an unreproducible build is rejected before it costs
  anything. It also reports how far the pin trails upstream `main`.
- The runtime image is stamped with the upstream revision it was compiled
  against (`io.celikgo.cachelib.upstream.revision`), and the release workflow
  reads that label back off the pushed images and fails if it disagrees with the
  Dockerfile pin. The revision in the release notes is therefore a fact about
  the artefact, not a claim about the source tree.
- Releases carry assets: `cache.proto`, `provenance.json`,
  `benchmark-results.json`, `LICENSE`, `NOTICE` and `SHA256SUMS`.
- A `preflight` job that asks GHCR, before the build starts, whether
  `GITHUB_TOKEN` may actually push to the package, and stops the release in
  about a second if it may not. Three release runs had already been spent
  discovering that at the push step, each after ~50 minutes of compilation.

### Fixed
- The `1.7.0` release failed to publish: both architectures compiled for ~48
  minutes and then hit `denied: permission_denied: write_package` on push. When
  the server was extracted out of the fork, the
  `ghcr.io/celikgo/cachelib-grpc-server` package was left both unlinked from any
  repository and without this repository on its Actions access list. The package
  is now linked here (its images carry `org.opencontainers.image.source`), and
  the repository was granted `Write` in the package's Actions access list --
  these are two separate settings, and only the second one grants push. The 25
  previously published versions were preserved throughout.

## [1.6.0] — 2026-05-02

### Added
- **`Incr` RPC** for fixed-window rate-limit buckets. On a miss it creates the
  key with `value=delta` and stamps `TTL=ttl_seconds` atomically, reporting
  `ttl_set=true`; on a hit it increments and leaves the existing TTL untouched
  (`ttl_set=false`). This is the semantic a fixed window needs — the first
  request of a window seals it and later hits do not slide it forward.
  Distinct from `Increment`, which overrides the TTL whenever one is supplied.

Additive only; no proto field renumbering. v1.5.x clients keep working.

## [1.5.0] — 2026-03-28

### Changed
- Synced 185 commits from upstream CacheLib, picking up Navy's `FixedSizeIndex`,
  access-time tracking, `FlashCacheComponent`, and custom reinsertion policies.
- Docker build updated for the new upstream tree.

## [1.4.0]

### Added
- `size_bytes` in `SetResponse`.
- `Scan` enriched with `KeyInfo` (`include_details` returns per-key TTL and size).

## [1.3.1] — 2026-02-10

### Fixed
- `Flush` and `Scan` were stubs; both now implemented.

### Added
- `--version` flag.
- Prometheus metrics improvements.

## [1.3.0]

### Added
- gRPC server reflection.
- `Pipeline` bidirectional streaming RPC.
- `MultiDelete`.
- Prometheus metrics endpoint.

## [1.2.2]

### Added
- Multi-platform (amd64 + arm64) container publishing in CI.

## [1.2.0]

### Added
- Redis-parity operations: `SetNX`, `CompareAndSwap`, `GetTTL`, `Touch`,
  `Increment`, `Decrement`.

### Fixed
- Stats counter visibility under concurrency (sequential consistency).

## [1.0.0]

- Initial standalone gRPC server and Java client.

[fork]: https://github.com/celikgo/CacheLib
[pkg]: https://github.com/celikgo/cachelib-grpc-server/pkgs/container/cachelib-grpc-server
