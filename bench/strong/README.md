# Current-version comparative benchmark harness

This is separate from `bench/results-summary.json` and the historical 1.6.0
tables. Do not copy numbers between environments or silently replace either.
The current release-candidate report is `REPORT.md` after qualification.

Run from the repository root on an otherwise idle Docker host with at least
eight Docker CPUs. The harness uses an internal task-owned network, no
published ports, fresh service containers per run, and exact task-owned
flash/cache directories. It never formats a device or prunes Docker state.
The image arguments must identify the implementation being studied.

```bash
docker build --target tester --network=host -t cachelib-candidate-tester:local .
docker build --network=host -t cachelib-candidate:1.8.0 .
docker build -f bench/investigation/Dockerfile.client -t cachelib-investigation-client:local .
docker build -f bench/strong/Dockerfile.client -t cachebench-go:rc .
scripts/release-smoke.sh cachelib-candidate:1.8.0 1.8.0 /tmp/cachelib-flash-smoke.json

# Five independent, randomized-order, 60-second measurements per engine.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis,valkey,memcached --cases hit_1k_c8 \
  --reps 5 --seconds 60 --warmup 15 --output bench/strong/runs/rc-primary-c8

# Equal configured 96 MiB RAM plus explicitly additional 512 MiB backing file.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,valkey,memcached,memcached_extstore \
  --cases origin_uniform_64k_5ms --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/rc-hybrid-5ms

# Same scheduled 1,500 arrivals/s and global key trace for absolute origin counts.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,memcached_extstore \
  --cases offered_origin_64k_1500 --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/rc-offered-origin

# Equivalent full-hit objective bought with more DRAM; still no cost claim.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines redis,memcached --cases origin_uniform_64k_5ms \
  --cache-mb 192 --memory-mb 384 --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/rc-ram192-objective

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc --cases origin_uniform_64k_5ms \
  --cache-mb 192 --memory-mb 384 --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/rc-grpc-ram192-objective
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc_nvm --cases origin_uniform_64k_5ms \
  --cache-mb 96 --memory-mb 384 --flash-mb 512 --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/rc-nvm384-objective
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines memcached_extstore --cases origin_uniform_64k_5ms \
  --cache-mb 96 --memory-mb 384 --flash-mb 512 --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/rc-extstore384-objective

# Exploratory one-run matrix. These ten-second runs are not headline claims.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis,memcached \
  --cases hit_100b_c8,hit_16k_c8,hit_64k_c8,hit_256k_c8,hit_1m_c4,hit_1k_c1,hit_1k_c4,hit_1k_c16,readheavy_1k_c8,mixed_1k_c8 \
  --reps 1 --seconds 10 --warmup 15 \
  --output bench/strong/runs/rc-exploratory-kv

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis,memcached --cases batch_get_1k_c8 \
  --reps 2 --seconds 20 --warmup 15 --output bench/strong/runs/rc-batch-get
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis --cases batch_set_1k_c8 \
  --reps 2 --seconds 20 --warmup 15 --output bench/strong/runs/rc-batch-set
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc --cases pipeline_1k_c8 \
  --reps 2 --seconds 20 --warmup 15 --output bench/strong/runs/rc-pipeline

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis --cases offered_1k_20k,offered_1k_50k \
  --reps 1 --seconds 20 --warmup 15 \
  --output bench/strong/runs/rc-offered-saturation
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis --cases offered_1k_80k \
  --reps 1 --seconds 20 --warmup 15 --output bench/strong/runs/rc-offered-80k

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,memcached_extstore \
  --cases origin_uniform_64k_1ms,origin_uniform_64k_20ms \
  --reps 1 --seconds 10 --warmup 15 \
  --output bench/strong/runs/rc-origin-sensitivity
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,memcached_extstore \
  --cases origin_near_64k_5ms,origin_skew_64k_5ms,origin_shift_64k_5ms,cold_origin_uniform_64k_5ms,onepass_64k_5ms \
  --reps 1 --seconds 10 --warmup 15 \
  --output bench/strong/runs/rc-origin-patterns

# Common HTTP interface: NGINX and gRPC+Python adapter each get 2 CPUs/768 MiB.
# The original repeated-hit media runs used the archived pre-keepalive
# NGINX config and pre-reuse adapter. Recreate that configuration without
# modifying the current adapter source; keep its output separate.
docker build -f bench/investigation/Dockerfile.client \
  --build-arg HTTP_SERVICE_SOURCE=bench/strong/provenance/http_service.before_reuse.py \
  -t cachelib-investigation-client:before-reuse .
python3 bench/strong/run_http.py --grpc-image cachelib-candidate:1.8.0 \
  --adapter-image cachelib-investigation-client:before-reuse \
  --adapter-source bench/strong/provenance/http_service.before_reuse.py \
  --nginx-config bench/strong/provenance/nginx.before_keepalive.conf \
  --cases repeated_256k_c8 --reps 5 --seconds 60 --warmup 15 \
  --output bench/strong/runs/replay-original-http

# The corrected one-pass comparison uses the current adapter and config.
python3 bench/strong/run_http.py --grpc-image cachelib-candidate:1.8.0 \
  --cases onepass_256k_c8 --reps 2 --seconds 30 --warmup 10 \
  --output bench/strong/runs/rc-http-onepass-reuse

python3 bench/strong/summarize.py --kind kv --input bench/strong/runs/rc-primary-c8 \
  --output bench/strong/release-results/primary
```

