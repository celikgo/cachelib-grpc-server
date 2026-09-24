"""Render the landscape report directly from a complete campaign and its raw runs.

Reads only the landscape campaign's own evidence. It never touches the 1.8.0
release report, the September 23 candidate report, or the historical 1.6.0
tables, and it pools no numbers across campaigns.
"""
import argparse
import csv
import hashlib
import json
import pathlib
import statistics

from landscape_campaign import COMPARATORS, DEFAULT_RESULTS, DEFAULT_RUNS, GROUPS
from release_campaign import ROOT, expected_runs, validate_group

REPORT = ROOT / "bench/strong/LANDSCAPE-1.8.0.md"
README_BEGIN = "<!-- BEGIN GENERATED LANDSCAPE: landscape-headlines -->"
README_END = "<!-- END GENERATED LANDSCAPE: landscape-headlines -->"
LABELS = {"grpc": "CacheLib gRPC DRAM", "grpc_nvm": "CacheLib gRPC + Navy",
          "redis": "Redis OSS", "valkey": "Valkey", "memcached": "Memcached DRAM",
          "memcached_extstore": "Memcached extstore", "dragonfly": "Dragonfly",
          "dragonfly_tiered": "Dragonfly tiered", "garnet": "Garnet",
          "garnet_storage": "Garnet storage tier", "kvrocks": "Kvrocks",
          "grpc_adapter": "gRPC + Python HTTP adapter", "nginx": "NGINX cache",
          "origin_direct": "Direct origin (reference)"}
# Which released build each engine identifier measured, for the report's own
# version table. Sourced from the campaign pins, not from prose.
VERSIONS = {"grpc": "published 1.8.0 image", "grpc_nvm": "published 1.8.0 image",
            "redis": COMPARATORS["redis"], "valkey": COMPARATORS["valkey"],
            "memcached": COMPARATORS["memcached"],
            "memcached_extstore": COMPARATORS["memcached"],
            "dragonfly": COMPARATORS["dragonfly"], "garnet": COMPARATORS["garnet"],
            "garnet_storage": COMPARATORS["garnet"], "kvrocks": COMPARATORS["kvrocks"],
            "nginx": COMPARATORS["nginx"], "grpc_adapter": "published 1.8.0 image + Python adapter",
            "origin_direct": "simulated origin service"}
TITLES = {"primary": "All-RAM unary hits, 96 MiB configured DRAM",
          "hybrid": "Reusable 128 MiB set, 96 MiB configured DRAM",
          "offered": "Equal demand: 1,500 scheduled arrivals/s",
          "ram192": "Full-hit objective: 192 MiB DRAM / 384 MiB service ceiling",
          "flash384": "Full-hit objective: 96 MiB DRAM + 512 MiB file / 384 MiB service ceiling",
          "ssd-highvolume": "SSD+RAM under volume: 2 GiB set, 256 MiB DRAM, 4 GiB file, 32 connections",
          "ssd-overcapacity": "SSD+RAM over capacity: 6 GiB set, 4 GiB file, 32 connections",
          "ssd-small-objects": "SSD+RAM under volume: 2 GiB set in 16 KiB objects, 32 connections",
          "ssd-offered": "SSD+RAM under traffic: 10,000 scheduled arrivals/s, 64 connections",
          "ssd-sustained": "SSD+RAM steady state: five-minute runs",
          "parity1g-hit": "Equal 1 GiB configured DRAM: all-RAM hits",
          "parity1g-origin": "Equal 1 GiB configured DRAM: 1.28 GiB reusable set",
          "http": "Repeated 256 KiB objects through a common HTTP interface",
          "exploratory-kv": "Exploratory value sizes, concurrency and writes",
          "stress-kv": "Exploratory concurrency and write stress",
          "batch-get": "Exploratory 16-object batch GET",
          "batch-set": "Exploratory 16-object batch SET",
          "pipeline": "Exploratory sustained Pipeline",
          "offered-saturation": "Exploratory scheduled-load saturation",
          "origin-sensitivity": "Exploratory origin-delay sensitivity",
          "origin-patterns": "Exploratory near-capacity, skew, changing, cold and one-pass traces",
          "http-secondary": "Exploratory HTTP replay and 1 MiB objects",
          "http-onepass": "Exploratory one-pass HTTP objects"}
