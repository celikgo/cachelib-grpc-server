"""Deterministic binary cache trace runner. Results are JSON, one invocation per run."""
import argparse
import concurrent.futures
import hashlib
import json
import math
import random
import statistics
import threading
import time

import grpc
import redis
from pymemcache.client.base import Client as MemcacheClient

import cache_pb2 as pb
import cache_pb2_grpc as rpc


def payload(size):
    block = bytes(range(256))
    return (block * (size // 256 + 1))[:size]


class Backend:
    def __init__(self, kind, host, port):
        self.kind, self.host, self.port = kind, host, port
        self.local = threading.local()

    def client(self):
        if not hasattr(self.local, "client"):
            if self.kind == "grpc":
                channel = grpc.insecure_channel(f"{self.host}:{self.port}", options=[
                    ("grpc.max_receive_message_length", 8 * 1024 * 1024),
                    ("grpc.max_send_message_length", 8 * 1024 * 1024),
                ])
                self.local.client = rpc.CacheServiceStub(channel)
            elif self.kind in ("redis", "valkey"):
                self.local.client = redis.Redis(host=self.host, port=self.port, socket_timeout=10)
            else:
                self.local.client = MemcacheClient((self.host, self.port), connect_timeout=10,
                                                   timeout=10, no_delay=True)
        return self.local.client

    def get(self, key):
        c = self.client()
        if self.kind == "grpc":
            r = c.Get(pb.GetRequest(key=key), timeout=10)
            return r.value if r.found else None
        return c.get(key)

    def set(self, key, value, ttl=0):
        c = self.client()
        if self.kind == "grpc":
            return c.Set(pb.SetRequest(key=key, value=value, ttl_seconds=ttl), timeout=10).success
        if self.kind in ("redis", "valkey"):
            return bool(c.set(key, value, ex=ttl or None))
        return bool(c.set(key, value, expire=ttl))

    def delete(self, key):
        c = self.client()
        if self.kind == "grpc":
            return c.Delete(pb.DeleteRequest(key=key), timeout=10).success
        return bool(c.delete(key))

    def stats(self):
        c = self.client()
        if self.kind == "grpc":
            r = c.Stats(pb.StatsRequest(), timeout=10)
            return {f.name: (len(getattr(r, f.name)) if f.label == 3 else getattr(r, f.name))
                    for f in r.DESCRIPTOR.fields}
        if self.kind in ("redis", "valkey"):
            info = c.info()
            return {k: info.get(k) for k in (
                "redis_version", "valkey_version", "used_memory", "used_memory_rss",
                "keyspace_hits", "keyspace_misses", "evicted_keys", "total_commands_processed")}
        return {k.decode(): (v.decode() if isinstance(v, bytes) else v) for k, v in c.stats().items()
                if k in (b"version", b"bytes", b"curr_items", b"get_hits", b"get_misses", b"evictions")
                or k.startswith(b"extstore_")}


def verify(b):
    key = "inv:verify:binary"
    value = payload(1024) + b"\x00\xff\x80"
    b.delete(key)
    assert b.get(key) is None, "miss"
    assert b.set(key, value), "binary set"
    assert b.get(key) == value, "binary round trip"
    assert b.set(key, b"new\x00value"), "overwrite"
    assert b.get(key) == b"new\x00value", "overwrite read"
    assert b.delete(key), "delete"
    assert b.get(key) is None, "deleted key"
    assert b.set(key, value, 1), "TTL set"
    start = time.monotonic()
    while b.get(key) is not None and time.monotonic() - start < 4:
        time.sleep(0.1)
    expiry_s = time.monotonic() - start
    assert b.get(key) is None, f"TTL did not expire within 4 s (requested 1 s)"
    out = {"basic": "passed", "ttl_requested_s": 1, "ttl_observed_expiry_s": expiry_s}
    if b.kind == "grpc":
        c = b.client()
        items = [("inv:batch:a", value), ("inv:batch:b", b"two")]
        s = c.MultiSet(pb.MultiSetRequest(items=[pb.SetRequest(key=k, value=v) for k, v in items]), timeout=10)
        assert s.succeeded_count == 2 and s.failed_count == 0, "MultiSet"
        g = c.MultiGet(pb.MultiGetRequest(keys=[k for k, _ in items]), timeout=10)
        assert [(x.key, x.value) for x in g.results] == items, "MultiGet order/bytes"
        requests = iter([
            pb.PipelineRequest(sequence_id=41, set=pb.SetRequest(key="inv:pipe", value=value)),
            pb.PipelineRequest(sequence_id=42, get=pb.GetRequest(key="inv:pipe")),
            pb.PipelineRequest(sequence_id=43, delete=pb.DeleteRequest(key="inv:pipe")),
        ])
        responses = list(c.Pipeline(requests, timeout=10))
        assert [r.sequence_id for r in responses] == [41, 42, 43], "Pipeline correlation/order"
        assert responses[0].set.success and responses[1].get.value == value and responses[2].delete.success
        out["batch_pipeline"] = "passed"
    return out


def limits(b):
    if b.kind != "grpc":
        raise ValueError("limits probes the wrapper's gRPC and CacheLib boundaries")
    c = b.client()
    out = {"key_lengths": {}, "value_lengths": {}}
    for n in (250, 255, 256, 512):
        key = "k" * n
        try:
            s = c.Set(pb.SetRequest(key=key, value=b"z"), timeout=10)
            out["key_lengths"][str(n)] = {"success": s.success,
                                           "roundtrip": b.get(key) == b"z" if s.success else False}
        except grpc.RpcError as exc:
            out["key_lengths"][str(n)] = {"rpc_error": exc.code().name}
    for n in (4193000, 4194303, 4194305, 4196000):
        key = f"limit:{n}"
        try:
            expected = payload(n)
            s = c.Set(pb.SetRequest(key=key, value=expected), timeout=20)
            out["value_lengths"][str(n)] = {"success": s.success,
                                             "roundtrip": b.get(key) == expected if s.success else False}
        except grpc.RpcError as exc:
            out["value_lengths"][str(n)] = {"rpc_error": exc.code().name}
    big = payload(2500000)
    try:
        r = c.MultiSet(pb.MultiSetRequest(items=[pb.SetRequest(key=f"limit:batch:{i}", value=big)
                                               for i in range(2)]), timeout=20)
        out["multiset_over_message_limit"] = {"success": r.success}
    except grpc.RpcError as exc:
        out["multiset_over_message_limit"] = {"rpc_error": exc.code().name}
    for i in range(2):
        assert b.set(f"limit:batch:{i}", big)
    try:
        r = c.MultiGet(pb.MultiGetRequest(keys=[f"limit:batch:{i}" for i in range(2)]), timeout=20)
        out["multiget_over_message_limit"] = {"results": len(r.results)}
    except grpc.RpcError as exc:
        out["multiget_over_message_limit"] = {"rpc_error": exc.code().name}
    return out


def quantile(values, q):
    if not values:
        return None
    s = sorted(values)
    x = (len(s) - 1) * q
    lo = math.floor(x)
    return s[lo] + (s[min(lo + 1, len(s)-1)] - s[lo]) * (x - lo)


def make_trace(args):
    rng = random.Random(args.seed)
    trace = []
    for i in range(args.requests):
        if args.pattern == "one_pass":
            index = args.objects + i
        elif args.pattern == "hot_shift":
            width = max(1, args.objects // 10)
            base = 0 if i < args.requests // 2 else args.objects - width
            index = base + rng.randrange(width)
        elif args.pattern == "skew":
            index = int(args.objects * rng.betavariate(0.35, 2.0))
        else:
            index = rng.randrange(args.objects)
        trace.append(index)
    return trace


def run(args, b):
    value = payload(args.value_bytes)
    prefix = f"inv:{args.run_id}:"
    # Preload the complete logical dataset for hit tests. For origin tests,
    # begin cold so cache fills, evicts, and exposes origin traffic.
    preload = args.mode in ("hit", "mixed") or args.preload
    if preload:
        for i in range(args.objects):
            if not b.set(prefix + str(i), value):
                raise RuntimeError(f"preload failed at {i}")
    trace = make_trace(args)
    # Fixed warmup is explicitly excluded. A hit test reuses the same trace;
    # an origin test warms via the first 10% of its trace, then measures the rest.
    warm_count = min(args.warmup, len(trace))
    warm = trace[:warm_count] if args.mode == "origin" else trace[:warm_count]
    measured = trace[warm_count:] if args.mode == "origin" else trace

    def call(index, write=False):
        key = prefix + str(index)
        start = time.perf_counter_ns()
        if args.mode == "bypass":
            time.sleep(args.origin_ms / 1000)
            return ((time.perf_counter_ns() - start) / 1e6, False, 1, None, False)
        if write:
            ok = b.set(key, value)
            return ((time.perf_counter_ns() - start) / 1e6, False, 0,
                    None if ok else "write failed", True)
        found = b.get(key)
        hit = found is not None
        origin = 0
        error = None
        if hit:
            if found != value:
                error = "value mismatch"
        elif args.mode == "origin":
            time.sleep(args.origin_ms / 1000)
            origin = 1
            if not b.set(key, value):
                error = "cache fill failed"
        else:
            error = "unexpected miss"
        return ((time.perf_counter_ns() - start) / 1e6, hit, origin, error, False)

    for index in warm:
        call(index)
    before = b.stats()
    start = time.perf_counter()
    operations = [(index, args.mode == "mixed" and n % 10 == 0)
                  for n, index in enumerate(measured)]
    if args.concurrency == 1:
        records = [call(i, w) for i, w in operations]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.concurrency) as ex:
            records = list(ex.map(lambda pair: call(*pair), operations))
    elapsed = time.perf_counter() - start
    after = b.stats()
    latencies = [r[0] for r in records]
    errors = [r[3] for r in records if r[3]]
    hits = sum(r[1] for r in records)
    origins = sum(r[2] for r in records)
    writes = sum(r[4] for r in records)
    return {
        "schema": 1, "args": vars(args), "backend": b.kind,
        "definitions": {"operation": "one object GET plus optional origin delay and cache SET on miss; mixed includes SET; bypass models only origin delay",
                        "payload_bytes": "bytes in successful cache GET responses",
                        "origin_bytes_avoided": "hits times fixed object size, relative to fetching every request"},
        "preloaded_objects": args.objects if preload else 0,
        "warmup_operations": len(warm), "measured_operations": len(records),
        "read_operations": len(records)-writes, "write_operations": writes,
        "elapsed_s": elapsed, "successful_ops_s": (len(records)-len(errors))/elapsed,
        "payload_mib_s": hits*args.value_bytes/1048576/elapsed,
        "object_hits": hits, "byte_hits": hits*args.value_bytes,
        "origin_requests": origins, "origin_bytes": origins*args.value_bytes,
        "origin_bytes_avoided": hits*args.value_bytes,
        "errors": len(errors), "error_examples": errors[:5],
        "latency_ms": {f"p{q}": quantile(latencies, f) for q, f in
                       [(50, .5), (95, .95), (99, .99), (999, .999)] if q != 999 or len(latencies) >= 10000},
        "stats_before": before, "stats_after": after,
    }


def batch(args, b):
    """Compare complete batches; one operation is one object GET."""
    value = payload(args.value_bytes)
    prefix = f"inv:{args.run_id}:"
    for i in range(args.objects):
        assert b.set(prefix + str(i), value)
    trace = make_trace(args)
    batches = [trace[i:i+args.batch_size] for i in range(0, len(trace), args.batch_size)]
    latencies, errors = [], []
    start = time.perf_counter()
    for indices in batches:
        keys = [prefix + str(i) for i in indices]
        t = time.perf_counter_ns()
        if b.kind == "grpc":
            if args.batch_kind == "pipeline":
                reqs = (pb.PipelineRequest(sequence_id=j, get=pb.GetRequest(key=k))
                        for j, k in enumerate(keys))
                rs = list(b.client().Pipeline(reqs, timeout=30))
                got = [r.get.value if r.get.found else None for r in rs]
                if [r.sequence_id for r in rs] != list(range(len(keys))):
                    errors.append("sequence mismatch")
            else:
                rs = b.client().MultiGet(pb.MultiGetRequest(keys=keys), timeout=30)
                got = [r.value if r.found else None for r in rs.results]
        elif b.kind in ("redis", "valkey"):
            pipe = b.client().pipeline(transaction=False)
            for k in keys:
                pipe.get(k)
            got = pipe.execute()
        else:
            mapping = b.client().get_many(keys)
            got = [mapping.get(k, mapping.get(k.encode())) for k in keys]
        latencies.append((time.perf_counter_ns()-t)/1e6)
        if len(got) != len(keys) or any(v != value for v in got):
            errors.append("missing or corrupt value")
    elapsed = time.perf_counter() - start
    return {"schema": 1, "args": vars(args), "backend": b.kind,
            "definitions": {"operation": "one object GET", "batch": "one network call or pipelined group"},
            "operations": len(trace), "batches": len(batches), "errors": len(errors),
            "error_examples": errors[:5], "elapsed_s": elapsed,
            "successful_ops_s": (len(trace) if not errors else 0)/elapsed,
            "payload_mib_s": len(trace)*args.value_bytes/1048576/elapsed,
            "batch_latency_ms": {f"p{q}": quantile(latencies, f) for q, f in
                                 [(50, .5), (95, .95), (99, .99)]}}


def main():
    p = argparse.ArgumentParser()
    p.add_argument("command", choices=["verify", "run", "stats", "batch", "limits"])
    p.add_argument("--backend", required=True, choices=["grpc", "redis", "valkey", "memcached"])
    p.add_argument("--host", required=True)
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--run-id", default="pilot")
    p.add_argument("--mode", choices=["hit", "origin", "mixed", "bypass"], default="hit")
    p.add_argument("--pattern", choices=["uniform", "skew", "hot_shift", "one_pass"], default="uniform")
    p.add_argument("--objects", type=int, default=256)
    p.add_argument("--requests", type=int, default=2000)
    p.add_argument("--value-bytes", type=int, default=1024)
    p.add_argument("--concurrency", type=int, default=1)
    p.add_argument("--batch-size", type=int, default=16)
    p.add_argument("--batch-kind", choices=["multiget", "pipeline"], default="multiget")
    p.add_argument("--warmup", type=int, default=200)
    p.add_argument("--preload", action="store_true")
    p.add_argument("--origin-ms", type=float, default=5.0)
    p.add_argument("--seed", type=int, default=20260923)
    args = p.parse_args()
    b = Backend(args.backend, args.host, args.port)
    result = (verify(b) if args.command == "verify" else
              limits(b) if args.command == "limits" else
              b.stats() if args.command == "stats" else
              batch(args, b) if args.command == "batch" else run(args, b))
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
