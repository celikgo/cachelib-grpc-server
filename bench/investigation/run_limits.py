"""Run destructive-size probes only against a newly created task-owned server."""
import argparse
import datetime
import json
import pathlib
import subprocess
import time
import uuid


def docker(*args, check=True):
    p = subprocess.run(["docker", *args], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and p.returncode:
        raise RuntimeError(p.stderr[-1200:])
    return p


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--image", default="ghcr.io/celikgo/cachelib-grpc-server:latest")
    p.add_argument("--output", default="bench/investigation/runs/2026-09-23-limits-1.6")
    args = p.parse_args()
    out = pathlib.Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    suffix = uuid.uuid4().hex[:10]
    network = "cllimit-" + suffix
    server = "cllimit-server-" + suffix
    docker("network", "create", "--internal", network)
    try:
        server_cmd = ["run", "-d", "--name", server, "--network", network,
                      "--cpus", "4", "--memory", "768m", args.image,
                      "--cache_size=100663296", "--metrics_port=0"]
        (out / "server-command.json").write_text(json.dumps(server_cmd, indent=2) + "\n")
        docker(*server_cmd)
        client_cmd = ["run", "--rm", "--network", network, "--cpus", "2",
                      "cachelib-investigation-client:local", "limits", "--backend", "grpc",
                      "--host", server, "--port", "50051"]
        (out / "client-command.json").write_text(json.dumps(client_cmd, indent=2) + "\n")
        for _ in range(30):
            ready = docker("inspect", "-f", "{{.State.Health.Status}}", server, check=False)
            if ready.stdout.strip() == "healthy":
                break
            time.sleep(1)
        else:
            raise RuntimeError("server did not become healthy")
        result = docker(*client_cmd)
        (out / "result.json").write_text(json.dumps(json.loads(result.stdout), indent=2) + "\n")
        (out / "manifest.json").write_text(json.dumps({
            "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "image": args.image,
            "repo_digests": docker("image", "inspect", args.image, "--format", "{{json .RepoDigests}}").stdout.strip(),
        }, indent=2) + "\n")
        print(result.stdout.strip())
    finally:
        (out / "server.log").write_text(docker("logs", server, check=False).stderr)
        docker("stop", server, check=False)
        docker("rm", server, check=False)
        docker("network", "rm", network, check=False)


if __name__ == "__main__":
    main()
