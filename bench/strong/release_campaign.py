"""Run the complete release comparison sequentially, preserving every attempt.

Build and smoke-test images first. Do not build images or run other benchmarks
while this campaign is active. Tags are resolved to immutable local image IDs
once, before any measurement. No pull, build, retry, or global cleanup occurs.
"""
import argparse
import datetime as dt
import hashlib
import json
import pathlib
import platform
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_RUNS = pathlib.Path("bench/strong/runs/release-1.8.0-20260924")
DEFAULT_RESULTS = pathlib.Path("bench/strong/release-1.8.0-results")


def group(name, cases, engines, reps=5, seconds=60, *, kind="kv", cache=96,
          memory=768, headline=True):
    return dict(name=name, cases=cases.split(","), engines=engines.split(","),
                reps=reps, seconds=seconds, warmup=15, kind=kind,
                cache_mb=cache, memory_mb=memory, headline=headline)


GROUPS = [
    group("primary", "hit_1k_c8", "grpc,redis,valkey,memcached"),
    group("hybrid", "origin_uniform_64k_5ms", "grpc,grpc_nvm,redis,valkey,memcached,memcached_extstore"),
    group("offered", "offered_origin_64k_1500", "grpc,grpc_nvm,redis,memcached_extstore"),
    group("ram192", "origin_uniform_64k_5ms", "grpc,redis,memcached", cache=192, memory=384),
    group("flash384", "origin_uniform_64k_5ms", "grpc_nvm,memcached_extstore", memory=384),
    group("http", "repeated_256k_c8", "grpc_adapter,nginx", kind="http"),
    group("exploratory-kv", "hit_100b_c8,hit_16k_c8,hit_64k_c8,hit_256k_c8,hit_1m_c4,hit_1k_c1,hit_1k_c4,hit_1k_c16,readheavy_1k_c8,mixed_1k_c8",
          "grpc,redis,memcached", 1, 10, headline=False),
    group("batch-get", "batch_get_1k_c8", "grpc,redis,memcached", 2, 20, headline=False),
    group("batch-set", "batch_set_1k_c8", "grpc,redis", 2, 20, headline=False),
    group("pipeline", "pipeline_1k_c8", "grpc", 2, 20, headline=False),
    group("offered-saturation", "offered_1k_20k,offered_1k_50k,offered_1k_80k", "grpc,redis", 1, 20, headline=False),
    group("origin-sensitivity", "origin_uniform_64k_1ms,origin_uniform_64k_20ms", "grpc,grpc_nvm,redis,memcached_extstore", 1, 10, headline=False),
    group("origin-patterns", "origin_near_64k_5ms,origin_skew_64k_5ms,origin_shift_64k_5ms,cold_origin_uniform_64k_5ms,onepass_64k_5ms",
          "grpc,grpc_nvm,redis,memcached_extstore", 1, 10, headline=False),
    group("http-secondary", "recent_replay_256k_c8,repeated_1m_c8", "grpc_adapter,nginx", 2, 30, kind="http", headline=False),
    group("http-onepass", "onepass_256k_c8", "grpc_adapter,nginx,origin_direct", 2, 30, kind="http", headline=False),
]


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def command(parts):
    return subprocess.check_output(parts, cwd=ROOT, text=True).strip()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def evidence_hashes(raw, generated):
    paths = [path for directory in (raw, generated) for path in directory.rglob("*")
             if path.is_file() and path.suffix in (".json", ".txt", ".csv", ".svg", ".conf")
             and not ({"flash", "cache-files"} & set(path.relative_to(directory).parts))]
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(paths)}


def inspect_image(reference):
    data = json.loads(command(["docker", "image", "inspect", reference]))[0]
    return {"requested": reference, "id": data["Id"],
            "repo_digests": data.get("RepoDigests", []),
            "architecture": data["Architecture"], "os": data["Os"],
            "labels": data.get("Config", {}).get("Labels", {})}


def expected_runs(spec):
    return [(case, f"r{rep}", engine) for case in spec["cases"]
            for rep in range(spec["reps"]) for engine in spec["engines"]]