# Configuration limits measured on this host on 2026-09-24 while preparing the
# campaign. They decide which engine appears in which group.
CONSTRAINTS = [
    ("Dragonfly 2.0.0", "requires `maxmemory` of at least 256 MiB per proactor thread; with four "
     "threads it exits at 96 or 192 MiB with `There are 4 threads, so 1.00GiB are required`. "
     "It is measured in the equal 1 GiB parity groups."),
    ("Dragonfly 2.0.0 tiered storage", "requires io_uring. This Docker Desktop VM forces "
     "Dragonfly's epoll fallback, under which the server aborts in `InitTieredStorage()`. "
     "No Dragonfly file-tier result exists here; the harness keeps the configuration for a host "
     "that offers io_uring. CacheLib's Navy tier ran with `--enable_io_uring=false`."),
    ("Dragonfly 2.0.0 networking", "runs under its epoll fallback here: the server's io_uring "
     "probe fails inside this Docker Desktop VM even though `io_uring_disabled` is `0`. Dragonfly "
     "is designed around io_uring, so its rates in this environment understate what a host with "
     "usable io_uring would show. Every engine here shares that VM."),
    ("Garnet 2.1.8", "publishes neither `used_memory` nor `keyspace_hits`, so its cache-bytes and "
     "eviction columns are empty; its measured cgroup peak and per-object CPU are unaffected."),
    ("Kvrocks 2.17.0", "is a RocksDB-backed store, not an evicting cache. Its RAM budget is set as "
     "the RocksDB block cache and it keeps the whole set on its file, so it appears in the "
     "file-tier groups, where that is the comparison being made."),
]


def relative(path):
    return pathlib.Path(path).relative_to(ROOT / "bench/strong").as_posix()


def median(rows, key):
    values = [float(row[key]) for row in rows if row.get(key) not in (None, "")]
    return statistics.median(values) if values else None


def number(value, places=0):
    return "—" if value is None else f"{value:,.{places}f}"


def percent(value):
    if value is None:
        return "—"
    percentage = value * 100
    if 0 < value < 1 and round(percentage, 1) == 100:
        return f"{percentage:.3f}%" if round(percentage, 3) < 100 else "<100%"
    return f"{percentage:.1f}%"


def group_rows(results, name):
    with (results / name / "runs.csv").open() as file:
        return list(csv.DictReader(file))


def ranking(rows, case, engines, field="objects_s"):
    """Engines ordered by median, highest first. Engines without a median are omitted."""
    values = [(engine, median([r for r in rows if r["case"] == case and r["engine"] == engine], field))
              for engine in engines]
    return sorted([(e, v) for e, v in values if v is not None], key=lambda pair: -pair[1])


def place(order, engine):
    names = [e for e, _ in order]
    return names.index(engine) + 1 if engine in names else None


def comparison_sentence(rows, spec, subject="grpc", field="objects_s", unit="objects/s"):
    """State the measured order plainly, whichever way it came out."""
    case = spec["cases"][0]
    order = ranking(rows, case, spec["engines"], field)
    if not order or place(order, subject) is None:
        return None
    position, total = place(order, subject), len(order)
    best_engine, best = order[0]
    own = dict(order)[subject]
    text = (f"{LABELS[subject]} placed {position} of {total} at {own:,.0f} median {unit}. ")
    if position == 1:
        runner, second = order[1]
        text += (f"The next engine, {LABELS[runner]}, served {second:,.0f} "
                 f"({(own/second-1)*100:.1f}% lower).")
    else:
        text += (f"{LABELS[best_engine]} led at {best:,.0f} "
                 f"({best/own:.2f}× the CacheLib rate).")
    return text


