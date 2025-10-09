# slab

A size-class memory allocator in one C file, loadable over any dynamically
linked program with `LD_PRELOAD` to replace `malloc` and friends.

```
$ make
$ LD_PRELOAD=./libslab.so sort /etc/services | wc -l
```

## Why this exists

Because "my allocator passes my tests" is worth nothing, and "`gcc` compiles
a file correctly with my allocator loaded instead of glibc's" is worth
something. An allocator is one of the few pieces of software with a
ready-made, adversarial, enormous test corpus: every program on the machine.
So this is a real allocator -- `mmap`, size classes, free lists, direct `mmap`
for large blocks -- written small enough that the interesting part is the
oracle rather than the code.

It is also an answer to a narrower question: how much does a per-object
header cost? glibc puts 8 to 16 bytes of chunk metadata in front of every
allocation. slab puts 64 bytes at the base of each 64 KiB span instead and
finds it with one `AND`, which is 0.1% overhead instead of 100% on a 16-byte
object. That is the one benchmark slab wins, and it wins it by 39%.

## The oracle

`make test` runs 24 checks in three tiers.

**Tier 1, which is the whole point:** real system binaries, run twice -- once
normally, once with `LD_PRELOAD=./libslab.so` -- demanding that stdout,
stderr and the exit status are *byte-identical*. `ls -la /usr/bin`,
`ls -R /usr/include`, `cat` and `sort` and `wc` on a 3.4 MB generated file,
`grep -r` across `/usr/include`, a `cut | sort | uniq -c | sort -rn` shell
pipeline, `python3` doing a 40 000-object JSON round trip, a 300 000-key
`Counter`, and 20 000 rounds of string concatenation, and `gcc -O2 -S`
compiling the test program itself. Any diagnostic from slab on stderr also
fails the check, so a path we do not handle cannot pass quietly.

**Tier 2:** the things output comparison cannot see -- alignment of every size
from 0 to 4096, `posix_memalign` from 8 bytes to 4 MiB of alignment,
`calloc` zeroing *verified after dirtying and freeing the same block*,
`realloc` content preservation across every class boundary in both
directions, `free(NULL)`, `malloc(0)`, and a byte-pattern stress test: 3
million random operations over 2048 live slots, each block filled with a
pattern derived from a unique tag and verified on every touch. Plus a direct
assault on the hard invariant -- 30 000 simultaneous live blocks, sorted by
address, asserting that none overlaps the next -- and the same stress on four
threads with blocks handed between them so cross-thread `free` is exercised.
The stress and semantic tests also run against glibc `malloc`, because if
they fail there then the test is wrong, not slab.

**Tier 3:** the oracle checked for teeth. `make test` builds a slab whose
objects overlap by 8 bytes and asserts the stress test rejects it.

```
$ make test
ok    ls -la /usr/bin (82504 bytes identical)
ok    ls -R /usr/include (101687 bytes identical)
ok    ls error path (5 bytes identical)
ok    cat /etc/services (12990 bytes identical)
ok    cat big file (3378195 bytes identical)
ok    grep /etc/services (9083 bytes identical)
ok    grep -r /usr/include (5459 bytes identical)
ok    grep -c big file (7 bytes identical)
ok    sort big file (3378195 bytes identical)
ok    sort -u | wc (24 bytes identical)
ok    sort -k2 /etc/services (12990 bytes identical)
ok    wc -lwc (49 bytes identical)
ok    shell pipeline (623 bytes identical)
ok    python3 json churn (40 bytes identical)
ok    python3 dict/sort (48 bytes identical)
ok    python3 str building (23 bytes identical)
ok    gcc -S the stress test (5 bytes identical)
ok    nested shell + subproc (9 bytes identical)
ok    semantics (alignment, calloc, realloc, memalign, no-overlap)
ok    stress: 3M random ops, byte pattern verified
ok    stress: 4 threads, 400k ops each, cross-thread free
ok    oracle agrees with glibc malloc
ok    peak RSS 1.08x glibc on a churn workload (limit 3.0x)
ok    self-check: oracle rejects an allocator with overlapping objects

24 passed, 0 failed
```

