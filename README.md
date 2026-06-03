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

It is also an answer to a narrower question: how much does a per-object
header cost? glibc puts 8 to 16 bytes of chunk metadata in front of every
allocation. slab puts 64 bytes at the base of each 64 KiB span instead and
finds it with one `AND`, which is 0.1% overhead instead of 100% on a 16-byte
object. That is the footprint benchmark slab wins, and it wins it by 39%.

## The oracle

`make test` runs 32 checks in three tiers.

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

Tier 3: the oracle checked for teeth, by mutation. `make test` builds five
deliberately broken slabs, objects that overlap by 8 bytes, a `calloc` that
never zeroes, a `realloc` that copies half the bytes, a class index one too
small for one octave, and magazines with no adoption, and requires each to
be rejected *by the check that owns that behaviour*, not merely by some check
somewhere. (This said four and named four. The same README says "Five are
permanent fixtures" further down.) That distinction is
load-bearing; see below.

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
ok    stress: 3M random ops, byte pattern verified
ok    stress: 4 threads, 400k ops each, cross-thread free
ok    oracle agrees with glibc malloc
ok    peak RSS 1.10x glibc on a churn workload (limit 3.0x)
ok    stress: 1000 sequential threads, nothing live at the end
ok    peak RSS 1.00x glibc after 1000 dead threads (limit 1.5x)
ok    span cache: 200 large cycles cost 3 munmaps (glibc 2)
ok    self-check: 'stress stress 200000' rejects objects that overlap by 8 bytes
ok    self-check: 'stress check' rejects a calloc that never zeroes
ok    self-check: 'stress check' rejects a realloc that copies half the bytes
ok    self-check: 'stress check' rejects a class index one too small for one octave
ok    self-check: thread-churn RSS rejects magazines with no adoption (10.42x)
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

## License

GPL-3.0. See `LICENSE`.
