"""Sequential common-HTTP media-object comparison with a compiled full-body client."""
import argparse
import datetime as dt
import hashlib
import json
import pathlib
import platform
import random
import subprocess
import sys
import time
import uuid


CASES = {
    "repeated_256k_c8": (262144, 64, 8, "repeated"),
    "repeated_1m_c8": (1048576, 32, 8, "repeated"),
    "recent_replay_256k_c8": (262144, 64, 8, "recent_replay"),
    "onepass_256k_c8": (262144, 64, 8, "one_pass"),
}


def command(parts, check=True, timeout=180):
    p = subprocess.run(parts, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f"{parts!r}: {(p.stderr or p.stdout)[-1000:]}")
    return p


def docker(*parts, **kwargs):
    return command(["docker", *parts], **kwargs)


def image_info(image):
    info = json.loads(docker("image", "inspect", image, "--format", "{{json .}}").stdout)
    return {"id": info["Id"], "repo_digests": info.get("RepoDigests", []),
            "architecture": info.get("Architecture"),
            "labels": info.get("Config", {}).get("Labels")}


def runtime_source_hash():
    files = ["Dockerfile", "CMakeLists.txt", "proto/cache.proto"]
    files += [str(path) for pattern in ("*.cc", "*.h", "patches/*.patch")
              for path in sorted(pathlib.Path(".").glob(pattern))]
    digest = hashlib.sha256()
    for path in files:
        digest.update(path.encode())
        digest.update(pathlib.Path(path).read_bytes())
    return digest.hexdigest()


def cgroup(name):
    p = docker("exec", name, "sh", "-c",
               "cat /sys/fs/cgroup/cpu.stat /sys/fs/cgroup/memory.current /sys/fs/cgroup/memory.peak /sys/fs/cgroup/io.stat",
               check=False)
    return p.stdout if not p.returncode else ""


def start(directory, network, name, alias, cpu, memory, image, extra, cpuset,
          entry=None, docker_options=None):
    args = ["docker", "run", "-d", "--name", name, "--network", network, "--network-alias", alias,
            "--cpus", str(cpu), "--cpuset-cpus", cpuset,
            "--memory", f"{memory}m", "--memory-swap", f"{memory}m"]
    if entry:
        args += ["--entrypoint", entry]
    args += docker_options or []
    args += [image, *extra]
    (directory / f"{name}.command.json").write_text(json.dumps(args, indent=2) + "\n")
    command(args)


def ready(network, host, utility_image):
    for _ in range(60):
        p = docker("run", "--rm", "--network", network, "--entrypoint", "python",
                   utility_image, "-c",
                   f"import urllib.request; r=urllib.request.urlopen('http://{host}:8080/obj/100/0',timeout=2); assert r.status==200",
                   check=False, timeout=12)
        if not p.returncode:
            return
        time.sleep(1)
    raise RuntimeError("HTTP target never became ready")


