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
7. **Mutation self-check.** `make test` builds a slab whose objects overlap
   by 8 bytes and asserts the stress test rejects it. A suite that cannot
   fail is not a suite -- and this is not hypothetical: it is how we found
   that gcc was deleting the `calloc` zero-check as a provable tautology.
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

- [x] The allocator: spans, size classes, free lists, large-via-mmap
      - why: the whole design hinges on one decision -- metadata at a
        64 KiB-aligned span base instead of a per-object header -- so it is
        not separable into smaller commits that build and run.
- [x] The oracle: `t/stress.c`, `t/run.sh`, `Makefile`
      - why: the allocator is worthless without evidence it does not
        corrupt memory, and the evidence has to be real programs rather
        than our own idea of what a program does.
- [x] Harden the oracle against the compiler
      - why: mutation testing showed gcc was constant-folding the `calloc`
        zero-check and the alignment checks into `true`, because it knows
        what the standard promises. A test that the optimiser can prove
        tests nothing.
- [x] Benchmarks and documentation
      - why: the numbers that matter are the ones where we lose, and they
        are only credible if measured and published rather than estimated.

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
