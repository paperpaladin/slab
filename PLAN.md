# slab

## Contract

`slab` is a drop-in replacement for the C allocator, loaded with
`LD_PRELOAD=./libslab.so`, providing `malloc`, `free`, `calloc`, `realloc`,
`posix_memalign`, `aligned_alloc`, `memalign`, `valloc`, `pvalloc`,
`reallocarray` and `malloc_usable_size`.

It gets memory from `mmap(2)` and nothing else -- never from `brk`, never
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
  offset 64, and every class size is a multiple of 16 -- so every object
  offset within a span is a multiple of 16. The `_Static_assert`s on
  `HDR % 16` and `sizeof(struct hdr) <= HDR` are the part a compiler can
  check; the class table being all multiples of 16 is checked by the oracle.

Over-aligned requests leave the size classes entirely and get a dedicated
mapping, so they cannot weaken either half.

## Oracle

Ranked by how much it would embarrass us to fail it.

1. **Real programs, byte-identical output.** `ls`, `cat`, `grep`, `sort`,
   `wc`, `sh` pipelines, `python3` doing JSON and dict and string-building
   work, and `gcc` compiling a C file -- each run twice, once normally and
   once under the preload, demanding identical stdout, identical stderr and
   identical exit status. Nothing we could write is as good an allocator
   test as `sort` on a 3 MB file or CPython under a JSON round-trip.
2. **The byte-pattern stress test.** Millions of random
   `malloc`/`calloc`/`posix_memalign`/`realloc`/`free` over 2048 live slots,
   with each block filled with a pattern derived from a unique tag and
   verified on every subsequent touch. This is the only thing that reliably
   catches overlapping allocations, which is the bug that matters most
   because it corrupts silently and far from its cause.
3. **The direct no-overlap test.** Hold 30 000 live blocks of every
   interesting size at once, sort them by address, assert no block's payload
   reaches into the next one's. Attacks the hard invariant head-on instead
   of waiting for a pattern mismatch.
4. **Cross-thread stress.** Four threads churning, handing blocks to each
   other through a shared pool so blocks are freed on a thread other than
   the one that allocated them.
5. **Semantic conformance.** Alignment of every size from 0 to 4096 plus
   large and over-aligned cases; `posix_memalign` from 8 bytes to 4 MiB of
   alignment including the `EINVAL` cases; `calloc` zeroing *after
   dirtying and freeing the same block*, which is what catches an allocator
   that only zeroes fresh pages; `realloc` content preservation across every
   class boundary in both directions; `free(NULL)`; `malloc(0)`;
   out-of-range sizes returning `NULL`.
6. **The oracle checked against glibc.** The same stress and semantic tests
   run against glibc `malloc`. If they fail there, the test is wrong, not
   slab.
7. **Mutation self-check.** `make test` builds four deliberately broken
   slabs -- overlapping objects, a `calloc` that never zeroes, a `realloc`
   that copies half the bytes, a class index one too small for one octave --
   and requires each to be rejected *by the check that owns that behaviour*,
   not merely by some check somewhere. A suite that cannot fail is not a
   suite, and this is not hypothetical: it is how we found that gcc was
   deleting the `calloc` zero-check as a provable tautology, and the
   ownership requirement is what made it visible rather than masked by the
   fuzzer catching it incidentally.
8. **Footprint.** Peak RSS under the preload compared against glibc on a
   churn workload, bounded at 3x. Makes the "never returns memory to the OS"
   shortcut a measured number rather than a footnote.

## Non-goals

- Beating glibc. glibc's `malloc` has per-thread tcaches, per-thread arenas
  and twenty years of tuning; a 300-line allocator with one global lock will
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
        head-on test of the no-overlap invariant rather than waiting for a
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
        and turns "peak RSS is sticky" from an assertion into 1.08x.
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

### Milestone 1 -- the things the benchmarks say to fix, in order of payoff

- [ ] Per-thread magazines: a small per-class cache of objects owned by one
      thread, refilled and flushed in batches under the global lock.
      - why: the 4-thread benchmark is 16x slower than glibc, entirely
        because of lock contention. This is the single largest loss.
- [ ] A free-span cache for large allocations: keep recently freed large
      mappings instead of `munmap`ing them, matched by size.
      - why: large-block churn is 7.7x slower than glibc, which is almost
        all `mmap`/`munmap` syscalls and page faults glibc avoids by
        caching.
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
        spacing rather than a measurement of a real workload.
- [ ] Trace-replay benchmarking: record every allocator call from a real
      program to a ring buffer, replay the trace against slab and glibc.
      - why: the synthetic benchmark is our guess at a realistic pattern.
        A recorded `gcc` or CPython trace is not a guess.
- [ ] `madvise(MADV_HUGEPAGE)` on arenas and a measurement of whether it
      helps or hurts.
      - why: 4 MiB arenas are exactly the shape THP wants; worth knowing
        whether the TLB win beats the zeroing cost.
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
        and the next `malloc` returns a live block -- the exact silent
        corruption the hard invariant is supposed to rule out.
