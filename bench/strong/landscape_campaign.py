"""Measure the published 1.8.0 image against the current Redis-compatible landscape.

Separate from the 1.8.0 release qualification in release_campaign.py: newer
comparator versions, more engines, its own output paths. Neither campaign's
numbers may be pooled with the other's, and neither report is regenerated from
the other's evidence.

Build and smoke-test the clients first. Do not build images or run another
benchmark while this campaign is active. Tags are resolved to immutable local
image IDs once, before any measurement. No pull, build, retry, or global
cleanup occurs.
"""
import json
import pathlib

from release_campaign import group, parser, plan, run

DEFAULT_RUNS = pathlib.Path("bench/strong/runs/landscape-1.8.0")
DEFAULT_RESULTS = pathlib.Path("bench/strong/landscape-1.8.0-results")

# Latest official releases as of 2026-09-24, pinned to explicit patch versions.
COMPARATORS = {"redis": "redis:8.10.2-alpine",
               "valkey": "valkey/valkey:9.1.2-alpine",
               "memcached": "memcached:1.6.45-alpine",
               "nginx": "nginx:1.30.5-alpine",
               "dragonfly": "docker.dragonflydb.io/dragonflydb/dragonfly:v2.0.0",
               "garnet": "ghcr.io/microsoft/garnet:2.1.8",
               "kvrocks": "apache/kvrocks:2.17.0"}

# Engine-set membership follows constraints measured on this host on 2026-09-24,
# not preference:
#   * Dragonfly 2.0.0 exits unless maxmemory is at least 256 MiB per proactor
#     thread, so it cannot be configured at 96 or 192 MiB. The parity1g groups
#     measure every engine at 1 GiB, the smallest budget Dragonfly accepts.
#   * Dragonfly tiered storage requires io_uring and aborts in
#     InitTieredStorage under the epoll fallback this Docker Desktop VM forces,
#     so dragonfly_tiered is not scheduled. The harness keeps its definition for
#     a host where io_uring is available.
#   * Kvrocks is a RocksDB-backed store, not an evicting cache; it is scheduled
#     in the file-tier groups, where that is the comparison being made.
DRAM = "grpc,redis,valkey,memcached,garnet"
FILE_TIER = "grpc_nvm,memcached_extstore,garnet_storage,kvrocks"
PARITY = "grpc,redis,valkey,memcached,dragonfly,garnet"
# High-volume SSD+RAM groups keep a DRAM-only engine as the reference for what
# the file tier is buying.
SSD = f"{FILE_TIER},grpc"

GROUPS = [
    group("primary", "hit_1k_c8", DRAM),
    group("hybrid", "origin_uniform_64k_5ms",
          "grpc,grpc_nvm,redis,valkey,memcached,memcached_extstore,garnet,garnet_storage,kvrocks",
          discard_flash=True),
    group("offered", "offered_origin_64k_1500",
          "grpc,grpc_nvm,redis,memcached_extstore,garnet_storage,kvrocks", discard_flash=True),
    group("ram192", "origin_uniform_64k_5ms", DRAM, cache=192, memory=384),
    group("flash384", "origin_uniform_64k_5ms", FILE_TIER, memory=384, discard_flash=True),
    # SSD+RAM under volume and concurrency. 256 MiB DRAM, a 4 GiB file, a 2 GiB
    # working set and 32 connections: the file tier must actually evict, reclaim
    # and serve concurrent reads, none of which a 128 MiB set in a 512 MiB file
    # asks of it. Backing files are sized and then discarded per run.
    group("ssd-highvolume", "origin_uniform_64k_2gib_c32,origin_skew_64k_2gib_c32", SSD,
          cache=256, memory=1024, warmup=30, flash=4096, discard_flash=True),
    # Over capacity: a 6 GiB set against the same 4 GiB file, so the tier evicts
    # and reclaims continuously instead of holding the whole set.
    group("ssd-overcapacity", "origin_uniform_64k_6gib_c32", SSD, 3, 60,
          cache=256, memory=1024, warmup=60, flash=4096, discard_flash=True, headline=False),
    # Same volume in 16 KiB objects: four times the items, four times the index.
    group("ssd-small-objects", "origin_uniform_16k_2gib_c32", SSD,
          cache=256, memory=1024, warmup=30, flash=4096, discard_flash=True),
    # High traffic at a fixed schedule: 10,000 arrivals/s over 64 connections,
    # where queueing and dropped arrivals become visible.
    group("ssd-offered", "offered_origin_64k_2gib_10k", SSD,
          cache=256, memory=1024, warmup=30, flash=4096, discard_flash=True,
          headline=False, overload_probe=True),
    # Five-minute runs, because reclaim and compaction need time to reach steady
    # state; a 60-second window can finish before either engages.
    group("ssd-sustained", "origin_uniform_64k_2gib_c32", FILE_TIER, 3, 300,
          cache=256, memory=1024, warmup=60, flash=4096, discard_flash=True, headline=False),
    group("parity1g-hit", "hit_1k_c8", PARITY, cache=1024, memory=2048),
    group("parity1g-origin", "origin_uniform_64k_1gib", PARITY, cache=1024, memory=2048),
    group("http", "repeated_256k_c8", "grpc_adapter,nginx", kind="http"),
    group("exploratory-kv",
          "hit_100b_c8,hit_16k_c8,hit_64k_c8,hit_256k_c8,hit_1m_c4,hit_1k_c1,hit_1k_c4,hit_1k_c16,"
          "readheavy_1k_c8,mixed_1k_c8", DRAM, 1, 10, headline=False),
    group("stress-kv", "hit_1k_c32,hit_1k_c64,mixed_1k_c32", DRAM, 2, 20, headline=False),
    group("batch-get", "batch_get_1k_c8", f"{DRAM},kvrocks", 2, 20, headline=False, discard_flash=True),
    group("batch-set", "batch_set_1k_c8", "grpc,redis,valkey,garnet,kvrocks", 2, 20,
          headline=False, discard_flash=True),
    group("pipeline", "pipeline_1k_c8", "grpc", 2, 20, headline=False),
    group("offered-saturation", "offered_1k_20k,offered_1k_50k,offered_1k_80k,offered_1k_120k",
          "grpc,redis,valkey,garnet", 1, 20, headline=False),
    group("origin-sensitivity", "origin_uniform_64k_1ms,origin_uniform_64k_20ms",
          f"grpc,{FILE_TIER},redis", 1, 10, headline=False, discard_flash=True),
    group("origin-patterns",
          "origin_near_64k_5ms,origin_skew_64k_5ms,origin_shift_64k_5ms,"
          "cold_origin_uniform_64k_5ms,onepass_64k_5ms", f"grpc,{FILE_TIER},redis", 1, 10,
          headline=False, discard_flash=True),
    group("http-secondary", "recent_replay_256k_c8,repeated_1m_c8", "grpc_adapter,nginx", 2, 30,
          kind="http", headline=False),
    group("http-onepass", "onepass_256k_c8", "grpc_adapter,nginx,origin_direct", 2, 30,
          kind="http", headline=False),
]


def main():
    p = parser(__doc__, DEFAULT_RUNS, DEFAULT_RESULTS,
               "cachebench-go:landscape", "cachelib-investigation-client:landscape")
    args = p.parse_args()
    if args.plan:
        print(json.dumps(plan(GROUPS), indent=2))
        return
    run(p, args, GROUPS, COMPARATORS)


if __name__ == "__main__":
    main()