def validate_group(spec, directory):
    """Check raw results; saturation drops and deliberately cold starts remain data."""
    problems, observations, limitations = [], [], []
    for case, rep, engine in expected_runs(spec):
        run = directory / case / rep / engine
        label = f"{case}/{rep}/{engine}"
        result = run / "result.json"
        if not result.exists():
            error = (run / "error.txt").read_text().strip() if (run / "error.txt").exists() else "missing result.json"
            problems.append(f"{label}: {error}")
            continue
        try:
            d = json.loads(result.read_text())
        except (ValueError, OSError) as exc:
            problems.append(f"{label}: cannot read result: {exc}")
            continue
        required = {"errors", "client_exit_code", "elapsed_s", "latency_samples"}
        required |= ({"args", "operations", "successful_ops_s", "hits", "origins", "latency_ms",
                      "dropped_arrivals", "warmup_stable", "server_stats_before", "server_stats_after",
                      "client_cgroup_before", "client_cgroup_after"} if spec["kind"] == "kv" else
                     {"measure_seconds", "warmup_seconds", "warmup_success", "warmup_errors",
                      "successful_objects", "objects_s", "p50_ms", "p95_ms", "p99_ms",
                      "origin_requests_delta", "path_containers", "origin_container",
                      "client_cpu_usage_usec", "client_memory_peak_bytes"})
        missing = sorted(required - d.keys())
        if missing:
            problems.append(f"{label}: missing fields {missing}")
            continue
        if d["latency_samples"] <= 0:
            problems.append(f"{label}: no latency samples")
        seconds = d["args"].get("measure_seconds") if spec["kind"] == "kv" else d["measure_seconds"]
        if seconds != spec["seconds"]:
            problems.append(f"{label}: measurement duration configuration {seconds} != {spec['seconds']}")
        # The Go KV client deliberately exits 3 for errors OR dropped arrivals.
        # A drop-only result remains valid evidence of overload.
        drop_only_exit = (spec["kind"] == "kv" and d["client_exit_code"] == 3
                          and d["dropped_arrivals"] > 0 and d["errors"] == 0)
        for field in ("errors", "client_exit_code", "warmup_errors"):
            if field == "client_exit_code" and drop_only_exit:
                continue
            if d.get(field, 0):
                problems.append(f"{label}: {field}={d[field]}")
        elapsed = d.get("elapsed_s", 0)
        if elapsed < spec["seconds"] * .99:
            problems.append(f"{label}: truncated measurement ({elapsed}s)")
        if spec["kind"] == "kv":
            if not (run / "correctness.json").exists():
                problems.append(f"{label}: missing correctness probe")
            else:
                probe = json.loads((run / "correctness.json").read_text())
                if probe.get("basic") != "passed" or (engine.startswith("grpc") and probe.get("batch_pipeline") != "passed"):
                    problems.append(f"{label}: correctness probe did not pass")
            if d.get("dropped_arrivals", 0):
                target = observations if spec["name"] == "offered-saturation" else limitations
                target.append(f"{label}: dropped_arrivals={d['dropped_arrivals']}")
            if not d.get("warmup_stable", False) and not case.startswith("cold_"):
                target = limitations if spec["headline"] else observations
                target.append(f"{label}: warmup did not stabilize")
            if spec["name"] in ("primary", "ram192", "flash384") and d.get("origins", 0):
                limitations.append(f"{label}: full-hit objective had {d['origins']} origin requests")
        elif spec["name"] == "http" and d.get("origin_requests_delta", 0):
            limitations.append(f"{label}: repeated-hit objective had {d['origin_requests_delta']} origin requests")
        if (run / "error.txt").exists() and not (d.get("dropped_arrivals", 0) and not d["errors"] and (not d["client_exit_code"] or drop_only_exit)):
            problems.append(f"{label}: {(run / 'error.txt').read_text().strip()}")
    return {"expected_runs": len(expected_runs(spec)), "problems": problems,
            "observations": observations, "claim_limitations": limitations,
            "valid": not problems, "qualified": not problems and not limitations}


