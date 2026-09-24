"""Render the release report directly from a complete campaign and its raw runs."""
import argparse
import csv
import hashlib
import json
import pathlib
import statistics

from release_campaign import DEFAULT_RESULTS, DEFAULT_RUNS, GROUPS, ROOT, expected_runs, validate_group

REPORT = ROOT / "bench/strong/RELEASE-1.8.0.md"
README_BEGIN = "<!-- BEGIN GENERATED RELEASE: release-headlines -->"
README_END = "<!-- END GENERATED RELEASE: release-headlines -->"
LABELS = {"grpc": "CacheLib gRPC DRAM", "grpc_nvm": "CacheLib gRPC + Navy",
          "redis": "Redis OSS", "valkey": "Valkey", "memcached": "Memcached DRAM",
          "memcached_extstore": "Memcached extstore", "grpc_adapter": "gRPC + Python HTTP adapter",
          "nginx": "NGINX cache", "origin_direct": "Direct origin (reference)"}
TITLES = {"primary": "All-RAM unary hits", "hybrid": "Reusable 128 MiB set, 96 MiB configured DRAM",
          "offered": "Equal demand: 1,500 scheduled arrivals/s", "ram192": "Full-hit objective: 192 MiB DRAM / 384 MiB service ceiling",
          "flash384": "Full-hit objective: 96 MiB DRAM + 512 MiB file / 384 MiB service ceiling",
          "http": "Repeated 256 KiB objects through a common HTTP interface",
          "exploratory-kv": "Exploratory value sizes, concurrency and writes", "batch-get": "Exploratory 16-object batch GET",
          "batch-set": "Exploratory 16-object batch SET", "pipeline": "Exploratory sustained Pipeline",
          "offered-saturation": "Exploratory scheduled-load saturation", "origin-sensitivity": "Exploratory origin-delay sensitivity",
          "origin-patterns": "Exploratory near-capacity, skew, changing, cold and one-pass traces",
          "http-secondary": "Exploratory HTTP replay and 1 MiB objects", "http-onepass": "Exploratory one-pass HTTP objects"}


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


def measured_versions(results):
    """Name the comparator builds this campaign actually resolved and ran."""
    campaign = json.loads((results / "campaign.json").read_text())
    versions = {}
    for role, identity in campaign["images"].items():
        requested = identity.get("requested", "")
        if ":" in requested and not requested.startswith("sha256:"):
            versions[role] = requested.split(":", 1)[1]
    return versions


