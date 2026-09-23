"""Common HTTP media-object client, used for NGINX and the gRPC adapter."""
import argparse
import concurrent.futures
import http.client
import json
import math
import random
import threading
import time


def quantile(items, q):
    xs = sorted(items)
    at = (len(xs)-1)*q
    low = math.floor(at)
    return xs[low] + (xs[min(low+1, len(xs)-1)]-xs[low])*(at-low)


def origin_count(host):
    c = http.client.HTTPConnection(host, 8080, timeout=10)
    c.request("GET", "/stats")
    r = c.getresponse()
    n = json.loads(r.read())["origin_requests"]
    c.close()
    return n


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--origin", default="origin")
    p.add_argument("--size", type=int, required=True)
    p.add_argument("--objects", type=int, default=64)
    p.add_argument("--requests", type=int, default=500)
    p.add_argument("--concurrency", type=int, default=1)
    p.add_argument("--pattern", choices=["repeated", "one_pass"], required=True)
    args = p.parse_args()
    rng = random.Random(20260923)
    keys = [rng.randrange(args.objects) if args.pattern == "repeated" else args.objects + i
            for i in range(args.requests)]
    local = threading.local()
    expected = (bytes(range(256)) * (args.size // 256 + 1))[:args.size]

    def get(index):
        if not hasattr(local, "conn"):
            local.conn = http.client.HTTPConnection(args.host, 8080, timeout=30)
        t = time.perf_counter_ns()
        local.conn.request("GET", f"/obj/{args.size}/{index}")
        r = local.conn.getresponse()
        body = r.read()
        ms = (time.perf_counter_ns() - t) / 1e6
        ok = r.status == 200 and body == expected
        return ms, r.getheader("X-Cache"), ok

    if args.pattern == "repeated":
        for i in range(args.objects):
            _, _, ok = get(i)
            assert ok
    before = origin_count(args.origin)
    t = time.perf_counter()
    if args.concurrency == 1:
        results = [get(i) for i in keys]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as pool:
            results = list(pool.map(get, keys))
    elapsed = time.perf_counter() - t
    after = origin_count(args.origin)
    lat = [r[0] for r in results]
    print(json.dumps({"args": vars(args), "elapsed_s": elapsed, "requests": len(results),
                      "ops_s": len(results)/elapsed, "payload_mib_s": len(results)*args.size/1048576/elapsed,
                      "errors": sum(not r[2] for r in results),
                      "cache_status": {x: sum(r[1] == x for r in results) for x in set(r[1] for r in results)},
                      "origin_requests_delta": after-before,
                      "origin_bytes_delta": (after-before)*args.size,
                      "p50_ms": quantile(lat, .5), "p95_ms": quantile(lat, .95),
                      "p99_ms": quantile(lat, .99)}, sort_keys=True))


if __name__ == "__main__":
    main()
