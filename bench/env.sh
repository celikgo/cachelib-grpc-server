#!/usr/bin/env bash
# Emit a machine-readable environment record for a benchmark run.
#
# bench/summarize.py reads this. It exists because the Environment table in
# BENCHMARKS.md used to be typed in by hand: the digest, the kernel version and
# the CPU counts were assertions about the run rather than records of it, and
# nothing could tell you if they had gone stale.
#
# Usage: write_environment <outdir> <image> <server_cpus> <client_cpus> \
#                          <cache_bytes> <workset_keys> <value_bytes> <requests>
write_environment() {
  local outdir="$1" image="$2" server_cpus="$3" client_cpus="$4"
  local cache_bytes="$5" workset_keys="$6" value_bytes="$7" requests="$8"

  local digest host
  digest=$(docker inspect --format '{{index .RepoDigests 0}}' "$image" 2>/dev/null | cut -d@ -f2)
  [ -n "$digest" ] || digest="unknown (image not pulled by digest)"

  if [ "$(uname -s)" = Darwin ]; then
    host="$(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cores, \
$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GiB, macOS $(sw_vers -productVersion)"
  else
    host="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//'), \
$(nproc) cores, $(( $(awk '/MemTotal/{print $2}' /proc/meminfo) / 1024 / 1024 )) GiB, \
$(uname -s) $(uname -r)"
  fi

  local ghz_version
  ghz_version=$(docker run --rm "${GHZ_IMAGE:-cachelib-bench-ghz:local}" --version 2>/dev/null | tr -d '\r\n')
  [ -n "$ghz_version" ] || ghz_version="unknown"

  python3 - "$outdir" "$image" "$digest" "$host" "$server_cpus" "$client_cpus" \
           "$cache_bytes" "$workset_keys" "$value_bytes" "$requests" \
           "$ghz_version" <<'PY'
import json, subprocess, sys, pathlib
(outdir, image, digest, host, server_cpus, client_cpus,
 cache_bytes, workset_keys, value_bytes, requests, ghz) = sys.argv[1:12]

def docker_info(fmt):
    try:
        return subprocess.run(["docker", "info", "--format", fmt],
                              capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return "unknown"

ncpu = docker_info("{{.NCPU}}")
mem = docker_info("{{.MemTotal}}")
try:
    mem_gib = f"{int(mem) / 1024**3:.1f} GiB"
except ValueError:
    mem_gib = "unknown"

env = {
    "host": host,
    "runtime": f'{docker_info("{{.OperatingSystem}}")}, '
               f'kernel {docker_info("{{.KernelVersion}}")}, '
               f'{docker_info("{{.Architecture}}")}',
    "vm_resources": f"{ncpu} vCPU, {mem_gib}",
    "image": image,
    "image_digest": digest,
    "server_cpus": server_cpus,
    "client_cpus": client_cpus,
    "load_generator": f"ghz {ghz}, built natively for "
                      f'{docker_info("{{.Architecture}}")}',
    "cache": f"{int(cache_bytes) / 1024**3:g} GiB DRAM, NVM/SSD tier disabled",
    "workset": f"{int(workset_keys):,} keys x {int(value_bytes) // 1024} KiB values",
    "requests_per_measurement": int(requests),
}
p = pathlib.Path(outdir) / "environment.json"
p.write_text(json.dumps(env, indent=2) + "\n")
print(f"==> environment recorded in {p}")
PY
}
