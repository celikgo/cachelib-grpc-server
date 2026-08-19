#!/usr/bin/env bash
# Concurrency sweep for the Get (100% hit) path: finds where the server
# saturates and how tail latency grows past that point.
# Usage: ./sweep.sh [image] [output-dir]
set -euo pipefail
IMAGE="${1:-ghcr.io/celikgo/cachelib-grpc-server:latest}"
OUTDIR="${2:-$(dirname "$0")/results/sweep}"
NET=cachelib-sweep-net
SRV=cachelib-sweep-server
GHZ_IMAGE=cachelib-bench-ghz:local
WORKSET="${WORKSET:-200000}"
REQUESTS="${REQUESTS:-200000}"
LEVELS="${LEVELS:-1 8 25 50 100 200 400 800}"
mkdir -p "$OUTDIR"
cleanup() { docker rm -f "$SRV" >/dev/null 2>&1 || true; docker network rm "$NET" >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker build --quiet -t "$GHZ_IMAGE" -f "$(dirname "$0")/Dockerfile.ghz" "$(dirname "$0")" >/dev/null
cleanup; docker network create "$NET" >/dev/null
docker run -d --name "$SRV" --network "$NET" --cpuset-cpus "${SERVER_CPUS:-0-5}" \
  "$IMAGE" --address=0.0.0.0 --port=50051 --cache_size=4294967296 >/dev/null
for _ in $(seq 1 60); do [ "$(docker inspect -f '{{.State.Health.Status}}' "$SRV" 2>/dev/null)" = healthy ] && break; sleep 1; done
VALUE=$(head -c 1024 /dev/zero | tr '\0' 'x' | base64 | tr -d '\n')
echo "==> preload $WORKSET keys"
docker run --rm --network "$NET" --cpuset-cpus "${CLIENT_CPUS:-6-11}" "$GHZ_IMAGE" --insecure \
  --call cachelib.grpc.CacheService/Set \
  -d "{\"key\":\"bench:{{.RequestNumber}}\",\"value\":\"$VALUE\",\"ttl_seconds\":0}" \
  -n "$WORKSET" -c 50 --connections 8 -O json "$SRV":50051 > "$OUTDIR/preload.json"
echo "==> warmup (discarded)"
docker run --rm --network "$NET" --cpuset-cpus "${CLIENT_CPUS:-6-11}" "$GHZ_IMAGE" --insecure \
  --call cachelib.grpc.CacheService/Get -d '{"key":"bench:{{.RequestNumber}}"}' \
  -n 100000 -c 50 --connections 8 -O json "$SRV":50051 > /dev/null

for c in $LEVELS; do
  conns=$(( c < 8 ? c : 8 )); [ "$c" -ge 100 ] && conns=16
  echo "==> concurrency $c (connections $conns)"
  docker run --rm --network "$NET" --cpuset-cpus "${CLIENT_CPUS:-6-11}" "$GHZ_IMAGE" --insecure \
    --call cachelib.grpc.CacheService/Get -d '{"key":"bench:{{.RequestNumber}}"}' \
    -n "$REQUESTS" -c "$c" --connections "$conns" -O json "$SRV":50051 > "$OUTDIR/c$c.json"
done
echo "==> sweep complete: $OUTDIR"
