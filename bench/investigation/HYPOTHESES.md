# Predesignated hypotheses (before the full matrix)

The pilot validates the tools only. A result needs correctness checks, a recorded
image digest, and raw output before it counts as evidence. An SSD advantage
requires an increase in actual flash hits, not just `--enable_nvm`.

| Hypothesis | Supporting evidence | Rejecting evidence |
|---|---|---|
| H1: With data in RAM, this complete gRPC service differs from Redis Open Source in latency, throughput, and CPU cost. | Repeated same-payload, same-trace, connection-reusing hit runs show a consistent difference with server CPU and memory measured. | Confidence intervals overlap or differences vanish when client cost is controlled. |
| H2: At fixed configured RAM, an operating SSD tier retains reusable content and avoids origin work. | Verified nonzero Navy flash hits and disk I/O, with fewer measured origin GETs than RAM-only CacheLib, Redis, Valkey, and Memcached on the same trace. | No flash hits, no reduction in origin calls, or unacceptable write cost. Equal configured RAM is not equal total resource use; RAM and SSD are reported separately. |
| H3: A slower SSD hit can improve end-to-end application latency when origin is expensive. | Cache+5 ms simulated origin experiment has lower end-to-end p50/p95/p99 or higher successful throughput than RAM-only under the same trace, together with fewer origin requests. | Flash overhead exceeds avoided 5 ms misses, or origin traffic does not fall. The 5 ms is an assumption, not measured infrastructure latency. |
| H4: MultiGet or Pipeline reduce this server's unary RPC cost, but competitors also batch. | Complete-service runs show more objects/s for a fixed batch size than unary, and the Redis/Valkey pipeline and Memcached multi-get are measured too. | No repeatable gain or increased errors/tail latency. Compare operations and batches separately. |
| H5: Repeated media objects benefit; one-pass objects do not. | 256 KiB and 1 MiB repeated-object traces yield high byte hit ratio and lower origin traffic; 64 KiB one-pass yields near-zero hits and additional cache writes. | A repeated trace misses frequently or one-pass caching still improves the stated objective. No playback quality conclusion follows from object GET timing. |

The Python trace client is designed for correctness, hit ratios, and low-load
latency. It may cap small-object throughput; saturation claims require a
separate native-client pass and client CPU measurement. Docker Desktop's virtual
disk and page cache prevent a physical SSD latency or endurance claim.
