# slab

## Contract

`slab` is a drop-in replacement for the C allocator, loaded with
`LD_PRELOAD=./libslab.so`, providing `malloc`, `free`, `calloc`, `realloc`,
`posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`,
`reallocarray` and `malloc_usable_size`.

It gets memory from `mmap(2)` and nothing else, never from `brk`, never
from the real `malloc`. Small requests (<= 8192 bytes) are rounded to one of
32 size classes and served from per-class free lists carved out of 64 KiB
spans; anything larger gets its own `mmap`.

Behaviour commitments, beyond the obvious:

- `free(NULL)` is a no-op.
- `malloc(0)` returns a distinct, freeable, non-`NULL` pointer.
- `calloc` returns zeroed memory, including when the block is recycled.
- `realloc` preserves content up to `min(old, new)`; `realloc(NULL, n)` is
  `malloc(n)`; `realloc(p, 0)` frees `p` and returns `NULL`, matching glibc.
  C17 made that undefined, but a drop-in replacement follows the thing it
  is dropping in for.
- On failure `malloc`/`calloc`/`realloc` return `NULL` with `errno` set to
  `ENOMEM`, and on `realloc` failure the old block is still valid.
- `posix_memalign` rejects an alignment that is not a power of two multiple
  of `sizeof(void *)` with `EINVAL`.

## Hard invariant

**Two live allocations never overlap, and every returned pointer is aligned
to `_Alignof(max_align_t)` (16 bytes on x86-64).**

Everything else is a performance property. These two are correctness, and
every test in the oracle exists to attack one of them:

