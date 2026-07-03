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
So this is a real allocator: `mmap`, size classes, free lists, direct `mmap`
for large blocks, written small enough that the interesting part is the
oracle, not the code.

It is also an answer to a narrower question. How much does a per-object
header cost? glibc puts 8 to 16 bytes of chunk metadata in front of every
allocation. slab puts 64 bytes at the base of each 64 KiB span instead and
finds it with one `AND`, which is 0.1% overhead instead of 100% on a 16-byte
object. That is the footprint benchmark slab wins, and it wins it by 39%.

## The oracle

`make test` runs 47 checks in three tiers.

Tier 1, and the whole point: real system binaries, run twice, once
normally, once with `LD_PRELOAD=./libslab.so`, demanding that stdout,
stderr and the exit status are *byte-identical*. `ls -la /usr/bin`,
`ls -R /usr/include`, `cat` and `sort` and `wc` on a 3.4 MB generated file,
`grep -r` across `/usr/include`, a `cut | sort | uniq -c | sort -rn` shell
pipeline, `python3` doing a 40 000-object JSON round trip, a 300 000-key
`Counter`, and 20 000 rounds of string concatenation, and `gcc -O2 -S`
compiling the test program itself. Any diagnostic from slab on stderr also
fails the check, so a path we do not handle cannot pass quietly.

Tier 2: the things output comparison cannot see: alignment of every size
from 0 to 4096, `posix_memalign` from 8 bytes to 4 MiB of alignment,
`calloc` zeroing *verified after dirtying and freeing the same block*,
`realloc` content preservation across every class boundary in both
directions, `free(NULL)`, `malloc(0)`, and a byte-pattern stress test: 3
million random operations over 2048 live slots, each block filled with a
pattern derived from a unique tag and verified on every touch. Plus a direct
assault on the hard invariant: 30 000 simultaneous live blocks, sorted by
address, asserting that none overlaps the next, and the same stress on four
threads with blocks handed between them so cross-thread `free` is exercised.
The stress and semantic tests also run against glibc `malloc`, because if
they fail there then the test is wrong, not slab.

Two tier-2 checks are measurements rather than assertions about output.
`t/stress reclaim` allocates 200 MB of 56-byte objects, dirties every byte,
frees all of it, and reads resident size out of `/proc/self/statm` before,
at the peak and after. slab gives back 94% of the peak. It also requires
20 000 `calloc`s out of the re-armed spans to come back zero, which is the
disclosure half of returning memory: the first page of a span cannot be
given back, because the header is in it. And peak RSS of the byte-pattern
stress run is held under 1.2x glibc, which is the only check that notices a
span leaking.

One tier-2 check is slab-specific and says so: `t/stress guard` asserts that
`realloc` grows a large block *without moving it* (the same pointer comes
back), that the kernel reports the reserved tail as `---p` in
`/proc/self/maps`, that writing one byte past the end of a block kills a
forked child with `SIGSEGV`, and that a grown block returns to its original
size when freed. glibc fails all eight of its assertions, and the suite
asserts that too, because a guard assertion glibc also satisfied would be
testing the C library rather than the reservation.

Tier 3: the oracle checked for teeth, by mutation. `make test` builds
fifteen deliberately broken slabs: objects that overlap by 8 bytes, a
`calloc` that never zeroes, a `realloc` that copies half the bytes, a class
index one too small for one octave, magazines with no adoption, a `realloc`
that moves a large block instead of growing it, a reserved tail mapped
writable instead of `PROT_NONE`, a reservation that is never taken, a grown
block that keeps its grown size when freed, a shrink that drops the pages
but leaves the tail writable, a wholly free span that is never given back, a
reclaim that re-arms a span without the `madvise`, a reclaim that leaves the
span's first page dirty, a displaced spare span reclaimed without checking
it is still empty, and a span that is never relinked when it stops being
full. Each must be rejected *by the check that owns that behaviour*, not
merely by some check somewhere. That distinction is load-bearing; see below.

Two things in `slab.c` cannot be mutation-tested and the README says so
rather than leaving the impression that everything is covered: removing the
one-span hysteresis in `free_small` and removing the `RESERVE_MIN` floor are
both pure slowdowns, 23% and 4x on one benchmark row each, and the suite
asserts no timings.

The mutation harness needed hardening to be worth anything, and the bug was
found by being fooled by it. `|` is both sed's delimiter and a C operator,
so the expression for "a reserved tail mapped writable" (which has to write
`PROT_READ | PROT_WRITE`) made sed exit 1 and leave an EMPTY mutant source.
An empty file compiles into a shared object with no `malloc` in it, `cmp -s`
confirms it differs from `slab.c`, and every mode duly "rejects" it. The
harness now checks sed's exit status and requires the built mutant to change
`malloc_usable_size(malloc(1))`, and both halves of that have been watched
to fail.

