# Upstream patches

Three small diffs applied to the [facebook/CacheLib][up] tree during the
container build, at the revision pinned by `ARG CACHELIB_REF` in the
`Dockerfile`. They are applied with `git apply`, so if a pin bump makes one stop
applying the build fails immediately and loudly instead of much later with a
confusing error. CI verifies they still apply on every push, in about 20
seconds, without waiting on a full build.

| Patch | Touches | Why |
|---|---|---|
| `0001-folly-disable-io-uring.patch` | `build/fbcode_builder/manifests/folly` | Drops `libaio` from the Linux dependencies and forces `FOLLY_USE_IO_URING=OFF`, so folly builds without `io_uring`. The Docker Desktop VM kernel does not expose the features folly probes for, and the stock manifest yields a binary that fails at startup. |
| `0002-cachelib-common-optional-targets.patch` | `cachelib/common/CMakeLists.txt` | Makes three hard link targets optional behind `if(TARGET …)` — folly's exception tracer, FBThrift's `thrift_dynamic_value`, and `magic_enum`. The exception tracer needs debug symbols and `libiberty` internals that the slim build image does not carry. |
| `0003-xz-download-url.patch` | `build/fbcode_builder/manifests/xz` | Changes only the host spelling for the pinned xz 5.2.5 archive to the working official `www.tukaani.org` endpoint. Its upstream SHA-256 stays `f6f4910f…f4aba10`; the downloaded bytes were independently checked against that value. |

The first two replaced whole-file copies of the upstream files. That approach
carried a trap: the copies were taken from a tree ~450 commits behind upstream,
so once `CACHELIB_REF` was pinned to a current revision the stale folly manifest
still declared a `double-conversion` dependency that upstream had removed, and
the build died with `ManifestNotFound`. A diff cannot drift silently that way —
it either applies or fails.

## Attribution

The context lines in these diffs are Meta's code, licensed under the Apache
License 2.0. See [`../NOTICE`](../NOTICE). Nothing here removes or alters an
upstream copyright header.

## Verifying and regenerating

From this repository's root:

```bash
cachelib_patch_ref=$(sed -n 's/^ARG CACHELIB_REF=\([0-9a-f]\{40\}\)$/\1/p' Dockerfile)
test -n "$cachelib_patch_ref"
cachelib_patch_checkout=$(mktemp -d "${TMPDIR:-/tmp}/cachelib-patches.XXXXXX")
git clone https://github.com/facebook/CacheLib "$cachelib_patch_checkout"
git -C "$cachelib_patch_checkout" checkout --detach "$cachelib_patch_ref"
git -C "$cachelib_patch_checkout" apply --check -v "$PWD"/patches/*.patch
```

This leaves the temporary upstream checkout available for inspection. Applying
the patches and running `getdeps.py --allow-system-packages list-deps cachelib`
there additionally checks dependency-graph resolution, as CI does.

To move to a newer upstream revision: bump `CACHELIB_REF`, run the check above,
and if a patch no longer applies, re-cut it against the new revision rather than
force-fitting the old one.

## Why the pin sits where it does

`CACHELIB_REF` retains the upstream revision selected for the historical
1.6.0 image on 2026-05-02. It is also the input to the 1.8.0 qualification;
the release provenance records the exact commit. During the original pin
investigation, newer revisions bumped mvfst to a version that failed under
GCC 13 on Ubuntu 24.04 with

```
error: default member initializer for 'quic::DatagramFlowManager::QueuedDatagram::enqueueTime'
       required before the end of its enclosing class
```

That was an upstream/toolchain incompatibility. It does not establish that
every later upstream revision is broken today. Evaluate a candidate pin with
the current compiler, all three patches, dependency-graph checks, and the full
test/runtime qualification before changing it.

[up]: https://github.com/facebook/CacheLib