- No overlap holds because an object's extent is `[span + bump, span + bump
  + cls_size)` and `bump` only ever advances by exactly `cls_size`, because
  a span serves exactly one class for its whole life, and because a pointer
  on a free list is not reachable from anywhere else.
- Alignment holds because spans are 64 KiB-aligned, payloads start at
  offset 64, and every class size is a multiple of 16, so every object
  offset within a span is a multiple of 16. The `_Static_assert`s on
  `HDR % 16` and `sizeof(struct hdr) <= HDR` are the part a compiler can
  check. Class sizes being all multiples of 16 is checked by the oracle.

Over-aligned requests leave the size classes entirely and get a dedicated
mapping, so they cannot weaken either half.

## Oracle

Ranked by how much it would embarrass us to fail it.

1. Real programs, byte-identical output. `ls`, `cat`, `grep`, `sort`,
   `wc`, `sh` pipelines, `python3` doing JSON and dict and string-building
   work, and `gcc` compiling a C file, each run twice, once normally and
   once under the preload, demanding identical stdout, identical stderr and
   identical exit status. Nothing we could write is as good an allocator
   test as `sort` on a 3 MB file or CPython under a JSON round-trip.
2. The byte-pattern stress test. Millions of random
   `malloc`/`calloc`/`posix_memalign`/`realloc`/`free` over 2048 live slots,
   with each block filled with a pattern derived from a unique tag and
   verified on every subsequent touch. This is the only thing that reliably
   catches overlapping allocations, the bug that matters most
   because it corrupts silently and far from its cause.
3. The direct no-overlap test. Hold 30 000 live blocks of every
   interesting size at once, sort them by address, assert no block's payload
   reaches into the next one's. Attacks the hard invariant head-on instead
   of waiting for a pattern mismatch.
4. Cross-thread stress. Four threads churning, handing blocks to each
   other through a shared pool so blocks are freed on a thread other than
   the one that allocated them.
5. Semantic conformance. Alignment of every size from 0 to 4096 plus
   large and over-aligned cases; `posix_memalign` from 8 bytes to 4 MiB of
   alignment including the `EINVAL` cases; `calloc` zeroing *after
   dirtying and freeing the same block*, and that is what catches an allocator
   that only zeroes fresh pages; `realloc` content preservation across every
   class boundary in both directions; `free(NULL)`; `malloc(0)`;
   out-of-range sizes returning `NULL`.
6. The oracle checked against glibc. Those same stress and semantic tests run against glibc `malloc`. If they fail there, the test is wrong, not
   slab.
6b. LD_PRELOAD interposition, checked first. `malloc_usable_size(malloc(1))`
   is compared with and without the preload, before any allocator behaviour is
   asserted. Without it the entire suite was green against a library that
   defines no `malloc`: a `libslab.so` built from one unused variable passes
   31 of the 32 checks, including the RSS ratios, the span-cache munmap count
   and all five mutants. This was missing from the Oracle list entirely, and
   it is the one check everything else depends on.

7. Mutation self-check. `make test` builds five deliberately broken
   slabs, overlapping objects, a `calloc` that never zeroes, a `realloc`
   that copies half the bytes, a class index one too small for one octave,
   and magazines with no adoption (this list named four of the five),
   and requires each to be rejected *by the check that owns that behaviour*,
   not merely by some check somewhere. A suite that cannot fail is not a
   suite, and this is not hypothetical: it is how we found that gcc was
   deleting the `calloc` zero-check as a provable tautology, and the
   ownership requirement is what made it visible, not masked by the
   fuzzer catching it incidentally.
8. Footprint. Peak RSS under the preload compared against glibc on a
   churn workload, bounded at 3x. Makes the "never returns memory to the OS"
   shortcut a measured number, not a footnote.

## Non-goals

- Beating glibc. glibc's `malloc` has per-thread tcaches, per-thread arenas
  and twenty years of tuning. A 300-line allocator with one global lock will
  lose, and the README says where.
- Hardening. No guard pages, no free-list pointer mangling, no double-free
  detection, no canaries. A double free corrupts the free list silently.
- Returning memory to the OS. Spans are mapped once and kept.
- Debugging features: no leak tracking, no `mtrace`, no allocation logging.
- Non-glibc platforms, non-x86-64 architectures, 32-bit, or kernels whose
  page size is not 4096.
- Statically linked programs. `LD_PRELOAD` has nothing to interpose.
- Being fork-safe in a multithreaded process without an intervening `exec`.

## Commit sequence

- [x] A correct allocator with one mmap per allocation
      - why: establishes the span-aligned header lookup and the re-entrancy
        constraints with nothing else in the way.  Correct, runs real
        programs, and costs a page per allocation - everything after it
        removes that cost without changing the behaviour.
- [x] Size classes, spans carved from arenas, and the global lock
      - why: the lock belongs in the same commit as the first shared mutable
        state, not before it.  Also where gcc was caught rewriting
        malloc+memset into a recursive call to our own calloc.
- [x] Per-class free lists, threaded through the dead objects
      - why: the second half of the no-per-object-header argument - the size
        comes from the span, the list linkage comes from the corpse.
- [x] calloc skips the memset when the page is provably still zero
      - why: the proof depends on three separate facts about spans, two of
        which are consequences of never returning memory to the OS, so it
        has to be written down next to the code.
- [x] posix_memalign and the aligned family, without a side table
      - why: huge alignments make the payload span-aligned, which collides
        with the header lookup.  The collision is turned into the signal.
- [x] Finish the glibc surface, and write down what is not fixed
      - why: mallopt and friends must exist or glibc's versions run against
        an arena we are not using; and the remaining ceilings need to be
        recorded before anything is tested, not after.
- [x] t/stress.c: semantic conformance, and the hard invariant tested directly
      - why: the contract checks output comparison can never see, plus a
        head-on test of the no-overlap invariant instead of waiting for a
        symptom.
- [x] t/run.sh: real programs under LD_PRELOAD, and `make test`
      - why: the only tier that really matters.  ls, cat, grep, sort, wc, a
        shell pipeline, CPython and gcc are a better corpus than anything
        we would write.
- [x] t/stress.c: the byte-pattern churn
      - why: overlapping allocations are silent and surface far from their
        cause; a per-block pattern is what makes them local and immediate.
- [x] t/stress.c: four threads, and blocks that cross between them
      - why: the first thing in the repo that exercises the lock at all, and
        cross-thread free is the case a per-thread cache would get wrong.
- [x] t/run.sh: run the oracle against glibc, and measure the footprint
      - why: guards against the suite drifting into a description of slab;
        and turns "peak RSS is sticky" from an assertion into a measurement:
        1.10x on the churn workload, measured twice.
- [x] t/run.sh: mutation self-check - RED, it indicts the oracle
      - why: `make test` fails at this commit on purpose.  A slab whose
        calloc never zeroes passes the targeted calloc check, because gcc
        proved the zero-checking loop could not observe a nonzero byte and
        deleted it.
- [x] Stop the compiler from deleting the oracle - GREEN
      - why: asm-volatile pointer laundering, volatile reads, and
        -fno-builtin on the oracle.  Three layers because any one of them is
        a bet on compiler behaviour, and those bets lose silently.
- [x] A benchmark harness, and the measured number behind the lock's ceiling
      - why: a shortcut: comment claiming a ceiling nobody measured reads
        like a finding and is worth less than no comment.
- [x] README: the numbers, including the four benchmarks slab loses
      - why: admitting a 72x loss with the reason is more credible than
        claiming the one win.
- [x] PLAN: this file
      - why: the ranked backlog is derived from the benchmark, so it comes
        after it.

### Milestone 1: the things the benchmarks say to fix, in order of payoff

- [x] Per-thread magazines: a small per-class cache of objects owned by one
      thread, refilled and flushed in batches under the global lock.
      - why: the 4-thread benchmark is 16x slower than glibc, entirely
        because of lock contention. This is the single largest loss.
      - done: 20.8x -> 1.2x, measured against the previous commit in the same
        sitting; the other four benchmark rows did not move. The TLS the
        feature needs is one eight-byte pointer with tls_model
        initial-exec, the only model that cannot reach malloc
        through __tls_get_addr. The magazines live in our own memory with a
        registry that outlives every thread, and a dead thread's set is
        adopted by the next thread to ask, tested with tgkill(tid, 0),
        pthread_key_create's destructor is the usual answer and is ruled out
        because pthread_setspecific allocates. New thread-churn test: 1000
        threads created and joined one at a time, peak RSS 1.00x glibc, and
        about 10.5x with the adoption removed (10.47x, 10.53x, 10.58x over
        three runs; this said 17x). Keeping half the magazine on a drain
        was implemented, measured, found to be inside the noise on all five
        benchmarks, and deleted.
- [x] A free-span cache for large allocations: keep recently freed large
      mappings instead of `munmap`ing them, matched by size.
      - why: large-block churn is 7.7x slower than glibc, which is almost
        all `mmap`/`munmap` syscalls and page faults glibc avoids by
        caching.
      - done: three rows moved. realloc-grow 81x -> 3.0x, mixed-sizes
        3.1x -> 1.3x, large-churn 8.2x -> 3.9x, all measured against the
        previous commit in one sitting. "Matched by size" turned out to mean
        best-fit within 25%, not exact: exact matching is the obvious design
        and moved large-churn by 2%, because that benchmark picks random sizes
        and page-rounding still leaves ~250 distinct lengths for a 16-entry
        cache to miss. Bounded on bytes (64 MiB), not entries, and the
        waste shows up as 1.10x glibc peak RSS against a 3.0x limit.
      - the correctness half: a recycled mapping is dirty, and calloc was
        `return malloc(size)` for anything over 8 KiB with the comment "fresh
        mmap: already zero". True until this cache existed and a disclosure
        bug the moment it did. Removing the memset makes calloc(1, 8193)
        return the previous owner's 0xaa bytes and the targeted calloc check
        catches it.
      - measured from outside with strace, because "mappings are reused" is a
        claim about syscalls and nothing in a program's output can see it:
        200 large alloc/free cycles cost 601 munmaps before and 3 after, with
        glibc at 2. Disabling the cache fails that assertion and nothing else.
- [ ] Grow large blocks without copying: reserve a `PROT_NONE` tail per
      large mapping and `mprotect` it in on growth.
      - why: `realloc`-grow is 72x slower than glibc. Every doubling past
        8 KiB is a fresh `mmap` plus a full `memcpy` plus a `munmap`, where
        glibc gets an `mremap` page-table edit.
- [ ] Return memory with `madvise(MADV_DONTNEED)` once a span is wholly
      free, keeping the mapping so the AND-mask lookup still works.
      - why: peak RSS is currently sticky forever; a program with a large
        transient peak never gives it back.
- [ ] Reserve one span-aligned address-space region up front and carve
      large mappings from it with `MAP_FIXED`.
      - why: removes the two trimming `munmap` calls that `map_aligned`
        pays on every large allocation and every new arena.
- [ ] A fragmentation visualiser: dump span occupancy to a file descriptor
      on a signal, render it as a PNG or an HTML heat map offline.
      - why: internal fragmentation is currently an argument from the class
        spacing, not a measurement of a real workload.
- [ ] Trace-replay benchmarking: record every allocator call from a real
      program to a ring buffer, replay the trace against slab and glibc.
      - why: the synthetic benchmark is our guess at a realistic pattern.
        A recorded `gcc` or CPython trace is not a guess.
- [ ] `madvise(MADV_HUGEPAGE)` on arenas and a measurement of whether it
      helps or hurts.
      - why: 4 MiB arenas are exactly the shape THP wants; worth knowing
        whether the TLB win beats the zeroing cost.
- [ ] Return a dead thread's magazines eagerly, not on adoption.
      - why: adoption bounds the stranded memory by peak thread count, not by
        threads created, which is enough for a thread pool and not for a
        program that peaks at 500 threads and then runs one. The honest
        options are a pthread_key_create destructor behind a recursion guard
        (pthread_setspecific allocates for high key indices) or a sweep of
        the registry with tgkill whenever a refill is about to map a new
        span.
- [ ] A `pthread_atfork` child handler that re-initialises the lock.
      - why: a multithreaded program that `fork()`s without `exec()` can
        deadlock in the child's first `malloc` today.
- [ ] Read the page size from `AT_PAGESZ` in the auxiliary vector.
      - why: 4096 is hardcoded, which is simply wrong on a 16 KiB-page
        arm64 kernel, and `sysconf` is not safe to call this early.
- [ ] Shrink large blocks in place on `realloc` down, `munmap`ing the tail.
      - why: `realloc(p_1MB, 100)` keeps a megabyte resident today.
- [ ] Optional free-list pointer mangling and a cheap double-free check,
      behind a compile-time flag.
      - why: a double free currently corrupts the free list into a cycle
        and the next `malloc` returns a live block: the exact silent
        corruption the hard invariant is supposed to rule out.