```
$ make test
ok    interposition: LD_PRELOAD replaces malloc (usable(malloc(1)) 24 -> 16)
ok    ls -la /usr/bin (86606 bytes identical)
ok    ls -R /usr/include (101762 bytes identical)
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
ok    guard: large blocks grow in place, and the tail faults
ok    guard: glibc fails 8 of its assertions, so none of them is free
ok    stress: 3M random ops, byte pattern verified
ok    stress: 4 threads, 400k ops each, cross-thread free
ok    oracle agrees with glibc malloc
ok    peak RSS 1.09x glibc on a churn workload (limit 3.0x)
ok    span reclaim: RSS 31028 -> 265316 -> 45776 KiB, 204800 KiB requested, 94% of the peak given back (glibc: 0% of the peak given back)
ok    peak RSS 0.50x glibc on the byte-pattern stress (limit 1.2x)
ok    stress: 1000 sequential threads, nothing live at the end
ok    peak RSS 1.00x glibc after 1000 dead threads (limit 1.5x)
ok    span cache: 200 large cycles cost 3 munmaps (glibc 2)
ok    grow loop: 200 doubling chains cost 16 mmap+munmap (glibc 11)
ok    self-check: 'stress stress 200000' rejects objects that overlap by 8 bytes
ok    self-check: 'stress check' rejects a calloc that never zeroes
ok    self-check: 'stress check' rejects a realloc that copies half the bytes
ok    self-check: 'stress check' rejects a class index one too small for one octave
ok    self-check: 'stress guard' rejects a realloc that moves a large block instead of growing it
ok    self-check: 'stress guard' rejects a reserved tail that is mapped writable instead of PROT_NONE
ok    self-check: 'stress guard' rejects a reservation that is never taken
ok    self-check: 'stress guard' rejects a grown block that keeps its grown size when freed
ok    self-check: 'stress guard' rejects a shrink that drops the pages but leaves the tail writable
ok    self-check: 'stress reclaim 200' rejects a wholly free span that is never given back
ok    self-check: 'stress reclaim 200' rejects a reclaim that re-arms the span without the madvise
ok    self-check: 'stress reclaim 200' rejects a reclaim that leaves the span's first page dirty
ok    self-check: 'stress stress 200000' rejects a displaced spare reclaimed without checking it is still empty
ok    self-check: peak RSS rejects magazines with no adoption (10.56x, limit 1.5x)
ok    self-check: peak RSS rejects a span that is never relinked when it stops being full (1.64x, limit 1.2x)

47 passed, 0 failed, 0 skipped
```

`malloc_usable_size` is the cheapest observable that separates them,
slab's header is 16 bytes for a 1-byte request where this glibc's is 24,
and the two values are **compared, not pinned**, so the check
survives a libc whose own answer differs. With the empty library in place it
now reads:

```
FAIL  interposition: LD_PRELOAD changes nothing
      malloc_usable_size(malloc(1)) is 24 either way -- libslab.so is not
      providing malloc, and every check below passes anyway
```

## Performance

Six synthetic workloads in `t/stress bench`, nine repetitions each, medians
with the full spread in brackets, ns per allocator operation. Lower is
better. The external baselines are glibc 2.x `malloc`, jemalloc 5 and
tcmalloc (gperftools) `libtcmalloc_minimal`, all three preloaded into the
same binary on the same machine in the same sitting.

```
                        glibc              jemalloc             tcmalloc              slab
small-churn        13.5 [ 12.9-  14.5]  10.0 [  9.7-  14.9]   9.1 [  8.3-  10.5]   13.1 [  12.4-  13.5]
mixed-sizes        35.8 [ 34.0-  41.1]  24.5 [ 22.8-  29.1]  18.2 [ 17.5-  19.7]   46.7 [  45.0-  59.2]
realloc-grow       50.0 [ 46.1-  69.3] 128.6 [121.2- 158.3] 102.4 [ 97.2- 117.9]  149.9 [ 139.6- 169.3]
large-grow          807 [  798-   957] 13911 [12506- 18829] 15750 [14760- 16858]   2398 [  2223-  3103]
large-churn         498 [  475-   554]   412 [  378-   592]   237 [  213-   297]   2353 [  2215-  3006]
4-thread-churn      4.1 [  3.6-   5.5]   3.0 [  2.6-   4.9]   3.4 [  2.4-   5.0]    4.7 [   3.3-   5.6]
```

Against glibc, slab loses four rows and takes two: 1.30x on mixed sizes,
3.00x on short realloc chains, 2.97x on large-grow and 4.72x on large-block
churn, against 0.97x on small churn (its whole spread sits below glibc's
median) and 1.15x on four threads, which is inside the spread. Against
jemalloc and tcmalloc it loses five of six, and the worst of those is
large-block churn again, 5.7x and 9.9x. Those are the standings and nothing
here argues them away; a 1000-line allocator with one global lock and a
16-entry mapping cache is not going to beat three mature ones.

