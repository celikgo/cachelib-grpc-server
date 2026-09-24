# Contributing

Issues and pull requests are welcome.

## Scope

This repository is the **gRPC server**. Changes to caching behaviour itself —
eviction policy, allocator internals, the flash engine — belong upstream in
[facebook/CacheLib](https://github.com/facebook/CacheLib); this project consumes
CacheLib as a dependency and does not fork it.

In scope: the service definition, the gRPC implementation, cache lifecycle and
configuration, metrics, the container image, CI, and benchmarks.

## Building and testing

The container build is the reference environment. It pins CacheLib and gRPC
revisions and verifies the BoringSSL and xz downloads; OS package point
versions can still move.

```bash
docker build -t cachelib-grpc-server .
docker build --target tester -t cachelib-grpc-server:test .   # builds and runs unit tests
docker build --target sanitize -t cachelib-grpc-server:sanitize .
```

`tester` runs both CTest binaries. The sanitizer stage recompiles this
repository's sources with ASan/UBSan; see [its scope](docs/sanitizers.md).
For the runtime and flash smoke, build the separate correctness client once:

```bash
docker build -f bench/investigation/Dockerfile.client -t cachelib-investigation-client:local .
scripts/release-smoke.sh cachelib-grpc-server 1.8.0 /tmp/cachelib-release-smoke.json
```

Pull-request CI builds and runs both test binaries on `linux/amd64`. The
GitHub release workflow uses native `linux/amd64` and `linux/arm64` runners.
A local Docker release must record whether each architecture was native or
emulated, together with tests, runtime/flash smoke, source identity, and image
digests. Passing emulated tests does not establish native performance. See
[the release guide](docs/releasing.md) for the publication procedure.

For fast checks before a full build:

```bash
protoc --proto_path=proto --cpp_out=/tmp proto/cache.proto
python3 bench/render.py --check
python3 bench/strong/render_report_tables.py --check
python3 bench/strong/test_release_campaign.py
```

Once the complete release campaign is recorded, also run
`python3 bench/strong/render_release_report.py --check` to verify the current
release report against its raw runs and summaries.

### Linux host builds

`build.sh` is a convenience wrapper for an already provisioned Linux build
environment. Install CacheLib, its dependency chain, gRPC, Protobuf, CMake, and
a C++20 compiler first; GoogleTest is needed when building tests. The container
recipe documents the supported dependency versions. Then run:

```bash
CACHELIB_PREFIX=/path/to/cachelib \
  CMAKE_PREFIX_PATH='/path/to/dependencies;/usr/local' ./build.sh --tests
ctest --test-dir build --output-on-failure
```

`CACHELIB_PREFIX` is the installation prefix containing CacheLib headers and
libraries; it defaults to `/opt/cachelib`. `CMAKE_PREFIX_PATH` is a
semicolon-separated list of other installation prefixes required by
[`CMakeLists.txt`](CMakeLists.txt). The script does not clone CacheLib or assume
this repository is inside the upstream source tree. Native macOS server builds
are not qualified; use Linux containers through Docker Desktop on macOS.

## Changing the proto

[`proto/cache.proto`](proto/cache.proto) is a published wire contract — clients
are generated from it and the container ships it. Therefore:

- **Never** renumber or reuse a field number, and never change a field's type.
- Add new fields with new numbers; add new RPCs rather than changing existing
  signatures.
- RPC names follow the existing verbs (`Set`, `Get`, `Delete`, `Incr`, `MultiGet`),
  not HTTP verbs.
- An intentional incompatible semantic change needs a new RPC — `Incr` exists
  because replacing `Increment`'s TTL behavior would have broken callers.
  Fixes that restore the documented contract belong in the existing RPC,
  with regression tests and a changelog entry.
- Document the semantics in the proto comments; they are the reference docs.

## Benchmarks

If a change could plausibly affect performance, run the relevant harness
before and after and put both numbers in the pull request. The historical
1.6.0 harness is:

```bash
./bench/run.sh ghcr.io/celikgo/cachelib-grpc-server:1.6.0
python3 bench/report.py bench/results/*.json
```

State the hardware. Numbers without a stated environment are not useful. See
[BENCHMARKS.md](BENCHMARKS.md) for the methodology this project holds itself to
— repeated runs, medians, percentiles from raw samples, and an explicit note on
where the measurement environment limits the result.

The current-version comparative and media-object harnesses are documented in
[bench/strong/README.md](bench/strong/README.md). Keep their raw run manifests
and generated summaries separate from the historical 1.6.0 measurements.
The [1.8.0 release report](bench/strong/RELEASE-1.8.0.md) also separates the
fresh release campaign from the September 23 candidate qualification.
Record the source commit, dirty-tree fingerprint when applicable, image IDs or
digests, architecture, resource limits, and client versions. Preserve failed
runs and label exploratory runs separately from repeated headline results.
Do not describe a source-identical rebuild as the measured image without
checking the executable or rerunning the benchmark.

## Style

- C++20, matching the surrounding code.
- `clang-format` with the config in the repository root.
- Keep upstream copyright headers intact on any file that carries one. See
  [`NOTICE`](NOTICE) for why this matters.

## Licence

By contributing you agree that your contributions are licensed under the
Apache License 2.0, consistent with [`LICENSE`](LICENSE).
