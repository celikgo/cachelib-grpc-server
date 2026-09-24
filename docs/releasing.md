# Releasing from local Docker

The normal [Release workflow](../.github/workflows/release.yml) builds and
tests `linux/amd64` and `linux/arm64` on native GitHub runners. This procedure
publishes images built locally while retaining its source, correctness, and
provenance checks. It does not require a successful historical workflow run.

On an Apple Silicon Docker Desktop host, arm64 executes natively and amd64
executes through emulation. Record that distinction in the release notes and
provenance. An emulated amd64 correctness run is not native amd64 performance
evidence. Keep benchmark results tied to their measured image and host.

Run the commands below from the repository root in a Bash session. Configure
GitHub CLI authentication for releases and Docker authentication for GHCR
before beginning. Keep logs and assembled artifacts under ignored
`build/release/`; commit benchmark evidence intended to accompany the release.

## Freeze the source

Finish implementation, tests, and benchmark-driver changes, review them, and
commit them before qualification. This is the **build source commit**. Final
reports and documentation can be committed after measurement using the
metadata-only finalization below.
The version in `CacheManager.h`, `CMakeLists.txt`, `Dockerfile`, and the
qualification report must agree. Initially label images with their exact build
source commit; never label a dirty source tree with only its base commit.

```bash
set -euo pipefail
release_version=1.8.0
release_repo=celikgo/cachelib-grpc-server
release_image=ghcr.io/$release_repo
build_source_commit=$(git rev-parse HEAD)
release_commit=$build_source_commit
release_dir=$PWD/build/release
test -z "$(git status --porcelain)"
mkdir -p "$release_dir/dist"
```

Check that the remote tag, GitHub release, and versioned registry tags do not
already exist. If they do, inspect their immutable identities before taking
any action; use a new version for changed release content. Make the source
commit reachable on GitHub before publishing its tag.

CacheLib, gRPC, and the BoringSSL archive are pinned in the
[Dockerfile](../Dockerfile). Ubuntu packages and other build inputs can change
between builds, so record actual build dependencies and binary checksums.
These pins do not promise bit-for-bit binary reproducibility.

## Build and check both platforms

Use distinct tags so one architecture cannot replace the other in the local
image store. Build the correctness client for the host's native architecture;
it talks to each server over the isolated Docker network.

```bash
docker build -f bench/investigation/Dockerfile.client \
  -t cachelib-investigation-client:local .
for arch in arm64 amd64; do
  docker build --platform "linux/$arch" --target tester --progress=plain \
    -t "cachelib-release-tester:$release_version-$arch" . \
    2>&1 | tee "$release_dir/tester-$arch.log"
  docker build --platform "linux/$arch" --progress=plain \
    --build-arg SERVER_VERSION="$release_version" \
    --build-arg SOURCE_REVISION="$build_source_commit" \
    -t "cachelib-release:$release_version-$arch-measured" . \
    2>&1 | tee "$release_dir/runtime-$arch.log"
  test "$(docker image inspect "cachelib-release:$release_version-$arch-measured" \
    --format '{{.Architecture}}')" = "$arch"
  CACHELIB_SMOKE_PLATFORM="linux/$arch" scripts/release-smoke.sh \
    "cachelib-release:$release_version-$arch-measured" "$release_version" \
    "$release_dir/smoke-$arch.json" \
    2>&1 | tee "$release_dir/smoke-$arch.log"
done
```

The tester stage executes both registered CTest binaries. The smoke checks
exact version output, Docker health, gRPC health, RAM correctness and request
limits, and real Navy eviction/read-back with byte equality and TTL expiry.
It removes its own containers, network, and temporary flash directory on exit.
The Navy file remains subject to host page caching: this proves logical flash
tiering, not physical SSD throughput or endurance.

Also run the sanitizer target on the native host platform and retain its log:

```bash
docker build --target sanitize --progress=plain . \
  2>&1 | tee "$release_dir/sanitizers.log"
```

Record each server executable's SHA-256, the architecture, image ID, and
version/source/upstream labels. The checksum can be obtained without starting
the server:

```bash
for arch in arm64 amd64; do
  docker run --rm --platform "linux/$arch" --entrypoint sha256sum \
    "cachelib-release:$release_version-$arch-measured" \
    /usr/local/bin/cachelib-grpc-server \
    > "$release_dir/executable-$arch.sha256"
  docker image inspect "cachelib-release:$release_version-$arch-measured" \
    > "$release_dir/image-$arch.json"
done
```

Refresh the benchmark report and its generated tables using the documented
[benchmark procedure](../bench/strong/README.md). Compare like-for-like
workloads, retain raw manifests, and disclose native/emulated execution. Pass
the `-measured` runtime tag to the campaign and retain the immutable image ID
it records. Keep the inspection files and executable checksums above with that
evidence.

## Finalize labels after the report commit

Commit the completed reports and evidence, then label the qualified images
with that final release commit using a **metadata-only Docker build**. Do not
rebuild the production Dockerfile merely to change `SOURCE_REVISION`: its
revision label precedes the runtime package-install and user-creation steps.
Changing that label can rerun those steps and change filesystem layers even
when the server executable is identical. Such an image needs fresh runtime
qualification and affected benchmarks.

