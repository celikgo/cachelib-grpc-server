"""Render/check current-version report tables from committed run CSVs.

This deliberately leaves historical 1.6.0 BENCHMARKS.md to bench/render.py.
"""
import argparse
import csv
import pathlib
import statistics


ROOT = pathlib.Path(__file__).resolve().parent
REPORT = ROOT / "REPORT.md"
LABELS = {
    "primary": [("grpc", "CacheLib gRPC 1.8.0 candidate"),
                ("redis", "Redis OSS 8.2.10"), ("valkey", "Valkey 8.1.10"),
                ("memcached", "Memcached 1.6.45")],
    "hybrid": [("grpc", "CacheLib gRPC DRAM"),
               ("grpc_nvm", "CacheLib gRPC + Navy"),
               ("redis", "Redis OSS"), ("valkey", "Valkey"),
               ("memcached", "Memcached DRAM"),
               ("memcached_extstore", "Memcached extstore")],
    "offered": [("grpc", "CacheLib gRPC DRAM"),
                ("grpc_nvm", "CacheLib gRPC + Navy"),
                ("redis", "Redis OSS"),
                ("memcached_extstore", "Memcached extstore")],
    "objective": [("grpc", "CacheLib gRPC, 192 MiB DRAM", "grpc-ram192"),
                  ("grpc_nvm", "CacheLib gRPC, 96 MiB DRAM + Navy", "nvm384"),
                  ("redis", "Redis OSS, 192 MiB DRAM", "ram192"),
                  ("memcached", "Memcached, 192 MiB DRAM", "ram192"),
                  ("memcached_extstore", "Memcached, 96 MiB DRAM + extstore", "extstore384")],
    "http": [("grpc_adapter", "CacheLib gRPC + Python HTTP adapter"),
             ("nginx", "NGINX disk cache")],
}


def table(kind):
    if kind == "objective":
        grouped = {}
        for engine, _, source in LABELS[kind]:
            with (ROOT / "release-results" / source / "runs.csv").open() as file:
                grouped[engine] = sorted((r for r in csv.DictReader(file)
                                          if r["engine"] == engine),
                                         key=lambda r: int(r["repetition"][1:]))
    else:
        path = ROOT / "release-results" / kind / "runs.csv"
        with path.open() as file:
            rows = list(csv.DictReader(file))
        grouped = {engine: sorted((r for r in rows if r["engine"] == engine),
                                  key=lambda r: int(r["repetition"][1:]))
                   for engine, _ in LABELS[kind]}
    def invalid(r):
        if int(r["errors"]):
            return True
        if kind == "http":
            return int(r["origins"]) != 0
        return int(r["dropped_arrivals"]) != 0 or r["warmup_stable"] != "True"

    if any(len(rs) != 5 or {r["repetition"] for r in rs} !=
           {f"r{i}" for i in range(5)} or any(invalid(r) for r in rs)
           for rs in grouped.values()):
        raise ValueError(f"{kind} requires five distinct, successful, stable repetitions")

    if kind == "primary":
        lines = ["| Engine | Five objects/s runs, in repetition order | Median objects/s | "
                 "Median p99 ms | Median server CPU μs/object | Median cgroup peak MiB |",
                 "|---|---|---:|---:|---:|---:|"]
    elif kind == "offered":
        lines = ["| Mode | Five origin-request counts | Median origin requests | "
                 "Median p99 ms | Median cgroup peak MiB |",
                 "|---|---|---:|---:|---:|"]
    elif kind == "objective":
        lines = ["| Mode | Five objects/s runs | Median objects/s | Median p99 ms | "
                 "Server CPU μs/object | Cgroup peak MiB |",
                 "|---|---|---:|---:|---:|---:|"]
    elif kind == "http":
        lines = ["| HTTP path | Five objects/s runs | Median objects/s | Median p99 ms | "
                 "Path CPU μs/object | Path cgroup peak sum MiB |",
                 "|---|---|---:|---:|---:|---:|"]
    else:
        lines = ["| Mode | Five objects/s runs | Median objects/s | Object/byte hit ratio | "
                 "Median p99 ms | Server CPU μs/object | Cgroup peak MiB |",
                 "|---|---|---:|---:|---:|---:|---:|"]
    for entry in LABELS[kind]:
        engine, label = entry[:2]
        rs = grouped[engine]
        median = lambda field: statistics.median(float(r[field]) for r in rs)
        if kind == "offered":
            origins = "; ".join(f"{int(r['origins']):,}" for r in rs)
            lines.append("| " + " | ".join([label, origins,
                         f"{median('origins'):,.0f}", f"{median('p99_ms'):.2f}",
                         f"{median('server_memory_peak_bytes') / 1048576:.1f}"]) + " |")
            continue
        rates = "; ".join(f"{float(r['objects_s']):,.0f}" for r in rs)
        base = [label, rates, f"{median('objects_s'):,.0f}"]
        if kind == "hybrid":
            hit = median("hit_ratio") * 100
            base.append("100%" if abs(hit - 100) < .05 else f"{hit:.1f}%")
            base.append(f"{median('p99_ms'):.2f}")
        elif kind in ("objective", "http"):
            base.append(f"{median('p99_ms'):.2f}")
        else:
            base.append(f"{median('p99_ms'):.3f}")
        base.extend([f"{median('server_cpu_us_per_object'):.1f}",
                     f"{median('server_memory_peak_bytes') / 1048576:.1f}"])
        lines.append("| " + " | ".join(base) + " |")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    original = REPORT.read_text()
    rendered = original
    for kind in LABELS:
        start = f"<!-- {kind}-table:start -->\n"
        end = f"\n<!-- {kind}-table:end -->"
        if rendered.count(start) != 1 or rendered.count(end) != 1:
            raise ValueError(f"missing or duplicate {kind} table markers")
        head, tail = rendered.split(start, 1)
        _, rest = tail.split(end, 1)
        rendered = head + start + table(kind) + end + rest
    if args.check:
        if original != rendered:
            raise SystemExit("Strong report tables differ from run CSV; rerun without --check")
        print("Strong report tables match all five raw repetitions")
    else:
        REPORT.write_text(rendered)


if __name__ == "__main__":
    main()
