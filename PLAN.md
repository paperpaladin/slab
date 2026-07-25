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
   defines no `malloc`: a `libslab.so` built from one unused variable passed
   31 of the 32 checks of the day, including the RSS ratios, the span-cache
   munmap count and every mutant. This was missing from the Oracle list
   entirely, and it is the one check everything else depends on. Each mutant
   is now held to the same standard: the built mutant has to change that
   number, or rejecting it proves nothing.

6c. The reserved tail, `t/stress guard`. The one slab-specific mode, and the
   one place the kernel is the oracle directly: the permissions of the tail
   mapping are read back out of `/proc/self/maps` and have to be `---p`, and
   a byte written past the end of a block has to kill a forked child with
   SIGSEGV. `mprotect` returning 0 is not evidence. It also asserts that
   `realloc` returns THE SAME POINTER for a large block that quadrupled,
   which is the "did the new path run" check -- a reservation nobody grows
   into is invisible everywhere else -- and that a grown block comes back out
   of the span cache at its original size with its guard restored. glibc
   fails all eight of its assertions, and the suite asserts that, because an
   assertion glibc also satisfied would be about the C library.

7. Mutation self-check. `make test` builds ten deliberately broken
   slabs: overlapping objects, a `calloc` that never zeroes, a `realloc`
   that copies half the bytes, a class index one too small for one octave,
   magazines with no adoption, a `realloc` that moves a large block instead
   of growing it, a reserved tail mapped writable instead of `PROT_NONE`, a
   reservation that is never taken, a grown block that keeps its grown size
   when freed, and a shrink that drops the pages but leaves the tail
   writable. Each must be rejected *by the check that owns that behaviour*,
   not merely by some check somewhere. A suite that cannot fail is not a
   suite, and this is not hypothetical: it is how we found that gcc was
   deleting the `calloc` zero-check as a provable tautology, and the
   ownership requirement is what made it visible instead of masked by the fuzzer catching it incidentally.
   The harness itself was fooled once, by sed rather than by slab: `|` is both
   sed's delimiter and a C operator, so an expression containing
   `PROT_READ | PROT_WRITE` exited 1 and left an empty mutant source, which
   compiles into a library with no `malloc` and is duly "rejected" by every
   mode.
8. Footprint. Peak RSS under the preload compared against glibc on a
   churn workload, bounded at 3x. Makes the "never returns memory to the OS"
   shortcut a measured number, not a footnote.

## Non-goals

- Beating glibc. glibc's `malloc` has per-thread tcaches, per-thread arenas
  and twenty years of tuning. A 300-line allocator with one global lock will
  lose, and the README says where.
- Hardening, with one accident. No free-list pointer mangling, no
  double-free detection, no canaries; a double free corrupts the free list
  silently. The exception arrived as a side effect of growing large blocks
  in place: every large mapping of 128 KiB or more now ends in a `PROT_NONE`
  reservation, so a write past the end of such a block faults instead of
  landing in page-rounding slack. That is a real guard page and it is tested
  as one, but it covers only large blocks, only past the end of the page the
  payload ends in, and it was not the point.
