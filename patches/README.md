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
# The revision the container build pins (ARG CACHELIB_REF in the Dockerfile)
REF=6c222303ec8ca0654700b1dd01deb8c113d70321
git clone https://github.com/facebook/CacheLib /tmp/cachelib
git -C /tmp/cachelib checkout --detach "$REF"
diff /tmp/cachelib/build/fbcode_builder/manifests/folly patches/folly.manifest
diff /tmp/cachelib/cachelib/common/CMakeLists.txt patches/cachelib-common-CMakeLists.txt
```

Both files keep their upstream headers where upstream has one: the CMakeLists
carries Meta's copyright header and CI enforces that it stays. `folly.manifest`
is an INI manifest and has no header upstream either.

[up]: https://github.com/facebook/CacheLib
