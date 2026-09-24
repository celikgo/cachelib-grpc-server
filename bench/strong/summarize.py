"""Generate run-level CSV, repetition summaries, and source-linked SVG charts.

Historical bench/results-summary.json is deliberately untouched.
"""
import argparse
import csv
import json
import pathlib
import random
import statistics
from collections import defaultdict


def cgroup(text):
    rows = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("usage_usec",):
            rows[parts[0]] = int(parts[1])
        elif len(parts) == 1 and parts[0].isdigit():
            rows.setdefault("memory.current", int(parts[0]))
            rows["memory.peak"] = int(parts[0])
    return rows


def counter_delta(before, after, *names):
    for name in names:
        if name in after:
            return int(after[name]) - int(before.get(name, 0))
    return None


def io_counters(text):
    totals = {name: 0 for name in ("rbytes", "wbytes", "rios", "wios")}
    for line in text.splitlines():
        fields = line.split()
        if not fields or ":" not in fields[0]:
            continue
        for field in fields[1:]:
            name, sep, value = field.partition("=")
            if sep and name in totals:
                totals[name] += int(value)
    return totals


def bootstrap_median(values):
    if len(values) < 5:
        return None
    rng = random.Random(20260923)
    samples = []
    for _ in range(10000):
        draw = [values[rng.randrange(len(values))] for _ in values]
        samples.append(statistics.median(draw))
    samples.sort()
    return [samples[250], samples[9749]]


