# cachelib-grpc-server

A standalone gRPC server that puts [Meta's CacheLib][cachelib] behind a
network API: 19 RPCs over a Redis-flavoured key/value surface, a hybrid
DRAM+SSD cache underneath, Prometheus metrics, and multi-architecture
containers published to `ghcr.io/celikgo/cachelib-grpc-server`.

> **Provenance.** This is a gRPC server *built on* [CacheLib][cachelib], the
> in-process caching engine developed and open-sourced by Meta Platforms, Inc.
> CacheLib is consumed as a build dependency — it is cloned from upstream
> during the container build and is **not** vendored into this repository.
> CacheLib is licensed under the Apache License 2.0, and every file here that
> originates from or derives from it retains its original Meta copyright
> header; see [`NOTICE`](NOTICE) and [`patches/`](patches/). This project is
> **not affiliated with, sponsored by, or endorsed by Meta Platforms, Inc.**
>
> CacheLib gives you a cache inside *one* process. This server is the part that
> lets many processes, in any language, share one.

[![CI](https://github.com/celikgo/cachelib-grpc-server/actions/workflows/ci.yml/badge.svg)](https://github.com/celikgo/cachelib-grpc-server/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![Container](https://img.shields.io/badge/ghcr.io-cachelib--grpc--server-blue)](https://github.com/celikgo/cachelib-grpc-server/pkgs/container/cachelib-grpc-server)

---

## Quickstart (60 seconds)

```bash
docker run -d --name cache -p 50051:50051 -p 9090:9090 \
  ghcr.io/celikgo/cachelib-grpc-server:latest
```

The image is multi-arch (`linux/amd64`, `linux/arm64`) and ships gRPC
reflection, so you can talk to it without holding a copy of the `.proto`:

```bash
grpcurl -plaintext localhost:50051 list
# cachelib.grpc.CacheService
# grpc.health.v1.Health
# grpc.reflection.v1.ServerReflection

grpcurl -plaintext -d '{"key":"hello","value":"'$(printf world | base64)'","ttl_seconds":60}' \
  localhost:50051 cachelib.grpc.CacheService/Set

grpcurl -plaintext -d '{"key":"hello"}' \
  localhost:50051 cachelib.grpc.CacheService/Get
# { "found": true, "value": "d29ybGQ=", "ttl_remaining": "60" }
```

Prometheus metrics are on `:9090/metrics`; the container also carries
`grpc_health_probe` and a `HEALTHCHECK`, so orchestrators get liveness for free.

`value` is a protobuf `bytes` field, so it is base64-encoded in the JSON
representation that `grpcurl` uses. Native clients send raw bytes.

---

## Performance

Measured on an Apple M2 Max (12 vCPU) under Docker Desktop, 4 GiB DRAM cache,
200,000 x 1 KiB working set, 100% cache hits, median of 3 runs.
Full methodology and the reproducible harness: **[BENCHMARKS.md](BENCHMARKS.md)**.

<!-- BEGIN GENERATED: readme-sweep -->
| Concurrency | Throughput | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|
| 1 | 5,222 req/s | 0.11 ms | 0.30 ms | 1.47 ms |
| 25 | 24,062 req/s | 0.48 ms | 2.78 ms | 10.15 ms |
| **100** | **33,360 req/s** | **1.38 ms** | **6.81 ms** | **11.58 ms** |
| 400 | 38,320 req/s | 3.67 ms | 29.18 ms | 43.06 ms |
<!-- END GENERATED: readme-sweep -->

Throughput flattens at about concurrency 100; past that, extra concurrency buys
almost nothing and costs a lot of tail latency. `Get` runs at
<!-- BEGIN GENERATED: get-vs-ping -->91%<!-- END GENERATED: get-vs-ping -->
of the throughput of `Ping` — an RPC that touches no cache at all — so at this
scale the CacheLib lookup is effectively free and gRPC framing is the
bottleneck.

These are laptop-VM numbers, published because they are reproducible, not
because they are a ceiling. Real Linux server hardware will do better.

---

## RPC surface

All 19 RPCs live on `cachelib.grpc.CacheService`
([`proto/cache.proto`](proto/cache.proto)).

**Basic** — `Get`, `Set`, `Delete`, `Exists`

**Batch** — `MultiGet`, `MultiSet`, `MultiDelete`

**Atomic** — `SetNX`, `Increment`, `Decrement`, `Incr`, `CompareAndSwap`

**TTL** — `GetTTL`, `Touch`

**Scan** — `Scan` (glob-style pattern, cursor pagination, optional per-key metadata)

**Streaming** — `Pipeline` (bidirectional; `Get`/`Set`/`Delete`/`Exists` multiplexed over one stream, correlated by `sequence_id`)

**Admin** — `Stats`, `Ping`, `Flush`

A few worth calling out:

- **`Incr` vs `Increment`.** `Increment` applies `ttl_seconds` on *every* call
  when it is non-zero, resetting the expiry of a key that already exists — so a
  rate-limit window slides forward forever under sustained load. (Pass
  `ttl_seconds=0` and it preserves the existing expiry instead, which is not a
  window either.) `Incr` stamps the TTL **only when it creates the key** and
  leaves it alone afterwards, and reports which happened via `ttl_set`. That is
  the semantic a fixed-window rate limiter actually needs.
- **`CompareAndSwap`** carries `keep_ttl`, so optimistic-locking updates do not
  silently reset expiry.
- **`Pipeline`** amortises per-RPC overhead across a stream — worth reaching for
  given that transport, not the cache, is what limits throughput.
- **`Scan`** walks CacheLib's hash table, so its cost tracks the *configured
  cache size*, not the number of keys stored — a scan of an almost-empty 1 GiB
  cache costs the same as a scan of a full one. It is a debugging and
  administration tool, not something to put on a request path. Its cursor is **best effort**, not a
  snapshot: the cursor is the previous page's last key, and resuming re-walks
  the cache and skips until it sees that key — so if the cursor key is deleted
  between pages, the resumed page comes back empty and the iteration ends
  early. Within one page no key repeats; across pages nothing is guaranteed.

---

## Configuration

Flags are passed to the container as arguments.

| Flag | Default | Meaning |
|---|---|---|
| `--address` | `0.0.0.0` | Bind address |
| `--port` | `50051` | gRPC port |
| `--cache_name` | `grpc-cachelib` | Cache instance name |
| `--cache_size` | `1073741824` (1 GiB) | DRAM cache size, bytes |
| `--max_item_size` | `4194304` (4 MiB) | Upper bound the server pre-checks — see the note below |
| `--lru_refresh_time` | `60` | LRU refresh interval, seconds |
| `--metrics_port` | `9090` | Prometheus port (`0` disables) |
| `--log_level` | `INFO` | `DBG`, `INFO`, `WARN`, `ERR`, `CRITICAL` |
| `--enable_nvm` | `false` | Enable the SSD/flash tier |
| `--nvm_path` | `/tmp/cachelib_nvm` | Flash device or backing file |
| `--nvm_size` | `10737418240` (10 GiB) | Flash tier size, bytes |
| `--nvm_block_size` | `4096` | Flash block size |
| `--nvm_reader_threads` | `32` | Flash reader threads |
| `--nvm_writer_threads` | `32` | Flash writer threads |
| `--enable_io_uring` | `true` | io_uring for flash I/O — **inert in the published images**, see below |

> **The largest value you can actually store** is
> `min(--max_item_size, 4 MiB − 32 − len(key))`. CacheLib cannot allocate an
> item larger than one 4 MiB slab, and the 32-byte item header and the key
> share that budget with the value — so with the 4 MiB default and a one-byte
> key the ceiling is 4,194,271 bytes, and raising `--max_item_size` above
> 4 MiB changes nothing. Below the slab ceiling the flag is exact: with
> `--max_item_size=65536`, a 65,536-byte value stores and a 65,537-byte one
> does not. An oversized value comes back as `success=false`.

### Hybrid DRAM + SSD

CacheLib's headline capability is transparently spilling from DRAM to flash.
Give the container a device or a file and turn the tier on:

```bash
docker run -d -p 50051:50051 \
  -v /mnt/nvme:/data/nvm \
  ghcr.io/celikgo/cachelib-grpc-server:latest \
  --cache_size=8589934592 \
  --enable_nvm --nvm_path=/data/nvm/cache --nvm_size=107374182400
```

Reads and writes are unchanged; `Stats` reports the flash tier separately via
`nvm_enabled`, `nvm_size`, `nvm_hit_count`, and `nvm_miss_count`.

Two things behave differently once the tier is on, because CacheLib's iterator
and its remove result only see DRAM:

- **`Scan` and `Flush` do not see flash-only keys.** They walk the DRAM hash
  table, which does not contain items that have been evicted to flash. A
  `Flush` therefore leaves those entries in place and `items_removed` counts
  only what was in DRAM. `FlushRequest.include_nvm` is not implemented.
- **`Delete` reports DRAM residency, not existence.** A key that had been
  evicted to flash *is* deleted, but `key_existed` comes back `false`, and
  `MultiDelete`'s `deleted_count` / `not_found_count` are skewed the same way.
  Do not drive idempotency or lock-ownership decisions off that flag on a
  hybrid deployment.

> **Note on `--enable_io_uring`.** The flag does nothing in the published
> images. They are built against a folly configured without `io_uring` (see
> [`patches/`](patches/)), so `liburing` is absent at compile time, CacheLib's
> CMake defines `CACHELIB_IOURING_DISABLE`, and the io_uring paths are compiled
> out of Navy entirely — `nm` on `libcachelib_navy.a` finds no io_uring symbols
> at all. Flash I/O uses libaio regardless of what you pass. Setting it to
> `true` is not an error; it simply has no effect.
>
> On a host kernel with full `io_uring` support, rebuild with `liburing` present
> rather than relying on the published image for flash-heavy workloads.

### docker-compose

```bash
docker compose up -d
```

See [`docker-compose.yml`](docker-compose.yml).

---

## Building from source

The container build is the supported path, because it pins the whole
dependency chain (gRPC, folly, fbthrift, CacheLib) rather than trusting
whatever is on the host:

```bash
docker build -t cachelib-grpc-server .
docker build --target tester -t cachelib-grpc-server:test .   # build + run unit tests
```

Upstream CacheLib is cloned during the build at a **pinned commit**, so
rebuilding a release tag reproduces that release rather than picking up whatever
upstream happens to be that day. Repoint it explicitly to track a newer
revision:

```bash
docker build --build-arg CACHELIB_REF=<commit-sha> .
```

A host build via `./build.sh` exists and expects CacheLib and its dependencies
already installed; see [`CMakeLists.txt`](CMakeLists.txt) for the exact
`find_package` requirements.

---

## Project layout

```
proto/cache.proto     service definition (19 RPCs)
server.cc             entry point, flags, signal handling
CacheManager.*        CacheLib allocator lifecycle, pools, NVM configuration
CacheServiceImpl.*    the gRPC service implementation
MetricsServer.*       Prometheus /metrics endpoint
tests/                unit tests (gtest)
patches/              two upstream CacheLib files modified for the build
bench/                reproducible benchmark harness
```

---

## What is verified

150 tests across two binaries, run by `ctest` in the Dockerfile's `tester`
stage on every push. They exist because a cache is a component where a rare
concurrency bug or an off-by-one at a TTL boundary costs a user their data, and
because a guarantee this README makes and nothing tests is not a guarantee.

| Area | What is pinned |
|---|---|
| Atomics under contention | N threads released simultaneously onto one key: exactly one `SetNX` winner; `Incr` results forming exactly `1..N` with no duplicates and exactly one `ttl_set`; mixed `Increment`/`Decrement` netting to exactly zero; one `CompareAndSwap` winner per generation and a stale token that never mutates |
| TTL boundaries | expiry at, before and after the second boundary; TTL across an overwrite; `Incr`'s window not sliding and restarting once it lapses; the `-1`/`-2` sentinels; counters that overflow or are not entirely numeric |
| Scan | pagination completeness, cursor invalidation, mutation mid-scan, `count` clamping, metacharacters matched literally, and a time bound that fails if a backtracking matcher returns |
| Streaming | `sequence_id` correlation batched and interleaved, a malformed message and an empty key mid-stream, 20 client cancellations in a row, shutdown with a stream open under a hard bound, backpressure |
| Protocol edges | the 255/256-byte key boundary, the value-size boundary, binary and empty values, batch duplicates and partial failure, unicode and embedded-NUL keys |
| Hybrid DRAM+SSD | initialisation, both Navy engines, DRAM eviction and promotion, byte-exact binary round trips through flash, TTL surviving the trip |

A nightly run ([`nightly.yml`](.github/workflows/nightly.yml)) builds the same
suite with AddressSanitizer and UndefinedBehaviorSanitizer. Only this
repository's own translation units are instrumented, which bounds what that can
catch; [`docs/sanitizers.md`](docs/sanitizers.md) says what it covers, and why
there is no ThreadSanitizer job rather than a noisy one.

A fuzz corpus is replayed through the request-decode path on every CI build, in
about 150 ms — the regression half of fuzzing, without a second toolchain.

Two things are **not** covered, and are not claimed anywhere: authentication
and TLS (there are none — see [`SECURITY.md`](SECURITY.md)), and corruption
inside a CacheLib slab, which ASan cannot see because the slab arena is
`mmap`ed rather than `malloc`ed.

## Compatibility

The wire format is the `.proto`. Any gRPC language binding works; generate
clients straight from [`proto/cache.proto`](proto/cache.proto), or use
reflection. A Java client and a Spring Boot integration example live in the
[upstream fork][fork] this server was extracted from.

## Contributing, security, licence

- [`CONTRIBUTING.md`](CONTRIBUTING.md)
- [`SECURITY.md`](SECURITY.md) — note that the server has **no authentication or
  TLS**; it is designed to sit on a trusted network behind a mesh or ingress
  that terminates both.
- [`LICENSE`](LICENSE) — Apache License 2.0
- [`NOTICE`](NOTICE) — attribution for CacheLib and derived files

[cachelib]: https://github.com/facebook/CacheLib
[fork]: https://github.com/celikgo/CacheLib