def group_command(spec, images, output):
    script = "run_strong.py" if spec["kind"] == "kv" else "run_http.py"
    parts = [sys.executable, f"bench/strong/{script}", "--grpc-image", images["grpc"]["id"],
             "--go-client-image", images["go"]["id"], "--cases", ",".join(spec["cases"]),
             "--reps", str(spec["reps"]), "--seconds", str(spec["seconds"]),
             "--warmup", str(spec["warmup"]), "--output", str(output)]
    if spec["kind"] == "kv":
        parts += ["--engines", ",".join(spec["engines"]), "--cache-mb", str(spec["cache_mb"]),
                  "--memory-mb", str(spec["memory_mb"]), "--correctness-client-image", images["python"]["id"]]
        for engine in ("redis", "valkey", "memcached"):
            parts += [f"--{engine}-image", images[engine]["id"]]
    else:
        parts += ["--utility-image", images["python"]["id"], "--adapter-image", images["python"]["id"],
                  "--nginx-image", images["nginx"]["id"]]
    return parts


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--grpc-image")
    p.add_argument("--go-client-image", default="cachebench-go:rc")
    p.add_argument("--python-client-image", default="cachelib-investigation-client:local")
    p.add_argument("--runs", type=pathlib.Path, default=DEFAULT_RUNS)
    p.add_argument("--results", type=pathlib.Path, default=DEFAULT_RESULTS)
    p.add_argument("--resume", action="store_true", help="Continue only groups never started; no repetitions are rerun")
    p.add_argument("--plan", action="store_true", help="Print measurement schedule without accessing Docker")
    args = p.parse_args()
    if args.plan:
        print(json.dumps({"groups": GROUPS, "runs": sum(len(expected_runs(g)) for g in GROUPS),
                          "minimum_timed_seconds": sum(len(expected_runs(g)) * (g["seconds"] + g["warmup"]) for g in GROUPS)}, indent=2))
        return
    if not args.grpc_image:
        p.error("--grpc-image is required unless --plan is used")
    args.runs = args.runs.resolve()
    args.results = args.results.resolve()
    if not args.runs.is_relative_to(ROOT) or not args.results.is_relative_to(ROOT):
        p.error("campaign output paths must be inside the repository for portable evidence links")
    path = args.runs / "campaign.json"
    if args.resume:
        state = json.loads(path.read_text())
        changed = [name for name, digest in state["harness_sha256"].items()
                   if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != digest]
        if changed:
            p.error(f"harness changed since start: {changed}; use a new campaign path")
        if state["groups"] != GROUPS:
            p.error("campaign definition changed; use a new output path")
        if state["images"]["grpc"]["id"] != inspect_image(args.grpc_image)["id"]:
            p.error("runtime image changed; use a new output path")
        incomplete = [name for name, status in state["progress"].items() if status["status"] == "running"]
        if incomplete:
            p.error(f"interrupted groups {incomplete}; preserve evidence and investigate before using a new campaign path")
    else:
        if args.runs.exists() or args.results.exists():
            p.error("output paths already exist; use --resume or new paths, never overwrite evidence")
        info = json.loads(command(["docker", "info", "--format", "{{json .}}"])); refs = {
            "grpc": args.grpc_image, "go": args.go_client_image, "python": args.python_client_image,
            "redis": "redis:8.2-alpine", "valkey": "valkey/valkey:8.1-alpine",
            "memcached": "memcached:1.6-alpine", "nginx": "nginx:1.29-alpine"}
        if info.get("NCPU", 0) < 8:
            p.error("at least 8 Docker CPUs are required")
        state = {"schema": 1, "started_utc": now(), "host": platform.platform(),
                 "git_commit": command(["git", "rev-parse", "HEAD"]),
                 "git_status": command(["git", "status", "--short"]),
                 "docker": {k: info.get(k) for k in ("Architecture", "CgroupVersion", "KernelVersion", "NCPU", "MemTotal", "OperatingSystem", "ServerVersion")},
                 "disk_free_bytes_at_start": shutil.disk_usage(ROOT).free,
                 "images": {key: inspect_image(ref) for key, ref in refs.items()},
                 "harness_sha256": {str(file.relative_to(ROOT)): hashlib.sha256(file.read_bytes()).hexdigest()
                                    for file in sorted((ROOT / "bench/strong").glob("*.py"))},
                 "groups": GROUPS, "progress": {},
                 "policy": "Sequential; immutable image IDs; every attempt retained; no retries or discarded outliers."}
        architectures = {v["architecture"] for v in state["images"].values()}
        if len(architectures) != 1:
            p.error(f"mixed image architectures: {architectures}")
        args.runs.mkdir(parents=True)
        args.results.mkdir(parents=True)
        save(path, state)
    for spec in GROUPS:
        name = spec["name"]
        if name in state["progress"]:
            continue
        output = args.runs / name
        parts = group_command(spec, state["images"], output)
        status = {"status": "running", "started_utc": now(), "command": parts}
        state["progress"][name] = status
        save(path, state)
        print(f"START {name}: {len(expected_runs(spec))} runs", flush=True)
        code = subprocess.run(parts, cwd=ROOT).returncode
        summary = subprocess.run([sys.executable, "bench/strong/summarize.py", "--kind", spec["kind"],
                                  "--input", str(output.relative_to(ROOT)), "--output", str((args.results / name).relative_to(ROOT))], cwd=ROOT).returncode
        status.update(validate_group(spec, output))
        if code or summary:
            status["problems"].append(f"harness exit={code}; summarizer exit={summary}")
            status["qualified"] = False
            status["valid"] = False
        status["evidence_sha256"] = evidence_hashes(output, args.results / name)
        status.update(status="complete", finished_utc=now())
        save(path, state)
        save(args.results / "campaign.json", state)
        print(f"END {name}: qualified={status['qualified']}; problems={len(status['problems'])}; observations={len(status['observations'])}", flush=True)
    state["finished_utc"] = now()
    state["qualified"] = all(s["qualified"] for s in state["progress"].values())
    state["valid"] = all(s["valid"] for s in state["progress"].values())
    save(path, state)
    save(args.results / "campaign.json", state)
    print(f"Campaign finished; qualified={state['qualified']}", flush=True)
    raise SystemExit(0 if state["valid"] else 1)


if __name__ == "__main__":
    main()
