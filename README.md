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
object. That is the footprint benchmark slab wins, and it wins it by 39%.

## The oracle

`make test` runs 31 checks in three tiers.

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

**Tier 3:** the oracle checked for teeth, by mutation. `make test` builds four
deliberately broken slabs -- objects that overlap by 8 bytes, a `calloc` that
never zeroes, a `realloc` that copies half the bytes, a class index one too
small for one octave -- and requires each to be rejected *by the check that
owns that behaviour*, not merely by some check somewhere. That distinction is
load-bearing; see below.

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
ok    self-check: 'stress stress 200000' rejects objects that overlap by 8 bytes
ok    self-check: 'stress check' rejects a calloc that never zeroes
ok    self-check: 'stress check' rejects a realloc that copies half the bytes
ok    self-check: 'stress check' rejects a class index one too small for one octave

32 passed, 0 failed
```

### What the oracle caught

Two bugs, and gcc caused both of them. Neither was an allocator logic error:
the byte-pattern stress test has never found slab writing outside a block.

**The loud one.** The small-object `calloc` was first written as `malloc()`
followed by `memset(0)`. gcc recognises exactly that idiom and rewrites it
into a call to `calloc` -- which, compiled into this file, is a PLT call back
into *our own* `calloc`. Unconditional infinite tail-recursion.
`/usr/bin/wc` hung, and `objdump -d libslab.so` showed `jmp calloc@plt`
sitting at the end of `calloc`. The fix is `-fno-builtin-malloc
-fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free` on `libslab.so`
itself: inside the implementation of these functions, the compiler's
knowledge of what their names mean is a liability. `calloc` also stopped
calling `malloc` at all, so the idiom is not there to be recognised.

**The quiet one, which is the more interesting half.** Mutation testing found
that a slab whose `calloc` **never zeroed a single byte** passed the targeted
`calloc` check. The check was there -- dirty a block with `0xaa`, free it,
`calloc` the same size, read every byte -- and gcc deleted the loop, because
it knows what `calloc` is declared to promise and proved the loop could not
observe a nonzero byte. The suite was testing the C standard, not slab. The
same argument applies to every alignment check, since gcc also knows `malloc`
returns `max_align_t`-aligned memory.

It took one more step to even see it. The random churn in `stress` mode *does*
catch that mutant, because there gcc cannot see through a pointer kept in a
struct behind a dynamic loop bound. So a tier 3 that accepted a rejection
from any mode would have printed "rejected", stayed green, and left
`check_calloc` as dead code forever. Pinning each mutant to the check that
owns the behaviour is what turned a green tick into a statement.

The fix is three-layered, because any single layer is a bet on compiler
behaviour and the whole lesson is that those bets lose silently: every pointer
is laundered through an `asm volatile` barrier with a memory clobber before it
is inspected, the `calloc` checks read through a `volatile` pointer, and the
oracle is compiled `-fno-builtin-malloc -fno-builtin-calloc
-fno-builtin-realloc -fno-builtin-free`.

The contrast is the lesson worth keeping. The compiler bug in the allocator
was found in minutes by running a real program. The compiler bug in the test
cost nothing, broke nothing, and would have sat there indefinitely looking
like evidence. Only deliberately breaking the allocator found it.

With the hardening in place all six mutants are caught:

| mutant | caught by |
| --- | --- |
| class index off by one for one octave | `malloc_usable_size` under-reports; `realloc` loses content |
| span capacity one object too large | segfault, plus `free()` of a foreign pointer |
| `calloc` never zeroes | `calloc(1,16) byte 8 = 0xaa` |
| a class size not a multiple of 16 | alignment check and `usable_size` |
| `realloc` copies half the bytes | `realloc 101 -> 128 lost content` |
| objects overlap by 8 bytes | pattern mismatch within 81 iterations |

**Five** are permanent fixtures of `make test` (tier 3) and the suite prints
one `self-check:` line per mutant, so the count is in the output:
`objects overlap by 8 bytes`, `calloc never zeroes`, `realloc copies half the
bytes`, `a class index one too small for one octave`, and the hand-built
`magazines with no adoption`, which is checked by RSS rather than by
`stress check` and so is not in the table above. This said "four of the six"
and counted only the four `mutant` calls, missing the no-adoption one.

## Benchmarks

**Methodology.** Intel Core i9-9880H (8 cores, 16 threads, 2.30 GHz base),
Debian 13, glibc 2.41, gcc 14.2.0, `-O2`. `t/stress bench` times five
phases with `CLOCK_MONOTONIC`; the table is the **median of 7 runs**, with
the observed min and max as the spread. No pinning, no isolated cores, no
turbo control -- treat anything under 15% as noise. Reproduce with
`make bench` (which does 5 runs of each; the table used 7). Single-threaded
phases are one thread; the last is 4 threads on an 8-core machine.

All three slab columns come from one sitting, the "one lock" one being
`slab.c` at the commit before per-thread magazines, rebuilt with the same
flags. Comparing a new number against a figure measured on a different day is
how a change gets credit for the weather.

| phase | glibc (ns/op) | one lock | magazines | + span cache | ratio now |
| --- | --- | --- | --- | --- | --- |
| `small-churn` -- 8-256 B, 1024 live slots | 16.0 | 14.1 | 13.5 | **13.6** | 0.85x |
| `mixed-sizes` -- 70/20/8/2% size mix to 256 KiB | **38.7** | 119.7 | 103.4 | 50.6 | 1.3x slower |
| `realloc-grow` -- doubling 8 B to 64 KiB | **52.7** | 4276 | 3312 | 159 | 3.0x slower |
| `large-churn` -- 16 KiB-1 MiB, 64 live slots | **582** | 4765 | 4684 | 2279 | 3.9x slower |
| `4-thread-churn` -- 8-256 B, 4 threads | 3.9 | 5.2 | 3.5 | **3.5** | 0.90x |

Three columns of slab, all measured in one sitting, each one the commit before
the next. The magazines moved exactly one row, which is what should happen:
they take the lock off the small-object path and nothing else was waiting on
the lock. The span cache moved three, because what those three had in common
was syscalls.

`realloc-grow` is the one to look at: 4276 ns/op to 159, a factor of 27,
without touching `realloc` at all. Every doubling was an `mmap`, a `memcpy` and
an `munmap`, and the `mmap` was the expensive part -- a fresh mapping is faulted
in from zero one page at a time. Handing back a mapping that is already
resident removes the faults, and the copy that remains is what the 3.0x is.

### The span cache

Freed large mappings are kept instead of unmapped, and handed back to the next
request they fit. Sixteen of them, bounded at 64 MiB total rather than by
count, because sixteen mappings of a megabyte is a very different thing from
sixteen of a page and bytes are what a program notices.

Matched **best-fit within 25%**, and that number was measured rather than
chosen. Exact-length matching was tried first and is the obvious design: large
requests are page-rounded so lengths cluster. It moved `realloc-grow` from
4276 to 3312 and `large-churn` from 4765 to 4684 -- which is to say it did
almost nothing for the row it was aimed at, because that benchmark picks random
sizes between 16 KiB and 1 MiB and page-rounding leaves roughly 250 distinct
lengths for a sixteen-entry cache to miss. Allowing up to 25% slack took the
same row to 2279. The waste is bounded by that 25% and shows up as 1.10x glibc
peak RSS on the footprint test, against a 3.0x limit.

A recycled mapping is **dirty**, and that is the one thing here that is a
correctness problem rather than a performance one. `calloc` used to be
`return malloc(size)` for anything over 8 KiB, with the comment "fresh mmap:
already zero" -- true until this cache existed, and a disclosure bug the moment
it did. It now asks whether the mapping was fresh and `memset`s when it was
not. Removing that check makes `calloc(1, 8193)` return the previous owner's
bytes, and the targeted `calloc` check in the oracle says so.

Only the plain-large path is cached. The over-aligned path puts its header a
whole span below a payload that is itself span-aligned, so a mapping from one
is not interchangeable with a request for the other; the header records which
kind it is rather than leaving that to be re-derived at free time.

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

- **`realloc`-grow, 3.0x slower**, down from 81x. What is left is the `memcpy`:
  glibc grows a large block with `mremap`, a page-table edit that copies
  nothing. slab still allocates, copies and frees on every doubling, it just no
  longer pays for a fresh mapping each time. `mremap(MREMAP_MAYMOVE)` can
  return an address that is not 64 KiB-aligned, which would break the
  `AND`-mask header lookup the whole design rests on, so the fix is a
  `PROT_NONE` tail per large mapping with `mprotect` to grow into it. Still in
  `PLAN.md`, now worth much less than it was.
- **4-thread churn is a tie now, and was the worst row in the table.** Under
  one global lock it ran at 74.8 ns/op against glibc's 3.6 -- 21x, and 5x
  slower than slab's *own* single-threaded number, because every malloc and
  every free serialized on one cache line and the threads spent their time
  ping-ponging it. Per-class, per-thread magazines take the lock off that path
  and the row is 4.4 against 3.6. The ranges overlap (4.2-6.3 against
  3.4-5.2), so the honest claim is "about the same as glibc" rather than a
  win. The 21x is gone either way.
- **Large-block churn, 3.9x slower**, down from 8.2x. The cache covers the
  repeat cases; what remains is a cache miss, which still costs three syscalls
  rather than one because `map_aligned` over-maps and trims to get 64 KiB
  alignment. Reserving one span-aligned region up front and carving from it
  with `MAP_FIXED` is the `PLAN.md` item that removes those two trimming
  `munmap`s.
- **Mixed sizes, 1.3x slower**, down from 3.1x. It was the previous two rows
  leaking into a general workload -- only 2% of the allocations in that phase
  exceed 8 KiB and they accounted for most of the time, which is why fixing
  the large path moved this row by more than the small path ever did.
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
- **Objects stranded on a dead thread's magazine come back late, not never.**
  A thread that exits leaves its magazines in the registry holding up to 8 KiB
  per size class, and that memory is unavailable until some later thread
  claims that set. The cost is therefore bounded by peak thread count rather
  than by threads created: the thread-churn test holds peak RSS at 1.00x glibc
  across a thousand sequentially created threads, and removing the adoption
  takes that same measurement to 17x. A program whose thread count peaks high
  and then settles low keeps the high-water mark's worth of magazines.
- **Nothing of ours runs when a thread exits.** The usual mechanism is a
  `pthread_key_create` destructor, not used here because
  `pthread_setspecific` allocates for key indices past the first level and
  would call back into us in the middle of our own bookkeeping. Adoption
  tested with `tgkill(tid, 0)` is the substitute, and it is lazy by nature:
  memory returns when another thread asks for magazines, not when the owner
  dies.
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
- **Building `libslab.so` without `-fno-builtin-malloc`/`-calloc`/`-realloc`/
  `-free`** produces an allocator that can recurse into itself. The flags are
  in the `Makefile` with a comment; they are not optional.
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

**Thread safety** is per-class, per-thread magazines over one global
spinlock. A magazine is a free list threaded through the free objects, the
same trick the global lists use, so it costs one pointer and one count per
class and no metadata per object at all. `malloc` pops it and `free` pushes it
with no lock; the lock appears only to refill an empty magazine or drain a
full one, in batches of up to 4 KiB worth of objects, and for new spans and
large allocations. The spinlock under all that is a `pause` loop with a
`sched_yield` after 2000 spins.

The single `__thread` variable is an eight-byte pointer declared
`tls_model("initial-exec")`, the exception to the no-TLS rule above: that
model resolves the offset at load time, so the access is one `%fs`-relative
load with no call and nothing lazy to allocate. It is only valid for an object
present at program start, which is how this one is loaded. A `dlopen`ed slab
would be refused by the loader for want of static TLS surplus, which is a far
louder failure than the recursion the model avoids.

The magazines themselves sit in our own `mmap`ed memory rather than in TLS,
and a registry of them outlives every thread -- which is what makes it
possible to hand a dead thread's magazines to a live one instead of losing the
objects on them. See "What does not work" for where that stops.

Deliberate shortcuts are marked `shortcut:` in `slab.c`, each naming the
ceiling it imposes and the upgrade path out of it. There are seven.

## The suite was green on a library that interposes nothing

Everything in tiers 1 and 2 is satisfied by a shared object that loads
cleanly and provides no `malloc` at all. Verified by building one --
`gcc -shared -fPIC` over a file containing a single unused variable, copied
over `libslab.so`:

```
31 passed, 0 failed
```

The whole suite, green, on a functionless library. The mechanism is that
every tier-1 check compares a command's output **with and without**
`LD_PRELOAD` and requires them to match, tier 2 runs `t/stress` under
preload and requires exit 0, and the suite separately requires both to also
pass against glibc malloc -- because an oracle glibc fails is a wrong
oracle, which is correct reasoning that happens to make "nothing was
replaced" indistinguishable from "the replacement works". The RSS ratios
came out near 1.00 against limits of 3.0 and 1.5, the munmap counts matched
because both sides were glibc, and tier 3's mutants are compiled from
`slab.c` rather than from the `.so`, so they could not notice either.

The suite now starts by asking whether `LD_PRELOAD` changed anything:

```
ok    interposition: LD_PRELOAD replaces malloc (usable(malloc(1)) 24 -> 16)
```

`malloc_usable_size` is the cheapest observable that separates them --
slab's header is 16 bytes for a 1-byte request where this glibc's is 24 --
and the two values are **compared rather than pinned**, so the check
survives a libc whose own answer differs. With the empty library in place it
now reads:

```
FAIL  interposition: LD_PRELOAD changes nothing
      malloc_usable_size(malloc(1)) is 24 either way -- libslab.so is not
      providing malloc, and every check below passes anyway
```

## License

GPL-3.0. See `LICENSE`.
