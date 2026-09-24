# Current-version comparative benchmark harness

The [fresh 1.8.0 release campaign](RELEASE-1.8.0.md) is separate from the
[September 23 candidate qualification](REPORT.md). Raw evidence and generated
results are retained for both campaigns.

This is separate from `bench/results-summary.json` and the historical 1.6.0
tables. Do not copy numbers between environments or silently replace either.
The previous release-candidate report is `REPORT.md`; its numbers remain unchanged.

Run from the repository root on an otherwise idle Docker host with at least
eight Docker CPUs. The harness uses an internal task-owned network, no
published ports, fresh service containers per run, and exact task-owned
flash/cache directories. It never formats a device or prunes Docker state.
The image arguments must identify the implementation being studied.


## Fresh release campaign

Build and validate the native runtime and benchmark clients before starting
measurements. Reserve all eight Docker CPUs for the campaign: do not run
builds, tests, or another benchmark at the same time. The full campaign has
**195 runs in 15 groups**, including 105 headline runs at five repetitions
of 60 seconds. Exploratory durations remain one × 10 seconds, one × 20
seconds, or two × 20/30 seconds as specified in the plan. Timed measurement
plus minimum warmup takes 2 h 56 min; startup, correctness checks, file-tier
settling, and cleanup typically bring the full run to roughly four hours.
Allow at least 30 GiB free disk for the retained task-owned cache files.

```bash
# Review the complete schedule without accessing Docker.
python3 bench/strong/release_campaign.py --plan

# Run after local builds, native tests, and release-smoke.sh have passed.
# Tags are resolved once to immutable local IDs before any measurement.
python3 bench/strong/release_campaign.py \
  --grpc-image cachelib-release:1.8.0-arm64 \
  --go-client-image cachebench-go:rc \
  --python-client-image cachelib-investigation-client:local

python3 bench/strong/render_release_report.py
python3 bench/strong/render_release_report.py --check
```

The release renderer updates both `RELEASE-1.8.0.md` and the generated
`release-headlines` block in the root README; CI checks both against the complete
campaign. Historical generated blocks remain separate.

The default raw root is `runs/release-1.8.0-20260924`; generated CSV/JSON/SVG
files live in `release-1.8.0-results`. The complete campaign resolves the
runtime, both clients, Redis, Valkey, Memcached, and NGINX to image IDs. It
records the host, Docker resource allocation, source commit/status, harness
hashes, run schedule, start/finish times, and exact commands. The HTTP
configuration uses the current origin-connection reuse implementation and
NGINX upstream keepalive throughout. The 192 MiB RAM objective modes are
interleaved in one group; Navy/extstore are interleaved in another group.
Comparisons across those groups remain descriptive.

Every planned attempt is retained. The driver checks required result fields,
correctness probes, measurement duration, request errors, client exit status,
warmup stability, and full-hit objectives. It summarizes each group while
its backing files are still present, continues through honest adverse
performance outcomes, and exits nonzero for invalid or missing measurements.
Unstable warmup or a missed full-hit objective limits the affected claim;
it does not by itself mean the server failed correctness. Dropped arrivals
in the saturation sweep are measured overload outcomes. There are no silent
retries or discarded outliers. `--resume` continues groups never started;
an interrupted group requires investigation and a fresh output path.
The one-group commands below repeat the older workload shapes with the
current checkout and images. Reproducing the original artifact also requires
the source and image identities recorded in its archived report. Each
invocation uses a fresh temporary output root to preserve prior evidence.

## Individual groups and previous qualification

