# Upstream patches

These two files are copied verbatim from [facebook/CacheLib][up] and modified.
They are applied to the freshly-cloned upstream tree during the container build
(see the `COPY patches/...` lines in the `Dockerfile`). Both retain their
original Meta Platforms copyright headers.

| File | Replaces in upstream tree | Why |
|---|---|---|
| `folly.manifest` | `build/fbcode_builder/manifests/folly` | Builds folly without `io_uring`. The Docker Desktop VM kernel does not expose the `io_uring` features folly probes for, so the stock manifest produces a binary that fails at startup. |
| `cachelib-common-CMakeLists.txt` | `cachelib/common/CMakeLists.txt` | Makes the exception tracer optional. It requires debug symbols and `libiberty` internals that are not present in the slim build image. |

They are kept as whole files rather than `.patch` diffs because upstream
`main` moves frequently and a context-sensitive diff breaks more often than a
full-file replacement does.

To see exactly what changed, diff against upstream at the pinned revision:

```bash
git clone --depth 1 https://github.com/facebook/CacheLib /tmp/cachelib
diff /tmp/cachelib/build/fbcode_builder/manifests/folly patches/folly.manifest
diff /tmp/cachelib/cachelib/common/CMakeLists.txt patches/cachelib-common-CMakeLists.txt
```

[up]: https://github.com/facebook/CacheLib
