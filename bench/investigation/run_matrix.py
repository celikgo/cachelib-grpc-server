"""Run isolated, disposable cache experiments; retain every raw JSON result.

Use: python3 bench/investigation/run_matrix.py --engines redis,valkey,memcached,grpc,grpc_nvm,memcached_extstore
The script only stops/removes containers and a network that it created. It
never publishes a host port and never touches an existing container or volume.
"""
import argparse
import datetime
import json
import os
import pathlib
import platform
import subprocess
import sys
import time
import uuid

IMAGES = {
    "redis": "redis:8.2-alpine", "valkey": "valkey/valkey:8.1-alpine",
    "memcached": "memcached:1.6-alpine", "memcached_extstore": "memcached:1.6-alpine",
}


def command(parts, check=True, timeout=180, capture=True):
    p = subprocess.run(parts, check=False, text=True, stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE if capture else None, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f"{parts!r}: exit {p.returncode}: {p.stderr[-1500:] if p.stderr else ''}")
    return p


def docker(*args, **kwargs):
    return command(["docker", *args], **kwargs)


def image_info(name):
    p = docker("image", "inspect", name, "--format", "{{json .}}")
    raw = json.loads(p.stdout)
    return {"id": raw["Id"], "repo_digests": raw.get("RepoDigests"),
            "labels": raw.get("Config", {}).get("Labels"), "architecture": raw.get("Architecture")}


def docker_environment():
    """Keep reproducibility fields without copying local proxy or daemon IDs."""
    raw = json.loads(docker("info", "--format", "{{json .}}").stdout)
    keys = ("Architecture", "CgroupDriver", "CgroupVersion", "Driver",
            "KernelVersion", "MemTotal", "NCPU", "OSType", "OperatingSystem",
            "OSVersion", "ServerVersion")
    return {key: raw.get(key) for key in keys}


def cgroup(name):
    p = docker("exec", name, "sh", "-c", "cat /sys/fs/cgroup/cpu.stat /sys/fs/cgroup/memory.current /sys/fs/cgroup/io.stat",
               check=False)
    return p.stdout if p.returncode == 0 else None


def client_call(network, name, backend, port, args, client_cpuset, timeout=300):
    parts = ["run", "--rm", "--network", network, "--cpus", "2", "--memory", "768m",
             "--cpuset-cpus", client_cpuset,
             "cachelib-investigation-client:local", args.pop(0), "--backend", backend,
             "--host", name, "--port", str(port), *args]
    p = docker(*parts, check=False, timeout=timeout)
    if p.returncode:
        raise RuntimeError(f"client failed: {p.stderr[-1500:] if p.stderr else p.stdout[-1500:]}")
    return json.loads(p.stdout.strip().splitlines()[-1])


def setup(engine, name, network, directory, grpc_image, ram_mb, flash_mb, container_memory_mb, server_cpuset):
    image = grpc_image if engine.startswith("grpc") else IMAGES[engine]
    backend = "grpc" if engine.startswith("grpc") else "memcached" if engine.startswith("memcached") else engine
    port = 50051 if backend == "grpc" else 11211 if backend == "memcached" else 6379
    args = ["run", "-d", "--name", name, "--network", network,
            "--cpus", "4", "--cpuset-cpus", server_cpuset,
            "--memory", f"{container_memory_mb}m", "--memory-swap", f"{container_memory_mb}m"]
    flash_dir = directory / "flash"
    if engine in ("grpc_nvm", "memcached_extstore"):
        flash_dir.mkdir(exist_ok=True)
        args += ["--mount", f"type=bind,src={flash_dir.resolve()},dst=/data"]
    args.append(image)
    if engine.startswith("grpc"):
        args += ["--cache_size=" + str(ram_mb * 1048576), "--metrics_port=0",
                 "--nvm_reader_threads=4", "--nvm_writer_threads=4"]
        if engine == "grpc_nvm":
            args += ["--enable_nvm=true", "--enable_io_uring=false",
                     "--nvm_path=/data/navy", "--nvm_size=" + str(flash_mb * 1048576)]
    elif engine in ("redis", "valkey"):
        args += ["--save", "", "--appendonly", "no", "--maxmemory", str(ram_mb * 1048576),
                 "--maxmemory-policy", "allkeys-lru"]
    else:
        args += ["-m", str(ram_mb), "-t", "4", "-I", "5m", "-U", "0"]
        if engine == "memcached_extstore":
            args += ["-o", f"ext_path=/data/extstore:{flash_mb}m,ext_item_size=2048,ext_wbuf_size=8"]
    docker(*args)
    return backend, port, image, args