### What the oracle caught

The stress test did not find a bug in the allocator -- all 24 checks passed on
the first full run. It did find a bug in *itself*, which is more interesting.

Mutation testing (build a deliberately broken slab, confirm the suite
rejects it) found that a slab whose `calloc` **never zeroed anything at all**
passed cleanly. The reason: gcc knows what `calloc` is declared to promise,
so at `-O2` it proved the zero-checking loop could not observe a non-zero
byte and deleted the loop. The same hazard applied to the alignment checks,
since gcc also knows `malloc` returns `max_align_t`-aligned memory. The fix
is in `t/stress.c`: every pointer is laundered through an `asm volatile`
barrier before it is inspected, the `calloc` checks read through a `volatile`
pointer, and the oracle is compiled with `-fno-builtin-malloc
-fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free`. With that in
place all six mutants are caught:

| mutant | caught by |
| --- | --- |
| class index off by one for one octave | `malloc_usable_size` under-reports; `realloc` loses content |
| span capacity one object too large | segfault, plus `free()` of a foreign pointer |
| `calloc` never zeroes | `calloc(1,16) byte 8 = 0xaa` |
| a class size not a multiple of 16 | alignment check and `usable_size` |
| `realloc` copies half the bytes | `realloc 101 -> 128 lost content` |
| objects overlap by 8 bytes | pattern mismatch within 81 iterations |

## Benchmarks

**Methodology.** Intel Core i9-9880H (8 cores, 16 threads, 2.30 GHz base),
Debian 13, glibc 2.41, gcc 14.2.0, `-O2`. `t/stress bench` times five
phases with `CLOCK_MONOTONIC`; the table is the **median of 7 runs**, with
the observed min and max as the spread. No pinning, no isolated cores, no
turbo control -- treat anything under 15% as noise. Reproduce with
`make bench` (which does 5 runs of each; the table used 7). Single-threaded
phases are one thread; the last is 4 threads on an 8-core machine.

| phase | glibc (ns/op) | slab (ns/op) | ratio |
| --- | --- | --- | --- |
| `small-churn` -- 8-256 B, 1024 live slots | 15.2 (14.6-20.4) | **14.0** (13.5-15.3) | 0.92x |
| `mixed-sizes` -- 70/20/8/2% size mix to 256 KiB | **41.1** (37.6-43.6) | 116.5 (106.9-134.8) | 2.8x slower |
| `realloc-grow` -- doubling 8 B → 64 KiB | **55.9** (50.2-63.5) | 4020 (3931-4158) | **72x slower** |
| `large-churn` -- 16 KiB-1 MiB, 64 live slots | **622** (554-700) | 4765 (4490-5002) | 7.7x slower |
| `4-thread-churn` -- 8-256 B, 4 threads | **5.3** (3.5-6.0) | 82.4 (67.1-89.3) | **16x slower** |

