#!/usr/bin/env bash
set -euo pipefail

# Native-architecture verification of the exact runtime image argument.
# The flash file is task-owned and file-backed; this is a correctness smoke,
# not a physical SSD performance or endurance measurement.
image=${1:?usage: release-smoke.sh IMAGE VERSION OUTPUT_JSON}
expected_version=${2:?expected version required}
output=${3:?flash output path required}
mkdir -p "$(dirname "$output")"
client_image=${CACHELIB_SMOKE_CLIENT_IMAGE:-cachelib-investigation-client:local}
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
}
trap cleanup EXIT

version_output=$(docker run --rm "$image" --version)
case "$version_output" in
  *"$expected_version"*) echo "runtime version verified: $version_output" ;;
  *) echo "unexpected runtime version: $version_output" >&2; exit 1 ;;
esac

docker network create --internal "$network" >/dev/null
docker run -d --name "$ram_container" --network "$network" --network-alias cache \
  --cpus 2 --memory 512m --memory-swap 512m "$image" \
  --cache_size=134217728 --metrics_port=0 >/dev/null

for _ in $(seq 1 60); do
  if docker run --rm --network "$network" "$client_image" stats \
    --backend grpc --host cache --port 50051 >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
docker run --rm --network "$network" "$client_image" verify \
  --backend grpc --host cache --port 50051
docker run --rm --network "$network" "$client_image" limits \
  --backend grpc --host cache --port 50051 > "${output%.json}-limits.json"
python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); assert r["key_lengths"]["255"]["roundtrip"]; assert not r["key_lengths"]["256"].get("success", False); assert r["value_lengths"]["4193000"]["roundtrip"]; assert r["multiset_over_message_limit"].get("rpc_error")=="RESOURCE_EXHAUSTED"; assert r["multiget_over_message_limit"].get("rpc_error")=="RESOURCE_EXHAUSTED"' "${output%.json}-limits.json"

docker run -d --name "$flash_container" --network "$network" --network-alias cache-nvm \
  --cpus 2 --memory 768m --memory-swap 768m \
  --mount "type=bind,src=${flash_dir},dst=/data/nvm" "$image" \
  --cache_size=67108864 --enable_nvm=true --enable_io_uring=false \
  --nvm_path=/data/nvm/navy --nvm_size=536870912 \
  --nvm_reader_threads=4 --nvm_writer_threads=4 --metrics_port=0 >/dev/null

ready=false
for _ in $(seq 1 60); do
  if docker run --rm --network "$network" "$client_image" stats \
    --backend grpc --host cache-nvm --port 50051 >/dev/null 2>&1; then
    ready=true
    break
  fi
  if [ "$(docker inspect -f '{{.State.Running}}' "$flash_container")" = false ]; then
    docker logs "$flash_container" >&2
    exit 1
  fi
  sleep 1
done
if [ "$ready" != true ]; then
  docker logs "$flash_container" >&2
  exit 1
fi

docker run --rm --network "$network" --entrypoint python "$client_image" \
  /work/flash_smoke.py --host cache-nvm > "$output"
cat "$output"