def cases(level):
    if level == "pilot":
        return [("hit_1k", "hit", "uniform", 1024, 128, 500, 1, 0)]
    return [
        # Below RAM; direct service overhead and small versus medium objects.
        ("hit_100b", "hit", "uniform", 100, 1000, 3000, 1, 0),
        ("hit_1k_c1", "hit", "uniform", 1024, 1000, 3000, 1, 0),
        ("hit_1k_c8", "hit", "uniform", 1024, 1000, 3000, 8, 0),
        ("mixed_1k_90r10w", "mixed", "skew", 1024, 1000, 3000, 1, 0),
        ("batch_1k_16", "hit", "uniform", 1024, 1000, 3200, 1, 0),
        ("pipeline_1k_16", "hit", "uniform", 1024, 1000, 3200, 1, 0),
        ("hit_16k", "hit", "uniform", 16384, 1000, 2500, 1, 0),
        ("hit_64k", "hit", "uniform", 65536, 500, 2000, 1, 0),
        ("media_256k", "hit", "uniform", 262144, 160, 1000, 1, 0),
        ("media_1m", "hit", "uniform", 1048576, 40, 500, 1, 0),
        ("near_limit", "hit", "uniform", 4193000, 2, 20, 1, 0),
        # Above configured RAM: 2048 * 64 KiB = 128 MiB payload plus overhead.
        ("origin_skew_64k", "origin", "skew", 65536, 2048, 3500, 1, 5),
        ("origin_uniform_64k", "origin", "uniform", 65536, 2048, 3500, 1, 5),
        ("steady_uniform_64k", "origin", "uniform", 65536, 2048, 5000, 1, 5),
        ("origin_shift_64k", "origin", "hot_shift", 65536, 2048, 3500, 1, 5),
        ("origin_onepass_64k", "origin", "one_pass", 65536, 2048, 500, 1, 5),
        ("bypass_onepass_64k", "bypass", "one_pass", 65536, 2048, 500, 1, 5),
    ]


