#!/usr/bin/env bash
set -euo pipefail

# Verification of the exact runtime image argument. Set CACHELIB_SMOKE_PLATFORM
# when selecting an architecture explicitly; emulated runs prove correctness,
# not native performance. Correctness clients run on the host's default platform.
# The flash file is task-owned and file-backed; this is a correctness smoke,
# not a physical SSD performance or endurance measurement.
image=${1:?usage: release-smoke.sh IMAGE VERSION OUTPUT_JSON}
expected_version=${2:?expected version required}
output=${3:?flash output path required}
mkdir -p "$(dirname "$output")"
client_image=${CACHELIB_SMOKE_CLIENT_IMAGE:-cachelib-investigation-client:local}
server_platform=${CACHELIB_SMOKE_PLATFORM:-}
suffix="${RANDOM}-$$"
network="cachelib-release-smoke-${suffix}"
ram_container="cachelib-release-ram-${suffix}"
flash_container="cachelib-release-flash-${suffix}"
flash_dir=$(mktemp -d "${TMPDIR:-/tmp}/cachelib-release-flash.XXXXXX")
chmod 777 "$flash_dir"

cleanup() {
  docker stop "$ram_container" "$flash_container" >/dev/null 2>&1 || true
  docker rm "$ram_container" "$flash_container" >/dev/null 2>&1 || true
  docker network rm "$network" >/dev/null 2>&1 || true
  rm -rf -- "$flash_dir"
}
trap cleanup EXIT

server_run() {
  if [ -n "$server_platform" ]; then
    docker run --platform "$server_platform" "$@"
  else
    docker run "$@"
  fi
}

assert_health() {
  local container=$1 health
  health=$(docker inspect -f '{{.State.Health.Status}}' "$container")
  if [ "$health" != healthy ]; then
    echo "$container Docker health is $health, expected healthy" >&2
    docker logs "$container" >&2 || true
    return 1
  fi
  docker exec "$container" /usr/local/bin/grpc_health_probe -addr=:50051
}

wait_for_ready() {
  local container=$1 host=$2 state=unknown health=unknown
  for _ in $(seq 1 60); do
    state=$(docker inspect -f '{{.State.Status}}' "$container" 2>/dev/null) || state=missing
    health=$(docker inspect -f '{{.State.Health.Status}}' "$container" 2>/dev/null) || health=missing
    if [ "$state" = exited ] || [ "$state" = dead ] || [ "$state" = missing ]; then
      break
    fi
    if [ "$health" = healthy ] &&
      docker exec "$container" /usr/local/bin/grpc_health_probe -addr=:50051 >/dev/null 2>&1 &&
      docker run --rm --network "$network" "$client_image" stats \
        --backend grpc --host "$host" --port 50051 >/dev/null 2>&1; then
      echo "$container is ready: Docker healthy, gRPC SERVING, Stats responding"
      return 0
    fi
    sleep 1
  done
  echo "$container did not become ready: state=$state health=$health" >&2
  docker logs "$container" >&2 || true
  return 1
}

version_output=$(server_run --rm "$image" --version)
if [ "$version_output" != "cachelib-grpc-server $expected_version" ]; then
  echo "unexpected runtime version: $version_output" >&2
  exit 1
fi
echo "runtime version verified: $version_output"

docker network create --internal "$network" >/dev/null
server_run -d --name "$ram_container" --network "$network" --network-alias cache \
  --cpus 2 --memory 512m --memory-swap 512m "$image" \
  --cache_size=134217728 --metrics_port=0 >/dev/null

wait_for_ready "$ram_container" cache
docker run --rm --network "$network" "$client_image" verify \
  --backend grpc --host cache --port 50051
docker run --rm --network "$network" "$client_image" limits \
  --backend grpc --host cache --port 50051 > "${output%.json}-limits.json"
python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); assert r["key_lengths"]["255"]["roundtrip"]; assert not r["key_lengths"]["256"].get("success", False); assert r["value_lengths"]["4193000"]["roundtrip"]; assert r["multiset_over_message_limit"].get("rpc_error")=="RESOURCE_EXHAUSTED"; assert r["multiget_over_message_limit"].get("rpc_error")=="RESOURCE_EXHAUSTED"' "${output%.json}-limits.json"

assert_health "$ram_container"

server_run -d --name "$flash_container" --network "$network" --network-alias cache-nvm \
  --cpus 2 --memory 768m --memory-swap 768m \
  --mount "type=bind,src=${flash_dir},dst=/data/nvm" "$image" \
  --cache_size=67108864 --enable_nvm=true --enable_io_uring=false \
  --nvm_path=/data/nvm/navy --nvm_size=536870912 \
  --nvm_reader_threads=4 --nvm_writer_threads=4 --metrics_port=0 >/dev/null

wait_for_ready "$flash_container" cache-nvm

docker run --rm --network "$network" --entrypoint python "$client_image" \
  /work/flash_smoke.py --host cache-nvm > "$output"
cat "$output"
assert_health "$ram_container"
assert_health "$flash_container"
