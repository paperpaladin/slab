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

What that costs instead is internal fragmentation, because an object is
rounded to one of 32 size classes with nothing recording the request. That
used to be an argument about the table in `slab.c`; it is now measured over
every request size from 1 to 8192, and the worst case is 4097 bytes landing
in the 5120 class, 19.98% of the allocation wasted. Below 129 bytes the
classes are exact 16-byte steps and the worst slack is 15 bytes. See
[Fragmentation](#fragmentation).

## The oracle

`make test` runs 76 checks in three tiers, plus a recorded corpus.

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

Three tier-2 checks are measurements rather than assertions about output.
`t/stress frag` is described under [Fragmentation](#fragmentation).
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

Between tiers 2 and 3 sits the recorded corpus, which is described under
[Real programs, not our guess](#real-programs-not-our-guess): a trace of
every allocator call `gcc` and CPython make, replayed against slab and
against glibc, with the replay required to reproduce the trace exactly
before any timing from it is believed.

Tier 3: the oracle checked for teeth, by mutation. `make test` builds
twenty-nine deliberately broken slabs: objects that overlap by 8 bytes, a
`calloc` that never zeroes, a `realloc` that copies half the bytes, a class
index one too small for one octave, magazines with no adoption, a `realloc`
that moves a large block instead of growing it, a reserved tail mapped
writable instead of `PROT_NONE`, a reservation that is never taken, a grown
block that keeps its grown size when freed, a shrink that drops the pages
but leaves the tail writable, a wholly free span that is never given back, a
reclaim that re-arms a span without the `madvise`, a reclaim that leaves the
span's first page dirty, a displaced spare span reclaimed without checking
it is still empty, a span that is never relinked when it stops being full, a
size class widened until one octave wastes a third, a `malloc_stats` that
does not subtract the magazines, arenas that are never recorded so the dump
sees no spans, arenas that never ask for huge pages, arenas aligned to a
span instead of a huge page, magazines reclaimed only on adoption, a
sweep that drops a dead thread's magazines instead of returning them, no
`pthread_atfork` handlers at all, a prepare handler that does not take the
lock, a child that does not re-stamp its magazine set, and a
page size that is not the kernel's, a `realloc` down that keeps the whole
mapping resident, a trim that drops the pages but leaves them writable, and
a trimmed block that never goes back to its mapped size. Each must be rejected *by the check that owns that
behaviour*, not merely by some check somewhere. That distinction is
load-bearing; see below.

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
ok    ls -la /usr/bin (91690 bytes identical)
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
ok    peak RSS 1.12x glibc on a churn workload (limit 3.0x)
ok    fork: a multithreaded process forks and the child allocates
ok    fork: glibc malloc survives it too, so the oracle is right
ok    magazine sweep: 64 dead threads held 11087 KiB on their magazines, 7 KiB left after the next arena was wanted
ok    huge pages: AnonHugePages 2048 -> 26624 KiB over 25000 KiB of 64-byte objects
ok    fragmentation: 15 bytes of slack at or below 128, and 4097 bytes requested in a 5120-byte class = 19.98% of the allocation wasted
ok    fragmentation: 20000 objects, 81251 KiB requested, 88012 KiB in classes (7.68% of the allocation wasted), 1453 spans holding 92992 KiB (94% occupied by live objects)
ok    geometry: page=4096, span=65536 (sysconf says 4096, AT_PAGESZ says 4096)
ok    fragmentation: glibc fails 9 of its assertions, so they are about slab's own table and geometry
ok    span reclaim: RSS 32972 -> 266444 -> 46904 KiB, 204800 KiB requested, 95% of the peak given back (glibc: 0% of the peak given back)
ok    peak RSS 0.60x glibc on the byte-pattern stress (limit 1.2x)
ok    stress: 1000 sequential threads, nothing live at the end
ok    peak RSS 1.00x glibc after 1000 dead threads (limit 1.5x)
ok    span cache: 200 large cycles cost 2 munmaps (glibc 2)
ok    grow loop: 200 doubling chains cost 16 mmap+munmap (glibc 11)
ok    trace gcc: 836300 replayable ops, 11157 slots (peak live blocks), 407649 KiB requested
ok    replay gcc under slab: 2508900 ops in 0.130s = 51.9 ns/op (3 reps, 11157 slots) vs glibc 25.5 ns/op (3 reps, 11157 slots)
ok    replay gcc under glibc reproduces it too, so the trace is sound
ok    trace py: 20083 replayable ops, 6305 slots (peak live blocks), 97208 KiB requested
ok    replay py under slab: 60249 ops in 0.013s = 218.5 ns/op (3 reps, 6305 slots) vs glibc 306.8 ns/op (3 reps, 6305 slots)
ok    replay py under glibc reproduces it too, so the trace is sound
ok    self-check: the replay rejects itself when it skips one op in sixteen
ok    self-check: 'stress stress 200000' rejects objects that overlap by 8 bytes
ok    self-check: 'stress check' rejects a calloc that never zeroes
ok    self-check: 'stress check' rejects a realloc that copies half the bytes
ok    self-check: 'stress check' rejects a class index one too small for one octave
ok    self-check: 'stress guard' rejects a realloc that moves a large block instead of growing it
ok    self-check: 'stress guard' rejects a reserved tail that is mapped writable instead of PROT_NONE
ok    self-check: 'stress guard' rejects a reservation that is never taken
ok    self-check: 'stress guard' rejects a grown block that keeps its grown size when freed
ok    self-check: 'stress guard' rejects a restore that drops the pages but leaves the tail writable
ok    self-check: 'stress guard' rejects a realloc down that keeps the whole mapping resident
ok    self-check: 'stress guard' rejects a trim that drops the pages but leaves them writable
ok    self-check: 'stress guard' rejects a trimmed block that never goes back to its mapped size
ok    self-check: 'stress reclaim 200' rejects a wholly free span that is never given back
ok    self-check: 'stress reclaim 200' rejects a reclaim that re-arms the span without the madvise
ok    self-check: 'stress reclaim 200' rejects a reclaim that leaves the span's first page dirty
ok    self-check: 'stress fork 20' rejects no pthread_atfork handlers at all
ok    self-check: 'stress fork 50' rejects a prepare handler that does not take the lock
ok    self-check: 'stress fork 20' rejects a child that does not re-stamp its magazine set
ok    self-check: 'stress sweep' rejects magazines reclaimed only on adoption
ok    self-check: 'stress sweep' rejects a sweep that drops the magazines instead of returning them
ok    self-check: 'stress huge' rejects arenas that never ask for huge pages
ok    self-check: 'stress huge' rejects arenas aligned to a span instead of a huge page
ok    self-check: 'stress frag' rejects a page size that is not the kernel's
ok    self-check: 'stress frag' rejects a class that wastes more than a fifth of its allocation
ok    self-check: 'stress frag' rejects a dump that does not account for the magazines
ok    self-check: 'stress frag' rejects arenas that are never recorded, so the dump sees no spans
ok    self-check: 'stress stress 200000' rejects a displaced spare reclaimed without checking it is still empty
ok    self-check: peak RSS rejects magazines with no adoption (10.96x, limit 1.5x)
ok    self-check: peak RSS rejects a span that is never relinked when it stops being full (1.84x, limit 1.2x)

76 passed, 0 failed, 0 skipped
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

These six are a guess at how programs allocate, and
[the recorded traces below](#real-programs-not-our-guess) show the guess is
wrong in both directions. Read that section before taking any row here as a
statement about real software.

```
                        glibc              jemalloc             tcmalloc              slab
small-churn        14.0 [ 13.2-  15.5]  10.6 [  9.6-  15.0]   9.1 [  8.8-  12.5]   13.3 [  12.7-  19.8]
mixed-sizes        35.9 [ 34.9-  39.6]  25.0 [ 24.1-  31.1]  18.4 [ 17.6-  20.2]   46.8 [  42.5-  59.5]
realloc-grow       48.8 [ 47.1-  74.8] 134.2 [126.4- 146.5] 105.0 [100.4- 111.8]  145.0 [ 140.7- 168.8]
large-grow          917 [  799-  1075] 14642 [13287- 18784] 16961 [15630- 19758]   2275 [  2141-  2481]
large-churn         531 [  462-  1240]   414 [  385-   513]   232 [  221-   424]   1967 [  1856-  2119]
4-thread-churn      5.3 [  3.7-   5.7]   4.1 [  2.8-   5.8]   2.4 [  2.3-   5.8]    4.7 [   3.3-   7.4]
```

Against glibc, slab loses four rows and takes two: 1.30x on mixed sizes,
2.97x on short realloc chains, 2.48x on large-grow and 3.71x on large-block
churn, against 0.95x on small churn and 0.89x on four threads -- the last of
which is inside the spread and should be read as a draw. Against jemalloc
and tcmalloc it loses five of six, and the worst of those is large-block
churn again, 4.8x and 8.5x. Those are the standings and nothing here argues
them away; a 1000-line allocator with one global lock and a 16-entry mapping
cache is not going to beat three mature ones.

The row it takes outright is `large-grow`, a buffer grown from 16 KiB to
4 MiB by doubling, where slab is 6.4x FASTER than jemalloc and 7.5x faster
than tcmalloc. It is also the row that moved most: 216x glibc before the
reserved tail described below, and 2.48x after. Measured against the commit
before it in one interleaved run of ten repetitions each, 207606 ns/op to
2818, a factor of 74, while the other five moved by 1.01x to 1.06x.

`large-churn` is the worst row and the one still being worked on. It was
4.72x glibc until `map_aligned` stopped unmapping the alignment slack at the
end of a reserved mapping, which took it to 3.71x. That change is eleven
lines and it was sized before it was written, by building a `map_aligned`
with BOTH of its trimming `munmap` calls deleted -- which leaks address
space and could never ship, but puts an upper bound on the whole idea:
0.81x. Keeping the back slack gets 0.85x of it. The front trim is what is
left, it fires on fifteen calls in sixteen, and removing it needs a real
address-space allocator for 0.02x more; the PLAN records that and has moved
the item to the bottom of the list rather than leaving it ranked third on a
payoff nobody had measured. `munmap` over a benchmark run went from 5108 to
3479.

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

### Shrinking, and a 33% regression with the wrong obvious cause

`realloc(p, smaller)` used to return the same pointer and hold the whole
mapping: read a file into a growing buffer, trim it to the length it turned
out to be, and the peak stayed resident forever. It now hands the tail back
-- `mprotect` then `madvise`, not `munmap`, because unmapping the tail would
punch a hole in a mapping slab still owns and cost the block its
reservation. On a 64 MiB block trimmed to 1000 bytes:

```
slab, before      69204 -> 69384 KiB     nothing returned
slab, after       69272 ->  3796 KiB
glibc             66932 ->  1580 KiB
jemalloc 5        68276 ->  2868 KiB     (and it moved the block)
tcmalloc_minimal  72908 -> 72972 KiB     nothing returned
```

The first version of this cost **33% of the recorded CPython replay**, and
the obvious explanation -- two syscalls per trim -- was wrong. Counting the
reallocs in the trace showed fifteen large shrinks, exactly one of which
frees more than 128 KiB. One trim cannot cost 50 ns on every operation.

What it had done was move `commit0`, the size the mapping was made at, down
with the trim. `commit0` is what the span cache matches a freed mapping on,
so a trimmed block went into the cache at its trimmed size, never matched
the next request for the size it was made at, and the workload paid a fresh
`mmap` and several hundred page faults. Had the trace not been there to
count, the fix would have been to raise the threshold until the number went
away.

A block now goes back to `commit0` on the way into the cache from whichever
side it is on: grown needs both syscalls, trimmed needs only the `mprotect`
because the pages are already gone. After that, 0.92x to 1.05x everywhere.
The check is the mechanism rather than the timing, since this suite asserts
none: after a trim and a free, asking for the size the block was made at has
to return **that mapping**.

Below 128 KiB of saving the tail is kept, because the two syscalls and the
refault cost more than the pages are worth. `realloc(p_1MB, 100)` is fixed;
`realloc(p_100KB, 100)` is not.

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

## Real programs, not our guess

The six rows above are loops somebody wrote down as a guess at how programs
allocate. `t/librecord.so` is an `LD_PRELOAD` that writes every allocator
call a program makes to a file; `t/trace.py` turns the recorded pointers
into slot indices once, offline, so the replay driver needs no hash table
inside the loop being timed; and `t/replay` reissues the sequence against
whichever allocator is loaded. That separates the allocator's cost from the
program's own work, which timing the program end to end cannot do.

**The guess was wrong in both directions**, which is the whole reason for
doing it. Medians of eleven repetitions, order alternated between
allocators, ns per operation:

```
                       ops    glibc   jemalloc   tcmalloc     slab
gcc -O2 -S          619677     21.2       20.4       12.4     35.8   1.69x glibc
CPython JSON         20098      251       1063        612      150   0.60x glibc
```

On the `gcc` trace slab is 1.69x glibc, worse than any synthetic row except
large-block churn. On the CPython trace it is 0.60x glibc and beats jemalloc
by 7.1x and tcmalloc by 4.1x, because CPython serves its small objects out of
its own `obmalloc` and what reaches us is large blocks, which is where the
reserved tail and the span cache live. Neither of those is a number the
synthetic benchmark would have predicted.

### And end to end, which the replay deliberately does not measure

```
                        glibc        jemalloc      tcmalloc        slab
gcc -O2 -S              0.422s       1.019x        1.003x        1.048x
CPython JSON            0.190s       1.095x        1.059x        1.007x
CPython Counter 300k    0.121s       1.005x        1.005x        0.962x
sort, 3 MB              0.145s       0.988x        1.059x        0.993x
ls -R /usr/include      0.008s       1.173x        1.560x        1.023x
```

Medians of thirteen. Allocation is a few percent of what these programs
spend their time on, so a 1.69x allocator moves the wall clock by noise.
Both numbers are true and both are here: the replay says what the allocator
costs, and this says what it costs *you*.

### What the trace is worth, and what it is not

Three things are asserted before any number above is printed. The trace must
be whole: its trailer has to agree with how many records were read, and it
must contain no frees of pointers it never allocated. The replay must
reproduce it exactly -- every free finding its slot occupied, every
allocation finding it empty -- under slab **and** under glibc, because a
trace glibc cannot replay is a broken trace rather than a broken allocator.
And a replay that skips one operation in sixteen must be rejected: that one
measures 33.8 ns/op and reports 17954 frees of an empty slot, which is
exactly what a quietly lossy replay would otherwise have looked like.

The recorder had a bug that the trace itself found. `SLAB_TRACE` was a
filename, and `gcc` forks `cc1` and `as`, all of which inherit `LD_PRELOAD`,
all of which opened the same path with `O_TRUNC` and wrote from offset 0.
`cc1` wrote 673094 records; the driver finished afterwards and overwrote the
first 406 of them. The symptom was 254 frees of pointers nothing in the
trace had allocated. It is `<prefix>.<pid>` now, and `t/trace.py` refuses a
file containing two trailers.

It is a **file with a declared cap, not a ring buffer**. A ring keeps the
last N records, which throws away the allocations that the later frees refer
to, and then every one of those frees is an orphan with no lifetime.

The honest limit: the replay writes the first and last byte of each block.
The recorded program touched its memory in some pattern the trace does not
carry, so page-fault and cache behaviour here is the replay's, not the
program's. What carries over exactly is the sequence of sizes and the
lifetimes.

## fork() without exec(), from a thread

A multithreaded program that forks and then allocates in the child used to
hang: the child inherits the lock in whatever state it was in, and if
another thread held it at that instant nothing in the child will ever
release it. Not a corner case -- **120 of 200 forks** of a four-thread
process produced a child that could not allocate. glibc's malloc handles
this, so a drop-in replacement that does not is broken, whatever POSIX says
about calling malloc in a forked child.

Three `pthread_atfork` handlers. The **prepare** handler takes the lock, and
that is what makes this correct rather than merely unstuck: resetting the
lock in the child alone leaves whatever the holder was half-way through -- a
free list part-spliced, a span's live count part-updated -- visible there.
The mutant with prepare emptied out does not hang, it segfaults, in eight
runs of eight.

The **child** handler also re-stamps the thread's magazine set with its new
tid. Every set the child inherited names a tid from the parent, and a thread
created in the child calls the adoption path, finds a set whose owner is
gone, and takes the one its parent thread is still using -- two threads
pushing and popping the same magazines with no lock between them. That
mutant survived two versions of the check before the child was made to
create more threads than the parent had.

The one cost is a constructor, and the claim at the top of `slab.c` that
there is no initialisation step has been narrowed rather than deleted: it
registers three handlers and initialises no table, so there is still no "am
I initialised yet" flag to race on and an allocation before it runs is
served exactly as one after. What is missing in that window is fork safety,
and in that window the process has one thread.

## What a dead thread leaves behind

A thread that exits runs none of slab's code, so whatever is on its per-class
magazines -- up to 8 KiB a class, 256 KiB in all -- is stranded. Two things
get it back. A new thread **adopts** a set whose owner's tid no longer exists
(`tgkill(tid, 0)` returning `ESRCH`), which bounds the loss by the peak
number of threads rather than by the number created. And when `span_new` is
about to take another 4 MiB from the kernel, it **sweeps** the registry
first, which covers the case adoption does not: a program that peaks at many
threads and then runs one never asks for those sets again.

Per new arena, not per new span: a sweep costs a `tgkill` per live thread,
which per 4 MiB is nothing and per 64 KiB would be a syscall per seven
allocations in the 8192 class.

Measured with 64 threads held to a barrier so that 64 distinct magazine sets
exist at once: **11077 KiB on dead threads' magazines, 7 KiB after** the main
thread allocated enough to want another arena. The barrier is load-bearing --
without it the threads overlap unevenly, later ones adopt earlier ones' sets,
the sweep fires during the run, and the check measured 189 KiB stranded while
proving almost nothing.

The check also asks where the objects went, because a sweep that emptied the
magazine heads without handing the objects back to their spans would make the
figure above look perfect while the memory was gone for good. `live` in the
dump is net of the magazines, so only the objects the main thread actually
allocated may become live between the two dumps; the mutant that drops them
shows 109775 where 12000 were allocated.

## Huge pages, and a win none of the benchmarks could see

Arenas are 4 MiB, which is exactly the shape a transparent huge page wants,
so they are 2 MiB-aligned and `madvise(MADV_HUGEPAGE)`'d. The alignment is
not decoration: the kernel can only back a 2 MiB-aligned region, and a
span-aligned arena gets half of itself covered.

**Every benchmark in this README is blind to it.** All six synthetic rows
and both trace replays move by less than their run-to-run spread, because
they write one byte per block and move on, and a TLB entry is wasted on that
either way. The first measurement said 5% on small churn, and a three-way
run against a build with the alignment but no `madvise` showed the 5%
belonged to neither -- it was noise.

The workload that can see it is random access over a large live set, which
is `t/stress tlb`: 2 million live 64-byte objects, 20 million random reads.
Medians of three, ns per access, with the kernel's own `AnonHugePages`
figure beside them:

```
glibc                49.3 ns      0 KiB huge    173 MB resident
jemalloc 5           47.3 ns      0 KiB         148 MB
tcmalloc_minimal     45.3 ns      0 KiB         149 MB
slab, before         44.3 ns      0 KiB         142 MB
slab, after          38.3 ns    126976 KiB      144 MB
```

16% against slab's own previous commit and 23% against glibc, and slab is
still the smallest resident of the five.

The oracle is the kernel rather than the return value: `MADV_HUGEPAGE` is
advice and returns 0 on a kernel with `transparent_hugepage=never`, so the
check reads `AnonHugePages` out of `/proc/self/smaps_rollup` and demands
three quarters of the bytes asked for. It skips itself, loudly and counted,
when THP is not in `madvise` mode, because with `never` nothing can be
backed and with `always` every allocator gets huge pages without asking.

### What it costs, on the shape it is worst for

A churn over every size class leaving one block in eight live: **6.37 GB
resident becomes 6.83 GB, 7.2% worse**. And `span_reclaim`'s `MADV_DONTNEED`
then splits nearly all of those huge pages apart again -- 2 MB of
`AnonHugePages` survived out of 6.8 GB -- so that workload pays the cost and
keeps none of the benefit. Returning memory and huge pages are in tension,
and that is the measurement of by how much. In the suite, peak RSS on the
Python churn goes from 1.09x to 1.12x glibc against a 3.0x limit, and on the
byte-pattern stress from 0.50x to 0.68x against a 1.2x limit.

## Fragmentation

A span-based allocator has no per-object header, which is the 39% footprint
win above, and it pays for that in rounding: a request becomes one of 32
class sizes and nothing records what was asked for. The README used to argue
that four classes per octave above 128 bytes "caps internal fragmentation at
1/5". It does, and `t/stress frag` now measures it rather than asserting it,
over **every** request size from 1 to 8192 rather than a sample:

```
frag: worst case over 8192 sizes: 15 bytes of slack at or below 128,
      and 4097 bytes requested in a 5120-byte class = 19.98% of the
      allocation wasted
frag: 20000 objects, 81251 KiB requested, 88012 KiB in classes (7.68% of the
      allocation wasted), 1453 spans holding 92992 KiB (94% occupied by live
      objects)
```

glibc fails 9 of those per-size bounds, because `malloc_usable_size(malloc(1))`
is 24 there against slab's 16, and the suite asserts that it fails: a bound
both allocators satisfy would not be about slab's table.

The second line comes from slab's own per-span accounting, which is what
`malloc_stats(3)` prints. That was an empty stub until this was written, so
slab had been silently reporting nothing where glibc reports something. The
dump is one line per populated class plus a heat map -- one character per
span, tenths of its capacity in use, `#` for full and `.` for empty:

```
slab: class=31 objsize=8192 spans=67 live=310 capacity=469 livebytes=2539520
slab: heat 7575775757757557757577575757757577757777775777757775757577575775755
slab: small live=2760 livebytes=11844288 magazined=0 magbytes=0 spans=301 \
      mappedbytes=19726336 arenas=5 incomplete=0
slab: spancache entries=0 commitbytes=0
```

Two things it is not. It is not triggered by a signal, which is what the PLAN
asked for: the dump reads the class lists and the arena table, so it takes
the lock, and a handler that takes a lock the interrupted thread may already
hold deadlocks the process it was meant to diagnose. And there is no PNG or
offline renderer, because a line of digits per class is already the heat map
and a renderer would have no oracle behind it.

It does not cover live large blocks, which are one `mmap` each with no list
threaded through them; the only large accounting in the dump is the span
cache. `malloc_info(3)` is also still glibc's, running against an arena slab
does not use -- a real gap, not a claim.

The accounting is checked by differencing two dumps, one before the workload
and one with it live, so the C library's own startup allocations cancel
instead of being guessed at. The delta in live objects must equal the number
allocated **exactly**, and the delta in live bytes must equal the sum of
`malloc_usable_size` over the same blocks exactly: the same quantity reached
by two unrelated routes, per-span counters against per-object class lookups.

That exactness immediately found a defect in the dump. `h->live` counts an
object from the moment it leaves the bump pointer until it is handed back to
its span, so objects sitting on a thread's magazine -- already freed by the
program -- were reported as live. The first run said 19977 where 20000 were
expected, and the 23 were magazine stock left over from the size sweep that
runs before it. A fragmentation report that calls freed objects live is
wrong, so the magazines are walked and reported separately now.

## The page size, and what cannot be checked here

4096 was a compile-time constant until recently, which is simply wrong on a
16 KiB- or 64 KiB-page arm64 kernel: the trimming `munmap` in `map_aligned`
would be rejected for a non-page-aligned length and large allocations would
leak address space. It comes from `AT_PAGESZ` now, read with `getauxval`
rather than `sysconf`, because `sysconf` is allowed to allocate and this is
reached from inside `malloc`.

It is read lazily on first use and not in the constructor the `atfork`
handlers added, because a constructor runs after the dynamic linker has
already allocated through us, and a window in which a block is rounded with
one value and freed with another is worse than no change at all.

**What the suite can check is only half of it.** Every x86-64 kernel reports
4096, so the check compares slab's notion against `sysconf(_SC_PAGESIZE)`
and `getauxval(AT_PAGESZ)` and requires a span to be a whole number of
pages. It cannot run on the kernel the change is for. A mutant hardcoding
8192 is caught; a mutant reading `AT_CLKTCK` is deliberately absent, because
100 fails the power-of-two guard and falls back to 4096, which is the right
answer by accident.

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
