# Changelog

History prior to `1.6.0` was made in the [`celikgo/CacheLib`][fork] fork, where
this server lived as `standalone_server/` before being extracted into its own
repository. Container images for every version below are published to
[`ghcr.io/celikgo/cachelib-grpc-server`][pkg].

## [1.7.0] — 2026-08-19

First release built and published by this repository's own CI, on native
`amd64` and `arm64` runners rather than under QEMU emulation.

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

### Fixed
- The `1.7.0` release failed to publish: both architectures compiled for ~48
  minutes and then hit `denied: permission_denied: write_package` on push. The
  `ghcr.io/celikgo/cachelib-grpc-server` package had been left with no linked
  repository when the server was extracted out of the fork, and an orphaned
  user-scoped package grants no repository `GITHUB_TOKEN` write access. The
  package is now linked to this repository, so `GITHUB_TOKEN` inherits write
  access; the 25 previously published versions were preserved.

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