`run_strong.py --help` and `run_http.py --help` list the controls. Each run
records commands, image identity, correctness output, cgroup snapshots,
server/client logs, a result JSON, or an error file. `summarize.py` emits
run-level CSV, group JSON, and SVG charts. It keeps failed repetitions and
does not pool unequal workloads or per-run latency percentiles. Bootstrap
intervals summarize five or more observed run values; they are descriptive
on a shared developer VM.
`python3 bench/strong/render_report_tables.py --check` verifies that the
primary, hybrid, offered-load, equal-objective, and HTTP tables in `REPORT.md` are generated from their committed
five-repetition CSVs; CI runs this check alongside the historical renderer.

One operation means one key/value object; a 16-object batch counts as 16
operations and one batch. Payload throughput counts verified returned cache
bytes. A closed-loop client issues the next operation when the prior response
arrives, so its percentiles omit arrivals that would have occurred while
blocked. The offered-load mode schedules due times independently, measures
from due time through response, and reports dropped arrivals and scheduler
lag. Any client-limited run must be labeled as such.

The simulated origin in the KV harness waits the specified 1, 5, or 20 ms
per miss, returns deterministic binary content, and fills the cache. It is
not a measured remote service, WAN, or financial-cost model. The
`cold_origin_*` case deliberately overrides the requested warmup to zero
and starts without preload; its `warmup_stable=false` is expected.
The HTTP harness
uses a separate local HTTP origin with a disclosed 5 ms sleep. Both HTTP
paths verify complete, key-specific response bodies. The Python gRPC HTTP adapter is part
of the measured cache path and consumes half of its 2-CPU/768-MiB budget.
The HTTP client has its own 4-CPU/1,024-MiB cgroup on CPUs 2–5; the cache
path uses CPUs 0–1 and the common origin uses CPUs 6–7.
For the two-container gRPC HTTP path, the CSV's combined memory figure is
the **sum of the two component cgroup peaks**, an upper bound on a
simultaneous path peak. The individual adapter and gRPC peaks remain in the
CSV. NGINX's single-container peak is directly observed.
The `recent_replay_256k_c8` HTTP trace warms older segments, switches all
readers to newer segments for the middle third of measurement, then seeks
back to the older set; it measures object reuse, not playback quality.

The Docker Desktop bind-mounted Navy/extstore files and NGINX cache traverse
the VM and host page cache. Navy device-byte counters demonstrate logical
file I/O, not physical NAND writes, SSD endurance, or bare-metal latency.
There is no persistence, replication, compression, authentication, or TLS in
these comparisons.
