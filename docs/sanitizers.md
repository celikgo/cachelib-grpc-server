# Sanitizers

What runs, what it can catch, and what it cannot.

## What runs

One nightly job, [`sanitize`](../.github/workflows/nightly.yml), builds the
Dockerfile's `sanitize` stage: the whole test suite compiled with
`-fsanitize=address,undefined` and run through `ctest`.

Only **this repository's translation units** are instrumented. The separately
built gRPC/Protobuf libraries and the CacheLib/getdeps dependency chain are
not rebuilt with sanitizer instrumentation. This limits what the run proves:

- **ASan**'s allocator and its `memcpy`/`memmove`/`strlen` interceptors are
  process-wide, so they apply to calls made from uninstrumented code as well.
- **UBSan** is pure per-translation-unit instrumentation, so it checks exactly
  the code in this repository — which is the code under review.

CacheLib is heavily templated, so a good deal of it *is* instrumented: anything
that lives in a header and is expanded into `CacheManager.cc` or a test.

Run it locally from the repository root:

```bash
docker build --target sanitize .
```

Docker reuses compatible cached dependency layers when available. Without
that cache, the command first builds the full dependency chain.

In an already provisioned Linux development environment, configure a separate
build directory with the CacheLib and dependency prefixes:

```bash
cmake -S . -B build-sanitize -DBUILD_TESTS=ON -DSANITIZE=address,undefined \
  -DCMAKE_PREFIX_PATH='/path/to/cachelib;/path/to/dependencies;/usr/local'
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

The September 24, 2026 local Docker qualification passed the complete suite on
**native Linux arm64 under Docker Desktop on Apple Silicon**, both normally
and with ASan/UBSan. The suite registers **158 GoogleTest cases**: 91 in the
manager binary and 67 in the service binary. CTest reports these as two
executable-level tests; its success summary does not enumerate individual
cases or conditional skips. The runtime smoke check separately verifies Navy
eviction and read-back.

| Build | Manager binary | Service binary | Total CTest time | Result |
|---|---:|---:|---:|---|
| Normal `tester` | 73.09 s | 4.08 s | 77.18 s | Both passed |
| ASan + UBSan `sanitize` | 99.07 s | 37.85 s | 136.92 s | Both passed |

These are observed test runtimes, excluding compilation and image export.
The sanitized local image is
`sha256:6b879efd079af769ce1b90614b835723c395f4fc61a7dffe99e0cc2722604964`;
the normal tester image is
`sha256:0bc3ff07c99e886878fa06b16d2f13c0c60de80f79aa4f93b9c30d3498816bda`.
The retained logs are `build/release/build-sanitize-arm64-final.log` and
`build/release/build-tester-arm64-final.log`; the
[release procedure](releasing.md#assemble-assets-and-publish) includes these
build/check logs in the published evidence.

This result establishes native arm64 coverage only. It does not establish
amd64 sanitizer coverage or native amd64 performance. The separate amd64
qualification uses emulation on this host and is recorded separately in the
release provenance. A passed local qualification does not by itself mean the
release has been published.

Hash-table sizing matters to sanitizer runtime. It used to be a fixed 2^25
buckets, and CacheLib's iterator is
header-inlined into our instrumented translation units, so every scan-based
test walked 33.5 million instrumented bucket reads: one test ran for over an
hour before that changed.

## What ASan here can and cannot catch

**Can**: use-after-free and double-free of `ReadHandle`/`WriteHandle` and of the
`std::string` values copied out of them; heap overflows in this repository's own
buffers, including `MetricsServer`'s hand-rolled HTTP socket handling; leaks of
anything allocated by this code (leak detection is off in CI because folly's
singletons are reported at exit; run locally with `ASAN_OPTIONS=detect_leaks=1`
if you are chasing one).

**Cannot**: see corruption *inside* a CacheLib slab. `SlabAllocator` takes its
arena from `mmap`, not `malloc`, so there are no redzones between cached items.
An item overrunning into its neighbour has no per-item poisoned boundary for
ASan to detect, even when the copy passes through an intercepted `memcpy`.

So this job is a lifetime checker for our own code. It is **not** a
cache-corruption detector, and the README does not claim it is.

## Why there is no ThreadSanitizer

TSan needs to see both halves of every synchronisation edge. Here it cannot,
and the result would be reports it cannot rule out rather than races that
exist:

1. **Half-instrumented atomics.** CacheLib's locks and its item-refcount
   compare-and-swaps are compiled into `libcachelib_allocator.a` without
   instrumentation, while the handle refcount operations that CacheLib puts in
   headers *are* instrumented when expanded into our translation units. TSan
   sees one side of each pair and no happens-before edge between them, and
   manufactures a race.

2. **Upstream's own annotations are dead.** CacheLib marks its
   documented-benign races with `annotate_ignore_thread_sanitizer_guard`. Those
   calls live in the Folly dependency, which was built with folly's
   `kIsSanitizeThread == false`, so they compile to no-ops. Even the races
   upstream has explicitly declared benign can therefore be reported.

3. **Suppressions do not rescue it.** TSan suppresses a report if *any* frame
   matches, and nearly every stack here passes through `facebook::cachelib::`.
   The suppression that silences the noise can also silence the signal. Broad
   library suppressions can likewise hide defects in callers. Inspect the
   actual linkage when evaluating any proposed suppression.

Making TSan useful requires a compatible, consistently instrumented dependency
chain and validation of any remaining reports. From a separate, patched
CacheLib checkout, a starting experiment is:

```bash
python3 ./build/fbcode_builder/getdeps.py build \
  --extra-cmake-defines '{"CMAKE_CXX_FLAGS": "-fsanitize=thread"}' cachelib
