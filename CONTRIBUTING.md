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
```

Pull-request CI builds and runs both test binaries on `linux/amd64`. The
release workflow repeats the full tests and a DRAM+flash runtime smoke on
native `linux/amd64` and `linux/arm64` before publishing either architecture.

## Changing the proto

[`proto/cache.proto`](proto/cache.proto) is a published wire contract — clients
are generated from it and the container ships it. Therefore:

- **Never** renumber or reuse a field number, and never change a field's type.
- Add new fields with new numbers; add new RPCs rather than changing existing
  signatures.
- RPC names follow Redis-flavoured verbs (`Set`, `Get`, `Del`, `Incr`, `MGet`),
  not HTTP verbs.
- Any semantic change to an existing RPC needs a new RPC instead — `Incr` exists
  precisely because changing `Increment`'s TTL behaviour would have silently
  broken callers.
- Document the semantics in the proto comments; they are the reference docs.

## Benchmarks

If a change could plausibly affect performance, run the relevant harness
before and after and put both numbers in the pull request. The historical
1.6.0 harness is:

```bash
./bench/run.sh
python3 bench/report.py bench/results/*.json
```

State the hardware. Numbers without a stated environment are not useful. See
[BENCHMARKS.md](BENCHMARKS.md) for the methodology this project holds itself to
— repeated runs, medians, percentiles from raw samples, and an explicit note on
where the measurement environment limits the result.

The current-version comparative and media-object harnesses are documented in
[bench/strong/README.md](bench/strong/README.md). Keep their raw run manifests
and generated summaries separate from the historical 1.6.0 measurements.

## Style

- C++20, matching the surrounding code.
- `clang-format` with the config in the repository root.
- Keep upstream copyright headers intact on any file that carries one. See
  [`NOTICE`](NOTICE) for why this matters.

## Licence

By contributing you agree that your contributions are licensed under the
Apache License 2.0, consistent with [`LICENSE`](LICENSE).
