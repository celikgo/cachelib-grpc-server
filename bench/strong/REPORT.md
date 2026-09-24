# 1.8.0 release-candidate qualification

> Historical September 23 candidate evidence. See the separate [fresh local
> Docker release campaign](RELEASE-1.8.0.md) for the release measurements.
> The numeric results below remain unchanged.

Investigated 2026-09-23. This report describes the current source and a
locally built Linux arm64 candidate. The earlier
[investigation](../investigation/REPORT.md) and generated
[historical benchmark page](../../BENCHMARKS.md) describe a **different 1.6.0
image**; their measurements have not been replaced or pooled here.

## Source, build, and image identity

The candidate began at commit
`a4237c29f00aa819ad28d5ea94d3056026809bdb`, with task-owned changes to
the Docker build, flash metrics, tests, documentation, and harness. The
benchmark manifests record the complete dirty-tree status and SHA-256 of
runtime-relevant source files. The local runtime image is
`cachelib-candidate:1.8.0`, image ID
`sha256:ddf4a50f1612cc30bd20606f2018923707979141ddbbd609ddce70ac267de4a0`,
Linux arm64, reporting version 1.8.0. Its OCI revision label names the **base
commit**, not the full dirty tree. The measured source fingerprint is
`e373c4966f00acfa0f086ceaa2a4794ea70b66520677de860c1d78746ea6af2d`;
the candidate server executable SHA-256 is
`da20c751e007cbebd301a2625d3b4488f2349a9f1caea7b1f992f54af9c524cf`.
The manifest is the authority for the uncommitted changes. Release images
will be rebuilt from the final tag commit and labeled with that exact commit.
Binary equality or any post-release rerun must be checked before describing
the published image as the measured artifact.
After the final test-only and benchmark-harness changes, a fresh local
`cachelib-candidate:final` build had a different image ID
(`sha256:a15c6e33d7fb5c03835d4b581cb344bff00a71ce5896b6568d022dbdfa777048`)
but the **same executable SHA-256** shown above. That is a local binary
equality check, not a claim that a future CI-built release digest is equal.

The [Dockerfile](../../Dockerfile) uses Ubuntu 24.04 for build and runtime,
CacheLib commit `2aa2afbe97deadfb00f15260b26b566354c57a78`, gRPC v1.60.0
commit `0ef13a7555dbaadd4633399242524129eef5e231`, and its BoringSSL
submodule commit `2ff4b968a7e0cfee66d9f151cb95635b43dc1d5b`.
The measured arm64 builder reported GCC 13.3.0
(`13.3.0-6ubuntu2~24.04.1`), CMake 3.28.3, Ninja 1.11.1, and
`CMAKE_BUILD_TYPE=Release`.
The [dependency manifest](dependencies.arm64.json) records the exact
observed Folly, fbthrift, mvfst, Wangle, and Fizz checkout commits in that
builder, along with the executable checksum.
The historical gRPC Git submodule fetch failed before server compilation.
The build now verifies the gRPC commit, initializes only required Git
submodules, and fetches that exact BoringSSL tree as a SHA-256-checked archive
(`b21994a857a7aa6d5256ffe355c735ad4c286de44c6c81dfc04edc41a8feaeef`).
CacheLib's pinned xz-5.2.5 download moved to its official `www.tukaani.org`
URL while retaining the manifest's
`f6f4910fd033078738bd82bfba4f49219d03b17eb0794eb91efbae419f4aba10`
checksum. No upstream revision was replaced with an arbitrary latest one.
`getdeps.py` still resolves other dependencies from CacheLib's pinned-tree
manifests; OS package point versions can move, so this is a verified-input
build rather than a bit-reproducible binary build.
The timed KV and HTTP clients are compiled with `golang:1.23-bookworm` from
[go.mod](go.mod): grpc-go 1.68.1, go-redis 9.7.0, and a pinned
gomemcache revision. The separate correctness client uses pinned Python
grpcio 1.68.1, redis-py 5.2.1, and pymemcache 4.0.0 from its
[Dockerfile](../investigation/Dockerfile.client). Client and service image
IDs are recorded separately for every run series.
The client image was rebuilt between the 1 KiB, closed-loop capacity, and
scheduled-arrival series as the harness gained GET-denominator accounting and
a worker-independent offered trace. Those changes do not affect the original
read-only unary loop, but cross-series rates should not be pooled; each
within-series comparison uses one client image ID.

