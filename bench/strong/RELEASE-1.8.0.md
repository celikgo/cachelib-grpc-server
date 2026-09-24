# 1.8.0 local Docker release benchmarks

Measured 2026-09-24 to 2026-09-24 using native arm64 containers in Docker Desktop’s Linux VM on macOS.
The campaign attempted **195 independent runs across 15 groups**. All groups passed their correctness and completeness requirements. 0 groups have disclosed limits on headline claims.

This report is generated from the committed campaign, per-run JSON, and CSV data by [render_release_report.py](render_release_report.py). The [September 23 candidate report](REPORT.md) and [historical 1.6.0 page](../../BENCHMARKS.md) retain earlier measurements; their numbers are not pooled here.

## Artifact and environment

- Campaign: [release-1.8.0-results/campaign.json](release-1.8.0-results/campaign.json).
- Source base commit: `6a1f5a00b47f0ce1ec1ba429308017830d993ed7`. Each group manifest records dirty-tree status and runtime-source SHA-256.
- Host: `macOS-26.6.2-arm64-arm-64bit-Mach-O`.
- Docker: `29.8.0`, kernel `7.0.12-linuxkit`, 8 CPUs, 27.4 GiB VM memory.
- The runtime and clients were built locally before timing. Comparator images came from their registries. All tags were resolved once to immutable IDs; no image build ran during measurement.

| Role | Measured immutable image ID | Architecture |
|---|---|---|
| grpc | `sha256:959aa69a7ee0cac2f584a8b3603e19fd8eddde4824d628459be662162f67035e` | arm64 |
| go | `sha256:cd1ea0279f292e54b9dad180287c800fcc3dd9e72992f6f2efcbc3f88590fccc` | arm64 |
| python | `sha256:3753d36c934906544a046fe0f66066a0c65351887e968c98a4bc7fe850d80c72` | arm64 |
| redis | `sha256:e0e28b147e4fc2d7bdc28bab8a914a63a2e713fc0fcd09fcc97e24e423d3df1d` | arm64 |
| valkey | `sha256:1090d9113780a6c818824835da50ad4ae9445283710722fcd4ad13cd9cab59a4` | arm64 |
| memcached | `sha256:1dd5bd4099e3cca31288437e2dd63d71878251e9a528afa6064a7fc061fd0d4b` | arm64 |
| nginx | `sha256:7f7dcd27f920b22e980508bd5009b452e3d30b68c1041aa82e98e471d367db03` | arm64 |

These IDs identify local measured artifacts. Release provenance records the published registry digests and executable checksums; an image-ID or executable equality check is required before equating another artifact with these results.

## Method and interpretation

Headline groups retain five 60-second repetitions per mode after at least 15 seconds of warmup. The KV client checks the last three five-second warmup windows for a maximum 10% deviation from their mean, extending warmup up to 60 seconds. The HTTP client performs 15 seconds of warmup and full-body checks; it does not implement the KV stability-window test. Engine order is deterministically shuffled within repetitions. Exploratory groups retain the prior qualification's one/two-repetition durations and establish no precise population ranking.

KV services receive CPUs 0–3 and a 768 MiB memory ceiling, except the 384 MiB objective groups; clients receive CPUs 4–7 and 1,024 MiB. Configured cache capacity is 96 MiB unless labeled 192 MiB. Navy and extstore add a 512 MiB task-owned file. HTTP cache paths receive two CPUs and 768 MiB total: the gRPC path divides that budget equally between its cache and Python adapter. The HTTP client receives four CPUs/1,024 MiB, and a common simulated origin receives two separate CPUs/512 MiB. Container swap is disabled; no host ports are published.

Every response is byte-verified. The KV timed payload is shared across keys, so timed checks alone do not detect a wrong-key response; separate correctness probes check key-specific behavior. HTTP response checks include key identity. Closed-loop latency omits requests that could have arrived while a worker was blocked. Offered-load latency includes scheduled-arrival queueing, and dropped arrivals remain visible. One operation is one object; batch latency is per 16-object batch/window. Medians below summarize separate runs, including their per-run p99s; latency samples are never pooled. Closed-loop absolute origin counts reflect different completed request counts; use hit ratios or the equal-demand offered-load group for origin comparisons. RAM192 and flash384 objective modes ran as separate series with shuffled engine order inside each; cross-series rates are descriptive.