- ~~Returning memory to the OS.~~ No longer a non-goal, and the entry is
  kept rather than deleted because the argument it recorded was wrong. A
  span whose last object has been returned is `madvise(MADV_DONTNEED)`'d
  while staying mapped, which gives back 94% of a transient peak, and so is
  the part of a large mapping a `realloc` had grown into. What is still
  true: the address space is never given back, one empty span per class is
  kept, and the first page of every span stays resident because the header
  is in it.
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
- [x] Grow large blocks without copying: reserve a `PROT_NONE` tail per
      large mapping and `mprotect` it in on growth.
      - why: `realloc`-grow is 72x slower than glibc. Every doubling past
        8 KiB is a fresh `mmap` plus a full `memcpy` plus a `munmap`, where
        glibc gets an `mremap` page-table edit.
      - the 72x was already stale when this item came up: the span cache had
        taken `realloc-grow` to 2.9x glibc, because that benchmark stops at
        64 KiB and the cache serves every size in it. So the first thing
        this needed was a benchmark that reaches the case the item is about.
        New `large-grow` row: a buffer doubled from 16 KiB to 4 MiB, eight
        large-to-large steps where `realloc-grow` has two. Measured before
        any change: 189355 ns/op against glibc's 877, which is 216x, and
        13.5x jemalloc and 11.0x tcmalloc.
      - done: 189355 -> 2274 ns/op, 2.6x glibc, and 6.4x FASTER than
        jemalloc and 7.1x faster than tcmalloc. Measured against the
        previous commit in one interleaved run of ten repetitions each
        (207606 -> 2818, a factor of 74); the other five rows moved 1.01x to
        1.06x, and the 1.06x on large-block churn is repeatable and is one
        extra `mprotect` per large allocation. Peak RSS on the churn
        workload went 1.10x -> 1.09x glibc. The address-space cost is the
        real one: `VmPeak` 34 MB -> 996 MB on a 64-live-large-block
        workload, with `VmHWM` 1648 kB either way.
      - two knobs, both set by measurement and not by reasoning.
        `RESERVE_MIN` = 128 KiB, because the `mprotect` pair costs more than
        the copy below that: with no floor, `realloc-grow` went 151 -> 638
        ns/op, and the measured floors are 1 -> 638, 16 KiB -> 677,
        32 KiB -> 533, 64 KiB -> 155, 128 KiB -> 151. `RESERVE_K` = 31, so a
        mapping is 32x its request and a buffer doubles five times before it
        has to move; the `large-grow` row is bimodal in this because its
        chain spans exactly 32x from the first reserved block, which is
        written down in the README rather than quietly tuned into.
      - the bug it introduced, and the one worth remembering: a grown
        mapping went into the span cache at its grown size, nothing ever
        matched it again, and 200 rounds of the `realloc-grow` loop went
        from 17 `mmap`s and 9 `munmap`s to 214 and 587. `free` now restores
        a grown block to the size it was mapped with, which needs BOTH
        `madvise(MADV_DONTNEED)` (the pages) and `mprotect(PROT_NONE)` (the
        guard), and there is a mutant for each half. `make test` holds the
        syscall count down with `strace`, because nothing in a program's
        output can see it.
      - the oracle: `t/stress guard`, the one slab-specific mode in the
        suite. It asserts that `realloc` returns THE SAME POINTER for a
        large block that quadrupled (a reservation nobody grows into is
        invisible in every other check), that the kernel reports the tail
        as `---p` in `/proc/self/maps`, that writing one byte past the end
        kills a forked child with SIGSEGV, and that a grown block is
        recycled at its original size with its guard back. glibc fails all
        eight of its assertions and the suite asserts that too. Five new
        mutants, one per behaviour, all caught.
      - the harness bug found on the way: `|` is both sed's delimiter and a
        C operator, so the mutant expression containing `PROT_READ |
        PROT_WRITE` made sed exit 1 and leave an EMPTY mutant source, which
        compiles into a library with no `malloc` and is "rejected" by every
        mode. It read as a confirmation. `mutant()` now checks sed's exit
        status and requires the built mutant to change
        `malloc_usable_size(malloc(1))`; both halves have been watched to
        fail.
      - BLOCKED, honestly: no AddressSanitizer number for slab. ASan needs
        its runtime first in the initial library list and then its own
        `malloc` interposes ahead of ours (`t/stress interposed` prints 1,
        not 16), so the suite would pass while testing ASan's allocator.
        UBSan does not replace `malloc` and was run against slab instead:
        clean on all five modes and under `ls`, `sort`, CPython and `gcc`.
        ASan was run against the oracle, where it belongs, and it reports
        `guard`'s deliberate past-the-end write as a heap-buffer-overflow,
        which is independent confirmation that the byte is out of bounds.
