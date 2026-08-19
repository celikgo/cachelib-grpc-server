#!/usr/bin/env python3
"""Compute exact latency percentiles from a ghz JSON run.

ghz's own latencyDistribution stops at p99. This reads the raw per-request
`details` array so p99.9 is a real measurement rather than a p99 fallback.
"""
import json
import sys


def summarize(path):
    with open(path) as fh:
        d = json.load(fh)
    lat = sorted(x["latency"] / 1e6 for x in d["details"] if x["status"] == "OK")
    if not lat:
        raise SystemExit(f"{path}: no successful requests")

    def pct(p):
        # nearest-rank percentile
        return lat[min(len(lat) - 1, int(round(p / 100.0 * len(lat) + 0.5)) - 1)]

    return {
        "rps": d["rps"],
        "count": d["count"],
        "ok": d["statusCodeDistribution"].get("OK", 0),
        "codes": d["statusCodeDistribution"],
        "p50": pct(50), "p90": pct(90), "p99": pct(99),
        "p999": pct(99.9), "p9999": pct(99.99),
        "max": lat[-1],
    }


if __name__ == "__main__":
    for path in sys.argv[1:]:
        s = summarize(path)
        name = path.rsplit("/", 1)[-1].removesuffix(".json")
        print(f'{name:<12} {s["rps"]:>9,.0f} rps  p50 {s["p50"]:6.2f}  '
              f'p90 {s["p90"]:6.2f}  p99 {s["p99"]:6.2f}  p99.9 {s["p999"]:6.2f}  '
              f'p99.99 {s["p9999"]:6.2f} ms   {s["codes"]}')