The row it takes outright is `large-grow`, a buffer grown from 16 KiB to
4 MiB by doubling, where slab is 5.8x FASTER than jemalloc and 6.6x faster
than tcmalloc. It is also the row that moved: 216x glibc before the reserved
tail described below, and 2.97x after. Measured against the commit before it
in one interleaved run of ten repetitions each, 207606 ns/op to 2818, a
factor of 74, while the other five moved by 1.01x to 1.06x.

### Growing a large block without copying

A large block is one `mmap`. Doubling it used to mean a second `mmap`, a
`memcpy` and an `munmap`, and the `memcpy` is the cheap part: the
destination has to be faulted in a page at a time, so the 16 KiB-to-4 MiB
chain above faulted about 2048 pages and copied 8 MiB per round. glibc calls
`mremap` and the kernel edits a page table instead.

`mremap` is not available here. `MREMAP_MAYMOVE` can return an address that
is not 64 KiB-aligned, and the AND-mask header lookup depends on that
alignment; without `MREMAP_MAYMOVE` it fails whenever the next page is
taken. So slab reserves the space up front: a large mapping is made 32 times
the size it needs, with everything past the part in use mapped `PROT_NONE`,
and growth is one `mprotect`.

`PROT_NONE` is not decoration. A private anonymous mapping is only charged
against overcommit if it is writable, so the reservation costs no commit and
no RSS, and anything that runs off the end of a large block now hits a page
with no permissions instead of the up-to-4095 bytes of page-rounding slack
it used to find there.

What it costs is address space, and the number is large: on a workload
holding 64 large blocks of 16 KiB to 1 MiB, `VmPeak` goes from 34 MB to 996
MB, a factor of 29, while `VmHWM` (peak resident) is 1648 kB either way.
Each large block is also two mappings instead of one, so a process holding
more than about 32000 of them at once approaches the default
`vm.max_map_count`.

Two things had to be tuned by measurement rather than reasoning, and the
first one caught a 5x regression that no test in the suite could see.

`RESERVE_MIN` is 128 KiB: below that, no tail is reserved and the mapping is
made read-write in a single syscall, exactly as before. The `mprotect` pair
is not free. With no floor at all, `realloc-grow` (a chain that stops at
64 KiB) went from 151 ns/op to between 530 and 690, because at those sizes
the copy comes out of a span-cache mapping whose pages are already warm and
costs less than the syscalls do. Measured floors: 1 gives 638 ns/op,
16 KiB gives 677, 32 KiB gives 533, 64 KiB gives 155, 128 KiB gives 151.

`RESERVE_K` is 31, so a mapping is 32 times the size it was asked for and a
buffer can double five times before it has to move. The `large-grow` number
is very sensitive to this and should be read with care: that benchmark's
chain spans exactly 32x from the first reserved block, so K=31 is the
smallest value that makes it copy-free and anything smaller lands on the
wrong side of a cliff (2.3 us/op at K=31 and K=63, 62 us at K=15, 120 us at
K=7). The general law is the one to trust: one copy every log2(K+1)
doublings. Pushed past the span cache's reach the picture is noisier, and
these are the measurements rather than an argument: a chain to 16 MiB
improves 1.3x over the previous commit and a chain to 64 MiB improves 19x.

### Giving a transient peak back

Spans used to be mapped once and kept forever, so a program that allocated
500 MB of small objects and freed them stayed at 500 MB. A span whose last
object has been returned is now `madvise(MADV_DONTNEED)`'d. The mapping
stays, because the AND-mask header lookup depends on it; the pages go, and
the next touch faults in a fresh zero page.

Measured from inside the process, resident size read out of
`/proc/self/statm` before allocating 200 MB of 56-byte objects, at the peak,
and after freeing all of them (56 and not 64 so that glibc's 64-byte chunk
and slab's 64-byte class carry the same per-object overhead and the
comparison is about reclaiming rather than headers):

```
slab                     31004 -> 265292 -> 45692 KiB     94% given back
glibc                    30932 -> 264988 -> 264988 KiB      0%
jemalloc 5               32196 -> 274520 -> 274524 KiB      0%
tcmalloc_minimal         36712 -> 272204 -> 272204 KiB      0%

glibc, then malloc_trim(0)           264992 -> 30940 KiB
jemalloc, dirty_decay_ms:0           274580 -> 40532 KiB
jemalloc, then sleep(16)             274568 -> 274636 KiB
```

The difference is when, not whether. glibc hands it back the moment you ask
with `malloc_trim(0)`, and jemalloc does it on its decay schedule, which
with the default `dirty_decay_ms` and no background thread is driven by
allocator activity rather than by a clock -- hence the sixteen seconds of
sleeping that changed nothing. slab does it on the free, with nothing to
configure, which is why its own `malloc_trim` is still a no-op that returns
0.