def run_case(engine, network, out, grpc_image, ram_mb, flash_mb, container_memory_mb,
             server_cpuset, client_cpuset, case, rep):
    label, mode, pattern, size, objects, requests, concurrency, origin_ms = case
    run_id = f"{label}-{rep}"
    name = f"clinv-{engine.replace('_', '-')}-{uuid.uuid4().hex[:8]}"
    directory = out / engine / run_id
    directory.mkdir(parents=True, exist_ok=True)
    backend = port = image = argv = None
    try:
        backend, port, image, argv = setup(engine, name, network, directory, grpc_image,
                                           ram_mb, flash_mb, container_memory_mb, server_cpuset)
        (directory / "server-command.json").write_text(json.dumps(argv, indent=2) + "\n")
        ready = False
        for _ in range(60):
            state = docker("inspect", "-f", "{{.State.Running}}", name, check=False)
            if state.stdout.strip() == "false":
                raise RuntimeError("server exited before readiness")
            try:
                client_call(network, name, backend, port, ["stats"], client_cpuset, timeout=20)
                ready = True
                break
            except Exception:
                time.sleep(1)
        if not ready:
            raise RuntimeError("server did not become ready")
        verification = client_call(network, name, backend, port, ["verify"], client_cpuset)
        (directory / "correctness.json").write_text(json.dumps(verification, indent=2) + "\n")
        before = cgroup(name)
        opts = ["batch" if label.startswith(("batch_", "pipeline_")) else "run", "--run-id", run_id, "--mode", mode, "--pattern", pattern,
                "--value-bytes", str(size), "--objects", str(objects),
                "--requests", str(requests), "--concurrency", str(concurrency),
                "--origin-ms", str(origin_ms), "--warmup", "1000" if label.startswith("steady_") else "200"]
        if label.startswith("steady_"):
            opts.append("--preload")
        if label.startswith("pipeline_"):
            opts += ["--batch-kind", "pipeline"]
        result = client_call(network, name, backend, port, opts, client_cpuset, timeout=600)
        after = cgroup(name)
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        (directory / "cgroup-before.txt").write_text(before or "")
        (directory / "cgroup-after.txt").write_text(after or "")
        (directory / "docker-inspect.json").write_text(docker("inspect", name).stdout)
        if engine == "grpc_nvm" and mode == "origin":
            if result["stats_after"].get("nvm_hit_count", 0) <= result["stats_before"].get("nvm_hit_count", 0):
                (directory / "nvm-proof.txt").write_text("No measured NVM hit in this run; flash benefit unproven.\n")
            else:
                (directory / "nvm-proof.txt").write_text("NVM hit counter increased during measurement.\n")
        print(f"{engine} {run_id}: {result['successful_ops_s']:.1f} op/s, " +
              (f"{result['object_hits']}/{result['read_operations']} hits, " if 'object_hits' in result else "") +
              f"{result['errors']} errors", flush=True)
    except Exception as exc:
        (directory / "error.txt").write_text(str(exc) + "\n")
        logs = docker("logs", name, check=False)
        (directory / "server.log").write_text((logs.stdout or "") + (logs.stderr or ""))
        print(f"{engine} {run_id}: ERROR {exc}", file=sys.stderr, flush=True)
    finally:
        # Exact task-owned name; no broad prune, volume deletion, or rm -f.
        docker("stop", name, check=False, timeout=30)
        docker("rm", name, check=False, timeout=30)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--engines", default="redis,valkey,memcached,memcached_extstore,grpc,grpc_nvm")
    p.add_argument("--grpc-image", default="cachelib-investigation:a4237c2")
    p.add_argument("--output", default="bench/investigation/runs/2026-09-23-a4237c2")
    p.add_argument("--level", choices=["pilot", "matrix"], default="pilot")
    p.add_argument("--ram-mb", type=int, default=96)
    p.add_argument("--ram-map", help="Comma-separated engine:MiB overrides, e.g. grpc:96,redis:192")
    p.add_argument("--container-memory-mb", type=int, default=768)
    p.add_argument("--server-cpuset", default="0-3")
    p.add_argument("--client-cpuset", default="4-5")
    p.add_argument("--flash-mb", type=int, default=512)
    p.add_argument("--case", help="Run only one named matrix case")
    args = p.parse_args()
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    engines = args.engines.split(",")
    ram_map = dict((engine, int(value)) for engine, value in
                   (entry.split(":", 1) for entry in args.ram_map.split(","))) if args.ram_map else {}
    manifest = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "host": platform.platform(), "host_arch": platform.machine(),
                "docker_version": docker("version", "--format", "{{json .}}").stdout,
                "docker_info": docker_environment(),
                "client_image": image_info("cachelib-investigation-client:local"),
                "images": {e: image_info(args.grpc_image if e.startswith("grpc") else IMAGES[e]) for e in engines},
                "configuration": vars(args), "git_commit": command(["git", "rev-parse", "HEAD"]).stdout.strip()}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    network = f"clinv-{uuid.uuid4().hex[:12]}"
    docker("network", "create", "--internal", network)
    try:
        for engine in engines:
            for case in cases(args.level):
                if args.case and case[0] != args.case:
                    continue
                if case[0].startswith("pipeline_") and not engine.startswith("grpc"):
                    continue
                if engine == "memcached_extstore" and case[3] < 2048:
                    continue
                for rep in range(2 if args.level == "matrix" and case[0] in ("hit_1k_c1", "origin_skew_64k", "origin_uniform_64k", "steady_uniform_64k") else 1):
                    run_case(engine, network, out, args.grpc_image, ram_map.get(engine, args.ram_mb),
                             args.flash_mb, args.container_memory_mb, args.server_cpuset,
                             args.client_cpuset, case, rep)
    finally:
        docker("network", "rm", network, check=False)


if __name__ == "__main__":
    main()