def readme_headlines(runs, results):
    """Keep README claims bound to qualified raw runs and recorded summaries."""
    specs = [spec for spec in GROUPS if spec["headline"]]
    limited = [spec["name"] for spec in specs if not validate_group(spec, runs / spec["name"])["qualified"]]
    if limited:
        return ("Headline comparisons are withheld because these groups did not qualify: " +
                ", ".join(f"`{name}`" for name in limited) + ". See the "
                "[release report](bench/strong/RELEASE-1.8.0.md) for the retained results and limitations.")
    groups = {}
    for spec in specs:
        summary = json.loads((results / spec["name"] / "summary.json").read_text())
        expected = {f"{case}/{engine}" for case in spec["cases"] for engine in spec["engines"]}
        if (summary["failed_runs"] or set(summary["groups"]) != expected or
                any(g["n"] != spec["reps"] or g["errors"] for g in summary["groups"].values())):
            raise ValueError(f"README summary is incomplete or failed: {spec['name']}")
        groups[spec["name"]] = {engine: summary["groups"][f"{spec['cases'][0]}/{engine}"]["medians"]
                                for engine in spec["engines"]}
    def value(group, engine, field="objects_s", places=0):
        return number(groups[group][engine][field], places)
    for name in ("primary", "ram192", "flash384", "http"):
        with (results / name / "runs.csv").open() as source:
            for row in csv.DictReader(source):
                if float(row["origins"]) != 0 or float(row["hit_ratio"]) != 1:
                    raise ValueError(f"README full-hit statement is unsupported: {name}/{row['engine']}")
    if groups["hybrid"]["grpc_nvm"]["nvm_device_bytes_read_delta"] <= 0:
        raise ValueError("README Navy file-read statement requires measured device reads")
    primary = groups["primary"]
    comparison = ("slower than the three comparison services:" if
                  all(primary["grpc"]["objects_s"] < primary[e]["objects_s"] for e in ("redis", "valkey", "memcached"))
                  else "measured alongside three comparison services:")
    versions = measured_versions(results)
    rows = "\n".join(f"| {label} | {value('primary', engine)} | {value('primary', engine, 'p99_ms', 3)} ms |"
                     for engine, label in (("grpc", "CacheLib gRPC 1.8.0"),
                                           ("redis", f"Redis {versions.get('redis', '')}".strip()),
                                           ("valkey", f"Valkey {versions.get('valkey', '')}".strip()),
                                           ("memcached", f"Memcached {versions.get('memcached', '')}".strip())))
    return f"""Measured on native Linux arm64 under Docker Desktop on Apple Silicon, with
8 Docker CPUs, against the comparator releases current when this campaign ran:
Redis {versions.get('redis', '—')}, Valkey {versions.get('valkey', '—')}, Memcached
{versions.get('memcached', '—')} and NGINX {versions.get('nginx', '—')}. A newer comparison
against the current releases of those and of Dragonfly, Garnet and Kvrocks is a
separate campaign; these numbers are not restated for newer comparator versions.
The results below are medians of five 60-second repetitions;
all these headline runs had zero request errors. KV services each had four
CPUs, 96 MiB configured cache RAM, and a 768 MiB container limit unless stated
otherwise; the client used four separate CPUs.

For 1 KiB cache hits over eight connections, the complete gRPC service was
{comparison}

| Service | Requests/s | p99 |
|---|---:|---:|
{rows}

- **Data larger than RAM:** with a 128 MiB reusable set of 64 KiB objects and
  a simulated 5 ms origin, adding a 512 MiB Navy file gave a median of
  {value('hybrid', 'grpc_nvm', 'origins')} origin calls. CacheLib reached {value('hybrid', 'grpc_nvm')} objects/s with Navy versus
  {value('hybrid', 'grpc')} with RAM alone; Memcached extstore reached
  {value('hybrid', 'memcached_extstore')} under the same configured cache/file budgets.
  At a fixed 1,500 arrivals/s, Navy needed a median of {value('offered', 'grpc_nvm', 'origins')} origin calls versus
  {value('offered', 'grpc', 'origins')} for RAM-only CacheLib over 60 seconds. Configured cache RAM
  is not total process memory: the report includes measured cgroup peaks.
- **More RAM also works:** CacheLib gRPC, Redis, and Memcached avoided origin
  misses with 192 MiB configured cache RAM and a 384 MiB container limit. Separate
  384 MiB-limit Navy/extstore runs also met the full-hit objective. These
  separately scheduled groups do not establish a hardware-cost advantage.
- **HTTP objects:** for repeated 256 KiB objects, the gRPC service plus Python
  HTTP adapter delivered {value('http', 'grpc_adapter')} objects/s (p99
  {value('http', 'grpc_adapter', 'p99_ms', 3)} ms), versus NGINX's {value('http', 'nginx')} (p99
  {value('http', 'nginx', 'p99_ms', 3)} ms). Each complete cache path had two CPUs and 768 MiB;
  both avoided origin requests. This includes adapter overhead and measures
  object delivery, not playback quality.

These are shared laptop-VM results, not native amd64 or dedicated Linux
performance claims. Navy device reads prove logical file-tier use; Docker's
VM and host page caches prevent a physical SSD latency or endurance claim.
Origin delay is simulated, and none of these comparisons enables persistence,
replication, compression, authentication, or TLS."""


def replace_readme_headlines(readme, rendered):
    if readme.count(README_BEGIN) != 1 or readme.count(README_END) != 1:
        raise ValueError("README must contain exactly one generated release-headlines block")
    before, rest = readme.split(README_BEGIN)
    if README_END not in rest:
        raise ValueError("README release-headlines markers are out of order")
    _, after = rest.split(README_END)
    return before + README_BEGIN + "\n" + rendered + "\n" + README_END + after


