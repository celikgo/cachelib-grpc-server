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
import shutil
import subprocess
import sys
import time
import uuid


# Latest official releases as of 2026-09-24, pinned to explicit patch versions
# so the measured engine build is identifiable from the harness alone. A tag is
# still resolved to an immutable local image ID before any measurement.
IMAGES = {"redis": "redis:8.10.2-alpine", "valkey": "valkey/valkey:9.1.2-alpine",
          "memcached": "memcached:1.6.45-alpine", "memcached_extstore": "memcached:1.6.45-alpine",
          "dragonfly": "docker.dragonflydb.io/dragonflydb/dragonfly:v2.0.0",
          "dragonfly_tiered": "docker.dragonflydb.io/dragonflydb/dragonfly:v2.0.0",
          "garnet": "ghcr.io/microsoft/garnet:2.1.8",
          "garnet_storage": "ghcr.io/microsoft/garnet:2.1.8",
          "kvrocks": "apache/kvrocks:2.17.0"}
# Engines whose image is selected by one --<engine>-image argument.
BASE_ENGINES = ("redis", "valkey", "memcached", "dragonfly", "garnet", "kvrocks")
# Engines that receive the task-owned file mounted at /data.
FILE_TIER_ENGINES = ("grpc_nvm", "memcached_extstore", "dragonfly_tiered",
                     "garnet_storage", "kvrocks")