It is not free of consequences and two of them are worth stating. 6.25% of
every span stays resident: `madvise` is page-granular and the span header is
at offset 0, so the first page cannot go, and the objects living in it are
zeroed by hand instead -- without that, re-arming the bump pointer would
hand out dirty memory while telling `calloc` the span was untouched zero
page. And one empty span per class is kept back rather than reclaimed,
because without that the `mixed-sizes` row lost 23%: a span of the 8192
class holds seven objects, so a drifting workload emptied and refilled spans
constantly, and 520 extra `madvise` calls over a benchmark run cost about
10 ns on every operation in that row. The syscall count is not the problem;
each one flushes TLBs across every CPU the process has touched and then the
pages have to be faulted back in. Keeping one span back took the `madvise`
count from 624 to 125 and the row back to where it was.

Knowing a span is wholly free needed the small path rewritten. With one free
list per class threaded across every span, finding out means walking the
whole list, so each span now owns its free list and a live count and each
class keeps a list of the spans with room. That change alone, measured with
the `madvise` disabled, is 0.96x to 1.03x on the six rows, and the whole
commit is 0.94x to 1.02x.

### The leak that only a footprint number could see

A span is unlinked from its class when it fills. A version that never
relinked it when an object came back would leak every span it had ever used
-- and corrupt nothing, exit 0, pass the reclaim check above, and pass the
existing Python churn RSS ratio at 1.09x, which is exactly what the correct
code measures. The check that separates them is peak RSS of the byte-pattern
stress run: 0.51x glibc for the real thing against 1.67x for that mutant.
It is in the suite now, with that mutant driving it, and it was added
because the mutant survived everything else.

### The bug the reservation introduced

A grown mapping has to go back to its natural size when it is freed, and the
first version did not do that. The span cache matches a freed mapping on its
size, so a buffer malloc'd at 16 KiB and doubled to 64 KiB went into the
cache as a 64 KiB mapping, the next round asked for 16 KiB, nothing matched,
and every round paid a fresh `mmap`. Measured on 200 rounds of the
`realloc-grow` loop: 17 `mmap`s and 9 `munmap`s before the reservation
existed, 214 and 587 with it, and 17 and 9 again once `free` restored the
size. `make test` holds that number down with `strace`, because it is a
claim about syscalls and nothing in a program's output can see it.

Restoring the size needs both an `madvise(MADV_DONTNEED)` and an `mprotect`,
and neither is enough alone. `mprotect(PROT_NONE)` takes the access away but
leaves the pages resident and holding the last owner's bytes; `madvise`
drops them but leaves the tail writable, so the guard would stop at the
original end of any block that had ever grown. There is a mutant for each
half.

## Sanitizers, and what they cannot reach

Honestly: **slab itself cannot run under AddressSanitizer.** ASan requires
its runtime to come first in the initial library list, and once it is there
its own `malloc` interposes ahead of ours: `t/stress interposed` prints 1
rather than slab's 16, so the suite passes while testing ASan's allocator.
That is the "green suite over a library that does nothing" failure this
repo already has a check for, and it is why no ASan number is claimed here.

What did run:

- **UBSan against slab.** `libslab.so` built with `-fsanitize=undefined
  -fno-sanitize-recover=all` and preloaded behind `libubsan.so`, which does
  not replace `malloc`. `interposed` prints 16, so slab is serving. Clean on
  all seven modes -- `check`, `guard`, `reclaim 200`, `stress 500000`,
  `thread 100000`, `tchurn 200` -- and clean under `ls -la /usr/bin`,
  `sort /etc/services`, a CPython JSON workload and `gcc -O2 -S`.
- **ASan and UBSan against the oracle.** `t/stress` built with
  `-fsanitize=address,undefined` and run against the system allocator, which
  is what ASan can see. Clean on `stress`, `thread` and `tchurn`. `check`
  needs `ASAN_OPTIONS=allocator_may_return_null=1`, because it deliberately
  calls `calloc(SIZE_MAX/2, 4)` and ASan treats an overflowing `calloc` as a
  fatal error rather than returning NULL. `guard` is not runnable there at
  all: it deliberately writes one byte past the end of a block, and ASan
  reports that write as a heap-buffer-overflow and aborts, which is
  independent confirmation that the byte `guard` expects to fault on really
  is out of bounds. `reclaim` fails there, because ASan's allocator returns
  nothing either -- and that run earned its keep: ASan's quarantine grows
  while the frees happen, so resident size ended ABOVE the peak reading and
  the "percentage given back" line underflowed to 18446744073709551608%.
  That was a bug in the oracle, found by running the oracle somewhere it was
  not expected to pass.

## License

GPL-3.0. See `LICENSE`.
