# Benchmarks

All numbers below were measured on the hardware stated here, with the harness
in [`bench/`](bench/). Nothing is extrapolated, and nothing is copied from
CacheLib's own published figures. Re-run it yourself with `./bench/run.sh`.

## Environment

<!-- BEGIN GENERATED: environment -->
| | |
|---|---|
| Host | Apple M2 Max, 12 cores, 32 GiB, macOS 26.5 |
| Container runtime | Docker Desktop, Linux kernel 6.12.76-linuxkit, aarch64 |
| VM resources | 12 vCPU, 31.3 GiB |
| Server image | `ghcr.io/celikgo/cachelib-grpc-server:1.6.0` (`sha256:9dfc794c4a93`) |
| Server CPUs | pinned to cores 0-5 |
| Load generator | [ghz](https://ghz.sh) v0.120.0, built natively for arm64, pinned to cores 6-11 |
| Cache | 4 GiB DRAM, NVM/SSD tier disabled |
| Working set | 200,000 keys x 1 KiB values (~256 MiB, fits entirely in DRAM) |
| Requests per measurement | 200,000 |
| **Measured version** | **1.6.0 — this tree is 1.8.0, so these numbers predate it** |
<!-- END GENERATED: environment -->

Client and server run as containers on a shared Docker network, so traffic
never leaves the VM's kernel, and they are pinned to disjoint CPU sets so the
load generator cannot steal cycles from the server it is measuring.

**Read these as a laptop-class figure, not a datacenter one.** A shared
developer VM on Apple Silicon is not a server: absolute throughput on real
Linux hardware with more cores will be materially higher. The numbers are
published because they are reproducible and honestly measured, not because
they represent a ceiling.

## GET throughput and latency vs concurrency

100% cache-hit workload (`hit_rate: 1.0` confirmed by the server's own `Stats`
RPC). Median of 3 repetitions, 200,000 requests per level, after a discarded
warmup pass. Latency percentiles are computed from raw per-request samples,
so p99.9 is a real measurement rather than a p99 fallback.

<!-- BEGIN GENERATED: sweep -->
| Concurrency | Throughput (req/s) | Range across runs | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|---:|
| 1 | 5,222 | 4,790 – 5,739 | 0.11 ms | 0.30 ms | 1.47 ms |
| 8 | 12,516 | 12,322 – 14,771 | 0.28 ms | 2.62 ms | 8.22 ms |
| 25 | 24,062 | 21,746 – 24,813 | 0.48 ms | 2.78 ms | 10.15 ms |
| 50 | 29,370 | 18,369 – 33,966 | 0.84 ms | 3.81 ms | 8.05 ms |
| 100 | 33,360 | 33,158 – 34,759 | 1.38 ms | 6.81 ms | 11.58 ms |
| 200 | 36,908 | 34,836 – 37,221 | 2.12 ms | 14.17 ms | 22.41 ms |
| 400 | 38,320 | 29,713 – 39,161 | 3.67 ms | 29.18 ms | 43.06 ms |
<!-- END GENERATED: sweep -->

**Reading the curve.** Throughput climbs to roughly 33k req/s at
concurrency 100 and then flattens; past that point additional concurrency buys
almost no throughput and costs a great deal of tail latency (p99 goes from
6.8 ms at c=100 to 29.2 ms at c=400). Concurrency ~100 is
the knee, and the sensible place to operate.

Unloaded round-trip latency is **0.11 ms p50** (c=1), which is the
number to use when reasoning about a single cache lookup on the request path.

Run-to-run variance is real on a laptop VM — see the range column, which is
wide at c=50. That is the measurement environment, not the server.

## Operation mix

200,000 requests each at concurrency 50, 8 connections, 1 KiB values.

<!-- BEGIN GENERATED: operations -->
| Operation | Throughput (req/s) | p50 | p99 | p99.9 | Errors |
|---|---:|---:|---:|---:|---:|
| `Get` (100% hit) | 31,780 | 0.81 ms | 3.28 ms | 6.11 ms | 0 |
| `Set` | 26,205 | 0.83 ms | 4.04 ms | 8.09 ms | 0 |
| `Incr` (rate-limit bucket) | 30,745 | 0.81 ms | 3.38 ms | 8.00 ms | 0 |
| `Ping` (transport floor) | 34,907 | 0.72 ms | 3.02 ms | 8.15 ms | 0 |
<!-- END GENERATED: operations -->

`Ping` does no cache work at all, so it measures the gRPC transport floor.
`Get` reaches <!-- BEGIN GENERATED: get-vs-ping -->91%<!-- END GENERATED: get-vs-ping --> of that floor — meaning the CacheLib
lookup itself is nearly free at this scale and the cost is dominated by gRPC
framing and syscalls, not by the cache. Optimising the cache would not move
these numbers; optimising the transport would.

## Reproducing

One committed command regenerates every number on this page:

```bash
./bench/all.sh [image]              # a couple of hours; run it on an idle machine
```

It runs the operation mix once, the concurrency sweep three times — the
"median of 3" this page claims — writes `bench/results-summary.json` from the
raw ghz output, and re-renders the tables here and in the README.

Nothing on the path from measurement to page is transcribed by hand:

| | |
|---|---|
| `bench/run.sh`, `bench/sweep.sh` | drive the load, write raw ghz JSON and an `environment.json` describing the host, the image **digest** and the workload |
| `bench/report.py` | the single percentile implementation, computed from the raw per-request samples so p99.9 is a measurement and not a p99 fallback |
| `bench/summarize.py` | raw runs → `bench/results-summary.json` |
| `bench/render.py` | `bench/results-summary.json` → the tables on this page and in the README |
| `bench/aggregate.py` | prints the same medians for a human; not part of the chain |

Both `run.sh` and `sweep.sh` take an image reference as their first argument,
so you can point them at a locally built image.

CI runs `python3 bench/render.py --check` on every push, which fails the build
if the markdown and the measurement file disagree. Do not edit a number in this
file by hand — re-measure with `bench/all.sh`, or the check will tell on you.

> The committed `bench/results-summary.json` predates `bench/summarize.py` and
> was distilled by hand from the same runs; its `note` field says so. The first
> `bench/all.sh` run replaces it with a generated one.