The full `tester` stage passed both registered CTest binaries on native
Docker Desktop arm64. The initially added flash-hit assertion failed and
identified a real wrapper bug: it read nonexistent `navy_gets` and
`navy_hits` names. Pinned CacheLib's `EnginePair` exports
`navy_lookups` and `navy_succ_lookups`. The mapping was corrected in
[CacheManager.cc](../../CacheManager.cc#L928), and the complete suite then
passed. Additive Stats fields 20–21 expose Navy device bytes read/written;
the legacy `nvm_used` field remains wire-compatible but means **cumulative
device bytes written**, not occupied flash capacity.

## Implementation audit

| Topic | Current implementation and implication |
|---|---|
| Engine and RAM | [CacheManager.h](../../CacheManager.h#L150) selects `LruAllocator`. [Initialization](../../CacheManager.cc#L126) sizes the hash table from configured DRAM (2^16–2^25 buckets), up to 1,024 hash locks, and creates one pool with CacheLib's default allocation classes and MMLru read refresh. Slab-class rounding/fragmentation and class-local eviction can reduce useful payload capacity. `--cache_size` is pool capacity, not process RSS or cgroup memory; the stats `used_size` omits hash table, gRPC, allocator metadata, and page cache. |
| Navy | [configureNvmCache](../../CacheManager.cc#L180) truncates a file at startup, sets reader/writer counts before async I/O, uses 4 KiB blocks and 1 MiB max writes, 10% BigHash for ≤2 KiB items, 16 MiB checksummed BlockCache regions for larger items, and admission probability 1.0. CacheLib handles eviction into Navy and lookup/promotion; this wrapper exposes no admission or promotion policy control. The file is a cache tier, not a recovery guarantee. The default image builds Navy without io_uring; local logs show libaio. |
| Copying | [GET](../../CacheManager.cc#L253) copies CacheLib item memory into a string, and the [handler](../../CacheServiceImpl.cc#L47) copies into protobuf. [SET](../../CacheManager.cc#L304) allocates an item and copies decoded bytes into it. Full-object protocol encoding and network buffers add more work. No engine-only CacheLib result can be assigned to this service. |
| API and ordering | [Protocol](../../proto/cache.proto) exposes 19 RPCs. [MultiGet/MultiSet](../../CacheManager.cc#L384) loop over individual operations; they are not atomic. [Pipeline](../../CacheServiceImpl.cc#L517) reads, executes, and writes one request at a time within each stream, echoing `sequence_id`; concurrent streams can run separately. It is request/response caching, not publish/subscribe, media frames, or multicast. |
| Limits and semantics | [gRPC limits](../../server.cc#L171) apply to the **whole** serialized request/response: default 4,195,328 bytes. Default CacheLib slab/header/key overhead makes the maximum value smaller than 4 MiB; 255-byte keys worked, 256-byte keys failed in the candidate smoke. Two individually valid near-limit values in one MultiSet/MultiGet exceeded the RPC limit. A miss is `found=false`; an oversized SET normally returns `success=false`, while an oversized RPC is `RESOURCE_EXHAUSTED`. TTL is second-granular. |
| Hybrid API gaps | [Scan and Flush](../../CacheManager.cc#L812) walk DRAM only, and `Delete.key_existed` reports DRAM residency even when a flash-only key was removed. `FlushRequest.include_nvm` is unimplemented. No upstream CacheLib policies, per-pool controls, warm restart, or structured items are exposed merely because the engine has them. |
| Concurrency | Each unary call can run on a gRPC server thread and separate Pipeline streams can overlap, but operations within one stream are sequential. [GET/SET](../../CacheManager.cc#L253) update global sequentially consistent counters; counter/CAS operations use a shared mutex for their atomic contract. Thus a read hit and a contended counter update exercise different concurrency paths. |
| Operations and security | [server.cc](../../server.cc#L159) registers a synchronous gRPC service with insecure credentials. [SECURITY.md](../../SECURITY.md) confirms no authentication, ACL, or TLS. This wrapper has no replication, sharding, failover, durability, origin integration, or media transport; a caller able to reach it can invoke administrative RPCs. Redis/Valkey operational features are a real product difference. |

[Set](../../CacheManager.cc#L303) replaces the whole item; a zero or negative
TTL is normalized to no expiry, so overwriting without a TTL does not retain
the old expiry. The handler casts positive signed TTLs to `uint32_t` without
a range check, so values above that range wrap; the benchmark uses no expiry
except dedicated short-TTL correctness probes. An expired GET returns a miss.
[Delete](../../CacheServiceImpl.cc#L92)
reports successful execution even for an absent key, while `key_existed`
indicates DRAM residency in hybrid mode. `MultiSet` loops over items and
[drops empty-key entries](../../CacheServiceImpl.cc#L186) without counting
them as failures; it is not an atomic all-or-nothing operation. These are
the wrapper's observable semantics, irrespective of broader upstream
CacheLib features.

The final native arm64 [smoke result](runs/rc-smoke-final-client.json)
checked binary round trips, misses, overwrite, deletion, TTL, batch
partial errors, Pipeline sequence correlation, and
[request limits](runs/rc-smoke-final-client-limits.json).
With 64 MiB DRAM, a 512 MiB Navy file and 4,096 distinct 64 KiB values,
there were 3,157 DRAM evictions, four byte-verified flash-resident reads,
five additional Navy hits, 348,160 Navy device bytes read, and 201,326,592
Navy device bytes written. The flash-resident TTL probe expired. **Measured
in this repository:** the logical DRAM+file-backed flash path works. **Not
measured:** physical SSD latency, IOPS, endurance, or NAND write amplification;
Docker Desktop and the OS page cache lie between Navy and host storage.
The flash-resident probes use values above Navy's 2 KiB BigHash cutoff and
therefore establish BlockCache operation. A small-object BigHash eviction and
read-back workload has not been separately qualified; enabling BigHash in
configuration alone is not evidence of its measured benefit.

## Comparators and published evidence

The tested containers report Redis Open Source 8.2.10, Valkey 8.1.10,
Memcached 1.6.45, Memcached
extstore, and NGINX HTTP caching. Image digests and runtime-reported versions
are in each run manifest and correctness file. Redis/Valkey use allkeys-LRU
with RDB/AOF, replication, TLS, and compression off; Memcached uses four
worker threads and a 5 MiB item limit. NGINX is an HTTP delivery cache, not
a remote KV API; its comparison uses a common HTTP client and includes the
entire Python HTTP-to-gRPC adapter path. Redis Software/Cloud Flex is
documentation-only: no accessible commercial deployment or subscription was
used.

[Redis's official persistence documentation](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/)
describes RDB snapshots and AOF write logging as recovery options; neither
is active RAM+flash capacity tiering. [Redis Flex documentation](https://redis.io/docs/latest/operate/rs/flex/)
describes an actual RAM+SSD tier and key/value offloading. It is wrong to say
that all Redis products require the whole dataset in RAM.
[Memcached's extstore documentation](https://docs.memcached.org/features/flashstorage/)
describes keys/metadata in RAM and eligible values on flash, including write
and endurance tradeoffs. [NGINX's proxy-cache documentation](https://nginx.org/en/docs/http/ngx_http_proxy_module.html)
describes an HTTP disk cache, a more natural baseline for media delivery.

The [CacheLib OSDI 2020 paper](https://www.usenix.org/system/files/osdi20-berg.pdf)
reports in-process CacheLib/modified-Memcached comparisons and a separate
CacheLib FastCGI+NGINX HTTP system on Meta workloads; neither experiment
measures this gRPC wrapper. The earlier [investigation's source table](../investigation/REPORT.md#comparators-and-external-evidence)
records versions, hardware, object distributions, settings, authorship, and
the missing controls for those published results. CacheBench's
[official workload guide](https://cachelib.org/docs/Cache_Library_User_Guides/Cachebench_FB_HW_eval/)
is useful for trace design, not a performance ranking for this service.
Redis Flex vendor claims were not transcribed into local results.
Dragonfly is another remote-cache candidate but was not installed in this
bounded matrix; its vendor-produced Redis-protocol benchmarks are not
evidence about this gRPC service. RocksDB is an embedded database, not an
equivalent remote cache without an additional service and eviction/origin
policy. An in-process CacheLib application can avoid this wrapper's network
and serialization costs but is a different product category.

## Hypotheses and method

H1 asks whether this complete gRPC service differs from Redis for all-RAM
hits; a reproducible latency, throughput, CPU, or memory difference supports
it, without automatically identifying which engine/protocol/copy caused it.
H2 asks whether verified Navy capacity improves retention under equal
configured RAM and **additional reported flash**; Navy hits and fewer origin
misses support it. H3 asks whether that hit gain outweighs slower flash-hit
service time in end-to-end latency at disclosed origin costs. H4 asks whether
batch and sustained Pipeline help unary gRPC and how they compare with
Redis pipelines and Memcached native multiget. H5 asks whether reusable
media objects help while one-pass content does not. A null or adverse result
rejects a universal advantage.

The [KV harness](README.md) uses compiled Go clients, one persistent connection
per worker, the same deterministic access formula and binary payloads, an internal
Docker network, fresh sequential service containers, no public ports,
separate server CPUs 0–3 and client CPUs 4–7, four CPU/768 MiB service and
four CPU/1,024 MiB client limits, 96 MiB configured cache, and optional
512 MiB task-owned flash files. Swap is disabled by setting each container's
memory-swap ceiling equal to its memory ceiling. The Navy runs configure
four reader and four writer threads; Memcached uses four worker threads,
while Redis/Valkey use their default command execution model. The exact
launch commands are stored with every run. Principal tests warm up for at
least 15 s, check three five-second throughput windows for ≤10% spread, measure for at
least 60 s, and repeat five times with shuffled engine order. Every
repetition, including errors and outliers, is retained. Closed-loop latency
starts when the prior operation completes and therefore does not represent
all possible arrival processes; offered-load probes timestamp scheduled
arrivals, include queue delay, and report dropped arrivals and scheduler lag.
Batch throughput counts **objects**, while latency is per batch or per
16-operation Pipeline window. Payload MiB/s counts verified returned bytes.
Per-run p99.9 is shown only with at least 10,000 latency samples.
Server cgroup CPU snapshots are taken just after the client's measurement
marker and just after it exits; the recorded UTC snapshot times bound that
small sampling offset. CPU μs/object is therefore an approximate service
cost, not a cycle-accurate per-request profile.

For capacity tests, 2,048 × 64 KiB reusable objects form a 128 MiB logical
working set above configured DRAM. The KV origin is an explicitly **simulated**
fixed 1, 5, or 20 ms wait on a miss plus cache fill; origin counts and bytes
avoided are measured in this model, not WAN traffic or money saved. Fixed-size
objects make object and byte hit ratios equal. Hybrid comparisons hold
configured RAM constant and disclose 512 MiB additional flash; they do not
hold total resources constant. Cgroup current/peak memory and CPU include
process and charged file pages. Navy/extstore device counters are logical
file I/O; Docker Desktop page caching prevents a physical-SSD claim.
Closed-loop engines complete different request counts in the same minute,
so their per-worker deterministic traces end at different prefixes. Their
absolute origin counts are not equal-demand comparisons; hit ratios and
origin requests per 1,000 operations are comparable. A separate
scheduled-arrival run uses one global access sequence and common offered
rate to compare absolute origin traffic under equal demand.

## Measured all-RAM result: the former favorable case did not repeat

The [raw runs](runs/rc-primary-c8/) and generated
[CSV](release-results/primary/runs.csv),
[JSON](release-results/primary/summary.json), and
[chart](release-results/primary/throughput.svg) retain five independent
60-second repetitions per engine. The case is 1,000 preloaded keys with
1 KiB binary values, eight persistent connections, read-only uniform
closed-loop access, 96 MiB configured cache, and a 768 MiB/4-CPU service
limit. All 20 runs passed correctness, had zero errors, and met the warmup
stability rule. These are complete remote-service/client measurements, not
an isolated CacheLib engine benchmark.

<!-- primary-table:start -->
| Engine | Five objects/s runs, in repetition order | Median objects/s | Median p99 ms | Median server CPU μs/object | Median cgroup peak MiB |
|---|---|---:|---:|---:|---:|
| CacheLib gRPC 1.8.0 candidate | 44,312; 44,322; 42,427; 42,078; 42,211 | 42,427 | 0.508 | 64.8 | 26.8 |
| Redis OSS 8.2.10 | 112,745; 109,191; 108,869; 141,171; 109,464 | 109,464 | 0.123 | 6.8 | 13.2 |
| Valkey 8.1.10 | 111,724; 109,322; 109,188; 108,910; 104,196 | 109,188 | 0.123 | 6.9 | 12.0 |
| Memcached 1.6.45 | 103,557; 104,674; 105,072; 103,063; 103,034 | 103,557 | 0.133 | 12.6 | 7.4 |
<!-- primary-table:end -->

**Measured in this repository:** this gRPC build was slower than Redis in
all five paired repetitions. Its median rate was 61.2% lower using the
ratio of group medians (42,427 versus 109,464 objects/s), while the median
of paired percentage differences was 61.0% lower. Its median p99 was
0.508 versus 0.123 ms. Redis's 141,171 run is retained; its five-run
range was 108,869–141,171, so a narrow precision claim would be false.
The five paired percentage differences were −60.7%, −59.4%, −61.0%, −70.2%,
and −61.4%; a seeded bootstrap of their median gives a descriptive 95%
interval from −70.2% to −59.4%, still below zero but wide because five runs
cannot establish fine precision.
The seeded bootstrap 95% interval for a five-run median is
42,078–44,322 for gRPC and 108,869–141,171 for Redis; with only five
runs these are descriptive intervals, not strong population coverage.
Verified response payload throughput was 41.4 versus 106.9 MiB/s at
the respective medians. The Go client used a median 1.8 gRPC and 1.9 Redis
CPU cores out of four allowed; that argues against the *shared Python GIL*
explanation for the old result, but does not prove that either service
reached its maximum throughput. The server CPU figures include wrapper,
protocol, and engine work. No profiler isolated their individual costs.
The 1,000 values contain 0.977 MiB of useful payload; relative to median
server cgroup peak, that is about 3.65% for gRPC and 7.37% for Redis
(Memcached: 13.23%). This small, fully resident dataset is dominated by fixed
process and allocator overhead; those percentages must not be extrapolated
to a full cache or used as a general memory-efficiency ranking.

**Unsupported/contradicted:** the earlier approximately 0.3-second Python
8-worker gRPC-over-Redis ordering was exploratory, unreplicated, and on a
historical 1.6.0 image. It cannot be carried into a 1.8.0 release claim.

## Exploratory size, concurrency, and write-mix sweep

The [30 raw runs](runs/rc-exploratory-kv/), generated
[CSV](release-results/exploratory-kv/runs.csv),
[JSON](release-results/exploratory-kv/summary.json), and
[throughput chart](release-results/exploratory-kv/throughput.svg) use one
10-second measurement per engine/case after a stable 15-second warmup.
They are **single-run context**, not repeated statistical claims. Every run
passed full-byte response checks, reported zero errors, and met the warmup
stability rule. The read-heavy and mixed cases use a skewed key distribution
and 10% and 50% writes, respectively; the other cases are read-only uniform
hits. All use 4 CPU/768 MiB service and 4 CPU/1,024 MiB client ceilings,
96 MiB configured cache, and persistent connections.

The gRPC candidate was below both Redis and Memcached in successful
objects/s in all ten sampled cases. Representative gRPC/Redis rates were
40,876/108,484 at 100 B, 30,723/90,999 at 16 KiB,
20,849/39,335 at 64 KiB, 7,847/12,110 at 256 KiB, and
2,055/2,884 at 1 MiB (the last uses four rather than eight connections).
At 1 KiB, one, four, and sixteen connections gave gRPC/Redis rates of
8,796/18,138, 23,005/60,545, and 58,875/207,976. The read-heavy skewed
case was 39,551/108,122 and the 50%-write mixed case 39,697/108,599.
These runs show the previous favorable ordering does not reappear at nearby
concurrency or payload sizes, but they do not establish precise population
rankings for each dimension. The compiled client drove Memcached to
257,609 objects/s at 16 connections, well above the gRPC rate; this
demonstrates generator capacity for the observed gRPC result, although
closed-loop connection count still bounds each measured workload.

## Exploratory native batch and sustained Pipeline modes

The [batch GET](runs/rc-batch-get/), [batch SET](runs/rc-batch-set/), and
[sustained Pipeline](runs/rc-pipeline/) series each contain two independent
20-second repetitions after stable warmup. Generated
[GET](release-results/batch-get/runs.csv),
[SET](release-results/batch-set/runs.csv), and
[Pipeline](release-results/pipeline/runs.csv) CSVs and adjacent JSON/charts
retain both runs. Each batch requests or writes **16 objects**; rates below
are objects/s, while p99 is **per 16-object batch or window**, not per
object. Redis uses a 16-command native pipeline and Memcached its native
multiget; the gRPC MultiGet/MultiSet wrappers loop over items. The gRPC
Pipeline client opens eight streams before warmup and keeps them open
through the full measurement interval, sending and correlating 16 requests
per window. Per-stream creation took 0.17–0.99 ms across the two runs and
is reported separately, outside timed throughput. All 12 runs had zero
errors and stable warmup.

Median two-run GET object rates were 422,464 for gRPC MultiGet,
1,016,357 for Redis pipelining, and 1,011,507 for Memcached multiget;
median batch p99 values were 1.75, 0.34, and 1.12 ms. Median SET rates
were 469,538 for gRPC MultiSet and 891,933 for Redis pipelining, with
1.46 versus 0.31 ms batch p99. The held-open gRPC Pipeline delivered
223,830 GET objects/s with 2.11 ms median window p99, below its own
MultiGet rate. **Measured in this repository, exploratory:** batching
substantially raises this wrapper's object rate relative to its unary c8
measurement, but Redis and Memcached also benefit. Pipeline did not
outperform MultiGet in these two runs. These are different API paths and
only two repetitions, so the rates are not a universal ranking or a causal
measurement of serialization or CacheLib locks.

## Measured reusable 128 MiB set under 96 MiB configured DRAM

The [30 raw runs](runs/rc-hybrid-5ms/) and generated
[CSV](release-results/hybrid/runs.csv),
[JSON](release-results/hybrid/summary.json), and
[hit-ratio chart](release-results/hybrid/hit-ratio.svg) cover five independent
60-second repetitions of six modes. Each starts a fresh service, preloads the
same 2,048 × 64 KiB binary objects, warms for at least 15 seconds, and uses
eight persistent connections with uniform closed-loop requests. A miss waits
5 ms at a simulated origin and fills the cache. Every service has a 4-CPU,
768 MiB cgroup ceiling and 96 MiB configured cache. Navy and extstore also
get a 512 MiB file-tier limit. All 30 runs passed byte verification, reported
zero request errors, and met the warmup stability rule.

<!-- hybrid-table:start -->
| Mode | Five objects/s runs | Median objects/s | Object/byte hit ratio | Median p99 ms | Server CPU μs/object | Cgroup peak MiB |
|---|---|---:|---:|---:|---:|---:|
| CacheLib gRPC DRAM | 3,429; 3,293; 3,290; 3,294; 3,283 | 3,293 | 64.6% | 7.98 | 226.6 | 112.9 |
| CacheLib gRPC + Navy | 14,732; 15,253; 15,235; 15,050; 15,219 | 15,219 | 100% | 2.18 | 178.9 | 275.3 |
| Redis OSS | 2,651; 2,314; 2,394; 2,352; 2,336 | 2,352 | 58.6% | 10.01 | 109.9 | 95.1 |
| Valkey | 2,589; 2,411; 2,354; 2,362; 2,347 | 2,362 | 58.6% | 9.93 | 110.9 | 93.9 |
| Memcached DRAM | 3,761; 3,749; 3,783; 3,734; 3,758 | 3,758 | 69.0% | 7.98 | 101.6 | 101.8 |
| Memcached extstore | 44,024; 43,969; 44,245; 44,455; 44,917 | 44,245 | 100% | 0.80 | 45.5 | 179.4 |
<!-- hybrid-table:end -->

**Measured in this repository:** with an added file tier and this fixed 5 ms
origin model, Navy removed all measured origin misses after warmup, versus
354 misses per 1,000 GETs for gRPC DRAM-only and 414 for Redis. Navy's median
rate was 4.62× its DRAM-only rate (15,219 versus 3,293 objects/s) and 6.47×
Redis's (versus 2,352); its median p99 was 2.18 versus 7.98 and 10.01 ms,
respectively. This is an **application-level hit-rate advantage with extra
capacity**, not evidence that a Navy lookup is faster than a RAM lookup or
that this gRPC engine is intrinsically faster than Redis. Different engines
complete different request totals in a closed-loop minute: the median
DRAM-only gRPC run made 70,020 origin requests and Redis 58,378, but those
absolute counts have unequal denominators. The scheduled-arrival equal-demand
probe is reported separately below.

**Measured loss:** extstore also removed all measured origin misses and
reached 44,245 objects/s, 2.91× Navy's rate, with a 0.80 versus 2.18 ms
median p99 and 45.5 versus 178.9 server CPU μs/object. This trace is not a
CacheLib win over the directly relevant flash-backed comparator. Navy
reported a median 320,662 flash hits and 20.47 GiB of logical Navy device
reads per measurement interval; extstore reported 908,077 flash-object reads
and 55.51 GiB of logical extstore reads. A median 35.1% of Navy GETs were
successful flash lookups, leaving an inferred 64.9% DRAM-hit share. The
inference subtracts successful Navy lookups from verified total hits; it is
not a separate allocator counter. Navy's cumulative device-write counter
was already 128 MiB after preload/warmup and did not advance during
the measured interval; extstore wrote a median 26.5 MiB during measurement.
These are file-device counters, **not** physical NAND bytes or write
amplification. The cgroup `io.stat` block-I/O deltas were zero even in the
Navy and extstore runs, so that interface gives no usable host-device IOPS
or utilization measurement in this Docker Desktop setup. The Navy backing
file occupied 512 MiB in these runs, while
extstore's allocated file space was about 80 MiB. The service cgroup charged
roughly 275 MiB at peak for Navy versus 179 MiB for extstore and 95 MiB for
Redis. Equal configured 96 MiB cache is plainly **not equal actual RAM or
total storage** here; the extra resource use is part of the result.
The 128 MiB of repeatedly accessible payload is about 46.5% of Navy's peak
cgroup memory and 71.4% of extstore's, before counting their additional
allocated file space. These ratios describe this working set and Docker
accounting, not intrinsic allocator efficiency.
The compiled client used a median 1.39 CPU cores for Navy and 2.03 for
extstore out of four permitted. That makes a shared Python GIL explanation
inapplicable to this KV comparison, but client implementation and protocol
costs still belong to the end-to-end result.

## Measured equal-demand origin traffic at 1,500 arrivals/s

The [offered-load runs](runs/rc-offered-origin/), generated
[CSV](release-results/offered/runs.csv),
[JSON](release-results/offered/summary.json), and
[hit-ratio chart](release-results/offered/hit-ratio.svg) schedule one global uniform
trace at 1,500 64 KiB GETs/s for 60 seconds, repeated five times per mode.
All four modes completed about 90,000 operations per run with zero errors,
zero dropped arrivals, and stable warmup. The compiled client's median CPU
use stayed below 0.52 of four allowed cores. Mean scheduling lag was roughly
0.6–0.8 ms, and latency starts at the scheduled due time, so queueing and
client wakeup delay remain in the reported percentiles. The backing origin
is still a fixed 5 ms **simulation**, not a network origin.

<!-- offered-table:start -->
| Mode | Five origin-request counts | Median origin requests | Median p99 ms | Median cgroup peak MiB |
|---|---|---:|---:|---:|
| CacheLib gRPC DRAM | 31,800; 31,790; 31,733; 31,795; 31,802 | 31,795 | 9.64 | 111.7 |
| CacheLib gRPC + Navy | 0; 0; 0; 0; 0 | 0 | 3.13 | 254.8 |
| Redis OSS | 37,505; 37,327; 37,369; 37,453; 37,439 | 37,439 | 9.89 | 94.9 |
| Memcached extstore | 4; 9; 4; 9; 9 | 9 | 2.99 | 150.7 |
<!-- offered-table:end -->

**Measured in this repository:** under exactly this equal offered demand,
Navy avoided all measured origin requests versus Redis's median 37,439
(2.285 GiB of 64 KiB bodies) and DRAM-only CacheLib's 31,795. Its median
due-time p99 was 3.13 versus Redis's 9.89 ms, a 68.4% reduction in this
simulation. Extstore needed a median nine origin requests and its 2.99 ms
p99 was slightly lower than Navy's; their per-run p99 values overlap, so
that 0.14 ms difference is not a robust tail-latency ranking. At a fixed
1,500-arrival ceiling, all modes reported approximately 1,500 objects/s:
this test measures origin traffic and latency, **not maximum throughput**.
The median service CPU cost was about 386 μs/object for Navy, 172 for
extstore, and 134 for Redis; Redis's rate includes miss fills, so these are
whole-service costs for the stated mix, not pure lookup microbenchmarks.
Navy's larger actual RAM charge and 512 MiB file remain part of the cost.

## Controlled 1 KiB offered load near saturation (exploratory)

The [20k/50k runs](runs/rc-offered-saturation/) and separate
[80k probe](runs/rc-offered-80k/) use a compiled client, 16 persistent
connections at 50k/80k (eight at 20k), the same 1,000 preloaded 1 KiB
values, and due-time latency that includes queueing. The generated
[20k/50k CSV](release-results/offered-saturation/runs.csv) and
[80k CSV](release-results/offered-80k/runs.csv) retain one 20-second run
per engine/rate. These are **single-run saturation probes**, not headline
repeated comparisons. At 20k and 50k, both paths completed all scheduled
operations with zero errors and drops. At 20k, gRPC/Redis due-time p99 was
1.93/2.35 ms; at 50k it was 7.91/1.98 ms. This reversal is a concrete
reason not to project one concurrency point into a universal latency rank.

At 80k offered arrivals/s, Redis completed all 1.6 million scheduled
operations with 1.997 ms p99 and zero drops. The gRPC path completed
1,289,095, **dropped 310,905 arrivals (19.4%)**, reported about 62,519
successful objects/s over its measurement-plus-drain interval, and had
669 ms due-time p99 among completed operations. The harness deliberately
marks that run as a failure in its summary; it is retained, not hidden.
The compiled client used about 2.4 of its four CPU cores for gRPC and 1.3
for Redis in the 80k probe. Mean scheduler lag was about 0.11 ms for each;
the same generator delivered 80k to Redis. These facts support saturation
of the **complete gRPC service path** under this workload, not a precise
engine-only maximum or a diagnosis of the internal bottleneck. A profile
would be required before blaming protobuf, copies, locks, or CacheLib.

## Origin-cost and access-pattern sensitivity (exploratory)

The [origin-delay](runs/rc-origin-sensitivity/) and
[access-pattern](runs/rc-origin-patterns/) runs each have **one 10-second
measurement per case/mode**, with generated
[delay CSV](release-results/origin-sensitivity/runs.csv) and
[pattern CSV](release-results/origin-patterns/runs.csv) plus adjacent JSON
and charts. They are mechanism probes, not statistical claims. All 28 runs
had zero errors. Cold runs deliberately have no preload or warmup and are
therefore not marked stable or pooled with warmed runs. Other runs passed
the warmup stability rule. Configured cache RAM stays at 96 MiB, service
budget at 4 CPU/768 MiB, and Navy/extstore add a 512 MiB file tier.

Changing the simulated miss delay from 1 to 20 ms left the warmed Navy
trace at 100% hits and about 13.8k objects/s; Redis stayed near 59% hits
and went from 7,472 to 770 objects/s. Extstore also held 100% hits and
delivered about 32k objects/s at either delay. This directly supports a
conditional **application-level** origin-avoidance benefit; it does not
show SSD hits beating RAM hits, and the origin wait is an assumption.

Near configured DRAM capacity (1,400 × 64 KiB = 87.5 MiB payload),
DRAM-only CacheLib had 94.4% hits and Redis 85.8%, while Navy and extstore
had 100%. Their different process overhead and actual cgroup peaks
(111.9, 95.4, 255.1, and 107.6 MiB respectively for CacheLib DRAM,
Redis, Navy, and extstore) prevent treating this as equal real RAM. On a
skewed 128 MiB set, DRAM CacheLib/Redis hits rose to 78.1%/75.9%; both
hybrid modes again had 100% hits but extstore still served more objects/s
than Navy (32,277 versus 15,908). An uncached cold start caused about
2,060 Navy and 2,069 extstore origin fills before high reuse developed;
the cold ten seconds cannot be compared to the warmed steady-state rates.

In the changing-hot-set case, each hot tenth fits in DRAM. The set switch
caused about 215–226 origin requests across modes, then nearly all requests
hit. DRAM-only CacheLib and Navy ran at 20,019 and 19,925 objects/s; extra
flash capacity did not help that short trace. The one-pass trace had
**zero cache hits in all four modes**. DRAM-only CacheLib and Navy ran at
541 and 543 objects/s; Navy charged 309.1 versus 110.0 MiB peak cgroup
memory and reported roughly 336 MiB each of logical Navy device reads
and writes. Extstore reached 616 objects/s but charged 628.7 MiB peak and
reported about 367 MiB written to its file. These are one-run costs,
affected by page cache and file-allocation policy, but there is no measured
origin traffic avoided by either flash tier in one-pass content.

## Equal full-hit objective under a 384 MiB service ceiling

The 128 MiB reusable working set can also be kept entirely in DRAM by
raising configured cache capacity from 96 to 192 MiB. We therefore measured
five additional 60-second runs of each RAM-only mode and of Navy/extstore
with the original 96 MiB DRAM configuration, all under a **common 4 CPU /
384 MiB service cgroup ceiling**. This is a service-objective comparison:
every mode targets a warmed 100% hit rate with no simulated origin request.
Navy and extstore still require a separate 512 MiB file allowance. The
192 MiB RAM modes have no flash tier; equal cgroup ceilings are not equal
actual memory, storage, or cost. These were separate run series rather than
one interleaved engine schedule, so cross-series rates are descriptive.

<!-- objective-table:start -->
| Mode | Five objects/s runs | Median objects/s | Median p99 ms | Server CPU μs/object | Cgroup peak MiB |
|---|---|---:|---:|---:|---:|
| CacheLib gRPC, 192 MiB DRAM | 22,439; 22,276; 22,291; 22,303; 22,414 | 22,303 | 1.81 | 96.1 | 162.3 |
| CacheLib gRPC, 96 MiB DRAM + Navy | 14,704; 14,747; 14,918; 14,478; 14,957 | 14,747 | 2.16 | 184.1 | 278.9 |
| Redis OSS, 192 MiB DRAM | 53,999; 57,600; 52,606; 52,825; 52,475 | 52,825 | 0.62 | 16.6 | 151.1 |
| Memcached, 192 MiB DRAM | 65,754; 65,282; 65,518; 65,237; 65,292 | 65,292 | 0.66 | 24.7 | 145.2 |
| Memcached, 96 MiB DRAM + extstore | 44,369; 43,889; 43,427; 43,919; 43,980 | 43,919 | 0.80 | 46.0 | 186.9 |
<!-- objective-table:end -->

All runs and cgroup samples are retained in
[RAM-only alternatives](runs/rc-ram192-objective/),
[CacheLib DRAM](runs/rc-grpc-ram192-objective/),
[Navy](runs/rc-nvm384-objective/), and
[extstore](runs/rc-extstore384-objective/). The corresponding generated CSVs
and JSON summaries live under [release-results](release-results/).
The table is generated from those five-run CSVs by
[render_report_tables.py](render_report_tables.py).
All 25 runs passed full-byte correctness, had zero reported errors or origin
requests, and met the warmup stability criterion. **Measured in this
repository:** once the working set was resident, Redis's median 52,825
objects/s was 3.58× Navy's 14,747, with 0.62 versus 2.16 ms median p99.
Memcached DRAM reached 65,292 objects/s and extstore reached 43,919;
extstore was 2.98× Navy's rate while charging about 186.9 versus 278.9 MiB
at peak. Even the same gRPC service with enough DRAM reached 22,303
objects/s, 1.51× its Navy configuration. The Navy trace is therefore useful
where its extra retention avoids a sufficiently expensive origin at a
constrained **cache-size configuration**. This experiment does not show an
advantage when the alternative can use more RAM within the same 384 MiB
service ceiling. An actual total-cost or physical-SSD comparison would need
hardware price, page-cache, and endurance measurements unavailable here.

## Media-object scope

**Supported by implementation inspection:** this service can hold and return
binary encoded objects or segments within its whole-value and whole-RPC
limits. An online course, concert replay, podcast, or reusable text-to-speech
segment with repeated readers can be keyed and cached. Recent-content replay
and seeks can also reuse those objects if a surrounding application defines
segment names, TTLs, and origin refill. Thumbnails, previews, manifests, and
metadata are straightforward small-object cache candidates. The server does
not itself serve HTTP or byte ranges; a media application must supply an
adapter or another delivery layer.

**Measured in this repository:** repeated content can reduce origin
requests, while fresh one-pass media incurs lookup/fill/eviction work with
little reuse. The [common-HTTP harness](run_http.py) uses 256 KiB and 1 MiB
key-specific binary objects, multiple readers, a separate 5 ms simulated
origin, an NGINX disk-cache path, and a Python HTTP-to-gRPC path. Both paths
receive the same 2 CPU/768 MiB cache-path budget; the gRPC path splits it
between adapter and cache process. Its compiled client verifies complete
bodies and object identity. NGINX uses two workers and a 512 MiB bind-mounted cache file
budget while the repeated test's full logical working set fits the gRPC
path's 96 MiB DRAM. The direct-origin one-pass probe is a separate reference,
not an equal-resource cache comparison.
The HTTP client has a separate 4-CPU/1,024-MiB cgroup on CPUs 2–5, while
the cache path uses CPUs 0–1 and the common origin uses CPUs 6–7.
The HTTP origin is a separate local service that generates and transfers
key-specific bodies after a 5 ms sleep; it is a different model from the KV
client-side sleep, so their numeric rates should not be combined.

The [five full-length repetitions](runs/rc-http/) for a warmed, repeated
64-object × 256 KiB set produced this [generated CSV](release-results/http/runs.csv),
[JSON](release-results/http/summary.json), and
[chart](release-results/http/throughput.svg). Every returned body was checked
byte-for-byte against its key-specific expected value; all ten runs had zero
errors and zero origin requests during measurement. The gRPC path comprises
both the Python HTTP adapter and CacheLib service. For that two-container
path, the peak column is the **sum of component cgroup peaks**, an upper
bound on a simultaneous combined peak. The individual peaks and CPU costs
are in the CSV. NGINX has one directly observed cgroup peak. Its repeated
16 MiB object set can be served from page cache; this experiment does not
measure cold physical-disk reads.

<!-- http-table:start -->
| HTTP path | Five objects/s runs | Median objects/s | Median p99 ms | Path CPU μs/object | Path cgroup peak sum MiB |
|---|---|---:|---:|---:|---:|
| CacheLib gRPC + Python HTTP adapter | 2,063; 2,020; 2,094; 2,105; 2,118 | 2,094 | 10.68 | 533.5 | 102.4 |
| NGINX disk cache | 2,460; 2,503; 2,556; 2,547; 2,488 | 2,503 | 10.18 | 201.4 | 24.4 |
<!-- http-table:end -->

Here an object/s is one complete HTTP response, not a gRPC RPC. These
five repeated-hit runs used the original adapter image and NGINX config
archived in [provenance](provenance/README.md); neither path requested the
origin during measurement, so the later origin-connection changes do not
enter this timed result. Results from the two configurations are not pooled.

**Measured loss:** relative to NGINX's median 2,503 objects/s, the complete
gRPC HTTP path's median 2,094 was 16.3% lower at the same two-CPU/768 MiB
path budget. Median p99 was 10.68 versus 10.18 ms. The gRPC path spent a
median 39.8 CPU seconds in the adapter and 27.4 in the cache process during
each 60-second interval; NGINX spent 30.4 CPU seconds. These measurements do
not isolate CacheLib engine cost from adapter, protocol, or copying. Both
paths served about 500–626 MiB/s of verified payload, depending on run.
The compiled HTTP client used about 0.88 CPU cores for the gRPC path and
1.06 for NGINX out of four allowed; neither result looks like a four-core
load-generator ceiling, though a more optimized adapter could change the
comparison.

The [short pilot](runs/pilot-http-current/) and its
[summary](release-results/http-pilot/summary.json) are retained as initial
exploration. Two later 30-second repetitions each of recent-content replay
and repeated 1 MiB objects are in the
[secondary raw runs](runs/rc-http-secondary/) and
[summary](release-results/http-secondary-before-reuse/summary.json).
In replay, both paths fetched exactly 32 new 256 KiB segments when all
readers switched to newer content, then reused older cached segments on
seek; median gRPC/NGINX rates were 1,977/2,413 objects/s, zero errors.
For repeated 1 MiB objects, medians were 602/752 objects/s, zero errors
and zero origin requests. These are two-run exploratory comparisons, not
five-run headline rankings.

The first longer one-pass attempt exposed a **benchmark adapter defect**:
it opened a new origin TCP connection on every miss, exhausted ephemeral
ports, and produced 888 and 215 errors in its two 30-second repetitions.
Those failures, original adapter, and original NGINX configuration are
[preserved](provenance/README.md), not discarded or recast as CacheLib
failures. The adapter was changed to reuse origin connections and NGINX
was given upstream keepalive; the server candidate binary did not change.
The corrected [two-run one-pass series](runs/rc-http-onepass-reuse/), its
[CSV](release-results/http-onepass-reuse/runs.csv),
[summary](release-results/http-onepass-reuse/summary.json), and
[chart](release-results/http-onepass-reuse/throughput.svg) have zero errors
and full-body verification. Every request reached the origin in all three
paths: median direct-origin, gRPC-plus-adapter, and NGINX rates were
855, 838, and 692 objects/s. Relative to direct origin, the two cache
paths were about 2.0% and 19.1% slower in this simulated 5 ms origin
model, with no traffic avoided. The direct path is a reference without
the same cache-path resource allocation, not an equal-resource cache.
NGINX's cache directory allocated a median ~1,440 MiB by the end of the
30-second churn runs despite its configured 512 MiB `max_size`; its cleanup
lagged this write burst. That is measured file allocation, not a physical
NAND-write or steady-state occupancy claim. The two-run results do not
establish general cache costs across origin networks or storage devices.
All of these are **media-object cache** tests, not playback tests.

A cache retains reusable objects. A playback/jitter buffer absorbs timing
variation near a player; a durable recording store preserves the recording;
a publish/subscribe service distributes events; a media server handles
HTTP/range or streaming delivery and often authorization/transcoding; a
streaming transport carries timed frames. `Pipeline` is a bidirectional
stream of ordered cache RPCs. It supplies none of those missing playback,
synchronization, recording, pub/sub, or transport semantics. No viewer
latency, dropped frame, audio continuity, or live-stream quality result is
measured in this project.

## Hypothesis outcomes and evidence labels

| Hypothesis | Evidence and status |
|---|---|
| H1: all-RAM service differs from Redis | **Measured in this repository:** five 60-second 1 KiB c8 repetitions show 42,427 versus 109,464 median objects/s and 0.508 versus 0.123 ms median p99 for gRPC/Redis. The ten-case single-run sweep found no gRPC-service win. Engine versus protocol contributions remain unisolated. |
| H2: a verified SSD tier retains more under fixed configured RAM | **Measured in this repository:** flash-resident binary reads, Navy counters, 100% warmed hybrid hits, and zero equal-demand origin requests versus 37,439 Redis requests at 96 MiB configured cache. The tier adds a 512 MiB file and more actual cgroup memory; this is logical file-tier evidence, not physical NAND proof. |
| H3: higher hit rate may outweigh slower tier hits | **Measured in this repository for a simulated origin:** Navy's p99 was 3.13 versus Redis's 9.89 ms under 1,500 common arrivals/s and 5 ms per origin miss. The one-run 1/20 ms probes show that origin cost changes the size of the benefit. A production origin-cost threshold is **hypothesis not yet validated**. |
| H4: batching/Pipeline changes overhead | **Measured in this repository, exploratory:** two-run 16-object MultiGet and MultiSet rates exceed unary gRPC, but Redis pipelining and Memcached multiget were faster. Held-open gRPC Pipeline was slower than MultiGet in the sampled trace. The short batch runs do not establish broad rankings. |
| H5: repeated media helps, one-pass content does not | **Measured in this repository:** five 60-second repeated 256 KiB HTTP runs had no measured origin requests; two corrected 30-second one-pass runs had zero hits and sent every request to origin. **Unsupported:** any claim about viewer experience or live-stream quality, because no playback was measured. |

## Operational limits and interpretation guardrails

**Supported by implementation inspection:** there is no built-in TLS, auth,
ACL, replication, sharding, automatic failover, warm restart, or durability.
Every image used here disables the competitors' TLS and persistence for an
ephemeral-cache comparison, but that does not erase those competitors'
operational options. Navy files are truncated at startup. Flash-only
`Scan`/`Flush`/`Delete.key_existed` caveats and a roughly 4 MiB value
ceiling constrain general media use. Application code must handle misses,
origin refill, object naming, and segmenting larger media.

**Measured here with limited portability:** rates and latencies include the
Go client, protocol libraries, Docker virtual network, container scheduler,
and service. CPU deltas and memory peaks use cgroups, not configured cache
size. The hardware is one Apple M2 Max laptop with Docker Desktop's LinuxKit
aarch64 VM, not a dedicated Linux storage server. Disk files sit on a host
APFS internal SSD through a VM, bind mount, and page cache. Navy and extstore
counters are useful logical I/O proxies; zero or nonzero cgroup `io.stat`
is not physical device telemetry. No physical SSD utilization, IOPS,
endurance, NAND write amplification, energy, dollar savings, WAN latency,
TLS cost, failover, or production behavior has been established.
Background host activity was not eliminated; shuffled engine order and
retained outliers mitigate, but do not remove, that single-machine source
of variability.

The KV load generator uses one deterministic binary payload for all keys in
a case, verifying every returned byte but not independently identifying a
wrong-key response during the timed loop. Its separate pre-timing correctness
suite checks key-specific binary round trips and Pipeline sequence IDs. The
new HTTP media experiment verifies both full bytes and object identity.
The KV origin is a fixed-delay model, so an observed origin reduction is
evidence for that model only; origin-cost sensitivity matters. A higher
hit ratio can improve end-to-end latency without making a flash hit faster
than a RAM hit. None of the sources or local numbers isolates a particular
copy, lock, protobuf, or CacheLib operation as the cause of a throughput gap;
a CPU profile or controlled ablation would be needed before claiming that.

## Engineering priorities and interview explanation

The strongest immediate improvement is to expose precise DRAM and Navy hit
counters and real tier occupancy with unambiguous
cumulative-versus-capacity names, then validate them against controlled
eviction traces. The wrong upstream Navy
counter names were discovered by a regression test here; operational decisions
based on the old zero hit counts would have been misleading. Make the
resource decision explicit: this host's 192 MiB RAM configurations served
the full reusable set faster and with lower measured cgroup memory than
Navy's 96 MiB plus file tier. A deployment study should price RAM and SSD,
measure physical I/O and endurance, and compare the complete cost before
choosing hybrid capacity. Make the flash-aware semantics of `Delete`, `Scan`,
and `Flush` explicit or implement
them across both tiers, reject TTL values that do not fit the actual expiry
type, and count rejected `MultiSet` items. For a larger media use case, a carefully bounded
chunk/range API would avoid whole-object copies and the roughly 4 MiB RPC
limit, but it needs correctness and resource tests before it is a claimed
performance improvement. Authentication, TLS, and deployment controls are
prerequisites to exposing this service outside a trusted private network.
Any future performance optimization should start with a profile of the
measured workload and a before/after result under the same harness.

An honest interview answer is: “I built a standalone gRPC wrapper around
Meta’s CacheLib for my Nakitte financial-content application, which collected
news and repeatedly served requested content. I wanted explicit control over
cache RAM and to explore CacheLib’s DRAM-plus-flash design. My contribution
was the service API, concurrency and cache-operation semantics, deployment
and tests, and the measurement work; Meta built the caching engine. The
measured all-RAM 1 KiB case is substantially slower than Redis in this
environment, so I would not pitch the wrapper as a universally faster Redis.
In a controlled 64 KiB reusable-object trace with 96 MiB configured RAM,
the verified Navy tier avoided origin requests and lowered end-to-end p99
against Redis under a simulated 5 ms origin. That required a 512 MiB file
and more actual RAM, and Memcached extstore was the faster flash-backed
alternative. It is a measured tradeoff in a developer VM, not a proven
production saving. Redis, Valkey, or Memcached remain simpler choices when
their feature set and memory economics fit.” This describes the engineering
contribution and observed result without claiming the project was originally
built for video streaming.

Turkish: “Nakitte’nin tekrar istenen finans haberlerini önbelleğe almak için
Meta CacheLib’in üzerine bağımsız bir gRPC servisi geliştirdim. Amacım RAM
kullanımını yönetmek ve DRAM+flash katmanını incelemekti; CacheLib motorunu
ben yazmadım. API, servis davranışı, testler ve ölçüm altyapısı benim
katkımdır. Bu ortamdaki RAM içi 1 KiB karşılaştırmasında Redis daha hızlı.
96 MiB ayarlı RAM ve 5 ms benzetimli kaynak gecikmesiyle yaptığım 64 KiB
nesne testinde Navy, kaynağa giden istekleri sıfırladı ve Redis'e göre uçtan
uca p99'u düşürdü; buna ek olarak 512 MiB dosya alanı ve daha fazla gerçek
RAM kullandı. Memcached extstore ise aynı flash destekli testte daha hızlıydı.
Bu, geliştirme ortamında ölçülmüş koşullu bir sonuçtur, üretim tasarrufu
iddiası değildir. Uygun durumlarda Redis, Valkey veya Memcached daha sade
tercihlerdir.”
