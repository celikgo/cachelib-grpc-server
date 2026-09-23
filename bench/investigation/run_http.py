"""Common-HTTP media-object comparison: Python gRPC adapter versus NGINX disk cache."""
import argparse
import datetime
import json
import pathlib
import platform
import subprocess
import sys
import time
import uuid


def cmd(parts, check=True, timeout=120):
    p = subprocess.run(parts, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f"{parts!r}: {p.stderr[-1200:]}")
    return p


def docker(*args, **kwargs):
    return cmd(["docker", *args], **kwargs)


def memory_bytes(name):
    result = docker("exec", name, "cat", "/sys/fs/cgroup/memory.current", check=False)
    return int(result.stdout.strip()) if result.returncode == 0 else None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--grpc-image", default="ghcr.io/celikgo/cachelib-grpc-server:latest")
    p.add_argument("--output", default="bench/investigation/runs/2026-09-23-http-1.6")
    p.add_argument("--equal-budget", action="store_true",
                   help="Give NGINX and the combined adapter+gRPC path 2 CPUs/768 MiB each")
    args = p.parse_args()
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    network = "clhttp-" + uuid.uuid4().hex[:10]
    docker("network", "create", "--internal", network)
    containers = []

    def start(name, parts):
        (out / (name + ".server-command.json")).write_text(
            json.dumps(["docker", "run", "-d", "--name", name, "--network", network, *parts], indent=2) + "\n")
        docker("run", "-d", "--name", name, "--network", network, *parts)
        containers.append(name)

    try:
        start("clhttp-origin-" + network[-10:], ["--network-alias", "origin", "--cpus", "2", "--memory", "512m",
                                             *( ["--cpuset-cpus", "6-7"] if args.equal_budget else []),
                                             "--entrypoint", "python", "cachelib-investigation-client:local",
                                             "/work/http_service.py", "origin"])
        manifest = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "host": platform.platform(), "network": network, "grpc_image": args.grpc_image,
                    "client_image": docker("image", "inspect", "cachelib-investigation-client:local", "--format", "{{.Id}}").stdout.strip(),
                    "nginx_image": docker("image", "inspect", "nginx:1.29-alpine", "--format", "{{.Id}}").stdout.strip(),
                    "cachelib_image": docker("image", "inspect", args.grpc_image, "--format", "{{json .RepoDigests}}").stdout.strip(),
                    "origin_assumption_ms": 5, "host_ports_published": False,
                    "equal_combined_cache_budget": args.equal_budget,
                    "cache_path_cpu_quota": 2 if args.equal_budget else "nginx:2, adapter:2 + grpc:4",
                    "cache_path_memory_mib": 768 if args.equal_budget else "nginx:768, adapter:768 + grpc:768"}
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        for backend in ("nginx", "grpc_adapter"):
            if backend == "nginx":
                disk = out / "nginx-cache"
                disk.mkdir(exist_ok=True)
                name = "clhttp-nginx-" + network[-10:]
                start(name, ["--network-alias", "target", "--cpus", "2", "--memory", "768m",
                             *( ["--memory-swap", "768m", "--cpuset-cpus", "0-1"] if args.equal_budget else []),
                             "--mount", f"type=bind,src={(pathlib.Path(__file__).parent/'nginx.conf').resolve()},dst=/etc/nginx/nginx.conf,readonly",
                             "--mount", f"type=bind,src={disk.resolve()},dst=/data/cache", "nginx:1.29-alpine"])
            else:
                cache = "clhttp-cache-" + network[-10:]
                start(cache, ["--network-alias", "cache", "--cpus", "1" if args.equal_budget else "4",
                              "--memory", "384m" if args.equal_budget else "768m",
                              *( ["--memory-swap", "384m", "--cpuset-cpus", "0-1"] if args.equal_budget else []),
                              args.grpc_image, "--cache_size=100663296", "--metrics_port=0"])
                name = "clhttp-adapter-" + network[-10:]
                start(name, ["--network-alias", "target", "--cpus", "1" if args.equal_budget else "2",
                             "--memory", "384m" if args.equal_budget else "768m",
                             *( ["--memory-swap", "384m", "--cpuset-cpus", "0-1"] if args.equal_budget else []),
                             "--entrypoint", "python", "cachelib-investigation-client:local",
                             "/work/http_service.py", "adapter"])
            for _ in range(40):
                p0 = docker("run", "--rm", "--network", network, "--entrypoint", "python",
                            "cachelib-investigation-client:local", "-c",
                            "import urllib.request; print(urllib.request.urlopen('http://target:8080/obj/100/0',timeout=2).status)",
                            check=False, timeout=15)
                if p0.returncode == 0:
                    break
                time.sleep(1)
            else:
                raise RuntimeError(f"{backend} never became ready: {p0.stderr[-500:]}")
            for size, objects, requests, concurrency, pattern in [
                (262144, 64, 400, 1, "repeated"),
                (262144, 64, 400, 8, "repeated"),
                (1048576, 32, 250, 1, "repeated"),
                (262144, 64, 150, 1, "one_pass"),
            ]:
                label = f"{backend}-{size}-{concurrency}-{pattern}"
                parts = ["run", "--rm", "--network", network, "--cpus", "2", "--memory", "768m",
                         *( ["--cpuset-cpus", "4-5"] if args.equal_budget else []),
                         "--entrypoint", "python", "cachelib-investigation-client:local",
                         "/work/http_client.py", "--host", "target", "--size", str(size),
                         "--objects", str(objects), "--requests", str(requests),
                         "--concurrency", str(concurrency), "--pattern", pattern]
                (out / (label + ".command.json")).write_text(json.dumps(parts, indent=2) + "\n")
                result = docker(*parts, check=False, timeout=180)
                if result.returncode:
                    (out / (label + ".error.txt")).write_text(result.stderr)
                    print(f"{label}: ERROR {result.stderr[-300:]}", file=sys.stderr, flush=True)
                else:
                    r = json.loads(result.stdout)
                    if args.equal_budget:
                        r["cache_path_memory_after_bytes"] = sum(
                            memory_bytes(part) or 0 for part in ([name] if backend == "nginx" else [name, cache]))
                    (out / (label + ".json")).write_text(json.dumps(r, indent=2) + "\n")
                    print(f"{label}: {r['ops_s']:.1f} req/s, {r['origin_requests_delta']} origin requests, {r['errors']} errors", flush=True)
            docker("stop", name, check=False)
            docker("rm", name, check=False)
            containers.remove(name)
            if backend == "grpc_adapter":
                docker("stop", cache, check=False)
                docker("rm", cache, check=False)
                containers.remove(cache)
    finally:
        for name in reversed(containers):
            docker("stop", name, check=False)
            docker("rm", name, check=False)
        docker("network", "rm", network, check=False)


if __name__ == "__main__":
    main()