The following path is valid only while runtime source and build inputs remain
unchanged. First verify that the final commit descends from the build source
commit and that all production build inputs match. Compare the same runtime
fingerprint recorded by the benchmark harness at both commits:

```bash
release_commit=$(git rev-parse HEAD)
test -z "$(git status --porcelain)"
git merge-base --is-ancestor "$build_source_commit" "$release_commit"
git diff --exit-code "$build_source_commit" "$release_commit" -- \
  Dockerfile CMakeLists.txt build.sh proto cmake patches '*.cc' '*.h'
python3 - "$build_source_commit" "$release_commit" <<'PY'
import hashlib
import subprocess
import sys

def fingerprint(revision):
    names = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", revision], text=True).splitlines()
    paths = ["Dockerfile", "CMakeLists.txt", "proto/cache.proto"]
    paths += sorted(p for p in names if "/" not in p and p.endswith(".cc"))
    paths += sorted(p for p in names if "/" not in p and p.endswith(".h"))
    paths += sorted(p for p in names if p.startswith("patches/") and p.endswith(".patch"))
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.encode())
        digest.update(subprocess.check_output(["git", "show", f"{revision}:{path}"]))
    return digest.hexdigest()

before, after = map(fingerprint, sys.argv[1:])
assert before == after, "Runtime source changed; rebuild, qualify, and remeasure"
print("Unchanged runtime source fingerprint:", before)
PY
```

Require that printed fingerprint and each measured image ID also match the
campaign manifests. The broader Git diff protects build scripts and CMake
modules in addition to the harness fingerprint. Do not use this path for a
runtime source change, dependency update, or modified production Dockerfile.

Create a small separate Dockerfile containing only `FROM`, build arguments,
and the final revision label. Use Docker's local default builder, which can
resolve the verified images in the local image store. Each architecture gets
its own source tag and an explicit platform:

```bash
mkdir -p "$release_dir/finalize-context"
cat > "$release_dir/finalize-context/Dockerfile" <<'DOCKERFILE'
ARG QUALIFIED_IMAGE
FROM ${QUALIFIED_IMAGE}
ARG RELEASE_REVISION
LABEL org.opencontainers.image.revision="${RELEASE_REVISION}"
DOCKERFILE

for arch in arm64 amd64; do
  measured_tag="cachelib-release:$release_version-$arch-measured"
  source_tag="cachelib-finalization:$release_version-$arch"
  measured_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[0]["Id"])' \
    "$release_dir/image-$arch.json")
  test "$(docker image inspect "$measured_tag" --format '{{.Id}}')" = "$measured_id"
  test "$(docker image inspect "$measured_tag" --format '{{.Architecture}}')" = "$arch"
  docker tag "$measured_id" "$source_tag"
  docker build --builder default --platform "linux/$arch" --pull=false \
    --network=none --build-arg QUALIFIED_IMAGE="$source_tag" \
    --build-arg RELEASE_REVISION="$release_commit" \
    -t "cachelib-release:$release_version-$arch" "$release_dir/finalize-context"
  docker image inspect "cachelib-release:$release_version-$arch" \
    > "$release_dir/final-image-$arch.json"
  docker run --rm --platform "linux/$arch" --entrypoint sha256sum \
    "cachelib-release:$release_version-$arch" /usr/local/bin/cachelib-grpc-server \
    > "$release_dir/final-executable-$arch.sha256"
  cmp "$release_dir/executable-$arch.sha256" "$release_dir/final-executable-$arch.sha256"
  python3 - "$release_dir/image-$arch.json" "$release_dir/final-image-$arch.json" \
    "$build_source_commit" "$release_commit" "$arch" <<'PY'
import json
import sys

before, after = [json.load(open(path))[0] for path in sys.argv[1:3]]
assert before["Os"] == after["Os"] == "linux"
assert before["Architecture"] == after["Architecture"] == sys.argv[5]
assert before["RootFS"]["Layers"] == after["RootFS"]["Layers"], "Filesystem changed"
assert before["Config"]["Labels"]["org.opencontainers.image.revision"] == sys.argv[3]
expected_config = before["Config"]
expected_config["Labels"]["org.opencontainers.image.revision"] = sys.argv[4]
assert expected_config == after["Config"], "Configuration changed beyond revision label"
print("Verified metadata-only finalization:", before["Id"], "->", after["Id"])
PY
done
```

Retain the measured and final image inspections, executable checksums, and
fingerprint comparison. The image ID changes because its configuration changed;
the ordered filesystem layers and executable must be exactly equal. Verify
the final images with the smoke script before pushing, and repeat verification
against their pulled registry digests below. Any failed identity check requires
investigation and affected qualification/benchmarks before publication.

In provenance, distinguish `build_source_commit` from the final `commit`, state
that the latter was applied through metadata-only finalization, and record the
measured and final per-platform image IDs plus the equality checks. Do not
rewrite original run manifests to substitute the final commit or image ID.