- [x] Return memory with `madvise(MADV_DONTNEED)` once a span is wholly
      free, keeping the mapping so the AND-mask lookup still works.
      - why: peak RSS is currently sticky forever; a program with a large
        transient peak never gives it back.
      - done: 94% of a 200 MB transient peak comes back, measured from inside
        the process out of /proc/self/statm: RSS 31004 -> 265292 -> 45692 KiB
        over allocate, touch, free of 3.7 million 56-byte objects. glibc,
        jemalloc 5 and tcmalloc_minimal all give back 0% at that instant;
        glibc returns it on malloc_trim(0) (264992 -> 30940 KiB) and jemalloc
        with dirty_decay_ms:0 (274580 -> 40532), so the difference is when,
        not whether. jemalloc still held all of it after a 16-second sleep,
        because decay purging without background_thread is driven by
        allocator activity rather than by a timer.
      - it cost nothing: 0.94x to 1.02x of the previous commit on all six
        benchmark rows, interleaved, nine repetitions each. And it IMPROVED
        the one footprint number the suite already had a workload for: peak
        RSS of the byte-pattern stress run is 0.51x glibc.
      - the design, which is a rewrite of the small path. Knowing a span is
        wholly free is impossible with a free list threaded across spans
        (finding out means walking the whole list), so each span now owns its
        own free list and a live count, and each class keeps a list of the
        spans that have room. The invariant everything rests on is that a
        span is on that list if and only if it has a free object or an
        unbumped one. Measured separately from the madvise: the data
        structure change alone is 0.96x to 1.03x, so it is free.
      - MADV_DONTNEED and not munmap, because the mapping has to stay for the
        AND-mask lookup. The first page cannot go either (the header is at
        offset 0 and madvise is page-granular), so 6.25% of every span stays
        resident, and the objects in that page are zeroed by hand. Skipping
        that hand-zeroing is a disclosure bug: re-arming the bump pointer
        tells calloc the span is untouched zero page, and the first page is
        not. The reclaim check dirties every byte with 0xaa first and then
        requires 20000 callocs out of re-armed spans to be zero, which is how
        the mutant that drops the memset is caught.
      - 23% was lost and then recovered. With no hysteresis, a span of the
        8192 class holds seven objects, so a drifting workload emptied and
        refilled spans constantly: 520 extra madvise calls over a bench run
        cost 10 ns on every operation in the mixed-sizes row (46.7 -> 56.1
        ns/op). It is not the syscall count, it is the TLB flush across every
        CPU plus re-faulting the pages. Keeping one empty span per class back
        took madvise from 624 to 125 and the row to 48.7.
      - `check` cannot see any of this, and that is worth knowing: its frees
        go onto a magazine, the magazine does not drain in 4 KiB of traffic,
        so no span there ever reaches zero live objects. Four mutants, all
        caught: a span never given back and a reclaim without the madvise
        (both by the RSS reading), a reclaim that leaves the first page dirty
        (by the calloc check), and a displaced spare reclaimed without
        checking it is still empty (by the byte-pattern churn, at iteration
        43181 with eight blocks clobbered -- silent corruption, and the only
        check that could have found it).
      - and one leak that nothing caught until a check was added for it. A
        span is unlinked from its class when it fills; a mutant that never
        relinks it when it stops being full leaks every span it ever used and
        corrupts nothing, exits 0, passes the reclaim check, and passes the
        Python churn RSS ratio at 1.09x -- identical to the real thing. Peak
        RSS of the byte-pattern stress run separates them: 0.51x glibc
        against 1.67x. That check and that mutant are both new.
      - not mutation-testable, said plainly: removing the one-span hysteresis
        is a 23% slowdown on one row and nothing else, and the suite asserts
        no timings. Same for the RESERVE_MIN floor of the previous commit.
      - UBSan clean against slab on all seven modes (interposed, check,
        guard, reclaim, stress, thread, tchurn). ASan still cannot see slab
        at all, for the reason recorded above. Running the oracle under ASan
        did find a bug in the oracle: the "percentage given back" line
        underflowed to 18446744073709551608% when RSS ended above the peak
        reading, which is what ASan's own quarantine makes happen.
- [x] Stop unmapping the alignment slack at the end of a large mapping
      - why: this started as the item now at the bottom of this list,
        "reserve one span-aligned region and carve it with MAP_FIXED", whose
        stated payoff was the two trimming munmaps that map_aligned pays per
        large allocation. The payoff was measured before the work, by
        building a map_aligned with both trims simply deleted -- which leaks
        address space and is not shippable, but is an upper bound:
        large-churn 0.81x, mixed-sizes 0.94x, the other four flat. Worth
        having.
      - done, by taking the cheaper half. map_aligned over-maps by one span
        and trims both ends; the BACK trim can be skipped outright for a
        caller that can use address space at the end of its mapping, which
        is exactly what a large block's PROT_NONE reservation is. Eleven
        lines. Measured against the previous commit, eleven repetitions
        interleaved: large-churn 0.85x, which is 3.71x glibc down from
        4.72x; mixed-sizes 0.97x; realloc-grow 0.96x; the other three 0.98x
        to 1.00x. Syscalls over a benchmark run: munmap 5108 -> 3479, mmap
        and mprotect unchanged.
      - what it is NOT: the front trim is still there and fires on all but
        one call in sixteen (13 span-aligned returns in 200 probes). The
        upper-bound build says removing that one too is worth another 0.02x
        on large-churn and nothing elsewhere, which is why the MAP_FIXED
        item has moved to the bottom of this list with that number attached.
      - the arena path and the sub-RESERVE_MIN large path keep both trims on
        purpose. An arena cannot use a remainder smaller than a span, and
        the unreserved large path maps read-write, where folding slack in
        would leave bytes past the end of the block writable. The last
        assertion in `stress guard` is exactly that, and it fails if this is
        got wrong.
      - no new check, said plainly. The correctness of the fold is covered
        by `guard` (the tail still faults), by the no-overlap test and by
        tier 1; the gain is a timing and this suite asserts none. The
        mutation harness did earn its keep: the expression for "a reserved
        tail mapped writable" stopped matching when map_aligned grew an
        argument, and the did-sed-apply guard turned the suite red instead
        of printing a pass.
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
- [ ] Reserve one span-aligned address-space region up front and carve
      large mappings from it with `MAP_FIXED`.
      - why: removes the front trimming `munmap` that `map_aligned` still
        pays on fifteen out of sixteen large allocations and new arenas.
      - MOVED HERE FROM THIRD IN THIS MILESTONE, with a measurement. It was
        ranked on removing both trims. The back one is gone, and an
        upper-bound build with the front one deleted as well says the rest
        is worth 0.02x on large-churn and nothing on the other five rows.
        Doing it properly needs an address-space allocator: a free list of
        span-aligned ranges with coalescing, or a bitmap over a fixed set of
        reserved regions. That is around 120 lines and a new way for two
        large allocations to overlap, which is a bad trade at 2%.