PORTS = {"grpc": 50051, "memcached": 11211, "kvrocks": 6666,
         "redis": 6379, "valkey": 6379, "dragonfly": 6379, "garnet": 6379}
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
    "hit_1k_c32": (1024, 1000, 32, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "hit_1k_c64": (1024, 1000, 64, "uniform", "read-only", "unary", True, 0, "closed", 0),
    "readheavy_1k_c8": (1024, 1000, 8, "skew", "read-heavy", "unary", True, 0, "closed", 0),
    "mixed_1k_c8": (1024, 1000, 8, "skew", "mixed", "unary", True, 0, "closed", 0),
    "mixed_1k_c32": (1024, 1000, 32, "skew", "mixed", "unary", True, 0, "closed", 0),
    "batch_get_1k_c8": (1024, 1000, 8, "uniform", "read-only", "batch-get", True, 0, "closed", 0),
    "batch_set_1k_c8": (1024, 1000, 8, "uniform", "read-only", "batch-set", True, 0, "closed", 0),
    "pipeline_1k_c8": (1024, 1000, 8, "uniform", "read-only", "pipeline", True, 0, "closed", 0),
    # 128 MiB payload exceeds the configured 96 MiB DRAM cache.
    "origin_uniform_64k_1ms": (65536, 2048, 8, "uniform", "origin", "unary", True, 1, "closed", 0),
    "origin_near_64k_5ms": (65536, 1400, 8, "uniform", "origin", "unary", True, 5, "closed", 0),
    "origin_uniform_64k_5ms": (65536, 2048, 8, "uniform", "origin", "unary", True, 5, "closed", 0),
    # 1.28 GiB payload against a configured 1 GiB DRAM cache: the same 1.33x
    # oversubscription as the 96 MiB cases, at a budget every engine accepts.
    "origin_uniform_64k_1gib": (65536, 20480, 8, "uniform", "origin", "unary", True, 5, "closed", 0),
    # Starts measurement with no workload keys loaded or warmup operations.
    "cold_origin_uniform_64k_5ms": (65536, 2048, 8, "uniform", "origin", "unary", False, 5, "closed", 0),
    "origin_uniform_64k_20ms": (65536, 2048, 8, "uniform", "origin", "unary", True, 20, "closed", 0),
    "origin_skew_64k_5ms": (65536, 2048, 8, "skew", "origin", "unary", True, 5, "closed", 0),
    "origin_shift_64k_5ms": (65536, 2048, 8, "hot_shift", "origin", "unary", False, 5, "closed", 0),
    "onepass_64k_5ms": (65536, 2048, 4, "one_pass", "origin", "unary", False, 5, "closed", 0),
    # SSD+RAM at volume. A 2 GiB set fits the 4 GiB file but is eight times the
    # 256 MiB DRAM, so nearly every hit is a concurrent flash read — what a
    # 128 MiB set in a 512 MiB file over eight connections never asks for.
    "origin_uniform_64k_2gib_c32": (65536, 32768, 32, "uniform", "origin", "unary", True, 5, "closed", 0),
    "origin_skew_64k_2gib_c32": (65536, 32768, 32, "skew", "origin", "unary", True, 5, "closed", 0),
    "origin_uniform_16k_2gib_c32": (16384, 131072, 32, "uniform", "origin", "unary", True, 5, "closed", 0),
    # Over capacity: a 6 GiB set against a 4 GiB file. The tier must evict and
    # reclaim continuously and cannot avoid every origin request. No preload;
    # the trace fills the tier, so measurement observes steady-state reclaim.
    "origin_uniform_64k_6gib_c32": (65536, 98304, 32, "uniform", "origin", "unary", False, 5, "closed", 0),
    # Set offered rate after a closed-loop pilot establishes a feasible common rate.
    "offered_1k_20k": (1024, 1000, 8, "uniform", "read-only", "unary", True, 0, "offered", 20000),
    "offered_1k_50k": (1024, 1000, 16, "uniform", "read-only", "unary", True, 0, "offered", 50000),
    "offered_1k_80k": (1024, 1000, 16, "uniform", "read-only", "unary", True, 0, "offered", 80000),
    "offered_1k_120k": (1024, 1000, 16, "uniform", "read-only", "unary", True, 0, "offered", 120000),
    "offered_origin_64k_1500": (65536, 2048, 8, "uniform", "origin", "unary", True, 5, "offered", 1500),
    "offered_origin_64k_2gib_10k": (65536, 32768, 64, "uniform", "origin", "unary", True, 5, "offered", 10000),
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


def stats(network, name, backend, port, client_image):
    command = ["docker", "run", "--rm", "--network", network,
               client_image, "stats", "--backend", backend,
               "--host", name, "--port", str(port)]
    p = call(command, check=False, timeout=30)
    if p.returncode:
        raise RuntimeError(f"stats: {p.stderr[-500:]}")
    return json.loads(p.stdout.strip().splitlines()[-1])


def verify(network, name, backend, port, client_image):
    p = docker("run", "--rm", "--network", network,
               client_image, "verify", "--backend", backend,
               "--host", name, "--port", str(port), check=False, timeout=40)
    if p.returncode:
        raise RuntimeError(f"correctness: {p.stderr[-800:]}")
    return json.loads(p.stdout.strip().splitlines()[-1])


def server_arguments(engine, args):
    """Server arguments per engine: four threads, the configured RAM budget, no
    persistence, and cache-style eviction where the engine offers it. A file
    tier receives the task-owned /data mount and the same file budget."""
    cache_mb, flash_mb = args.cache_mb, args.flash_mb
    if engine.startswith("grpc"):
        out = [f"--cache_size={cache_mb*1048576}", "--metrics_port=0",
               "--nvm_reader_threads=4", "--nvm_writer_threads=4"]
        if engine == "grpc_nvm":
            out += ["--enable_nvm=true", "--enable_io_uring=false", "--nvm_path=/data/navy",
                    f"--nvm_size={flash_mb*1048576}"]
        return out
    if engine in ("redis", "valkey"):
        return ["--save", "", "--appendonly", "no", "--maxmemory", str(cache_mb*1048576),
                "--maxmemory-policy", "allkeys-lru"]
    if engine.startswith("memcached"):
        out = ["-m", str(cache_mb), "-t", "4", "-I", "5m", "-U", "0"]
        if engine == "memcached_extstore":
            out += ["-o", f"ext_path=/data/extstore:{flash_mb}m,ext_item_size=2048,ext_wbuf_size=8"]
        return out
    if engine.startswith("dragonfly"):
        # Measured on 2026-09-24 with 2.0.0: the server exits unless maxmemory is
        # at least 256 MiB per proactor thread, and a tiering file is at least
        # that large again. Fail here rather than mid-measurement.
        threads = 4
        if cache_mb < 256 * threads:
            raise ValueError(f"Dragonfly requires >= {256*threads} MiB maxmemory "
                             f"for {threads} threads; got {cache_mb} MiB")
        out = ["--bind", "0.0.0.0", "--port", "6379", f"--maxmemory={cache_mb}mb",
               "--cache_mode=true", f"--proactor_threads={threads}",
               "--dbfilename=", "--snapshot_cron="]
        if engine == "dragonfly_tiered":
            if flash_mb < 256 * threads:
                raise ValueError(f"Dragonfly tiering requires >= {256*threads} MiB "
                                 f"of file; got {flash_mb} MiB")
            # Tiering additionally requires io_uring; it aborts in
            # InitTieredStorage under the epoll fallback.
            out += ["--tiered_prefix=/data/dragonfly",
                    f"--tiered_max_file_size={flash_mb}mb"]
        return out
    if engine.startswith("garnet"):
        out = ["--port", "6379", "--bind", "0.0.0.0", "--memory", f"{cache_mb}m",
               "--index", "16m", "--no-obj", "--checkpointdir", "/tmp/garnet-checkpoint"]
        if engine == "garnet_storage":
            out += ["--storage-tier", "true", "--logdir", "/data"]
        return out
    if engine == "kvrocks":
        return ["--bind", "0.0.0.0", "--port", "6666", "--dir", "/data",
                "--rocksdb.block_cache_size", str(cache_mb), "--daemonize", "no"]
    raise ValueError(f"no server arguments defined for {engine}")


def setup(engine, name, network, directory, args):
    base = engine.split("_")[0]
    image = args.grpc_image if base == "grpc" else args.engine_images[engine]
    backend = "grpc" if base == "grpc" else "memcached" if base == "memcached" else engine
    port = PORTS[base]
    command = ["docker", "run", "-d", "--name", name, "--network", network,
               "--cpus", "4", "--cpuset-cpus", "0-3", "--memory", f"{args.memory_mb}m",
               "--memory-swap", f"{args.memory_mb}m"]
    if engine in FILE_TIER_ENGINES:
        flash = directory / "flash"
        flash.mkdir(exist_ok=True)
        os.chmod(flash, 0o777)  # only the task-owned directory; image runs non-root
        command += ["--mount", f"type=bind,src={flash.resolve()},dst=/data"]
    command.append(image)
    command += server_arguments(engine, args)
    (directory / "server-command.json").write_text(json.dumps(command, indent=2) + "\n")
    call(command)
    return backend, port


def record_flash_usage(directory, discard):
    """Record the backing file's measured size, then optionally reclaim the disk.

    The sizes are evidence; the bytes are not. Recording them lets a high-volume
    campaign drive multi-GiB files without retaining one per run.
    """
    flash = directory / "flash"
    if not flash.exists():
        return
    files = sorted(f for f in flash.rglob("*") if f.is_file())
    entries = [{"path": f.relative_to(flash).as_posix(), "logical_bytes": f.stat().st_size,
                "allocated_bytes": f.stat().st_blocks * 512} for f in files]
    usage = {"files": entries,
             "flash_file_logical_bytes": sum(e["logical_bytes"] for e in entries),
             "flash_file_allocated_bytes": sum(e["allocated_bytes"] for e in entries),
             "discarded_after_measurement": bool(discard)}
    (directory / "flash-usage.json").write_text(json.dumps(usage, indent=2) + "\n")
    if discard:
        shutil.rmtree(flash, ignore_errors=True)


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
                stats(network, name, backend, port, args.correctness_client_image)
                break
            except Exception:
                time.sleep(1)
        else:
            raise RuntimeError("server did not become ready")
        (directory / "correctness.json").write_text(json.dumps(verify(network, name, backend, port, args.correctness_client_image), indent=2) + "\n")
        command = ["docker", "run", "-d", "--name", client, "--network", network,
                   "--cpus", "4", "--cpuset-cpus", "4-7", "--memory", "1024m", "--memory-swap", "1024m",
                   args.go_client_image, "-backend", backend, "-address", f"{name}:{port}",
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
        stats_before = stats(network, name, backend, port, args.correctness_client_image)
        waited = docker("wait", client, check=False, timeout=args.seconds + 120)
        after = cgroup(name)
        stats_after = stats(network, name, backend, port, args.correctness_client_image)
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
        if engine in FILE_TIER_ENGINES:
            record_flash_usage(directory, args.discard_flash_files)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--grpc-image", required=True)
    parser.add_argument("--go-client-image", default="cachebench-go:rc")
    parser.add_argument("--correctness-client-image", default="cachelib-investigation-client:local")
    for engine in BASE_ENGINES:
        parser.add_argument(f"--{engine}-image", default=IMAGES[engine])
    parser.add_argument("--engines", default="grpc,redis,valkey,memcached")
    parser.add_argument("--cases", default="hit_1k_c1,hit_1k_c8")
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--warmup", type=int, default=15)
    parser.add_argument("--cache-mb", type=int, default=96)
    parser.add_argument("--memory-mb", type=int, default=768)
    parser.add_argument("--flash-mb", type=int, default=512)
    parser.add_argument("--discard-flash-files", action="store_true",
                        help="Delete each run's backing file after recording its measured size")
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.engine_images = {engine: getattr(args, f"{engine}_image") for engine in BASE_ENGINES}
    for variant in ("memcached_extstore", "dragonfly_tiered", "garnet_storage"):
        args.engine_images[variant] = args.engine_images[variant.split("_")[0]]
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
                "images": {e: image_info(args.grpc_image if e.startswith("grpc") else args.engine_images[e]) for e in engines},
                "go_client_image": image_info(args.go_client_image), "correctness_client_image": image_info(args.correctness_client_image),
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