A KV origin miss simulates a fixed 1, 5 or 20 ms wait before filling the cache; HTTP uses a separate local origin with a 5 ms sleep. These are different origin models. All current HTTP runs use connection reuse and NGINX upstream keepalive. File tiers traverse Docker Desktop and host page caches: these results do not measure physical SSD latency, NAND writes, endurance, or financial savings. A larger configured cache is not the same as a larger measured process footprint. HTTP path memory is the sum of component cgroup peaks, an upper bound on a simultaneous combined peak.

## All-RAM unary hits

5 repetitions × 60 seconds per mode/case. [20/20 result rows](release-1.8.0-results/primary/runs.csv); [summary](release-1.8.0-results/primary/summary.json); [raw manifest](runs/release-1.8.0-20260924/primary/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `hit_1k_c8` / CacheLib gRPC DRAM | 5 | 46,658 | 0.381 | 100.0% | 0 | 59.6 | 32.0 | 0 / 0 |
| `hit_1k_c8` / Redis OSS | 5 | 126,735 | 0.120 | 100.0% | 0 | 7.0 | 19.6 | 0 / 0 |
| `hit_1k_c8` / Valkey | 5 | 126,375 | 0.121 | 100.0% | 0 | 6.9 | 18.7 | 0 / 0 |
| `hit_1k_c8` / Memcached DRAM | 5 | 104,111 | 0.136 | 100.0% | 0 | 12.9 | 15.7 | 0 / 0 |

All expected runs passed correctness and duration checks and the KV warmup stability requirement.

## Reusable 128 MiB set, 96 MiB configured DRAM

5 repetitions × 60 seconds per mode/case. [30/30 result rows](release-1.8.0-results/hybrid/runs.csv); [summary](release-1.8.0-results/hybrid/summary.json); [raw manifest](runs/release-1.8.0-20260924/hybrid/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `origin_uniform_64k_5ms` / CacheLib gRPC DRAM | 5 | 3,688 | 6.544 | 64.6% | 78,626 | 172.1 | 117.7 | 0 / 0 |
| `origin_uniform_64k_5ms` / CacheLib gRPC + Navy | 5 | 16,553 | 1.471 | 100.0% | 0 | 176.2 | 283.8 | 0 / 0 |
| `origin_uniform_64k_5ms` / Redis OSS | 5 | 2,782 | 8.428 | 58.5% | 69,221 | 90.9 | 100.0 | 0 / 0 |
| `origin_uniform_64k_5ms` / Valkey | 5 | 2,773 | 8.336 | 58.7% | 68,747 | 89.9 | 99.0 | 0 / 0 |
| `origin_uniform_64k_5ms` / Memcached DRAM | 5 | 4,319 | 6.376 | 68.8% | 80,794 | 55.0 | 107.4 | 0 / 0 |
| `origin_uniform_64k_5ms` / Memcached extstore | 5 | 44,326 | 0.565 | 100.0% | 0 | 47.9 | 195.0 | 0 / 0 |

All expected runs passed correctness and duration checks and the KV warmup stability requirement.

## Equal demand: 1,500 scheduled arrivals/s

5 repetitions × 60 seconds per mode/case. [20/20 result rows](release-1.8.0-results/offered/runs.csv); [summary](release-1.8.0-results/offered/summary.json); [raw manifest](runs/release-1.8.0-20260924/offered/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `offered_origin_64k_1500` / CacheLib gRPC DRAM | 5 | 1,500 | 7.628 | 64.7% | 31,794 | 209.3 | 118.1 | 0 / 0 |
| `offered_origin_64k_1500` / CacheLib gRPC + Navy | 5 | 1,500 | 1.913 | 100.0% | 0 | 237.2 | 269.5 | 0 / 0 |
| `offered_origin_64k_1500` / Redis OSS | 5 | 1,500 | 7.667 | 58.4% | 37,448 | 58.2 | 99.6 | 0 / 0 |
| `offered_origin_64k_1500` / Memcached extstore | 5 | 1,500 | 1.615 | 99.990% | 9 | 69.0 | 155.9 | 0 / 0 |

All expected runs passed correctness and duration checks and the KV warmup stability requirement.

## Full-hit objective: 192 MiB DRAM / 384 MiB service ceiling

5 repetitions × 60 seconds per mode/case. [15/15 result rows](release-1.8.0-results/ram192/runs.csv); [summary](release-1.8.0-results/ram192/summary.json); [raw manifest](runs/release-1.8.0-20260924/ram192/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `origin_uniform_64k_5ms` / CacheLib gRPC DRAM | 5 | 27,038 | 0.879 | 100.0% | 0 | 99.9 | 167.2 | 0 / 0 |
| `origin_uniform_64k_5ms` / Redis OSS | 5 | 53,998 | 0.411 | 100.0% | 0 | 17.4 | 156.0 | 0 / 0 |
| `origin_uniform_64k_5ms` / Memcached DRAM | 5 | 71,069 | 0.457 | 100.0% | 0 | 24.9 | 150.9 | 0 / 0 |

All expected runs passed correctness and duration checks and the KV warmup stability requirement.

## Full-hit objective: 96 MiB DRAM + 512 MiB file / 384 MiB service ceiling

5 repetitions × 60 seconds per mode/case. [10/10 result rows](release-1.8.0-results/flash384/runs.csv); [summary](release-1.8.0-results/flash384/summary.json); [raw manifest](runs/release-1.8.0-20260924/flash384/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `origin_uniform_64k_5ms` / CacheLib gRPC + Navy | 5 | 16,666 | 1.456 | 100.0% | 0 | 174.4 | 278.3 | 0 / 0 |
| `origin_uniform_64k_5ms` / Memcached extstore | 5 | 45,847 | 0.552 | 100.0% | 0 | 46.2 | 193.4 | 0 / 0 |

All expected runs passed correctness and duration checks and the KV warmup stability requirement.

## Repeated 256 KiB objects through a common HTTP interface

5 repetitions × 60 seconds per mode/case. [10/10 result rows](release-1.8.0-results/http/runs.csv); [summary](release-1.8.0-results/http/summary.json); [raw manifest](runs/release-1.8.0-20260924/http/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `repeated_256k_c8` / gRPC + Python HTTP adapter | 5 | 2,745 | 5.881 | 100.0% | 0 | 590.2 | 122.3 | 0 / 0 |
| `repeated_256k_c8` / NGINX cache | 5 | 3,025 | 4.499 | 100.0% | 0 | 283.8 | 29.2 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory value sizes, concurrency and writes

1 repetition × 10 seconds per mode/case. [30/30 result rows](release-1.8.0-results/exploratory-kv/runs.csv); [summary](release-1.8.0-results/exploratory-kv/summary.json); [raw manifest](runs/release-1.8.0-20260924/exploratory-kv/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `hit_100b_c8` / CacheLib gRPC DRAM | 1 | 46,939 | 0.500 | 100.0% | 0 | 58.6 | 28.7 | 0 / 0 |
| `hit_100b_c8` / Redis OSS | 1 | 132,966 | 0.112 | 100.0% | 0 | 6.7 | 18.8 | 0 / 0 |
| `hit_100b_c8` / Memcached DRAM | 1 | 104,209 | 0.131 | 100.0% | 0 | 12.7 | 13.0 | 0 / 0 |
| `hit_16k_c8` / CacheLib gRPC DRAM | 1 | 40,330 | 0.698 | 100.0% | 0 | 64.4 | 50.5 | 0 / 0 |
| `hit_16k_c8` / Redis OSS | 1 | 87,381 | 0.267 | 100.0% | 0 | 10.5 | 38.2 | 0 / 0 |
| `hit_16k_c8` / Memcached DRAM | 1 | 91,769 | 0.319 | 100.0% | 0 | 17.1 | 30.0 | 0 / 0 |
| `hit_64k_c8` / CacheLib gRPC DRAM | 1 | 25,393 | 0.926 | 100.0% | 0 | 99.4 | 99.8 | 0 / 0 |
| `hit_64k_c8` / Redis OSS | 1 | 46,663 | 0.533 | 100.0% | 0 | 17.6 | 85.7 | 0 / 0 |
| `hit_64k_c8` / Memcached DRAM | 1 | 52,384 | 0.669 | 100.0% | 0 | 23.6 | 80.4 | 0 / 0 |
| `hit_256k_c8` / CacheLib gRPC DRAM | 1 | 10,295 | 1.774 | 100.0% | 0 | 272.2 | 85.1 | 0 / 0 |
| `hit_256k_c8` / Redis OSS | 1 | 18,335 | 1.013 | 100.0% | 0 | 46.5 | 61.4 | 0 / 0 |
| `hit_256k_c8` / Memcached DRAM | 1 | 18,543 | 1.279 | 100.0% | 0 | 51.5 | 68.1 | 0 / 0 |
| `hit_1m_c4` / CacheLib gRPC DRAM | 1 | 2,270 | 3.494 | 100.0% | 0 | 1,203.3 | 114.5 | 0 / 0 |
| `hit_1m_c4` / Redis OSS | 1 | 5,535 | 1.624 | 100.0% | 0 | 148.5 | 67.2 | 0 / 0 |
| `hit_1m_c4` / Memcached DRAM | 1 | 7,240 | 1.452 | 100.0% | 0 | 172.3 | 54.5 | 0 / 0 |
| `hit_1k_c1` / CacheLib gRPC DRAM | 1 | 9,905 | 0.161 | 100.0% | 0 | 85.2 | 32.0 | 0 / 0 |
| `hit_1k_c1` / Redis OSS | 1 | 19,056 | 0.070 | 100.0% | 0 | 17.5 | 19.1 | 0 / 0 |
| `hit_1k_c1` / Memcached DRAM | 1 | 20,288 | 0.064 | 100.0% | 0 | 16.6 | 13.0 | 0 / 0 |
| `hit_1k_c4` / CacheLib gRPC DRAM | 1 | 26,571 | 0.328 | 100.0% | 0 | 82.2 | 29.4 | 0 / 0 |
| `hit_1k_c4` / Redis OSS | 1 | 48,739 | 0.113 | 100.0% | 0 | 9.8 | 19.3 | 0 / 0 |
| `hit_1k_c4` / Memcached DRAM | 1 | 52,653 | 0.113 | 100.0% | 0 | 18.4 | 13.0 | 0 / 0 |
| `hit_1k_c16` / CacheLib gRPC DRAM | 1 | 69,409 | 0.822 | 100.0% | 0 | 42.2 | 32.4 | 0 / 0 |
| `hit_1k_c16` / Redis OSS | 1 | 176,801 | 0.170 | 100.0% | 0 | 5.5 | 19.7 | 0 / 0 |
| `hit_1k_c16` / Memcached DRAM | 1 | 258,928 | 0.182 | 100.0% | 0 | 8.8 | 13.0 | 0 / 0 |
| `readheavy_1k_c8` / CacheLib gRPC DRAM | 1 | 47,028 | 0.535 | 100.0% | 0 | 58.1 | 32.1 | 0 / 0 |
| `readheavy_1k_c8` / Redis OSS | 1 | 130,989 | 0.116 | 100.0% | 0 | 6.7 | 17.6 | 0 / 0 |
| `readheavy_1k_c8` / Memcached DRAM | 1 | 104,351 | 0.136 | 100.0% | 0 | 12.5 | 13.9 | 0 / 0 |
| `mixed_1k_c8` / CacheLib gRPC DRAM | 1 | 47,482 | 0.514 | 100.0% | 0 | 59.2 | 27.6 | 0 / 0 |
| `mixed_1k_c8` / Redis OSS | 1 | 122,703 | 0.125 | 100.0% | 0 | 7.0 | 19.6 | 0 / 0 |
| `mixed_1k_c8` / Memcached DRAM | 1 | 105,122 | 0.130 | 100.0% | 0 | 13.4 | 12.8 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory 16-object batch GET

2 repetitions × 20 seconds per mode/case. [6/6 result rows](release-1.8.0-results/batch-get/runs.csv); [summary](release-1.8.0-results/batch-get/summary.json); [raw manifest](runs/release-1.8.0-20260924/batch-get/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `batch_get_1k_c8` / CacheLib gRPC DRAM | 2 | 554,262 | 0.758 | 100.0% | 0 | 4.8 | 32.3 | 0 / 0 |
| `batch_get_1k_c8` / Redis OSS | 2 | 1,005,534 | 0.307 | 100.0% | 0 | 0.9 | 19.6 | 0 / 0 |
| `batch_get_1k_c8` / Memcached DRAM | 2 | 1,244,900 | 0.468 | 100.0% | 0 | 1.5 | 13.8 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory 16-object batch SET

2 repetitions × 20 seconds per mode/case. [4/4 result rows](release-1.8.0-results/batch-set/runs.csv); [summary](release-1.8.0-results/batch-set/summary.json); [raw manifest](runs/release-1.8.0-20260924/batch-set/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `batch_set_1k_c8` / CacheLib gRPC DRAM | 2 | 543,095 | 0.695 | — | 0 | 5.4 | 34.3 | 0 / 0 |
| `batch_set_1k_c8` / Redis OSS | 2 | 901,435 | 0.267 | — | 0 | 1.1 | 19.6 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory sustained Pipeline

2 repetitions × 20 seconds per mode/case. [2/2 result rows](release-1.8.0-results/pipeline/runs.csv); [summary](release-1.8.0-results/pipeline/summary.json); [raw manifest](runs/release-1.8.0-20260924/pipeline/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `pipeline_1k_c8` / CacheLib gRPC DRAM | 2 | 260,737 | 1.133 | 100.0% | 0 | 12.8 | 30.5 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory scheduled-load saturation

1 repetition × 20 seconds per mode/case. [6/6 result rows](release-1.8.0-results/offered-saturation/runs.csv); [summary](release-1.8.0-results/offered-saturation/summary.json); [raw manifest](runs/release-1.8.0-20260924/offered-saturation/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `offered_1k_20k` / CacheLib gRPC DRAM | 1 | 20,000 | 0.515 | 100.0% | 0 | 104.6 | 27.8 | 0 / 0 |
| `offered_1k_20k` / Redis OSS | 1 | 20,000 | 1.526 | 100.0% | 0 | 11.3 | 17.8 | 0 / 0 |
| `offered_1k_50k` / CacheLib gRPC DRAM | 1 | 50,000 | 0.953 | 100.0% | 0 | 56.5 | 32.5 | 0 / 0 |
| `offered_1k_50k` / Redis OSS | 1 | 50,000 | 0.891 | 100.0% | 0 | 10.1 | 19.5 | 0 / 0 |
| `offered_1k_80k` / CacheLib gRPC DRAM | 1 | 71,283 | 586.225 | 100.0% | 0 | 43.6 | 32.8 | 0 / 134,902 |
| `offered_1k_80k` / Redis OSS | 1 | 80,000 | 0.275 | 100.0% | 0 | 8.6 | 18.8 | 0 / 0 |

Recorded qualification problems/observations:

- `offered_1k_80k/r0/grpc: dropped_arrivals=134902`

Dropped arrivals represent overload outcomes and are retained; request errors remain qualification failures. Rates in this exploratory sweep do not establish a precise saturation threshold.

## Exploratory origin-delay sensitivity

1 repetition × 10 seconds per mode/case. [8/8 result rows](release-1.8.0-results/origin-sensitivity/runs.csv); [summary](release-1.8.0-results/origin-sensitivity/summary.json); [raw manifest](runs/release-1.8.0-20260924/origin-sensitivity/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `origin_uniform_64k_1ms` / CacheLib gRPC DRAM | 1 | 11,754 | 2.592 | 64.6% | 41,648 | 154.0 | 114.2 | 0 / 0 |
| `origin_uniform_64k_1ms` / CacheLib gRPC + Navy | 1 | 16,738 | 1.442 | 100.0% | 0 | 170.2 | 279.6 | 0 / 0 |
| `origin_uniform_64k_1ms` / Redis OSS | 1 | 9,281 | 2.642 | 58.6% | 38,396 | 33.5 | 100.5 | 0 / 0 |
| `origin_uniform_64k_1ms` / Memcached extstore | 1 | 40,945 | 0.667 | 100.0% | 0 | 46.5 | 164.0 | 0 / 0 |
| `origin_uniform_64k_20ms` / CacheLib gRPC DRAM | 1 | 925 | 26.490 | 62.6% | 3,464 | 360.1 | 115.2 | 0 / 0 |
| `origin_uniform_64k_20ms` / CacheLib gRPC + Navy | 1 | 16,198 | 1.503 | 100.0% | 0 | 173.1 | 274.6 | 0 / 0 |
| `origin_uniform_64k_20ms` / Redis OSS | 1 | 720 | 31.564 | 59.2% | 2,936 | 144.6 | 100.1 | 0 / 0 |
| `origin_uniform_64k_20ms` / Memcached extstore | 1 | 40,876 | 0.675 | 100.0% | 0 | 45.1 | 163.9 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory near-capacity, skew, changing, cold and one-pass traces

1 repetition × 10 seconds per mode/case. [20/20 result rows](release-1.8.0-results/origin-patterns/runs.csv); [summary](release-1.8.0-results/origin-patterns/summary.json); [raw manifest](runs/release-1.8.0-20260924/origin-patterns/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `origin_near_64k_5ms` / CacheLib gRPC DRAM | 1 | 15,318 | 5.537 | 94.2% | 8,817 | 125.1 | 116.1 | 0 / 0 |
| `origin_near_64k_5ms` / CacheLib gRPC + Navy | 1 | 26,044 | 1.006 | 100.0% | 0 | 98.0 | 255.8 | 0 / 0 |
| `origin_near_64k_5ms` / Redis OSS | 1 | 8,969 | 6.461 | 85.8% | 12,728 | 30.2 | 100.1 | 0 / 0 |
| `origin_near_64k_5ms` / Memcached extstore | 1 | 52,626 | 0.651 | 100.0% | 0 | 26.2 | 113.3 | 0 / 0 |
| `origin_skew_64k_5ms` / CacheLib gRPC DRAM | 1 | 5,811 | 6.473 | 78.4% | 12,562 | 154.1 | 118.2 | 0 / 0 |
| `origin_skew_64k_5ms` / CacheLib gRPC + Navy | 1 | 20,235 | 1.257 | 100.0% | 0 | 137.2 | 279.4 | 0 / 0 |
| `origin_skew_64k_5ms` / Redis OSS | 1 | 5,182 | 7.040 | 76.1% | 12,414 | 50.2 | 99.8 | 0 / 0 |
| `origin_skew_64k_5ms` / Memcached extstore | 1 | 41,015 | 0.675 | 100.0% | 0 | 44.7 | 164.2 | 0 / 0 |
| `origin_shift_64k_5ms` / CacheLib gRPC DRAM | 1 | 25,341 | 0.919 | 99.9% | 222 | 99.2 | 54.4 | 0 / 0 |
| `origin_shift_64k_5ms` / CacheLib gRPC + Navy | 1 | 23,482 | 0.992 | 99.9% | 225 | 100.5 | 154.2 | 0 / 0 |
| `origin_shift_64k_5ms` / Redis OSS | 1 | 46,776 | 0.528 | 99.951% | 228 | 17.5 | 47.7 | 0 / 0 |
| `origin_shift_64k_5ms` / Memcached extstore | 1 | 54,242 | 0.661 | 99.958% | 229 | 21.9 | 41.3 | 0 / 0 |
| `cold_origin_uniform_64k_5ms` / CacheLib gRPC DRAM | 1 | 3,498 | 6.750 | 63.1% | 12,926 | 184.4 | 117.9 | 0 / 0 |
| `cold_origin_uniform_64k_5ms` / CacheLib gRPC + Navy | 1 | 14,437 | 5.581 | 98.6% | 2,059 | 179.9 | 259.6 | 0 / 0 |
| `cold_origin_uniform_64k_5ms` / Redis OSS | 1 | 2,627 | 8.443 | 56.8% | 11,362 | 101.5 | 100.2 | 0 / 0 |
| `cold_origin_uniform_64k_5ms` / Memcached extstore | 1 | 34,665 | 0.794 | 99.4% | 2,067 | 48.4 | 155.0 | 0 / 0 |
| `onepass_64k_5ms` / CacheLib gRPC DRAM | 1 | 561 | 8.558 | 0.0% | 5,609 | 730.4 | 116.9 | 0 / 0 |
| `onepass_64k_5ms` / CacheLib gRPC + Navy | 1 | 582 | 8.457 | 0.0% | 5,820 | 897.2 | 307.4 | 0 / 0 |
| `onepass_64k_5ms` / Redis OSS | 1 | 560 | 8.549 | 0.0% | 5,606 | 284.4 | 99.1 | 0 / 0 |
| `onepass_64k_5ms` / Memcached extstore | 1 | 647 | 7.264 | 0.0% | 6,472 | 346.5 | 636.3 | 0 / 0 |

All expected runs passed correctness and duration checks.

The cold-start case intentionally has zero warmup and reports `warmup_stable=false`. Its startup results are not steady-state rates. A changing hot set or one-pass trace tests reuse, not playback.

## Exploratory HTTP replay and 1 MiB objects

2 repetitions × 30 seconds per mode/case. [8/8 result rows](release-1.8.0-results/http-secondary/runs.csv); [summary](release-1.8.0-results/http-secondary/summary.json); [raw manifest](runs/release-1.8.0-20260924/http-secondary/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `recent_replay_256k_c8` / gRPC + Python HTTP adapter | 2 | 2,698 | 6.473 | 99.960% | 32 | 557.8 | 120.3 | 0 / 0 |
| `recent_replay_256k_c8` / NGINX cache | 2 | 2,995 | 5.239 | 99.964% | 32 | 276.5 | 29.0 | 0 / 0 |
| `repeated_1m_c8` / gRPC + Python HTTP adapter | 2 | 867 | 17.213 | 100.0% | 0 | 1,902.8 | 367.2 | 0 / 0 |
| `repeated_1m_c8` / NGINX cache | 2 | 1,124 | 13.473 | 100.0% | 0 | 790.3 | 45.6 | 0 / 0 |

All expected runs passed correctness and duration checks.

## Exploratory one-pass HTTP objects

2 repetitions × 30 seconds per mode/case. [6/6 result rows](release-1.8.0-results/http-onepass/runs.csv); [summary](release-1.8.0-results/http-onepass/summary.json); [raw manifest](runs/release-1.8.0-20260924/http-onepass/manifest.json).

| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `onepass_256k_c8` / gRPC + Python HTTP adapter | 2 | 939 | 12.213 | 0.0% | 28,168 | 866.3 | 165.5 | 0 / 0 |
| `onepass_256k_c8` / NGINX cache | 2 | 749 | 25.793 | 0.0% | 22,482 | 572.6 | 27.8 | 0 / 0 |
| `onepass_256k_c8` / Direct origin (reference) | 2 | 1,253 | 8.932 | 0.0% | 37,608 | — | — | 0 / 0 |

All expected runs passed correctness and duration checks.

Direct origin has no allocated cache path and is a reference, not an equal-resource cache comparison. Cache-path CPU/memory are not applicable; measured origin CPU/memory remain in the CSV. NGINX's asynchronous cleanup can temporarily exceed its configured 512 MiB file limit; allocated bytes are retained in the CSV.

## Observed comparisons

For the all-RAM 1 KiB/eight-connection case, gRPC served 46,658 versus Redis's 126,735 median objects/s (63.2% lower).

For the 128 MiB reusable set under 96 MiB configured DRAM with a 5 ms simulated origin, Navy served 16,553 versus DRAM-only gRPC's 3,688 objects/s (4.49×). That comparison adds file capacity and changes actual memory usage; use the separate 384 MiB objective groups when comparing a full-hit service objective.

For repeated 256 KiB HTTP objects, the complete gRPC/Python path served 2,745 versus NGINX's 3,025 median objects/s (9.3% lower). This includes adapter, copying and protocol costs. It is not an isolated CacheLib engine result.


## Scope and reproduction

The service caches whole binary objects over gRPC. It supplies no HTTP/range delivery, authentication, TLS, replication, sharding, warm restart, recording, playback buffering or media transport. Navy files are truncated on startup. Simulated origin avoidance demonstrates reuse under the disclosed trace; it does not establish production cost, viewer latency, frame continuity or streaming quality.

Run the complete campaign with [the documented local Docker commands](README.md#fresh-release-campaign). All attempts and shuffled schedules remain in the raw manifests. The generated CSVs include per-run rates, latency percentiles, resource observations, tier counters and backing-file allocations. The report check verifies campaign completeness, raw-run qualification and CSV-derived tables; it never modifies the historical reports.
