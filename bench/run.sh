#!/usr/bin/env bash
# Reproducible benchmark harness for cachelib-grpc-server.
#
# Runs the load generator and the server as containers on a shared Docker
# network, so all traffic stays inside the Linux kernel and never crosses a
# host<->VM boundary. Server and client are pinned to disjoint CPU sets so the
# load generator cannot steal cycles from the server it is measuring.
#
# Usage: ./run.sh [image] [output-dir]
set -euo pipefail

IMAGE="${1:-ghcr.io/celikgo/cachelib-grpc-server:latest}"
OUTDIR="${2:-$(dirname "$0")/results}"
NET=cachelib-bench-net
SRV=cachelib-bench-server
GHZ_IMAGE=cachelib-bench-ghz:local

# Workload parameters
WORKSET="${WORKSET:-200000}"     # distinct keys preloaded and read back
VALUE_BYTES="${VALUE_BYTES:-1024}"
REQUESTS="${REQUESTS:-200000}"
CONCURRENCY="${CONCURRENCY:-50}"
CONNECTIONS="${CONNECTIONS:-8}"
CACHE_SIZE="${CACHE_SIZE:-4294967296}"   # 4 GiB DRAM, no NVM
SERVER_CPUS="${SERVER_CPUS:-0-5}"
CLIENT_CPUS="${CLIENT_CPUS:-6-11}"

mkdir -p "$OUTDIR"

cleanup() {
  docker rm -f "$SRV" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> Building native ghz client image"
docker build --quiet -t "$GHZ_IMAGE" -f "$(dirname "$0")/Dockerfile.ghz" "$(dirname "$0")" >/dev/null

echo "==> Recording environment"
DIGEST=$(docker inspect --format '{{index .RepoDigests 0}}' "$IMAGE" 2>/dev/null || echo "$IMAGE")
{
  echo "image:        $IMAGE"
  echo "digest:       $DIGEST"
  echo "docker:       $(docker info --format '{{.OperatingSystem}} kernel={{.KernelVersion}} arch={{.Architecture}} ncpu={{.NCPU}} mem={{.MemTotal}}')"
  echo "server_cpus:  $SERVER_CPUS"
  echo "client_cpus:  $CLIENT_CPUS"
  echo "workset:      $WORKSET keys x $VALUE_BYTES B"
  echo "load:         n=$REQUESTS c=$CONCURRENCY connections=$CONNECTIONS"
  echo "cache:        ${CACHE_SIZE} B DRAM, NVM disabled"
} | tee "$OUTDIR/environment.txt"

cleanup
docker network create "$NET" >/dev/null
echo "==> Starting server"
docker run -d --name "$SRV" --network "$NET" --cpuset-cpus "$SERVER_CPUS" \
  "$IMAGE" --address=0.0.0.0 --port=50051 --cache_size="$CACHE_SIZE" >/dev/null

for _ in $(seq 1 60); do
  [ "$(docker inspect -f '{{.State.Health.Status}}' "$SRV" 2>/dev/null)" = healthy ] && break
  sleep 1
done
[ "$(docker inspect -f '{{.State.Health.Status}}' "$SRV")" = healthy ] || { echo "server unhealthy"; docker logs "$SRV"; exit 1; }

VALUE=$(head -c "$VALUE_BYTES" /dev/zero | tr '\0' 'x' | base64 | tr -d '\n')

run() { # name, call, data, n
  local name="$1" call="$2" data="$3" n="${4:-$REQUESTS}"
  echo "==> $name"
  docker run --rm --network "$NET" --cpuset-cpus "$CLIENT_CPUS" "$GHZ_IMAGE" \
    --insecure --call "$call" -d "$data" \
    -n "$n" -c "$CONCURRENCY" --connections "$CONNECTIONS" \
    -O json "$SRV":50051 > "$OUTDIR/$name.json"
  python3 - "$OUTDIR/$name.json" "$name" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
lat={x["percentage"]:x["latency"]/1e6 for x in d.get("latencyDistribution",[])}
codes=d.get("statusCodeDistribution",{})
print(f'  {sys.argv[2]:<10} {d["rps"]:>10,.0f} rps   '
      f'p50 {lat.get(50,0):.2f} ms  p99 {lat.get(99,0):.2f} ms  '
      f'p999 {lat.get(99.9,lat.get(99,0)):.2f} ms   {codes}')
PY
}

# Preload the working set, then read it back. RequestNumber spans the same
# key range in both phases, so the Get phase is a 100% hit workload.
run set  cachelib.grpc.CacheService/Set \
  "{\"key\":\"bench:{{.RequestNumber}}\",\"value\":\"$VALUE\",\"ttl_seconds\":0}" "$WORKSET"
run get  cachelib.grpc.CacheService/Get  '{"key":"bench:{{.RequestNumber}}"}'
run incr cachelib.grpc.CacheService/Incr '{"key":"rl:{{.RequestNumber}}","delta":1,"ttl_seconds":60}'
run ping cachelib.grpc.CacheService/Ping '{}'

echo "==> Server-reported stats (ground truth)"
docker run --rm --network "$NET" fullstorydev/grpcurl:latest -plaintext \
  -d '{}' "$SRV":50051 cachelib.grpc.CacheService/Stats | tee "$OUTDIR/stats.json"
echo "==> Results in $OUTDIR"