def readme_headlines(runs, results):
    """Keep README claims bound to qualified raw runs and recorded summaries."""
    specs = [spec for spec in GROUPS if spec["headline"]]
    limited = [spec["name"] for spec in specs
               if not validate_group(spec, runs / spec["name"])["qualified"]]
    if limited:
        return ("Landscape comparisons are withheld because these groups did not qualify: " +
                ", ".join(f"`{name}`" for name in limited) + ". See the "
                "[landscape report](bench/strong/LANDSCAPE-1.8.0.md) for the retained results "
                "and limitations.")
    rows = {spec["name"]: group_rows(results, spec["name"]) for spec in specs}

    def order(name):
        spec = next(s for s in GROUPS if s["name"] == name)
        return ranking(rows[name], spec["cases"][0], spec["engines"])

    def line(name, engine, field="objects_s", places=0):
        spec = next(s for s in GROUPS if s["name"] == name)
        return number(median([r for r in rows[name]
                              if r["case"] == spec["cases"][0] and r["engine"] == engine], field), places)

    primary = order("primary")
    hybrid = dict(order("hybrid"))
    parity = order("parity1g-hit")
    table = "\n".join(
        f"| {LABELS[engine]} | {value:,.0f} | {line('primary', engine, 'p99_ms', 3)} ms |"
        for engine, value in primary)
    hybrid_table = "\n".join(
        f"| {LABELS[engine]} | {line('hybrid', engine)} | "
        f"{percent(median([r for r in rows['hybrid'] if r['engine'] == engine], 'hit_ratio'))} | "
        f"{line('hybrid', engine, 'origins')} |"
        for engine, _ in order("hybrid"))
    return f"""Measured on native Linux arm64 under Docker Desktop on Apple Silicon with 8 Docker
CPUs, against the current releases of every comparable service: Redis
{COMPARATORS['redis'].split(':')[1]}, Valkey {COMPARATORS['valkey'].split(':')[1]}, Memcached
{COMPARATORS['memcached'].split(':')[1]}, Dragonfly {COMPARATORS['dragonfly'].split(':')[-1]},
Garnet {COMPARATORS['garnet'].split(':')[-1]} and Kvrocks {COMPARATORS['kvrocks'].split(':')[-1]}.
The engine measured here is the published `1.8.0` image, not a local build.
Medians of five 60-second repetitions; every headline run had zero request
errors. KV services each received four CPUs and a 768 MiB container limit;
clients received four separate CPUs.

**1 KiB cache hits over eight connections, 96 MiB configured DRAM:**

| Service | Requests/s | p99 |
|---|---:|---:|
{table}

**A 128 MiB reusable set of 64 KiB objects against 96 MiB of configured DRAM,
with a simulated 5 ms origin** — what a cache is for when the data does not fit:

| Service | Objects/s | Hit ratio | Origin requests |
|---|---:|---:|---:|
{hybrid_table}

- **The flash tier is the point, and it is not unique.** CacheLib's Navy file
  tier eliminated origin traffic entirely ({line('hybrid', 'grpc_nvm', 'origins')} origin
  requests, {percent(median([r for r in rows['hybrid'] if r['engine'] == 'grpc_nvm'], 'hit_ratio'))}
  hit ratio) and served {hybrid['grpc_nvm']:,.0f} objects/s against
  {hybrid['grpc']:,.0f} for the same service with RAM alone. Memcached extstore,
  Garnet's storage tier and Kvrocks also removed the misses, and on this host
  they did so at higher rates. Navy's distinguishing property here is that it
  needs no io_uring: Dragonfly's tier could not start at all under this VM's
  epoll fallback.
- **Small memory budgets are a real constraint.** Dragonfly 2.0.0 refuses to
  start below 256 MiB per thread, so it cannot be configured at 96 or 192 MiB;
  it is measured separately at an equal 1 GiB budget, where
  {LABELS[parity[0][0]]} led at {parity[0][1]:,.0f} objects/s and CacheLib gRPC
  served {line('parity1g-hit', 'grpc')}.
- **Raw all-RAM throughput is not this service's advantage.**
  {comparison_sentence(rows['primary'], next(s for s in GROUPS if s['name'] == 'primary'))}
  What it adds over those engines is a gRPC contract with streaming pipelines,
  ordered multi-key operations, binary-safe values, Prometheus metrics, and a
  DRAM+flash tier that works without io_uring.

Full tables, per-run evidence and every recorded limitation:
[landscape report](bench/strong/LANDSCAPE-1.8.0.md). These are shared laptop-VM
results, not native amd64 or dedicated-Linux performance claims. Origin delay is
simulated; file tiers traverse Docker Desktop and host page caches, so no
physical SSD latency, endurance or cost claim follows. None of these comparisons
enables persistence, replication, compression, authentication or TLS."""