```bash
# A separate output root for this manual campaign; keep it after the run.
cachelib_manual_runs=$(mktemp -d "${TMPDIR:-/tmp}/cachelib-manual-bench.XXXXXX")
printf 'Manual evidence: %s\n' "$cachelib_manual_runs"

docker build --target tester --network=host -t cachelib-candidate-tester:local .
docker build --network=host -t cachelib-candidate:1.8.0 .
docker build -f bench/investigation/Dockerfile.client -t cachelib-investigation-client:local .
docker build -f bench/strong/Dockerfile.client -t cachebench-go:rc .
scripts/release-smoke.sh cachelib-candidate:1.8.0 1.8.0 /tmp/cachelib-flash-smoke.json

# Five independent, randomized-order, 60-second measurements per engine.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis,valkey,memcached --cases hit_1k_c8 \
  --reps 5 --seconds 60 --warmup 15 --output "$cachelib_manual_runs/rc-primary-c8"

# Equal configured 96 MiB RAM plus explicitly additional 512 MiB backing file.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,valkey,memcached,memcached_extstore \
  --cases origin_uniform_64k_5ms --reps 5 --seconds 60 --warmup 15 \
  --output "$cachelib_manual_runs/rc-hybrid-5ms"

# Same scheduled 1,500 arrivals/s and global key trace for absolute origin counts.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,memcached_extstore \
  --cases offered_origin_64k_1500 --reps 5 --seconds 60 --warmup 15 \
  --output "$cachelib_manual_runs/rc-offered-origin"

# Equivalent full-hit objective bought with more DRAM; still no cost claim.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines redis,memcached --cases origin_uniform_64k_5ms \
  --cache-mb 192 --memory-mb 384 --reps 5 --seconds 60 --warmup 15 \
  --output "$cachelib_manual_runs/rc-ram192-objective"

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc --cases origin_uniform_64k_5ms \
  --cache-mb 192 --memory-mb 384 --reps 5 --seconds 60 --warmup 15 \
  --output "$cachelib_manual_runs/rc-grpc-ram192-objective"
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc_nvm --cases origin_uniform_64k_5ms \
  --cache-mb 96 --memory-mb 384 --flash-mb 512 --reps 5 --seconds 60 --warmup 15 \
  --output "$cachelib_manual_runs/rc-nvm384-objective"
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines memcached_extstore --cases origin_uniform_64k_5ms \
  --cache-mb 96 --memory-mb 384 --flash-mb 512 --reps 5 --seconds 60 --warmup 15 \
  --output "$cachelib_manual_runs/rc-extstore384-objective"

# Exploratory one-run matrix. These ten-second runs are not headline claims.
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis,memcached \
  --cases hit_100b_c8,hit_16k_c8,hit_64k_c8,hit_256k_c8,hit_1m_c4,hit_1k_c1,hit_1k_c4,hit_1k_c16,readheavy_1k_c8,mixed_1k_c8 \
  --reps 1 --seconds 10 --warmup 15 \
  --output "$cachelib_manual_runs/rc-exploratory-kv"

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis,memcached --cases batch_get_1k_c8 \
  --reps 2 --seconds 20 --warmup 15 --output "$cachelib_manual_runs/rc-batch-get"
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis --cases batch_set_1k_c8 \
  --reps 2 --seconds 20 --warmup 15 --output "$cachelib_manual_runs/rc-batch-set"
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc --cases pipeline_1k_c8 \
  --reps 2 --seconds 20 --warmup 15 --output "$cachelib_manual_runs/rc-pipeline"

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis --cases offered_1k_20k,offered_1k_50k \
  --reps 1 --seconds 20 --warmup 15 \
  --output "$cachelib_manual_runs/rc-offered-saturation"
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,redis --cases offered_1k_80k \
  --reps 1 --seconds 20 --warmup 15 --output "$cachelib_manual_runs/rc-offered-80k"

python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,memcached_extstore \
  --cases origin_uniform_64k_1ms,origin_uniform_64k_20ms \
  --reps 1 --seconds 10 --warmup 15 \
  --output "$cachelib_manual_runs/rc-origin-sensitivity"
python3 bench/strong/run_strong.py --grpc-image cachelib-candidate:1.8.0 \
  --engines grpc,grpc_nvm,redis,memcached_extstore \
  --cases origin_near_64k_5ms,origin_skew_64k_5ms,origin_shift_64k_5ms,cold_origin_uniform_64k_5ms,onepass_64k_5ms \
  --reps 1 --seconds 10 --warmup 15 \
  --output "$cachelib_manual_runs/rc-origin-patterns"

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
  --output "$cachelib_manual_runs/replay-original-http"

# The corrected one-pass comparison uses the current adapter and config.
python3 bench/strong/run_http.py --grpc-image cachelib-candidate:1.8.0 \
  --cases onepass_256k_c8 --reps 2 --seconds 30 --warmup 10 \
  --output "$cachelib_manual_runs/rc-http-onepass-reuse"

python3 bench/strong/summarize.py --kind kv --input "$cachelib_manual_runs/rc-primary-c8" \
  --output "$cachelib_manual_runs/summary-primary"
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