```

This is not a complete TSan build recipe: gRPC/Protobuf are built separately
by the Dockerfile and need compatible instrumentation too. Changed compiler
flags invalidate dependency build outputs. No working full-dependency TSan
configuration is qualified at the current pin, and its build time is not
measured here.

The manager's read-modify-write operations use one `std::mutex`, and its five
operation counters are atomic. Metrics serving, lifecycle handling, gRPC, and
the cache engine also have concurrent behavior; this is not an inventory of
every synchronization edge in the process. Targeted coverage for the manager is
[`tests/AtomicityTest.cc`](../tests/AtomicityTest.cc): threads released
simultaneously onto a single key, asserting exact outcomes — one SetNX winner,
`Incr` results forming exactly `1..N` with no duplicates, exactly one
`ttl_set`. These tests detect incorrect outcomes in the exercised schedules;
they do not prove the absence of every data race.

Revisit if the locking gets more complicated than one mutex, or if upstream
ships a TSan-instrumented build.

## Fuzzing

[`tests/fuzz/FuzzCacheService.cc`](../tests/fuzz/FuzzCacheService.cc) holds one
fuzz body with two front ends.

The **regression half** runs on every CI build: `FuzzCorpusReplayTest.cc`
replays an embedded corpus through it inside `cache_service_test`.
No clang, no libFuzzer, no extra stage. Once a crasher is found it goes
in the corpus and cannot come back silently. The seeds are embedded rather than
read from disk so the test cannot quietly replay zero inputs because a
directory failed to copy into the image; set `CACHELIB_FUZZ_CORPUS` to a
directory to replay extra files as well.

The allocator is reused, but each input begins with reset value state: the
previous input's written key is removed and three fixed keys are restored,
including a 255-byte adversarial matcher key and a binary key. At most four
keys are present during a request. The fixture uses the supported minimum
2^16 hash buckets; the earlier 2^18-bucket table made the sanitizer request
timer largely measure empty-bucket traversal. The unchanged one-second bound
covers protobuf parsing, operation dispatch, and the actual `Scan`, with
fixture preparation outside the timer. This is a bounded fuzz regression
fixture, not a production-size `Scan` performance claim.

The 255/256-byte key seeds use valid protobuf varint length encodings.
Additional regressions check that these seeds decode, that one input's values
do not leak into the next, and that the seeded matcher keys remain covered.

The **discovery half** builds the same body with `-DCACHELIB_GRPC_LIBFUZZER`
under clang, exposing `LLVMFuzzerTestOneInput`. It uses the same reset-per-input
value fixture. Allocator internals are still reused, so this does not promise
bit-for-bit coverage determinism or exercise long multi-request histories.

`CacheServiceImpl::Pipeline`'s loop body is not reachable:
`grpc::ServerReaderWriter` has no public constructor, so the streaming
handler cannot be driven without standing up a real gRPC server, which is far
too slow per iteration.

The highest-value thing the target reaches is the **glob matcher**. Every input
is also used as a `Scan` pattern, because that pattern is attacker-controlled
on an unauthenticated port and is applied to every key in the cache. The
previous implementation compiled it into a regular expression that could be
made to backtrack exponentially: a 25-byte pattern pinned a core indefinitely,
and the work continued after the client disconnected. See
`fix(scan): replace the backtracking pattern matcher`.