def render(runs, results):
    campaign = json.loads((results / "campaign.json").read_text())
    if campaign["groups"] != GROUPS or set(campaign["progress"]) != {g["name"] for g in GROUPS} or not campaign.get("finished_utc"):
        raise ValueError("Campaign incomplete; every planned group must finish before rendering release claims")
    for status in campaign["progress"].values():
        for name, expected in status["evidence_sha256"].items():
            if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
                raise ValueError(f"Campaign evidence changed: {name}")
    total = sum(len(expected_runs(g)) for g in GROUPS)
    validations = {g["name"]: validate_group(g, runs / g["name"]) for g in GROUPS}
    problems = [(name, item) for name, validation in validations.items() for item in validation["problems"]]
    failed = [name for name, status in campaign["progress"].items() if not status["valid"]]
    limited = [name for name, value in validations.items() if value["claim_limitations"]]
    lines = ["# 1.8.0 local Docker release benchmarks", "",
             f"Measured {campaign['started_utc'][:10]} to {campaign['finished_utc'][:10]} using native arm64 containers in Docker Desktop’s Linux VM on macOS.",
             f"The campaign attempted **{total} independent runs across {len(GROUPS)} groups**. " +
             (f"All groups passed their correctness and completeness requirements. {len(limited)} groups have disclosed limits on headline claims." if not failed and not problems else
              f"**Qualification incomplete:** {len(failed)} groups failed; affected measurements are retained below and do not support release claims."),
             "", "This report is generated from the committed campaign, per-run JSON, and CSV data by "
             "[render_release_report.py](render_release_report.py). The [September 23 candidate report](REPORT.md) "
             "and [historical 1.6.0 page](../../BENCHMARKS.md) retain earlier measurements; their numbers are not pooled here.",
             "", "## Artifact and environment", "",
             f"- Campaign: [{relative(results / 'campaign.json')}]({relative(results / 'campaign.json')}).",
             f"- Source base commit: `{campaign['git_commit']}`. Each group manifest records dirty-tree status and runtime-source SHA-256.",
             f"- Host: `{campaign['host']}`.",
             f"- Docker: `{campaign['docker']['ServerVersion']}`, kernel `{campaign['docker']['KernelVersion']}`, "
             f"{campaign['docker']['NCPU']} CPUs, {campaign['docker']['MemTotal']/1073741824:.1f} GiB VM memory.",
             "- The runtime and clients were built locally before timing. Comparator images came from their registries. All tags were resolved once to immutable IDs; no image build ran during measurement.", "",
             "| Role | Measured immutable image ID | Architecture |", "|---|---|---|"]
    for role, identity in campaign["images"].items():
        lines.append(f"| {role} | `{identity['id']}` | {identity['architecture']} |")
    lines += ["", "These IDs identify local measured artifacts. Release provenance records the published registry "
              "digests and executable checksums; an image-ID or executable equality check is required before equating another artifact with these results.",
              "", "## Method and interpretation", "",
              "Headline groups retain five 60-second repetitions per mode after at least 15 seconds of warmup. "
              "The KV client checks the last three five-second warmup windows for a maximum 10% deviation from their mean, "
              "extending warmup up to 60 seconds. The HTTP client performs 15 seconds of warmup and full-body checks; "
              "it does not implement the KV stability-window test. Engine order is deterministically shuffled within repetitions. "
              "Exploratory groups retain the prior qualification's one/two-repetition durations and establish no precise population ranking.",
              "", "KV services receive CPUs 0–3 and a 768 MiB memory ceiling, except the 384 MiB objective groups; "
              "clients receive CPUs 4–7 and 1,024 MiB. Configured cache capacity is 96 MiB unless labeled 192 MiB. "
              "Navy and extstore add a 512 MiB task-owned file. HTTP cache paths receive two CPUs and 768 MiB total: "
              "the gRPC path divides that budget equally between its cache and Python adapter. The HTTP client receives "
              "four CPUs/1,024 MiB, and a common simulated origin receives two separate CPUs/512 MiB. "
              "Container swap is disabled; no host ports are published.",
              "", "Every response is byte-verified. The KV timed payload is shared across keys, so timed checks alone "
              "do not detect a wrong-key response; separate correctness probes check key-specific behavior. HTTP response "
              "checks include key identity. Closed-loop latency omits requests that could have arrived while a worker "
              "was blocked. Offered-load latency includes scheduled-arrival queueing, and dropped arrivals remain visible. "
              "One operation is one object; batch latency is per 16-object batch/window. Medians below summarize separate "
              "runs, including their per-run p99s; latency samples are never pooled. Closed-loop absolute origin counts "
              "reflect different completed request counts; use hit ratios or the equal-demand offered-load group for origin comparisons. "
              "RAM192 and flash384 objective modes ran as separate series with shuffled engine order inside each; cross-series rates are descriptive.",
              "", "A KV origin miss simulates a fixed 1, 5 or 20 ms wait before filling the cache; HTTP uses a separate "
              "local origin with a 5 ms sleep. These are different origin models. All current HTTP runs use connection "
              "reuse and NGINX upstream keepalive. File tiers traverse Docker Desktop and host page caches: these results "
              "do not measure physical SSD latency, NAND writes, endurance, or financial savings. A larger configured "
              "cache is not the same as a larger measured process footprint. HTTP path memory is the sum of component "
              "cgroup peaks, an upper bound on a simultaneous combined peak."]
    all_rows = {}
    for spec in GROUPS:
        name = spec["name"]
        with (results / name / "runs.csv").open() as file:
            rows = list(csv.DictReader(file))
        all_rows[name] = rows
        validation = validations[name]
        lines += ["", f"## {TITLES[name]}", "",
                  f"{spec['reps']} {'repetition' if spec['reps'] == 1 else 'repetitions'} × {spec['seconds']} seconds per mode/case. "
                  f"[{len(rows)}/{len(expected_runs(spec))} result rows]({relative(results / name / 'runs.csv')}); "
                  f"[summary]({relative(results / name / 'summary.json')}); "
                  f"[raw manifest]({relative(runs / name / 'manifest.json')}).",
                  "", "| Case / mode | n | Median objects/s | Median p99 ms | Hit ratio | Median origin requests | CPU μs/object | Peak MiB | Errors / drops |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for case in spec["cases"]:
            for engine in spec["engines"]:
                selected = [r for r in rows if r["case"] == case and r["engine"] == engine]
                hit, memory = median(selected, "hit_ratio"), median(selected, "server_memory_peak_bytes")
                cells = [f"`{case}` / {LABELS[engine]}", str(len(selected)), number(median(selected, "objects_s")),
                         number(median(selected, "p99_ms"), 3), percent(hit),
                         number(median(selected, "origins")), ("—" if engine == "origin_direct" else number(median(selected, "server_cpu_us_per_object"), 1)),
                         ("—" if engine == "origin_direct" else number(memory/1048576 if memory is not None else None, 1)),
                         f"{sum(int(r['errors']) for r in selected):,} / {sum(int(r.get('dropped_arrivals', 0)) for r in selected):,}"]
                lines.append("| " + " | ".join(cells) + " |")
        notes = validation["problems"] + validation["claim_limitations"] + validation["observations"]
        if notes:
            lines += ["", "Recorded qualification problems/observations:", ""]
            lines += [f"- `{note.replace('`', '')}`" for note in notes]
        else:
            lines += ["", "All expected runs passed correctness and duration checks" +
                      (" and the KV warmup stability requirement." if spec["kind"] == "kv" and spec["headline"] else ".")]
        if name == "offered-saturation":
            lines += ["", "Dropped arrivals represent overload outcomes and are retained; request errors remain qualification failures. "
                      "Rates in this exploratory sweep do not establish a precise saturation threshold."]
        if name == "origin-patterns":
            lines += ["", "The cold-start case intentionally has zero warmup and reports `warmup_stable=false`. "
                      "Its startup results are not steady-state rates. A changing hot set or one-pass trace tests reuse, not playback."]
        if name == "http-onepass":
            lines += ["", "Direct origin has no allocated cache path and is a reference, not an equal-resource cache comparison. "
                      "Cache-path CPU/memory are not applicable; measured origin CPU/memory remain in the CSV. "
                      "NGINX's asynchronous cleanup can temporarily exceed its configured 512 MiB file limit; allocated bytes are retained in the CSV."]
    lines += ["", "## Observed comparisons", ""]
    def result(name, engine, field):
        return median([r for r in all_rows[name] if r["engine"] == engine], field)
    comparisons = 0
    if validations["primary"]["qualified"]:
        grpc, redis = result("primary", "grpc", "objects_s"), result("primary", "redis", "objects_s")
        lines += [f"For the all-RAM 1 KiB/eight-connection case, gRPC served {grpc:,.0f} versus Redis's {redis:,.0f} median objects/s "
                  f"({abs(grpc/redis-1)*100:.1f}% {'higher' if grpc>redis else 'lower'}).", ""]
        comparisons += 1
    if validations["hybrid"]["qualified"]:
        navy, dram = result("hybrid", "grpc_nvm", "objects_s"), result("hybrid", "grpc", "objects_s")
        lines += [f"For the 128 MiB reusable set under 96 MiB configured DRAM with a 5 ms simulated origin, Navy served "
                  f"{navy:,.0f} versus DRAM-only gRPC's {dram:,.0f} objects/s ({navy/dram:.2f}×). "
                  "That comparison adds file capacity and changes actual memory usage; use the separate 384 MiB objective groups when comparing a full-hit service objective.", ""]
        comparisons += 1
    if validations["http"]["qualified"]:
        hgrpc, nginx = result("http", "grpc_adapter", "objects_s"), result("http", "nginx", "objects_s")
        lines += [f"For repeated 256 KiB HTTP objects, the complete gRPC/Python path served {hgrpc:,.0f} versus "
                  f"NGINX's {nginx:,.0f} median objects/s ({abs(hgrpc/nginx-1)*100:.1f}% {'higher' if hgrpc>nginx else 'lower'}). "
                  "This includes adapter, copying and protocol costs. It is not an isolated CacheLib engine result.", ""]
        comparisons += 1
    if limited or failed or problems:
        lines += ["The affected groups' recorded limits prevent their corresponding headline claims. "
                  "Qualified groups are interpreted independently; an exploratory overload outcome does not invalidate another group's measurements.", ""]
    if not comparisons:
        lines += ["No primary throughput comparison qualified; inspect the retained per-run evidence.", ""]
    lines += ["", "## Scope and reproduction", "",
              "The service caches whole binary objects over gRPC. It supplies no HTTP/range delivery, authentication, "
              "TLS, replication, sharding, warm restart, recording, playback buffering or media transport. Navy files "
              "are truncated on startup. Simulated origin avoidance demonstrates reuse under the disclosed trace; "
              "it does not establish production cost, viewer latency, frame continuity or streaming quality.",
              "", "Run the complete campaign with [the documented local Docker commands](README.md#fresh-release-campaign). "
              "All attempts and shuffled schedules remain in the raw manifests. The generated CSVs include per-run "
              "rates, latency percentiles, resource observations, tier counters and backing-file allocations. "
              "The report check verifies campaign completeness, raw-run qualification and CSV-derived tables; "
              "it never modifies the historical reports.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=pathlib.Path, default=ROOT / DEFAULT_RUNS)
    parser.add_argument("--results", type=pathlib.Path, default=ROOT / DEFAULT_RESULTS)
    parser.add_argument("--report", type=pathlib.Path, default=REPORT)
    parser.add_argument("--readme", type=pathlib.Path, default=ROOT / "README.md")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rendered = render(args.runs.resolve(), args.results.resolve())
    readme = args.readme.read_text()
    rendered_readme = replace_readme_headlines(readme, readme_headlines(args.runs.resolve(), args.results.resolve()))
    if args.check:
        if args.report.read_text() != rendered:
            raise SystemExit("Release report differs from campaign/raw results; rerun renderer without --check")
        if readme != rendered_readme:
            raise SystemExit("README headlines differ from qualified campaign results; rerun renderer without --check")
        print("Release report and README headlines match complete campaign and measured results")
    else:
        args.report.write_text(rendered)
        args.readme.write_text(rendered_readme)


if __name__ == "__main__":
    main()
