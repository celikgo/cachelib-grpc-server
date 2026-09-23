"""Sequential, task-owned, time-based Docker comparisons using the Go client.

No public ports, volume pruning, raw devices, or global Docker cleanup. Each
result includes exact commands, an immutable local image ID, cgroup snapshots,
correctness output, and both successful and failed repetitions.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import random
import subprocess
import sys
import time
import uuid


IMAGES = {"redis": "redis:8.2-alpine", "valkey": "valkey/valkey:8.1-alpine",
          "memcached": "memcached:1.6-alpine", "memcached_extstore": "memcached:1.6-alpine"}
CASES = {
    # Below a 96 MiB cache; hit tests are read-only unless stated otherwise.
    "hit_100b_c8": (100, 1000, 8, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_1k_c1": (1024, 1000, 1, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_1k_c4": (1024, 1000, 4, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_1k_c8": (1024, 1000, 8, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_1k_c16": (1024, 1000, 16, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_16k_c8": (16384, 1000, 8, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_64k_c8": (65536, 1000, 8, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_256k_c8": (262144, 160, 8, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_1m_c4": (1048576, 40, 4, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "readheavy_1k_c8": (1024, 1000, 8, "skew", "read-heavy", "unary", True, 0, "closed", 0),
    "mixed_1k_c8": (1024, 1000, 8, "skew", "mixed", "unary", True, 0, "closed", 0),
    "batch_get_1k_c8": (1024, 1000, 8, "uniform", "read-only", "batch-get", True, 0, "closed", 0),
    "batch_set_1k_c8": (1024, 1000, 8, "uniform", "read-only", "batch-set", True, 0, "closed", 0),
    "pipeline_1k_c8": (1024, 1000, 8, "uniform", "read-only", "pipeline", True, 0, "closed", 0),
    # 128 MiB payload exceeds the configured 96 MiB DRAM cache.
    "origin_uniform_64k_1ms": (65536, 2048, 8, "uniform", "origin", "unary", True, 1, "closed", 0),
    "origin_near_64k_5ms": (65536, 1400, 8, "uniform", "origin", "unary", True, 5, "closed", 0),
    "origin_uniform_64k_5ms": (65536, 2048, 8, "uniform", "origin", "unary", True, 5, "closed", 0),
    # Starts measurement with no workload keys loaded or warmup operations.
    "cold_origin_uniform_64k_5ms": (65536, 2048, 8, "uniform", "origin", "unary", False, 5, "closed", 0),
    "origin_uniform_64k_20ms": (65536, 2048, 8, "uniform", "origin", "unary", True, 20, "closed", 0),
    "origin_skew_64k_5ms": (65536, 2048, 8, "skew", "origin", "unary", True, 5, "closed", 0),
    "origin_shift_64k_5ms": (65536, 2048, 8, "hot_shift", "origin", "unary", False, 5, "closed", 0),
    "onepass_64k_5ms": (65536, 2048, 4, "one_pass", "origin", "unary", False, 5, "closed", 0),
    # Set offered rate after a closed-loop pilot establishes a feasible common rate.
    "offered_1k_20k": (1024, 1000, 8, "uniform", "read-only", "unary", True, 0, "offered", 20000),
    "offered_1k_50k": (1024, 1000, 16, "uniform", "read-only", "unary", True, 0, "offered", 50000),
    "offered_1k_80k": (1024, 1000, 16, "uniform", "read-only", "unary", True, 0, "offered", 80000),
    "offered_origin_64k_1500": (65536, 2048, 8, "uniform", "origin", "unary", True, 5, "offered", 1500),
}


def call(parts, *, check=True, timeout=300):
    p = subprocess.run(parts, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f"{parts!r}: exit {p.returncode}: {(p.stderr or p.stdout)[-1200:]}")
    return p


def docker(*parts, **kwargs):
    return call(["docker", *parts], **kwargs)


def image_info(image):
    data = json.loads(docker("image", "inspect", image, "--format", "{{json .}}").stdout)
    return {"id": data["Id"], "repo_digests": data.get("RepoDigests"),
            "architecture": data.get("Architecture"), "labels": data.get("Config", {}).get("Labels")}


def cgroup(name):
    p = docker("exec", name, "sh", "-c",
               "cat /sys/fs/cgroup/cpu.stat /sys/fs/cgroup/memory.current /sys/fs/cgroup/memory.peak /sys/fs/cgroup/io.stat",
               check=False)
    return p.stdout if p.returncode == 0 else ""


def stats(network, name, backend, port):
    command = ["docker", "run", "--rm", "--network", network,
               "cachelib-investigation-client:local", "stats", "--backend", backend,
               "--host", name, "--port", str(port)]
    p = call(command, check=False, timeout=30)
    if p.returncode:
        raise RuntimeError(f"stats: {p.stderr[-500:]}")
    return json.loads(p.stdout.strip().splitlines()[-1])


def verify(network, name, backend, port):
    p = docker("run", "--rm", "--network", network,
               "cachelib-investigation-client:local", "verify", "--backend", backend,
               "--host", name, "--port", str(port), check=False, timeout=40)
    if p.returncode:
        raise RuntimeError(f"correctness: {p.stderr[-800:]}")
    return json.loads(p.stdout.strip().splitlines()[-1])


def setup(engine, name, network, directory, args):
    image = args.grpc_image if engine.startswith("grpc") else IMAGES[engine]
    backend = "grpc" if engine.startswith("grpc") else "memcached" if engine.startswith("memcached") else engine
    port = 50051 if backend == "grpc" else 11211 if backend == "memcached" else 6379
    command = ["docker", "run", "-d", "--name", name, "--network", network,
               "--cpus", "4", "--cpuset-cpus", "0-3", "--memory", f"{args.memory_mb}m",
               "--memory-swap", f"{args.memory_mb}m"]
    if engine in ("grpc_nvm", "memcached_extstore"):
        flash = directory / "flash"
        flash.mkdir(exist_ok=True)
        os.chmod(flash, 0o777)  # only the task-owned directory; image runs non-root
        command += ["--mount", f"type=bind,src={flash.resolve()},dst=/data"]
    command.append(image)
    if engine.startswith("grpc"):
        command += [f"--cache_size={args.cache_mb*1048576}", "--metrics_port=0",
                    "--nvm_reader_threads=4", "--nvm_writer_threads=4"]
        if engine == "grpc_nvm":
            command += ["--enable_nvm=true", "--enable_io_uring=false", "--nvm_path=/data/navy",
                        f"--nvm_size={args.flash_mb*1048576}"]
    elif engine in ("redis", "valkey"):
        command += ["--save", "", "--appendonly", "no", "--maxmemory", str(args.cache_mb*1048576),
                    "--maxmemory-policy", "allkeys-lru"]
    else:
        command += ["-m", str(args.cache_mb), "-t", "4", "-I", "5m", "-U", "0"]
        if engine == "memcached_extstore":
            command += ["-o", f"ext_path=/data/extstore:{args.flash_mb}m,ext_item_size=2048,ext_wbuf_size=8"]
    (directory / "server-command.json").write_text(json.dumps(command, indent=2) + "\n")
    call(command)
    return backend, port


def run_once(engine, case_name, repeat, network, root, args):
    case = CASES[case_name]
    size, objects, concurrency, pattern, workload, operation, preload, origin_ms, model, offered_rate = case
    if operation == "pipeline" and engine != "grpc":
        return
    if operation == "batch-set" and engine.startswith("memcached"):
        return
    if engine == "memcached_extstore" and size < 2048:
        return
    directory = root / case_name / f"r{repeat}" / engine
    directory.mkdir(parents=True, exist_ok=True)
    name = f"cstrong-{engine.replace('_','-')}-{uuid.uuid4().hex[:8]}"
    client = f"cstrong-client-{uuid.uuid4().hex[:8]}"
    backend = None
    try:
        backend, port = setup(engine, name, network, directory, args)
        for _ in range(60):
            if docker("inspect", "-f", "{{.State.Running}}", name, check=False).stdout.strip() == "false":
                raise RuntimeError("server exited before readiness")
            try:
                stats(network, name, backend, port)
                break
            except Exception:
                time.sleep(1)
        else:
            raise RuntimeError("server did not become ready")
        (directory / "correctness.json").write_text(json.dumps(verify(network, name, backend, port), indent=2) + "\n")
        command = ["docker", "run", "-d", "--name", client, "--network", network,
                   "--cpus", "4", "--cpuset-cpus", "4-7", "--memory", "1024m", "--memory-swap", "1024m",
                   "cachebench-go:rc", "-backend", backend, "-address", f"{name}:{port}",
                   "-run-id", f"{case_name}-r{repeat}", "-value-bytes", str(size),
                   "-objects", str(objects), "-concurrency", str(concurrency),
                   "-pattern", pattern, "-workload", workload, "-operation", operation,
                   "-model", model, "-origin-ms", str(origin_ms), "-offered-rate", str(offered_rate or 1000),
                   "-batch-size", "16", "-preload=" + str(preload).lower(),
                   "-warmup-seconds", str(0 if case_name.startswith("cold_") else args.warmup),
                   "-measure-seconds", str(args.seconds)]
        (directory / "client-command.json").write_text(json.dumps(command, indent=2) + "\n")
        call(command)
        deadline = time.monotonic() + max(240, args.seconds + args.warmup + 180)
        while time.monotonic() < deadline:
            log_result = docker("logs", client, check=False)
            logs = (log_result.stdout or "") + (log_result.stderr or "")
            if "MEASURE_START" in logs:
                break
            state = docker("inspect", "-f", "{{.State.Running}}", client, check=False).stdout.strip()
            if state == "false":
                raise RuntimeError(f"client exited before measurement: {logs[-1200:]}")
            time.sleep(0.25)
        else:
            raise RuntimeError("client never started measurement")
        before_time = dt.datetime.now(dt.timezone.utc).isoformat()
        before = cgroup(name)
        stats_before = stats(network, name, backend, port)
        waited = docker("wait", client, check=False, timeout=args.seconds + 120)
        after = cgroup(name)
        stats_after = stats(network, name, backend, port)
        after_time = dt.datetime.now(dt.timezone.utc).isoformat()
        logs = docker("logs", client, check=False)
        (directory / "client.log").write_text((logs.stdout or "") + (logs.stderr or ""))
        result = json.loads(next(line for line in reversed(logs.stdout.splitlines()) if line.startswith("{")))
        result["server_stats_before"] = stats_before
        result["server_stats_after"] = stats_after
        result["server_cgroup_sample_times_utc"] = [before_time, after_time]
        result["client_exit_code"] = int(waited.stdout.strip() or -1)
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        (directory / "server-cgroup-before.txt").write_text(before)
        (directory / "server-cgroup-after.txt").write_text(after)
        (directory / "server-inspect.json").write_text(docker("inspect", name).stdout)
        if result["client_exit_code"] or result["errors"] or result["dropped_arrivals"]:
            (directory / "error.txt").write_text("Client errors, dropped arrivals, or nonzero exit; see result.json.\n")
        print(f"{case_name} r{repeat} {engine}: {result['successful_ops_s']:.0f} objects/s, "
              f"hits={result['hits']}, errors={result['errors']}, stable={result['warmup_stable']}", flush=True)
    except Exception as exc:
        (directory / "error.txt").write_text(str(exc) + "\n")
        print(f"{case_name} r{repeat} {engine}: ERROR {exc}", file=sys.stderr, flush=True)
    finally:
        for container in (client, name):
            logs = docker("logs", container, check=False)
            if container == name:
                (directory / "server.log").write_text((logs.stdout or "") + (logs.stderr or ""))
            docker("stop", container, check=False, timeout=30)
            docker("rm", container, check=False, timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--grpc-image", required=True)
    parser.add_argument("--engines", default="grpc,redis,valkey,memcached")
    parser.add_argument("--cases", default="hit_1k_c1,hit_1k_c8")
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--warmup", type=int, default=15)
    parser.add_argument("--cache-mb", type=int, default=96)
    parser.add_argument("--memory-mb", type=int, default=768)
    parser.add_argument("--flash-mb", type=int, default=512)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.reps < 1 or args.seconds < 1 or args.warmup < 1 or args.memory_mb < args.cache_mb:
        parser.error("invalid duration, repetition count, or memory budget")
    cases = args.cases.split(",")
    if any(case not in CASES for case in cases):
        parser.error("unknown case; see CASES in run_strong.py")
    engines = args.engines.split(",")
    if any(e not in ("grpc", "grpc_nvm", *IMAGES) for e in engines):
        parser.error("unknown engine")
    args.output.mkdir(parents=True, exist_ok=True)
    schedule = []
    for case in cases:
        for repeat in range(args.reps):
            order = engines[:]
            random.Random(20260923 + repeat * 101 + sum(map(ord, case))).shuffle(order)
            schedule.extend((case, repeat, engine) for engine in order)
    docker_info = json.loads(docker("info", "--format", "{{json .}}").stdout)
    if int(docker_info.get("NCPU", 0)) < 8:
        parser.error("strong harness requires at least 8 Docker CPUs for disjoint server/client cpusets")
    safe_info = {k: docker_info.get(k) for k in ("Architecture", "CgroupVersion", "Driver", "KernelVersion",
                                                  "MemTotal", "NCPU", "OperatingSystem", "ServerVersion")}
    source_files = ["Dockerfile", "CMakeLists.txt", "proto/cache.proto"]
    source_files += [str(path) for pattern in ("*.cc", "*.h", "patches/*.patch")
                     for path in sorted(pathlib.Path(".").glob(pattern))]
    digest = hashlib.sha256()
    for path in source_files:
        digest.update(path.encode())
        digest.update(pathlib.Path(path).read_bytes())
    manifest = {"utc": dt.datetime.now(dt.timezone.utc).isoformat(), "host": platform.platform(),
                "host_arch": platform.machine(), "docker": safe_info, "git_commit": call(["git", "rev-parse", "HEAD"]).stdout.strip(),
                "git_status": call(["git", "status", "--short", "--untracked-files=normal"]).stdout,
                "runtime_source_sha256": digest.hexdigest(), "configuration": vars(args) | {"output": str(args.output)},
                "images": {e: image_info(args.grpc_image if e.startswith("grpc") else IMAGES[e]) for e in engines},
                "go_client_image": image_info("cachebench-go:rc"), "correctness_client_image": image_info("cachelib-investigation-client:local"),
                "schedule": schedule, "host_ports_published": False}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    network = "cstrong-" + uuid.uuid4().hex[:10]
    docker("network", "create", "--internal", network)
    try:
        for case, repeat, engine in schedule:
            run_once(engine, case, repeat, network, args.output, args)
    finally:
        docker("network", "rm", network, check=False)


if __name__ == "__main__":
    main()
