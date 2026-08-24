---
name: updating-the-cachelib-pin
description: Move ARG CACHELIB_REF in the Dockerfile to a newer facebook/CacheLib revision — re-cut patches/, verify the getdeps graph and the ninja targets, and confirm the build still compiles. Use when bumping the upstream CacheLib pin, when a patch stops applying, when the build dies in mvfst or with ManifestNotFound, or when picking up an upstream CacheLib fix.
---

# Updating the CacheLib pin

## Where the pin lives

`Dockerfile`, at **global scope above the first `FROM`**:

```dockerfile
ARG CACHELIB_REPO=https://github.com/facebook/CacheLib.git
ARG CACHELIB_REF=2aa2afbe97deadfb00f15260b26b566354c57a78
```

Both are re-declared inside the `builder` and `runtime` stages so the two cannot disagree about
what was built. The runtime stage stamps them into the image as
`io.celikgo.cachelib.upstream.revision`, and the release workflow reads that label back off the
pushed image and fails if it disagrees with the Dockerfile — the revision in the release notes
is a fact about the artefact, not a claim about the source tree.

It is a 40-character commit SHA, never a branch or tag, for two reasons:

1. **Reproducibility.** Cloning `main` means rebuilding a release tag pulls whatever upstream is
   that day. Upstream can also move a tag.
2. **Buildability.** The current pin is upstream `main` as of 2026-05-02 — "the last revision
   that actually builds", the revision the last known-good published image (1.6.0) was built
   from. It is deliberately *not* the newest commit: later upstream revisions bump mvfst to a
   version that does not compile under GCC 13 on Ubuntu 24.04, failing ~30 minutes in with

   ```
   error: default member initializer for
   'quic::DatagramFlowManager::QueuedDatagram::enqueueTime'
   required before the end of its enclosing class
   ```

   That is an upstream/toolchain incompatibility, not something this repo introduces. Moving
   past it means waiting for upstream to fix mvfst, or moving the build image to a compiler that
   accepts it. **Check this first** — if the candidate revision still carries that mvfst bump,
   stop; there is nothing to gain from a 30-minute failure.

## The two patches

Applied with `git apply` to the pinned tree during the build, so a bad pin fails immediately and
loudly instead of much later with a confusing error. Read `patches/README.md` before touching
either.

| Patch | Touches | Does |
|---|---|---|
| `0001-folly-disable-io-uring.patch` | `build/fbcode_builder/manifests/folly` | Removes `libaio` from `[dependencies.os=linux]` and adds `FOLLY_USE_IO_URING=OFF`, `Liburing_FOUND=OFF`, `LIBURING_FOUND=OFF` to `[cmake.defines]`. The Docker Desktop VM kernel does not expose the features folly probes for; the stock manifest yields a binary that fails at startup. |
| `0002-cachelib-common-optional-targets.patch` | `cachelib/common/CMakeLists.txt` | Wraps three hard link targets in `if(TARGET …)`: folly's exception tracer trio, `FBThrift::thrift_dynamic_value`, and `magic_enum::magic_enum`. The exception tracer needs debug symbols and `libiberty` internals the slim build image does not carry. |

These are real diffs on purpose. They replaced whole-file copies taken from a tree ~450 commits
behind upstream; once the pin was added, the stale folly manifest still declared a
`double-conversion` dependency upstream had removed and the build died with `ManifestNotFound`
(`353d09d`). A diff cannot drift silently — it either applies or fails.

If a patch stops applying, **re-cut it against the new revision**. Never force-fit the old one,
never widen the context to make it apply.

## Run the CI check locally, before pushing

This is exactly what the CI `patches` job does, in ~20 seconds, without waiting on a build.

```bash
cd /path/to/cachelib-grpc-server

# 1. Read the pin back out of the Dockerfile the same way CI does (this regex
#    requires exactly 40 hex chars; if it prints nothing, your pin is malformed).
REF=$(grep -oE 'ARG CACHELIB_REF=[0-9a-f]{40}' Dockerfile | cut -d= -f2); echo "$REF"

# 2. Clone upstream at the pin.                                    (~1-2 min)
git clone --quiet https://github.com/facebook/CacheLib.git /tmp/cachelib
git -C /tmp/cachelib checkout --quiet --detach "$REF"

# 3. Patches must apply cleanly.                                   (seconds)
git -C /tmp/cachelib apply --check -v "$PWD"/patches/*.patch

# 4. Dependency graph must still resolve cachelib.                 (~1 min)
git -C /tmp/cachelib apply "$PWD"/patches/*.patch
(cd /tmp/cachelib && python3 ./build/fbcode_builder/getdeps.py --allow-system-packages \
   list-deps cachelib > /tmp/deps.txt)
grep -qx cachelib /tmp/deps.txt   # this is the failure the pin originally exposed

# 5. io_uring is actually disabled in folly.                       (instant)
m=/tmp/cachelib/build/fbcode_builder/manifests/folly
grep -qx 'FOLLY_USE_IO_URING=OFF' "$m"
grep -qx 'Liburing_FOUND=OFF'     "$m"
grep -qx 'LIBURING_FOUND=OFF'     "$m"
awk '/^\[dependencies.os=linux\]/{f=1;next} /^\[/{f=0} f' "$m" | grep -qx libaio && \
  echo "FAIL: folly still declares libaio; the io_uring patch did not apply"
```