def run_one(args, root, network, case, repeat, engine):
    size, objects, concurrency, pattern = CASES[case]
    directory = root / case / f"r{repeat}" / engine
    directory.mkdir(parents=True, exist_ok=True)
    suffix = uuid.uuid4().hex[:8]
    target = f"cm-{engine}-{suffix}"
    cache = f"cm-cache-{suffix}"
    client = f"cm-client-{suffix}"
    origin = "cm-origin-" + network[-8:]
    active = []
    try:
        if engine == "nginx":
            cache_dir = directory / "cache-files"
            cache_dir.mkdir()
            # nginx worker needs write access to this task-owned bind directory.
            cache_dir.chmod(0o777)
            start(directory, network, target, "target", 2, 768, args.nginx_image, [], "0-1",
                  docker_options=["--mount", f"type=bind,src={args.nginx_config.resolve()},dst=/etc/nginx/nginx.conf,readonly",
                                  "--mount", f"type=bind,src={cache_dir.resolve()},dst=/data/cache"])
            active.append(target)
        elif engine == "grpc_adapter":
            start(directory, network, cache, "cache", 1, 384, args.grpc_image,
                  ["--cache_size=100663296", "--metrics_port=0"], "0-1")
            active.append(cache)
            start(directory, network, target, "target", 1, 384, args.adapter_image,
                  ["/work/http_service_strong.py", "adapter"], "0-1", entry="python")
            active.append(target)
        target_host = "origin" if engine == "origin_direct" else "target"
        ready(network, target_host, args.utility_image)
        cmd = ["docker", "run", "-d", "--name", client, "--network", network,
               "--cpus", "4", "--cpuset-cpus", "2-5",
               "--memory", "1024m", "--memory-swap", "1024m",
               "--entrypoint", "/usr/local/bin/httpbench", args.go_client_image,
               "-target", target_host, "-origin", "origin", "-size", str(size),
               "-objects", str(objects), "-concurrency", str(concurrency),
               "-pattern", pattern, "-warmup-seconds", str(args.warmup),
               "-measure-seconds", str(args.seconds)]
        (directory / "client.command.json").write_text(json.dumps(cmd, indent=2) + "\n")
        command(cmd)
        active.append(client)
        deadline = time.monotonic() + args.warmup + 180
        while time.monotonic() < deadline:
            logs = docker("logs", client, check=False)
            combined = logs.stdout + logs.stderr
            if "MEASURE_START" in combined:
                break
            if docker("inspect", "-f", "{{.State.Running}}", client).stdout.strip() == "false":
                raise RuntimeError(f"client stopped before measurement: {combined[-1000:]}")
            time.sleep(.3)
        else:
            raise RuntimeError("no MEASURE_START marker")
        before = {part: cgroup(part) for part in [*active, origin]}
        before_at = dt.datetime.now(dt.timezone.utc).isoformat()
        exit_code = int(docker("wait", client, timeout=args.seconds + 120).stdout.strip())
        after = {part: cgroup(part) for part in [*active, origin]}
        after_at = dt.datetime.now(dt.timezone.utc).isoformat()
        logs = docker("logs", client)
        (directory / "client.log").write_text(logs.stdout + logs.stderr)
        result = json.loads(next(line for line in reversed(logs.stdout.splitlines()) if line.startswith("{")))
        result["client_exit_code"] = exit_code
        result["cgroup_sample_times_utc"] = [before_at, after_at]
        result["path_containers"] = active[:-1]
        result["origin_container"] = origin
        (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        for part in [*active, origin]:
            (directory / f"{part}.cgroup-before.txt").write_text(before[part])
            (directory / f"{part}.cgroup-after.txt").write_text(after[part])
        if exit_code or result["errors"]:
            (directory / "error.txt").write_text("HTTP client error or nonzero exit\n")
        print(f"{case} r{repeat} {engine}: {result['objects_s']:.0f} objects/s, "
              f"origin={result['origin_requests_delta']}, errors={result['errors']}", flush=True)
    except Exception as exc:
        (directory / "error.txt").write_text(str(exc) + "\n")
        print(f"{case} r{repeat} {engine}: ERROR {exc}", file=sys.stderr, flush=True)
    finally:
        for part in reversed(active):
            logs = docker("logs", part, check=False)
            if part != client:
                (directory / f"{part}.log").write_text(logs.stdout + logs.stderr)
            docker("stop", part, check=False, timeout=30)
            docker("rm", part, check=False, timeout=30)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--grpc-image", required=True)
    p.add_argument("--go-client-image", default="cachebench-go:rc")
    p.add_argument("--utility-image", default="cachelib-investigation-client:local")
    p.add_argument("--nginx-image", default="nginx:1.29-alpine")
    p.add_argument("--output", type=pathlib.Path, required=True)
    p.add_argument("--cases", default="repeated_256k_c8")
    p.add_argument("--reps", type=int, default=5)
    p.add_argument("--seconds", type=int, default=60)
    p.add_argument("--warmup", type=int, default=15)
    p.add_argument("--nginx-config", type=pathlib.Path,
                   default=pathlib.Path("bench/strong/nginx.conf"))
    p.add_argument("--adapter-source", type=pathlib.Path,
                   default=pathlib.Path("bench/strong/http_service.py"))
    p.add_argument("--adapter-image", default="cachelib-investigation-client:local")
    args = p.parse_args()
    cases = args.cases.split(",")
    if any(case not in CASES for case in cases):
        p.error("unknown case")
    args.output.mkdir(parents=True, exist_ok=True)
    nginx_source = args.nginx_config
    nginx_bytes = nginx_source.read_bytes()
    args.nginx_config = args.output / "nginx-config.evaluated.conf"
    args.nginx_config.write_bytes(nginx_bytes)
    adapter_bytes = args.adapter_source.read_bytes()
    image_adapter_hash = docker("run", "--rm", "--entrypoint", "sha256sum",
                                args.adapter_image,
                                "/work/http_service_strong.py").stdout.split()[0]
    if image_adapter_hash != hashlib.sha256(adapter_bytes).hexdigest():
        p.error("HTTP adapter client image is stale; rebuild bench/investigation/Dockerfile.client")
    info = json.loads(docker("info", "--format", "{{json .}}").stdout)
    if int(info.get("NCPU", 0)) < 8:
        p.error("HTTP harness requires at least 8 Docker CPUs for disjoint cache/client/origin cpusets")
    manifest = {"utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                "host": platform.platform(), "docker": {k: info.get(k) for k in
                ("Architecture", "CgroupVersion", "KernelVersion", "OperatingSystem", "ServerVersion")},
                "git_commit": command(["git", "rev-parse", "HEAD"]).stdout.strip(),
                "git_status": command(["git", "status", "--short", "--untracked-files=normal"]).stdout,
                "runtime_source_sha256": runtime_source_hash(),
                "nginx_config_source": str(nginx_source),
                "nginx_config_sha256": hashlib.sha256(nginx_bytes).hexdigest(),
                "adapter_source": str(args.adapter_source),
                "adapter_source_sha256": image_adapter_hash,
                "images": {image: image_info(image)
                           for image in (args.grpc_image, args.nginx_image, args.go_client_image,
                                         args.utility_image, args.adapter_image)},
                "configuration": vars(args) | {"output": str(args.output),
                                                "nginx_config": str(args.nginx_config),
                                                "adapter_source": str(args.adapter_source)},
                "cache_path_budget": {"cpus": 2, "memory_mib": 768, "cpuset": "0-1"},
                "grpc_path_split": {"grpc": "1 CPU/384 MiB", "Python HTTP adapter": "1 CPU/384 MiB"},
                "origin": "simulated 5 ms, separate 2 CPU/512 MiB container on CPUs 6-7",
                "nginx_disk_cache": "512 MiB max, bind-mounted Docker Desktop filesystem; page cache not disabled",
                "client": "separate 4 CPU/1024 MiB cgroup on CPUs 2-5, compiled Go, full body verification",
                "origin_direct": "one-pass reference only; no cache path and no equal-resource cache claim"}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    network = "cm-" + uuid.uuid4().hex[:8]
    docker("network", "create", "--internal", network)
    origin = "cm-origin-" + network[-8:]
    try:
        start(args.output, network, origin, "origin", 2, 512,
              args.utility_image, ["/work/http_service_strong.py", "origin"],
              "6-7", entry="python")
        for case in cases:
            for repeat in range(args.reps):
                engines = ["nginx", "grpc_adapter"]
                if CASES[case][3] == "one_pass":
                    engines.append("origin_direct")
                random.Random(20260923 + repeat * 17 + len(case)).shuffle(engines)
                for engine in engines:
                    run_one(args, args.output, network, case, repeat, engine)
    finally:
        docker("stop", origin, check=False, timeout=30)
        docker("rm", origin, check=False, timeout=30)
        docker("network", "rm", network, check=False)


if __name__ == "__main__":
    main()