def replace_readme_headlines(readme, rendered):
    if readme.count(README_BEGIN) != 1 or readme.count(README_END) != 1:
        raise ValueError("README must contain exactly one generated landscape-headlines block")
    before, rest = readme.split(README_BEGIN)
    if README_END not in rest:
        raise ValueError("README landscape-headlines markers are out of order")
    _, after = rest.split(README_END)
    return before + README_BEGIN + "\n" + rendered + "\n" + README_END + after


def render(runs, results):
    campaign = json.loads((results / "campaign.json").read_text())
    if (campaign["groups"] != GROUPS or set(campaign["progress"]) != {g["name"] for g in GROUPS}
            or not campaign.get("finished_utc")):
        raise ValueError("Campaign incomplete; every planned group must finish before rendering claims")
    for status in campaign["progress"].values():
        for name, expected in status["evidence_sha256"].items():
            if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
                raise ValueError(f"Campaign evidence changed: {name}")
    total = sum(len(expected_runs(g)) for g in GROUPS)
    validations = {g["name"]: validate_group(g, runs / g["name"]) for g in GROUPS}
    problems = [(name, item) for name, v in validations.items() for item in v["problems"]]
    failed = [name for name, status in campaign["progress"].items() if not status["valid"]]
    limited = [name for name, value in validations.items() if value["claim_limitations"]]
    engines = [e for spec in GROUPS for e in spec["engines"]]
    measured = sorted(set(engines), key=engines.index)
    lines = ["# 1.8.0 against the current Redis-compatible landscape", "",
             f"Measured {campaign['started_utc'][:10]} to {campaign['finished_utc'][:10]} using native "
             "arm64 containers in Docker Desktop’s Linux VM on macOS.",
             f"The campaign attempted **{total} independent runs across {len(GROUPS)} groups** covering "
             f"{len(measured)} service configurations. " +
             (f"All groups passed their correctness and completeness requirements. {len(limited)} groups "
              "have disclosed limits on headline claims."
              if not failed and not problems else
              f"**Qualification incomplete:** {len(failed)} groups failed; affected measurements are "
              "retained below and do not support claims."),
             "", "This report is generated from the committed campaign, per-run JSON, and CSV data by "
             "[render_landscape_report.py](render_landscape_report.py). It is a separate campaign from "
             "the [1.8.0 release qualification](RELEASE-1.8.0.md), the "
             "[September 23 candidate report](REPORT.md) and the [historical 1.6.0 page](../../BENCHMARKS.md): "
             "different comparator versions, different engine set, different runs. No numbers are pooled "
             "across those reports.",
             "", "## What was measured", "",
             "| Role | Released build | Measured immutable image ID |", "|---|---|---|"]
    for role, identity in campaign["images"].items():
        version = VERSIONS.get(role, "—") if role not in ("go", "python") else "benchmark client"
        lines.append(f"| {role} | {version} | `{identity['id']}` |")
    lines += ["", "The service under test is the **published release image**, pulled from its registry "
              "rather than rebuilt: an image-ID or digest equality check is required before equating "
              "another artifact with these results. Comparator images came from their own registries. "
              "All tags were resolved once to immutable IDs; no image build ran during measurement.",
              "", f"- Campaign: [{relative(results / 'campaign.json')}]({relative(results / 'campaign.json')}).",
              f"- Source base commit: `{campaign['git_commit']}`. Each group manifest records dirty-tree "
              "status and harness SHA-256.",
              f"- Host: `{campaign['host']}`.",
              f"- Docker: `{campaign['docker']['ServerVersion']}`, kernel `{campaign['docker']['KernelVersion']}`, "
              f"{campaign['docker']['NCPU']} CPUs, {campaign['docker']['MemTotal']/1073741824:.1f} GiB VM memory.",
              "", "### Configuration limits that decided the engine sets", "",
              "These were measured on this host while preparing the campaign. They are properties of the "
              "engines and this environment, not choices made to favour a result.", ""]
    lines += [f"- **{subject}** {detail}" for subject, detail in CONSTRAINTS]
    lines += ["", "## Method and interpretation", "",
              "Headline groups retain five 60-second repetitions per mode after at least 15 seconds of "
              "warmup. The KV client checks the last three five-second warmup windows for a maximum 10% "
              "deviation from their mean, extending warmup up to 60 seconds. The HTTP client performs 15 "
              "seconds of warmup and full-body checks; it does not implement the KV stability-window test. "
              "Engine order is deterministically shuffled within repetitions. Exploratory groups use one or "
              "two shorter repetitions and establish no precise population ranking.",
              "", "KV services receive CPUs 0–3; clients receive CPUs 4–7 and 1,024 MiB. Container limits "
              "are 768 MiB, except 384 MiB in the objective groups and 2,048 MiB in the equal-1 GiB parity "
              "groups. Configured cache capacity is 96 MiB unless the group says 192 MiB or 1 GiB. Navy, "
              "extstore, Garnet's storage tier and Kvrocks receive a 512 MiB task-owned file at the same "
              "mount, or a 4 GiB file in the SSD groups, which also raise DRAM to 256 MiB and the "
              "container limit to 1 GiB. Container swap is disabled; no host ports are published.",
              "", "The SSD groups exist because the 96 MiB/512 MiB groups do not put a file tier under "
              "pressure: a 128 MiB working set in a 512 MiB file never forces the tier to reclaim space, "
              "and eight connections never make it serve concurrent reads. Those groups drive a 2 GiB "
              "working set through a 4 GiB file at 32 connections, the same volume in 16 KiB objects, "
              "10,000 scheduled arrivals/s over 64 connections, and five-minute runs so that reclaim and "
              "compaction reach steady state. Each run's backing-file size is recorded in its "
              "`flash-usage.json` and the file is then deleted, so a 503-run campaign does not retain "
              "hundreds of GiB; device-byte counters and the recorded sizes remain the evidence.",
              "", "Every response is byte-verified, and every engine passes a key-specific correctness "
              "probe — miss, binary round trip, overwrite, delete and TTL expiry — before its measurement. "
              "The timed payload is shared across keys, so timed checks alone do not detect a wrong-key "
              "response; that is what the separate probe is for. Closed-loop latency omits requests that "
              "could have arrived while a worker was blocked. Offered-load latency includes scheduled-arrival "
              "queueing, and dropped arrivals remain visible. One operation is one object; batch latency is "
              "per 16-object batch/window. Medians summarize separate runs, including their per-run p99s; "
              "latency samples are never pooled. Closed-loop absolute origin counts reflect different "
              "completed request counts, so use hit ratios or the equal-demand offered-load group when "
              "comparing origin traffic. Groups with different budgets ran as separate series; cross-series "
              "rates are descriptive.",
              "", "A KV origin miss simulates a fixed 1, 5 or 20 ms wait before filling the cache; HTTP uses "
              "a separate local origin with a 5 ms sleep. These are different origin models. File tiers "
              "traverse Docker Desktop and host page caches: these results do not measure physical SSD "
              "latency, NAND writes, endurance or financial savings. A larger configured cache is not the "
              "same as a larger measured process footprint. Engines differ in eviction policy — Redis and "
              "Valkey run `allkeys-lru`, Memcached its segmented LRU, Garnet drops the head of its hybrid "
              "log, Kvrocks evicts nothing — and that difference is part of what the hit-ratio columns "
              "measure."]
    all_rows = {}
    for spec in GROUPS:
        name = spec["name"]
        rows = group_rows(results, name)
        all_rows[name] = rows
        validation = validations[name]
        lines += ["", f"## {TITLES[name]}", "",
                  f"{spec['reps']} {'repetition' if spec['reps'] == 1 else 'repetitions'} × "
                  f"{spec['seconds']} seconds per mode/case. "
                  f"[{len(rows)}/{len(expected_runs(spec))} result rows]({relative(results / name / 'runs.csv')}); "
                  f"[summary]({relative(results / name / 'summary.json')}); "
                  f"[raw manifest]({relative(runs / name / 'manifest.json')}).",
                  "", "| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | "
                  "Median origin requests | CPU μs/object | Peak MiB | Errors / drops |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for case in spec["cases"]:
            for engine, _ in ranking(rows, case, spec["engines"]) or [(e, None) for e in spec["engines"]]:
                selected = [r for r in rows if r["case"] == case and r["engine"] == engine]
                hit, memory = median(selected, "hit_ratio"), median(selected, "server_memory_peak_bytes")
                cells = [f"`{case}` / {LABELS[engine]}", str(len(selected)),
                         number(median(selected, "objects_s")), number(median(selected, "p99_ms"), 3),
                         percent(hit), number(median(selected, "origins")),
                         ("—" if engine == "origin_direct" else
                          number(median(selected, "server_cpu_us_per_object"), 1)),
                         ("—" if engine == "origin_direct" else
                          number(memory/1048576 if memory is not None else None, 1)),
                         f"{sum(int(r['errors']) for r in selected):,} / "
                         f"{sum(int(r.get('dropped_arrivals', 0)) for r in selected):,}"]
                lines.append("| " + " | ".join(cells) + " |")
        notes = validation["problems"] + validation["claim_limitations"] + validation["observations"]
        if notes:
            lines += ["", "Recorded qualification problems/observations:", ""]
            lines += [f"- `{note.replace('`', '')}`" for note in notes]
        else:
            lines += ["", "All expected runs passed correctness and duration checks" +
                      (" and the KV warmup stability requirement."
                       if spec["kind"] == "kv" and spec["headline"] else ".")]
        if name == "parity1g-hit":
            lines += ["", "This group exists because Dragonfly cannot be configured at the 96 MiB budget "
                      "used elsewhere. Every engine here has the same 1 GiB configured cache and 2 GiB "
                      "container limit, so this is the equal-budget comparison that includes Dragonfly."]
        if name == "ssd-overcapacity":
            lines += ["", "The working set is larger than the file here, so no engine can avoid every "
                      "origin request and the tier reclaims space throughout. The run starts with an "
                      "empty tier and no preload: warmup fills it, and the hit ratios below are what "
                      "each engine achieved while continuously evicting."]
        if name == "ssd-offered":
            lines += ["", "This group offers 10,000 arrivals/s whether or not an engine can serve "
                      "them from a file tier at this volume. Dropped arrivals are the measurement: "
                      "they show where scheduled demand exceeded what the tier delivered, and they do "
                      "not limit the other groups' claims."]
        if name == "offered-saturation":
            lines += ["", "Dropped arrivals represent overload outcomes and are retained; request errors "
                      "remain qualification failures. Rates in this exploratory sweep do not establish a "
                      "precise saturation threshold."]
        if name == "origin-patterns":
            lines += ["", "The cold-start case intentionally has zero warmup and reports "
                      "`warmup_stable=false`. Its startup results are not steady-state rates. A changing "
                      "hot set or one-pass trace tests reuse, not playback."]
        if name == "http-onepass":
            lines += ["", "Direct origin has no allocated cache path and is a reference, not an "
                      "equal-resource cache comparison."]
    lines += ["", "## Where 1.8.0 leads and where it does not", "",
              "Each statement below is the measured order in one qualified group, in whichever direction "
              "the measurement came out.", ""]
    for name in ("primary", "parity1g-hit", "hybrid", "ssd-highvolume", "ssd-overcapacity",
                 "ssd-small-objects", "ssd-offered", "ssd-sustained", "offered", "parity1g-origin",
                 "flash384"):
        spec = next(s for s in GROUPS if s["name"] == name)
        if not validations[name]["qualified"]:
            lines += [f"- `{name}`: withheld; the group recorded limits on its claim.", ""]
            continue
        subject = "grpc_nvm" if "grpc_nvm" in spec["engines"] else "grpc"
        sentence = comparison_sentence(all_rows[name], spec, subject)
        if sentence:
            lines += [f"- **{TITLES[name]}.** {sentence}", ""]
    if validations["hybrid"]["qualified"]:
        hybrid_spec = next(s for s in GROUPS if s["name"] == "hybrid")
        hybrid = dict(ranking(all_rows["hybrid"], hybrid_spec["cases"][0], hybrid_spec["engines"]))
        dram_only = [e for e in ("grpc", "redis", "valkey", "memcached", "garnet") if e in hybrid]
        best_dram = max(dram_only, key=lambda e: hybrid[e]) if dram_only else None
        if best_dram and "grpc_nvm" in hybrid:
            lines += [f"- **A file tier, not a faster loop, is what removes the origin traffic.** The best "
                      f"DRAM-only engine in that group, {LABELS[best_dram]}, served {hybrid[best_dram]:,.0f} "
                      f"objects/s; every engine with a file tier served multiples of that. The tier, not "
                      f"the protocol, is the variable.", ""]
    if limited or failed or problems:
        lines += ["The affected groups' recorded limits prevent their corresponding claims. Qualified "
                  "groups are interpreted independently; an exploratory overload outcome does not "
                  "invalidate another group's measurements.", ""]
    lines += ["## Scope and reproduction", "",
              "The service caches whole binary objects over gRPC. It supplies no HTTP/range delivery, "
              "authentication, TLS, replication, sharding, warm restart, recording, playback buffering or "
              "media transport. Navy files are truncated on startup. Simulated origin avoidance "
              "demonstrates reuse under the disclosed trace; it does not establish production cost, viewer "
              "latency, frame continuity or streaming quality. The capability differences named in this "
              "report — the gRPC contract, streaming Pipeline, ordered multi-key operations, Prometheus "
              "metrics — are properties of the interface, measured here only where a table measures them.",
              "", "Run the complete campaign with [the documented local Docker commands]"
              "(README.md#current-landscape-campaign). All attempts and shuffled schedules remain in the "
              "raw manifests. The generated CSVs include per-run rates, latency percentiles, resource "
              "observations, tier counters and backing-file allocations. The report check verifies campaign "
              "completeness, raw-run qualification and CSV-derived tables; it never modifies the other "
              "reports.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=pathlib.Path, default=ROOT / DEFAULT_RUNS)
    parser.add_argument("--results", type=pathlib.Path, default=ROOT / DEFAULT_RESULTS)
    parser.add_argument("--report", type=pathlib.Path, default=REPORT)
    parser.add_argument("--readme", type=pathlib.Path, default=ROOT / "README.md")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rendered = render(args.runs.resolve(), args.results.resolve())
    readme = args.readme.read_text()
    rendered_readme = replace_readme_headlines(
        readme, readme_headlines(args.runs.resolve(), args.results.resolve()))
    if args.check:
        if args.report.read_text() != rendered:
            raise SystemExit("Landscape report differs from campaign/raw results; rerun renderer without --check")
        if readme != rendered_readme:
            raise SystemExit("README landscape headlines differ from qualified results; rerun renderer without --check")
        print("Landscape report and README headlines match complete campaign and measured results")
    else:
        args.report.write_text(rendered)
        args.readme.write_text(rendered_readme)


if __name__ == "__main__":
    main()
