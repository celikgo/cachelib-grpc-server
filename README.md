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

| Concurrency | Throughput | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|
| 1 | 5,222 req/s | 0.11 ms | 0.30 ms | 1.47 ms |
| 25 | 24,062 req/s | 0.48 ms | 2.78 ms | 10.15 ms |
| **100** | **33,360 req/s** | **1.38 ms** | **6.81 ms** | **11.58 ms** |
| 400 | 38,320 req/s | 3.67 ms | 29.18 ms | 43.05 ms |

Throughput flattens at about concurrency 100; past that, extra concurrency buys
almost nothing and costs a lot of tail latency. `Get` runs at 91% of the
throughput of `Ping` — an RPC that touches no cache at all — so at this scale
the CacheLib lookup is effectively free and gRPC framing is the bottleneck.

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

- **`Incr` vs `Increment`.** `Increment` refreshes the TTL on every call, which
  makes a rate-limit window slide forward forever under sustained load. `Incr`
  stamps the TTL **only when it creates the key** and leaves it alone
  afterwards, and reports which happened via `ttl_set`. That is the semantic a
  fixed-window rate limiter actually needs.
- **`CompareAndSwap`** carries `keep_ttl`, so optimistic-locking updates do not
  silently reset expiry.
- **`Pipeline`** amortises per-RPC overhead across a stream — worth reaching for
  given that transport, not the cache, is what limits throughput.
- **`Scan`** is O(n) over the cache and is a debugging and administration tool,
  not something to put on a request path.

---

## Configuration

Flags are passed to the container as arguments.

| Flag | Default | Meaning |
|---|---|---|
| `--address` | `0.0.0.0` | Bind address |
| `--port` | `50051` | gRPC port |
| `--cache_name` | `grpc-cachelib` | Cache instance name |
| `--cache_size` | `1073741824` (1 GiB) | DRAM cache size, bytes |
| `--max_item_size` | `4194304` (4 MiB) | Largest storable value |
| `--lru_refresh_time` | `60` | LRU refresh interval, seconds |
| `--metrics_port` | `9090` | Prometheus port (`0` disables) |
| `--log_level` | `INFO` | `DBG`, `INFO`, `WARN`, `ERR`, `CRITICAL` |
| `--enable_nvm` | `false` | Enable the SSD/flash tier |
| `--nvm_path` | `/tmp/cachelib_nvm` | Flash device or backing file |
| `--nvm_size` | `10737418240` (10 GiB) | Flash tier size, bytes |
| `--nvm_block_size` | `4096` | Flash block size |
| `--nvm_reader_threads` | `32` | Flash reader threads |
| `--nvm_writer_threads` | `32` | Flash writer threads |
| `--enable_io_uring` | `true` | io_uring for flash I/O |

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

> **Note on `--enable_io_uring`.** The published images are built against a
> folly configured without `io_uring` (see [`patches/`](patches/)), because the
> Docker Desktop VM kernel does not expose what folly probes for. On a host
> kernel with full `io_uring` support you will want to rebuild rather than rely
> on the published image for flash-heavy workloads.

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

Upstream CacheLib is cloned during the build and can be repointed:

```bash
docker build --build-arg CACHELIB_BRANCH=main \
             --build-arg CACHELIB_REPO=https://github.com/facebook/CacheLib.git .
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
