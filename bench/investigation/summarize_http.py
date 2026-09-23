"""Generate an auditable CSV/JSON and SVGs from raw common-HTTP runs."""
import argparse
import csv
import json
import pathlib

from summarize import chart


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_dir", type=pathlib.Path)
    args = parser.parse_args()
    rows = []
    for path in sorted(args.run_dir.glob("*.json")):
        if path.name == "manifest.json" or path.name.endswith((".command.json", ".server-command.json")):
            continue
        raw = json.loads(path.read_text())
        if "ops_s" not in raw:
            continue
        options = raw["args"]
        engine = "NGINX disk cache" if path.stem.startswith("nginx-") else "gRPC HTTP adapter"
        case = f"{options['pattern']}-{options['size']}-{options['concurrency']}"
        rows.append({"engine": engine, "case": case, "requests": raw["requests"],
                     "ops_s": raw["ops_s"], "payload_mib_s": raw["payload_mib_s"],
                     "p50_ms": raw["p50_ms"], "p95_ms": raw["p95_ms"],
                     "p99_ms": raw["p99_ms"], "errors": raw["errors"],
                     "origin_requests": raw["origin_requests_delta"],
                     "origin_bytes": raw["origin_bytes_delta"],
                     "cache_path_memory_after_mib": (raw["cache_path_memory_after_bytes"] / 1048576
                                                     if "cache_path_memory_after_bytes" in raw else None),
                     "raw_path": str(path)})
    if not rows:
        raise SystemExit("No HTTP result files")
    rows.sort(key=lambda row: (row["case"], row["engine"]))
    with (args.run_dir / "summary.csv").open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.run_dir / "summary.json").write_text(json.dumps(rows, indent=2) + "\n")
    for case, title in (("repeated-262144-1", "Repeated 256 KiB objects, one reader"),
                        ("repeated-1048576-1", "Repeated 1 MiB objects, one reader"),
                        ("one_pass-262144-1", "One-pass 256 KiB objects, one reader")):
        chart(args.run_dir / f"chart-{case}.svg", rows, case, "ops_s", title, "HTTP requests/s")
    chart(args.run_dir / "chart-repeated-256k-memory.svg", rows, "repeated-262144-1",
          "cache_path_memory_after_mib", "Repeated 256 KiB: cache-path container memory", "MiB")
    print(f"{len(rows)} HTTP runs summarized")


if __name__ == "__main__":
    main()