Footprint, 2 million simultaneous live allocations, peak RSS divided by
allocation count (includes an 8-byte index entry per allocation, so subtract
8 for the allocator's own cost):

| object size | glibc | slab | |
| --- | --- | --- | --- |
| 16 B | 40.7 B | **24.8 B** | **39% less** |
| 24 B | 40.7 B | 40.7 B | tie |
| 48 B | 72.7 B | **56.7 B** | 22% less |
| 100 B | 120.6 B | 120.9 B | tie |

### Where slab loses, and why

This is the honest part. slab loses four of five timing benchmarks, two of
them badly.

- **`realloc`-grow, 72x slower.** This is the worst result in the suite and
  it is entirely a missing feature. glibc grows a large block with
  `mremap`, which is a page-table edit -- no copying. slab does a fresh
  `mmap`, a full `memcpy` of the old contents, and a `munmap`, on every
  doubling. It does that because `mremap(MREMAP_MAYMOVE)` can return an
  address that is not 64 KiB-aligned, which would break the `AND`-mask
  header lookup the whole design rests on. The fix is to reserve a
  `PROT_NONE` tail per large mapping and `mprotect` it in, so growth never
  has to move; that is the third item in `PLAN.md`.
- **4-thread churn, 16x slower.** One global spinlock, no per-thread caches.
  glibc gets *faster* per operation with more threads (5.3 ns/op across 4
  threads versus 15.2 single-threaded) because per-thread tcaches mean the
  threads barely interact. slab gets 6x *slower* than its own
  single-threaded number, because every operation serializes and the threads
  spend their time ping-ponging one cache line. A global lock was a
  deliberate shortcut -- it is marked `shortcut:` in `slab.c` with the
  ceiling named -- but the measured cost of that shortcut is this row.
- **Large-block churn, 7.7x slower.** glibc keeps freed large blocks in its
  top chunk or its `mmap` cache and hands them straight back; slab calls
  `munmap` on every free and `mmap` on every allocation, so every block is
  faulted in from scratch. Three syscalls per allocation rather than one,
  too, because `map_aligned` over-maps and trims to get 64 KiB alignment.
- **Mixed sizes, 2.8x slower.** This is the previous row leaking into a
  general workload: only 2% of the allocations in that phase exceed 8 KiB,
  and they account for most of the time.
- **Small-object churn is a tie.** slab's median is 8% ahead but the ranges
  overlap, so the honest claim is "about the same", not a win. The pop-from-
  free-list fast path is genuinely short -- a lock, a load, a store, an
  unlock -- but so is glibc's tcache path, and glibc's does not take a lock.

The one unambiguous win is footprint for the smallest objects, and it comes
from the design decision at the top of `slab.c`: no per-object header. At 24
and 100 bytes it evaporates, because glibc's 32- and 112-byte chunk sizes
happen to land exactly on slab's class boundaries, so both allocators round
to the same number.

## Status

Working. Every dynamically linked program tried so far runs correctly under
it, including `gcc` compiling C and CPython running a JSON workload.

### What does not work

- **Returning memory to the OS.** Never happens. Spans are mapped once and
  kept forever. A program with a 500 MB transient peak of small objects
  holds 500 MB until it exits.
- **Multithreaded performance.** See the benchmark; 16x slower than glibc at
  4 threads. Correct, but a global lock.
- **`fork()` without `exec()` in a multithreaded program.** If a thread
  holds the spinlock when another thread forks, the child inherits a locked
  lock and hangs on its first allocation. There is no `pthread_atfork`
  handler. `fork`-then-`exec` is fine, which is why the shell pipeline tests
  pass.
- **Double free, invalid free, use after free.** Undetected. A double free
  puts the same block on the free list twice and a later `malloc` hands out
  a live block. There are no canaries, no guard pages, no free-list pointer
  mangling. Do not use this as a hardening measure; it is the opposite of
  one.
- **`free()` of a pointer slab did not allocate** is detected only when the
  `AND`-masked address does not carry our magic number. If it happens we
  print one line to stderr and leak the pointer rather than crash. If the
  masked address is unmapped, we segfault.
- **Non-4096-byte pages.** The page size is a compile-time constant. On a
  16 KiB-page arm64 kernel the trailing `munmap` in `map_aligned` would be
  rejected and large allocations would leak address space.
- **Statically linked programs**, and anything that calls libc's allocator
  through a symbol `LD_PRELOAD` cannot interpose.
- **Shrinking a large block** with `realloc` keeps the whole mapping
  resident: `realloc(p_1MB, 100)` still holds a megabyte.
- **`malloc_stats`, `malloc_trim`, `mallopt`** are accepted and ignored.
  `malloc_info`, `mcheck`, `mtrace` and the glibc hook variables are not
  provided at all.
- 32-bit, non-x86-64, and non-glibc C libraries are untested.

## Design

```
                    64 KiB
  +--------+--------+--------+--------+ ... 4 MiB arena, 64 KiB-aligned
  | span 0 | span 1 | span 2 | span 3 |
  +--------+--------+--------+--------+
      |
      v   offset 0        offset 64                      offset cap
      +---------------+----+----+----+----+-- ... --+----+--------+
      | struct hdr    | o0 | o1 | o2 | o3 |         | oN | unused |
      +---------------+----+----+----+----+-- ... --+----+--------+
                        ^-- all objects one size class, 16-aligned
```

**Span-based metadata.** Address space is taken from `mmap` in 4 MiB arenas,
forced to 64 KiB alignment by over-mapping and trimming. Each arena is
bump-carved into 64 KiB spans. A span belongs to one size class for its
whole life and holds a 64-byte header at offset 0; objects start at offset
64. Given any pointer, `p & ~0xffff` is its span header -- one instruction,
no per-object header, 0.1% metadata overhead. The alternative, a size field
in front of every allocation, costs 16 bytes per object once you account for
keeping the payload 16-byte aligned.

**32 size classes, 16 to 8192 bytes.** Exact 16-byte steps up to 128, then
four classes per power-of-two octave: 160/192/224/256, 320/384/448/512, and
so on to 8192. 16 bytes is `_Alignof(max_align_t)` on x86-64, so a finer
bottom step would be unrepresentable -- every pointer has to be 16-aligned
anyway, which makes the small classes exact for free. Four steps per octave
above that caps internal fragmentation at a fifth (a 2049-byte request lands
in the 2560 class) while keeping the table at 32 entries -- small enough that
the class index is a `clz` and a shift rather than a search, and small
enough to be a compile-time constant with no initialisation.

**Allocation.** Pop the class's free list; if empty, bump-allocate from the
class's current span; if that span is full, take a new span from the arena.
Free pushes onto the class's free list, threading the next pointer through
the first 8 bytes of the freed object. `calloc` skips the `memset` when the
object came from the bump pointer, because that memory is still the kernel's
zero page and spans are never recycled across classes.

**Large allocations** (> 8192 bytes) get their own `mmap`, 64 KiB-aligned,
with the same span header in front of the payload -- so `free` finds them by
the same `AND` mask, with no special case and no side table.

**Over-aligned allocations** also get their own `mmap`. For alignments below
64 KiB the payload is pushed up to the first aligned offset inside the first
span, so masking still reaches the header. For alignments of 64 KiB and
above the payload is itself span-aligned, which leaves no room for a header
below it -- so that becomes the signal. A pointer from any other path always
sits at least 64 bytes into its span, so `p == (p & ~0xffff)` is an exact
O(1) test for "the header is one span lower down". No side table, no search.

**No initialisation.** Every table is a compile-time constant or computed
arithmetically, so there is no "am I initialised yet" flag and therefore no
initialisation re-entrancy window -- the hazard that kills preloaded
allocators, where the first `malloc` arrives from inside the dynamic linker
or locale setup. For the same reason this file calls only `mmap`, `munmap`,
`memcpy`, `memset`, `write`, `strlen` and `sched_yield`; no stdio (a
`printf` would allocate), no `getenv`-driven configuration, no `__thread`
variables (a first dynamic-TLS access can reach `malloc` through
`__tls_get_addr`), no `pthread_once`, no `atexit`. Diagnostics go through
`write(2)`.

**Thread safety** is one global spinlock with a `pause` loop and a
`sched_yield` after 2000 spins. See the benchmark for what that costs, and
the `shortcut:` comment on it in `slab.c` for the upgrade path.

Deliberate shortcuts are marked `shortcut:` in `slab.c`, each naming the
ceiling it imposes and the upgrade path out of it. There are six.

## License

GPL-3.0. See `LICENSE`.
