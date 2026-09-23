"""Rebuild machine-readable summary, CSV, and two SVG charts from raw runs."""
import argparse
import csv
import json
import pathlib
import re
import statistics


def counters(path):
    if not path.exists():
        return {}
    text = path.read_text()
    output = {}
    for key in ("usage_usec", "user_usec", "system_usec"):
        match = re.search(rf"^{key} (\d+)$", text, re.M)
        if match:
            output[key] = int(match.group(1))
    current = re.search(r"^(\d+)$", text, re.M)
    if current:
        output["memory_current"] = int(current.group(1))
    for key in ("rbytes", "wbytes", "rios", "wios"):
        output[key] = sum(int(x) for x in re.findall(rf"\b{key}=(\d+)", text))
    return output


def row(result_path):
    r = json.loads(result_path.read_text())
    args = r["args"]
    directory = result_path.parent
    before = counters(directory / "cgroup-before.txt")
    after = counters(directory / "cgroup-after.txt")
    n = r.get("measured_operations", r.get("operations", 0))
    hits = r.get("object_hits", 0)
    reads = r.get("read_operations", n)
    latency = r.get("latency_ms", r.get("batch_latency_ms", {}))
    row = {
        "engine": directory.parent.name, "case": directory.name,
        "version": (r.get("stats_after", {}).get("valkey_version") or
                    r.get("stats_after", {}).get("version") or
                    r.get("stats_after", {}).get("redis_version", "")),
        "value_bytes": args["value_bytes"], "pattern": args["pattern"], "mode": args["mode"],
        "concurrency": args["concurrency"], "operations": n, "batches": r.get("batches", ""),
        "ops_s": r["successful_ops_s"], "payload_mib_s": r["payload_mib_s"],
        "p50_ms": latency.get("p50"), "p95_ms": latency.get("p95"),
        "p99_ms": latency.get("p99"), "p999_ms": latency.get("p999"),
        "errors": r["errors"], "hits": hits, "read_operations": reads,
        "hit_ratio": hits / reads if reads and "object_hits" in r else None,
        "origin_requests": r.get("origin_requests", ""),
        "origin_bytes_avoided": r.get("origin_bytes_avoided", ""),
        "nvm_hits_delta": (r.get("stats_after", {}).get("nvm_hit_count", 0) -
                           r.get("stats_before", {}).get("nvm_hit_count", 0)),
        "cpu_us_per_op": (after.get("usage_usec", 0) - before.get("usage_usec", 0)) / n if n else None,
        "server_cpu_cores": ((after.get("usage_usec", 0) - before.get("usage_usec", 0)) /
                             (r.get("elapsed_s", 0) * 1_000_000) if r.get("elapsed_s") else None),
        "memory_after_mib": after.get("memory_current", 0) / 1048576,
        "disk_read_mib": (after.get("rbytes", 0) - before.get("rbytes", 0)) / 1048576,
        "disk_write_mib": (after.get("wbytes", 0) - before.get("wbytes", 0)) / 1048576,
        "raw_path": str(result_path),
    }
    return row


def chart(path, rows, case, field, title, unit):
    selected = [r for r in rows if r["case"].startswith(case) and r[field] is not None]
    engines = sorted(set(r["engine"] for r in selected))
    values = [(e, statistics.median(r[field] for r in selected if r["engine"] == e)) for e in engines]
    if not values:
        return
    peak = max(v for _, v in values) or 1
    width = 760
    height = 85 + 46 * len(values)
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
             '<rect width="100%" height="100%" fill="white"/>',
             f'<text x="20" y="30" font-family="sans-serif" font-size="18">{title}</text>']
    for i, (engine, value) in enumerate(values):
        y = 55 + i * 46
        bar = int(475 * value / peak)
        lines += [f'<text x="20" y="{y+18}" font-family="sans-serif" font-size="13">{engine}</text>',
                  f'<rect x="165" y="{y}" width="{bar}" height="25" fill="#326baf"/>',
                  f'<text x="{175+bar}" y="{y+18}" font-family="sans-serif" font-size="13">{value:.3f} {unit}</text>']
    lines.append('</svg>')
    path.write_text("\n".join(lines) + "\n")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("run_dirs", type=pathlib.Path, nargs="+")
    p.add_argument("--output", type=pathlib.Path,
                   help="Output directory; defaults to the first input. Later inputs replace earlier engine/case rows, while all raw runs remain intact.")
    args = p.parse_args()
    out = args.output or args.run_dirs[0]
    out.mkdir(parents=True, exist_ok=True)
    selected = {}
    for run_dir in args.run_dirs:
        for result_path in sorted(run_dir.glob("*/*/result.json")):
            r = row(result_path)
            selected[(r["engine"], r["case"])] = r
    rows = list(selected.values())
    rows.sort(key=lambda r: (r["case"], r["engine"]))
    if not rows:
        raise SystemExit("No raw results")
    with (out / "summary.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (out / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    (out / "selection.json").write_text(json.dumps({"input_directories": [str(p) for p in args.run_dirs],
                                              "selection_rule": "later engine/case result replaces earlier; raw inputs preserved"}, indent=2) + "\n")
    chart(out / "chart-hit-1k.svg", rows, "hit_1k_c1", "p50_ms", "1 KiB RAM hit, unary p50", "ms")
    chart(out / "chart-origin-skew.svg", rows, "origin_skew_64k", "hit_ratio", "64 KiB skewed origin workload: object hit ratio", "fraction")
    chart(out / "chart-steady-hit.svg", rows, "steady_uniform_64k", "hit_ratio", "Prefilled 64 KiB uniform workload: object hit ratio", "fraction")
    chart(out / "chart-steady-memory.svg", rows, "steady_uniform_64k", "memory_after_mib", "Prefilled 64 KiB uniform workload: container memory", "MiB")
    print(f"{len(rows)} raw runs summarized")


if __name__ == "__main__":
    main()