## Push and verify immutable images

After source is frozen and all checks pass, push the architecture tags and
assemble the versioned index from their registry digests:

```bash
for arch in arm64 amd64; do
  docker tag "cachelib-release:$release_version-$arch" \
    "$release_image:$release_version-$arch"
  docker push "$release_image:$release_version-$arch"
  docker buildx imagetools inspect "$release_image:$release_version-$arch" \
    --format '{{.Manifest.Digest}}' > "$release_dir/digest-$arch"
done
arm_digest=$(cat "$release_dir/digest-arm64")
amd_digest=$(cat "$release_dir/digest-amd64")
docker buildx imagetools create -t "$release_image:$release_version" \
  "$release_image@$amd_digest" "$release_image@$arm_digest"
index_digest=$(docker buildx imagetools inspect "$release_image:$release_version" \
  --format '{{.Manifest.Digest}}')
printf '%s\n' "$index_digest" > "$release_dir/digest-index"
```

Inspect the index and ensure it resolves both platforms to the recorded child
digests. Pull each child digest with an explicit platform, inspect its labels
and architecture, and repeat the smoke against that digest. For example:

```bash
for arch in arm64 amd64; do
  child_digest=$(cat "$release_dir/digest-$arch")
  docker pull --platform "linux/$arch" "$release_image@$child_digest"
  test "$(docker image inspect "$release_image@$child_digest" \
    --format '{{.Architecture}}')" = "$arch"
  CACHELIB_SMOKE_PLATFORM="linux/$arch" scripts/release-smoke.sh \
    "$release_image@$child_digest" "$release_version" \
    "$release_dir/verify-$arch.json" \
    2>&1 | tee "$release_dir/verify-$arch.log"
done
```

Pulling a multi-platform index without a platform on Apple Silicon selects
arm64; doing that twice does not verify amd64. Check each executable checksum
again after pulling, and require the source, version, and CacheLib pin labels
to match the frozen checkout.

## Assemble assets and publish

Use the asset names in the [Release workflow](../.github/workflows/release.yml):
`cache.proto`, `provenance.json`, `benchmark-current-*.json`,
`comparative-report.md`, `benchmark-historical-20260923-dependencies.json`,
`benchmark-historical-1.6.json`, `LICENSE`, `NOTICE`, and `SHA256SUMS`.
Attach the new smoke/limits evidence and build/check logs as well. Historical
benchmark and candidate dependency assets must retain their original scope;
do not present them as observations from a new builder.

`provenance.json` must contain the version, repository, exact build source and
final release commits, any metadata-only finalization,
registry image, index and per-platform digests, upstream CacheLib/gRPC/BoringSSL
pins, runtime source fingerprint, and per-platform executable checksums.
Include build host/engine details, execution mode for each architecture,
validation results, and the measured candidate identity. Release notes must
describe local Docker publication and any amd64 emulation explicitly.

Generate checksums after assembling all assets, excluding the checksum file
itself, and verify them before upload. `shasum -a 256` is available on macOS;
Linux can use `sha256sum` with the same checksum-file format.

Tag pushes matching `v*` trigger the Release workflow. Temporarily disable
**only that workflow** before pushing the release tag, so it cannot rebuild
and overwrite the locally verified images. Leave CI and Nightly enabled.
The following restores a previously active Release workflow on shell exit:

```bash
test "$(gh api "repos/$release_repo/actions/workflows/release.yml" \
  --jq .state)" = active
restore_release_workflow() {
  gh workflow enable release.yml --repo "$release_repo"
}
trap restore_release_workflow EXIT
gh workflow disable release.yml --repo "$release_repo"
test "$(gh api "repos/$release_repo/actions/workflows/release.yml" \
  --jq .state)" = disabled_manually

test "$(git rev-parse HEAD)" = "$release_commit"
test -z "$(git status --porcelain)"
git tag -a "v$release_version" "$release_commit" \
  -m "Release $release_version built and verified with local Docker"
git push origin "refs/tags/v$release_version"
gh release create "v$release_version" --repo "$release_repo" --verify-tag \
  --title "v$release_version" --notes-file "$release_dir/notes.md" \
  "$release_dir/dist/"*

docker buildx imagetools create -t "$release_image:latest" \
  "$release_image@$index_digest"
test "$(docker buildx imagetools inspect "$release_image:latest" \
  --format '{{.Manifest.Digest}}')" = "$index_digest"

restore_release_workflow
trap - EXIT
```

If Release was already disabled, preserve that state and establish why before
continuing. If the shell is killed or loses connectivity, explicitly verify
the workflow state and restore it as needed. GitHub documents the
[disable and enable commands](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/disable-and-enable-workflows).

Finally, download the public release assets into a new directory and verify
`SHA256SUMS`. Confirm the tag resolves to the recorded source commit, the
versioned registry index and `latest` have the recorded digest, and each
platform can still be pulled. Repeat the digest smoke after publication and
record the resulting release URL and evidence. A failed final check leaves
the release unqualified until the mismatch is resolved.