Step 5's last line asserts on the **folly manifest**, not on `/tmp/deps.txt`. cachelib's own
manifest declares `libaio` independently, so it legitimately stays in the resolved graph — do
not "fix" that by asserting `libaio` is absent from the dep list.

The release workflow adds a `pin` job that fails the release if `CACHELIB_REF` is not a full
40-character SHA or does not resolve upstream (~15 s, ahead of the ~50-minute compile), and
reports how far the pin trails upstream `main`.

## Failure modes, most likely first

1. **A patch stops applying** — upstream edited the same lines. `git apply --check -v` names the
   hunk. Re-cut the patch against the new revision.
2. **A manifest the patch references was deleted or renamed upstream.** `0001` targets
   `build/fbcode_builder/manifests/folly`; if that path moves, the patch fails at step 3 with
   "No such file or directory", not a hunk conflict.
3. **The getdeps graph stops resolving cachelib** — step 4's `grep -qx cachelib` fails, or
   `list-deps` itself errors with `ManifestNotFound` because a patched manifest names a
   dependency upstream removed. This is the exact class of failure that motivated switching from
   whole-file copies to diffs.
4. **The ninja target list in the `Dockerfile` changes.** The build asks for exactly
   `cachelib_allocator cachelib_navy cachelib_common cachelib_shm cachelib_datatype
   cachelib_nvmitem`, then copies six matching `.a` files by hand from
   `allocator/`, `navy/`, `common/`, `shm/`, `datatype/`. If upstream renames, splits or merges a
   library, ninja fails on the unknown target, or the build succeeds and the `cp` fails. Both are
   loud, and both need the target list *and* the copy list updated together.
5. **A header the server includes moves.** The server includes only two upstream headers —
   `<cachelib/allocator/CacheAllocator.h>` (`CacheManager.h`) and
   `<cachelib/allocator/nvmcache/NavyConfig.h>` (`CacheManager.cc`) — but the headers are
   installed by `cp -r ../cachelib/* /opt/cachelib/include/cachelib/`, so a moved header shows up
   as a compile error in the server build, an hour in, long after the checks above have passed.
   Also watch `magic_enum`: `EventSink.h` needs it, and the Dockerfile hand-copies it from
   `/opt/getdeps-install/magic_enum-*/include/magic_enum` (`c4cfe286` in the fork).

Failure modes 1–3 are caught in ~20 seconds by the local check. 4 and 5 need a real build.

## Testing the bump for real

```bash
docker build --target tester -t cachelib-grpc-server:test .
```

Cold, that is roughly an hour: gRPC v1.60.0, then getdeps' `--only-deps cachelib` (folly,
fbthrift, fizz, wangle, mvfst — the bulk of it), then CacheLib, then the server, then `ctest`
over both test binaries. A pin bump invalidates every layer; the buildx cache cannot help. The
mvfst failure, if the pin has it, appears ~30 minutes in.

To try a candidate revision without editing the file:

```bash
docker build --build-arg CACHELIB_REF=<40-char-sha> --target tester .
```

## Commit rule

**A pin bump is its own commit.** Nothing else in it — no server changes, no changelog rewrite
beyond the pin's own entry, no drive-by fixes. When the build breaks an hour in, the commit that
moved the pin must be revertible on its own. Precedent in this history: `44f68db` *"Pin upstream
to the last revision that actually builds"*, `8015d9a` *"Pin upstream CacheLib to a commit, and
fix the header check"*, with the changelog note landing separately in `767a773` *"Note the
patches change in the 1.7.0 changelog entry"*.

Message form:

```
build(cachelib): bump the upstream pin to <short-sha>
```

Body: why you are moving it (which upstream fix you need), which patches you re-cut, and that
`--target tester` passed locally.