def svg_chart(path, title, groups, field, unit):
    selected = [(case, engine, d["median"][field]) for (case, engine), d in groups.items()
                if field in d["median"]]
    selected.sort()
    if not selected:
        return
    width, row_h, left = 1100, 27, 400
    height = 80 + row_h * len(selected)
    maximum = max(v for _, _, v in selected) or 1
    colors = {"grpc": "#1d6fa5", "grpc_nvm": "#158b8b", "redis": "#b94a48",
              "valkey": "#8b5a9f", "memcached": "#d48524",
              "memcached_extstore": "#b59a1d", "nginx": "#367b45",
              "grpc_adapter": "#1d6fa5", "dragonfly": "#3f7f4f",
              "dragonfly_tiered": "#6a9e56", "garnet": "#a14a72",
              "garnet_storage": "#c0748f", "kvrocks": "#7a6a52"}
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
             f'viewBox="0 0 {width} {height}">',
             '<rect width="100%" height="100%" fill="white"/>',
             f'<text x="18" y="30" font-family="sans-serif" font-size="18">{title}</text>',
             f'<text x="{left}" y="52" font-family="sans-serif" font-size="12">{unit}; median of independent runs</text>']
    for i, (case, engine, value) in enumerate(selected):
        y = 68 + i * row_h
        label = f"{case} / {engine}"
        bar = int((width-left-150) * value / maximum)
        lines.append(f'<text x="18" y="{y+14}" font-family="monospace" font-size="12">{label}</text>')
        lines.append(f'<rect x="{left}" y="{y}" width="{bar}" height="18" '
                     f'fill="{colors.get(engine, "#666")}"/>')
        lines.append(f'<text x="{left+bar+8}" y="{y+14}" font-family="sans-serif" '
                     f'font-size="12">{value:,.1f}</text>')
    lines.append("</svg>")
    path.write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--kind", choices=["kv", "http"], required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rows, failures = [], []
    for path in sorted(args.input.glob("*/r*/*/")):
        case, repeat, engine = path.parts[-3:]
        result_path = path / "result.json"
        if not result_path.exists():
            failures.append({"case": case, "repetition": repeat, "engine": engine,
                             "error": (path / "error.txt").read_text() if (path / "error.txt").exists()
                             else "missing result.json"})
            continue
        if (path / "error.txt").exists():
            failures.append({"case": case, "repetition": repeat, "engine": engine,
                             "error": (path / "error.txt").read_text()})
        d = json.loads(result_path.read_text())
        row = {"case": case, "repetition": repeat, "engine": engine,
               "errors": d["errors"], "elapsed_s": d["elapsed_s"]}
        if args.kind == "kv":
            before = cgroup((path / "server-cgroup-before.txt").read_text())
            after = cgroup((path / "server-cgroup-after.txt").read_text())
            io_before = io_counters((path / "server-cgroup-before.txt").read_text())
            io_after = io_counters((path / "server-cgroup-after.txt").read_text())
            stats_before, stats_after = d["server_stats_before"], d["server_stats_after"]
            # A high-volume run records its backing-file size and then reclaims
            # the disk, so prefer the recorded measurement when it exists.
            # Recursive: Navy and extstore write one top-level file, while
            # Garnet's storage tier and Kvrocks write into subdirectories.
            usage_path = path / "flash-usage.json"
            if usage_path.exists():
                usage = json.loads(usage_path.read_text())
                flash_logical = usage["flash_file_logical_bytes"]
                flash_allocated = usage["flash_file_allocated_bytes"]
            else:
                flash_files = [f for f in (path / "flash").rglob("*") if f.is_file()]
                flash_logical = sum(f.stat().st_size for f in flash_files)
                flash_allocated = sum(f.stat().st_blocks*512 for f in flash_files)
            cpu = (after.get("usage_usec", 0)-before.get("usage_usec", 0))/1e6
            completed = d["operations"]-d["errors"]
            gets = d.get("get_attempts", d["operations"])
            row |= {"objects_s": d["successful_ops_s"], "payload_mib_s": d["payload_mib_s"],
                    "operations": d["operations"], "batches": d["batches"],
                    "get_attempts": gets, "hits": d["hits"], "origins": d["origins"],
                    "hit_ratio": d["hits"]/gets if gets else None,
                    "byte_hit_ratio": d["hits"]/gets if gets else None,
                    "origin_per_1000_gets": 1000*d["origins"]/gets if gets else None,
                    "origin_bytes": d["origin_bytes"],
                    "origin_bytes_avoided": d["origin_bytes_avoided"],
                    "p50_ms": d["latency_ms"].get("p50"), "p95_ms": d["latency_ms"].get("p95"),
                    "p99_ms": d["latency_ms"].get("p99"), "p999_ms": d["latency_ms"].get("p999"),
                    "samples": d["latency_samples"], "warmup_stable": d["warmup_stable"],
                    "dropped_arrivals": d["dropped_arrivals"],
                    "server_cpu_s": cpu, "server_cpu_us_per_object": cpu*1e6/completed if completed else None,
                    "server_memory_current_bytes": after.get("memory.current"),
                    "server_memory_peak_bytes": after.get("memory.peak"),
                    **{f"cgroup_io_{name}_delta": io_after[name]-io_before[name]
                       for name in io_after},
                    "client_cpu_s": (d["client_cgroup_after"].get("usage_usec", 0)-
                                     d["client_cgroup_before"].get("usage_usec", 0))/1e6,
                    "client_memory_peak_bytes": d["client_cgroup_after"].get("memory.peak"),
                    "flash_file_logical_bytes": flash_logical,
                    "flash_file_allocated_bytes": flash_allocated,
                    "nvm_hits_delta": int(d["server_stats_after"].get("nvm_hit_count", 0))-
                                      int(d["server_stats_before"].get("nvm_hit_count", 0)),
                    "nvm_written_bytes_delta": int(d["server_stats_after"].get("nvm_used", 0))-
                                               int(d["server_stats_before"].get("nvm_used", 0)),
                    "nvm_device_bytes_written_delta": int(d["server_stats_after"].get("nvm_device_bytes_written", 0))-
                                                      int(d["server_stats_before"].get("nvm_device_bytes_written", 0)),
                    "nvm_device_bytes_read_delta": int(d["server_stats_after"].get("nvm_device_bytes_read", 0))-
                                                   int(d["server_stats_before"].get("nvm_device_bytes_read", 0)),
                    "extstore_objects_read_delta": int(d["server_stats_after"].get("extstore_objects_read", 0))-
                                                   int(d["server_stats_before"].get("extstore_objects_read", 0)),
                    "extstore_bytes_written_delta": int(d["server_stats_after"].get("extstore_bytes_written", 0))-
                                                    int(d["server_stats_before"].get("extstore_bytes_written", 0)),
                    "extstore_bytes_read_delta": counter_delta(stats_before, stats_after,
                                                               "extstore_bytes_read"),
                    "evictions_delta": counter_delta(stats_before, stats_after,
                                                     "eviction_count", "evicted_keys", "evictions"),
                    "cache_item_count_after": next((int(stats_after[name]) for name in
                                                   ("item_count", "curr_items") if name in stats_after), None),
                    "cache_used_bytes_after": next((int(stats_after[name]) for name in
                                                    ("used_size", "bytes", "used_memory") if name in stats_after), None)}
            if engine == "grpc_nvm":
                # Navy's successful lookups are the flash-hit source. The
                # difference from verified GET hits is inferred DRAM hits.
                row["flash_hits"] = row["nvm_hits_delta"]
                row["inferred_dram_hits"] = d["hits"] - row["flash_hits"]
                row["flash_hit_ratio"] = row["flash_hits"] / gets if gets else None
                row["inferred_dram_hit_ratio"] = row["inferred_dram_hits"] / gets if gets else None
            elif engine == "grpc":
                row["flash_hits"] = 0
                row["inferred_dram_hits"] = d["hits"]
                row["flash_hit_ratio"] = 0
                row["inferred_dram_hit_ratio"] = d["hits"] / gets if gets else None
        else:
            # These containers have separate cgroups. The sum of their
            # individual peaks is an upper bound, not a sampled simultaneous
            # peak of the entire HTTP path.
            cpu, memory = 0.0, 0
            component_cpu, component_memory = [], []
            origin_name = d.get("origin_container")
            origin_before = cgroup((path / f"{origin_name}.cgroup-before.txt").read_text()) if origin_name else {}
            origin_after = cgroup((path / f"{origin_name}.cgroup-after.txt").read_text()) if origin_name else {}
            cache_files = [f for f in (path / "cache-files").rglob("*") if f.is_file()]
            for name in d["path_containers"]:
                before = cgroup((path / f"{name}.cgroup-before.txt").read_text())
                after = cgroup((path / f"{name}.cgroup-after.txt").read_text())
                component_cpu.append((after.get("usage_usec", 0)-before.get("usage_usec", 0))/1e6)
                component_memory.append(after.get("memory.peak", 0))
                cpu += component_cpu[-1]
                memory += component_memory[-1]
            row |= {"objects_s": d["objects_s"], "payload_mib_s": d["payload_mib_s"],
                    "operations": d["successful_objects"], "origins": d["origin_requests_delta"],
                    "origin_bytes": d["origin_bytes_delta"],
                    "origin_bytes_avoided": max(0, d["successful_objects"]-
                                                d["origin_requests_delta"])*d["size_bytes"],
                    "hit_ratio": 1-d["origin_requests_delta"]/max(1, d["successful_objects"]),
                    "byte_hit_ratio": 1-d["origin_requests_delta"]/max(1, d["successful_objects"]),
                    "p50_ms": d["p50_ms"], "p95_ms": d["p95_ms"], "p99_ms": d["p99_ms"],
                    "p999_ms": d.get("p999_ms"), "samples": d["latency_samples"],
                    "server_cpu_s": cpu, "server_cpu_us_per_object": cpu*1e6/max(1, d["successful_objects"]),
                    "server_memory_peak_bytes": memory,
                    "path_component_peak_sum_upper_bound_bytes": memory,
                    "client_cpu_s": d["client_cpu_usage_usec"]/1e6,
                    "client_memory_peak_bytes": d["client_memory_peak_bytes"],
                    "cache_file_logical_bytes": sum(f.stat().st_size for f in cache_files),
                    "cache_file_allocated_bytes": sum(f.stat().st_blocks*512 for f in cache_files),
                    "origin_cpu_s": (origin_after.get("usage_usec", 0)-
                                     origin_before.get("usage_usec", 0))/1e6 if origin_name else None,
                    "origin_memory_peak_bytes": origin_after.get("memory.peak") if origin_name else None,
                    "nginx_cpu_s": component_cpu[0] if engine == "nginx" else None,
                    "adapter_cpu_s": component_cpu[1] if engine == "grpc_adapter" else None,
                    "grpc_server_cpu_s": component_cpu[0] if engine == "grpc_adapter" else None,
                    "adapter_memory_peak_bytes": component_memory[1] if engine == "grpc_adapter" else None,
                    "grpc_server_memory_peak_bytes": component_memory[0] if engine == "grpc_adapter" else None}
        rows.append(row)
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with (args.output / "runs.csv").open("w", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    groups = defaultdict(list)
    for row in rows:
        groups[(row["case"], row["engine"])].append(row)
    summary = {}
    numeric = ("objects_s", "payload_mib_s", "p50_ms", "p95_ms", "p99_ms",
               "p999_ms", "hit_ratio", "byte_hit_ratio", "origins", "origin_bytes",
               "origin_bytes_avoided", "origin_per_1000_gets",
               "server_cpu_us_per_object", "server_memory_current_bytes",
               "server_memory_peak_bytes", "client_cpu_s", "client_memory_peak_bytes",
               "evictions_delta", "cache_item_count_after", "cache_used_bytes_after",
               "nvm_hits_delta", "nvm_device_bytes_read_delta", "nvm_device_bytes_written_delta",
               "cgroup_io_rbytes_delta", "cgroup_io_wbytes_delta",
               "cgroup_io_rios_delta", "cgroup_io_wios_delta",
               "flash_hits", "inferred_dram_hits", "flash_hit_ratio",
               "inferred_dram_hit_ratio",
               "extstore_objects_read_delta", "extstore_bytes_read_delta",
               "extstore_bytes_written_delta", "flash_file_allocated_bytes",
               "cache_file_allocated_bytes", "nginx_cpu_s", "adapter_cpu_s",
               "grpc_server_cpu_s", "adapter_memory_peak_bytes",
               "grpc_server_memory_peak_bytes", "origin_cpu_s",
               "origin_memory_peak_bytes", "path_component_peak_sum_upper_bound_bytes")
    for key, group in sorted(groups.items()):
        medians = {field: statistics.median([r[field] for r in group if r.get(field) is not None])
                   for field in numeric if any(r.get(field) is not None for r in group)}
        summary["/".join(key)] = {"n": len(group), "medians": medians,
                                  "range_objects_s": [min(r["objects_s"] for r in group),
                                                      max(r["objects_s"] for r in group)],
                                  "bootstrap95_median_objects_s": bootstrap_median(
                                      [r["objects_s"] for r in group]),
                                  "repetitions": [r["repetition"] for r in group],
                                  "errors": sum(r["errors"] for r in group)}
    (args.output / "summary.json").write_text(json.dumps({
        "source": str(args.input), "kind": args.kind, "groups": summary,
        "failed_runs": failures,
        "method": "Per-run p50/p95/p99/p999 are not pooled. Group medians use run values; "
                  "95% intervals are 10,000 seeded bootstrap resamples of run medians when n>=5. "
                  "Small-n bootstrap intervals are descriptive, not a guarantee of coverage."
    }, indent=2) + "\n")
    chart_groups = {key: {"median": {"objects_s": value["medians"]["objects_s"],
                                    **({"hit_ratio": value["medians"]["hit_ratio"]}
                                       if "hit_ratio" in value["medians"] else {})}}
                    for key, value in ((tuple(k.split("/")), v) for k, v in summary.items())}
    svg_chart(args.output / "throughput.svg", "Successful objects per second",
              chart_groups, "objects_s", "objects/s")
    svg_chart(args.output / "hit-ratio.svg", "Object hit ratio",
              chart_groups, "hit_ratio", "fraction")
    print(f"{len(rows)} successful runs, {len(failures)} failed or missing runs")


if __name__ == "__main__":
    main()
