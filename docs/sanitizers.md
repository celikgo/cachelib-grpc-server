# Sanitizers

What runs, what it can catch, and what it cannot.

## What runs

One nightly job, [`sanitize`](../.github/workflows/nightly.yml), builds the
Dockerfile's `sanitize` stage: the whole test suite compiled with
`-fsanitize=address,undefined` and run through `ctest`.

Only **this repository's translation units** are instrumented. gRPC, folly,
fbthrift and CacheLib come from `getdeps.py` as uninstrumented static
libraries. That is fine for these two sanitizers:

- **ASan**'s allocator and its `memcpy`/`memmove`/`strlen` interceptors are
  process-wide, so they apply to calls made from uninstrumented code as well.
- **UBSan** is pure per-translation-unit instrumentation, so it checks exactly
  the code in this repository — which is the code under review.

CacheLib is heavily templated, so a good deal of it *is* instrumented: anything
that lives in a header and is expanded into `CacheManager.cc` or a test.

Run it locally against a prebuilt `builder` image:

```bash
docker build --target sanitize .
```

Or configure a sanitizer build directly:

```bash
cmake -DBUILD_TESTS=ON -DSANITIZE=address,undefined ..
```

Measured on an Apple M2 Max by building the `sanitize` stage itself — the same
thing the nightly builds — **150 tests, 147 seconds**, on top of the dependency
build. Sanitizers are not what makes this suite slow.

That number is only reachable because the hash table is sized from the cache
size. It used to be a fixed 2^25 buckets, and CacheLib's iterator is
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
An item overrunning into its neighbour is invisible unless the write goes
through an intercepted `memcpy` or runs past the end of the whole mapping.

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
   calls live in `libfolly.a`, which was built with folly's
   `kIsSanitizeThread == false`, so they compile to no-ops. Even the races
   upstream has explicitly declared benign would be reported.

3. **Suppressions do not rescue it.** TSan suppresses a report if *any* frame
   matches, and nearly every stack here passes through `facebook::cachelib::`.
   The suppression that silences the noise silences the signal. `called_from_lib:`
   does not apply either, because folly and CacheLib are static archives, not
   shared objects.

Making TSan meaningful means rebuilding the dependency chain under
`-fsanitize=thread`:

```bash
python3 ./build/fbcode_builder/getdeps.py build \
  --extra-cmake-defines '{"CMAKE_CXX_FLAGS": "-fsanitize=thread"}' cachelib
```

That changes `CMAKE_CXX_FLAGS` in the *first* expensive layer, so no buildx
cache is reusable and every run is a cold build of folly, fbthrift, gRPC and
CacheLib. The Dockerfile already calls that "about an hour" uninstrumented;
under TSan, plausibly two to four — nightly, every night. Whether folly and
mvfst even compile clean under TSan at the current pin is unknown; nobody has
tried.

Weighed against a concurrency surface that is **one `std::mutex` and five
`std::atomic` counters**, that is not a good trade today. What guards that
surface instead is
[`tests/AtomicityTest.cc`](../tests/AtomicityTest.cc): threads released
simultaneously onto a single key, asserting exact outcomes — one SetNX winner,
`Incr` results forming exactly `1..N` with no duplicates, exactly one
`ttl_set`. A lost update fails those tests deterministically, without TSan.

Revisit if the locking gets more complicated than one mutex, or if upstream
ships a TSan-instrumented build.

## Fuzzing

[`tests/fuzz/FuzzCacheService.cc`](../tests/fuzz/FuzzCacheService.cc) holds one
fuzz body with two front ends.

The **regression half** runs on every CI build: `FuzzCorpusReplayTest.cc`
replays an embedded corpus through it inside `cache_service_test`, in about
150 ms. No clang, no libFuzzer, no extra stage. Once a crasher is found it goes
in the corpus and cannot come back silently. The seeds are embedded rather than
read from disk so the test cannot quietly replay zero inputs because a
directory failed to copy into the image; set `CACHELIB_FUZZ_CORPUS` to a
directory to replay extra files as well.

The **discovery half** builds the same body with `-DCACHELIB_GRPC_LIBFUZZER`
under clang, exposing `LLVMFuzzerTestOneInput`. Two caveats worth knowing
before wiring it into CI:

- The target is **stateful**. The cache is constructed once per process and
  never flushed, because per-iteration construction would cost more than
  everything else combined. libFuzzer assumes rough determinism; coverage
  feedback varies between runs and a crasher may not reproduce from its file
  alone. Reproduction means replaying the corpus in order.
- `CacheServiceImpl::Pipeline`'s loop body is not reachable:
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
