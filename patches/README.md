# Upstream patches

Two small diffs applied to the [facebook/CacheLib][up] tree during the
container build, at the revision pinned by `ARG CACHELIB_REF` in the
`Dockerfile`. They are applied with `git apply`, so if a pin bump makes one stop
applying the build fails immediately and loudly instead of much later with a
confusing error. CI verifies they still apply on every push, in about 20
seconds, without waiting on a full build.

| Patch | Touches | Why |
|---|---|---|
| `0001-folly-disable-io-uring.patch` | `build/fbcode_builder/manifests/folly` | Drops `libaio` from the Linux dependencies and forces `FOLLY_USE_IO_URING=OFF`, so folly builds without `io_uring`. The Docker Desktop VM kernel does not expose the features folly probes for, and the stock manifest yields a binary that fails at startup. |
| `0002-cachelib-common-optional-targets.patch` | `cachelib/common/CMakeLists.txt` | Makes three hard link targets optional behind `if(TARGET …)` — folly's exception tracer, FBThrift's `thrift_dynamic_value`, and `magic_enum`. The exception tracer needs debug symbols and `libiberty` internals that the slim build image does not carry. |

These replaced whole-file copies of the two upstream files. That approach
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

```bash
REF=$(grep -oE 'CACHELIB_REF=[0-9a-f]+' ../Dockerfile | cut -d= -f2)
git clone https://github.com/facebook/CacheLib /tmp/cachelib
git -C /tmp/cachelib checkout --detach "$REF"
git -C /tmp/cachelib apply --check -v /path/to/patches/*.patch
```

To move to a newer upstream revision: bump `CACHELIB_REF`, run the check above,
and if a patch no longer applies, re-cut it against the new revision rather than
force-fitting the old one.

[up]: https://github.com/facebook/CacheLib
