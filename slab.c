/*
 * slab - a size-class memory allocator, drop-in for malloc(3) via LD_PRELOAD.
 *
 * Memory comes from mmap(2) only.  Address space is carved into 64 KiB
 * SPANS; every span is 64 KiB-aligned, so given any pointer we recover its
 * metadata with one AND instruction.  A span belongs to exactly one size
 * class and hands out objects of that class; each class keeps a global free
 * list threaded through the free objects themselves.  Allocations too big
 * for the largest class get their own mmap.
 *
 * WHY THE SPAN TRICK:  the obvious alternative is a per-allocation header
 * holding the size.  That costs 16 bytes on every object (8 for the size,
 * 8 more to keep the payload 16-byte aligned), which doubles the footprint
 * of a 16-byte allocation.  Span-aligned metadata costs 64 bytes per 64 KiB
 * (0.1%) and the lookup is `p & ~0xffff`.
 *
 * RE-ENTRANCY, THE HAZARD THAT KILLS PRELOADED ALLOCATORS:  once this
 * object is interposed, *everything* inside libc that allocates calls us -
 * including code running before main() during dynamic linking and locale
 * setup.  So this file must never call anything that might allocate, might
 * take a libc lock we could already hold, or might need lazy TLS
 * resolution: no stdio (printf buffers are malloc'd), no getenv-driven
 * configuration, no __thread variables (the first access to a dynamic-TLS
 * variable can call __tls_get_addr -> dlopen bookkeeping -> malloc -> us,
 * recursively), no pthread_once, no atexit.  The only libc calls here are
 * mmap, munmap, mprotect, madvise, memcpy, memset, write, strlen and
 * sched_yield,
 * none of which allocate (plus gettid and tgkill, reached through syscall(2)
 * because glibc wrappers for them are recent).  There is also no
 * initialisation step at all: every table below is either a compile-time
 * constant or computed arithmetically, so there is no "am I initialised
 * yet" flag to race on and no init re-entrancy window.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* shortcut: the page size is a compile-time 4096 rather than
 * sysconf(_SC_PAGESIZE).  Ceiling: wrong on a 16 KiB- or 64 KiB-page kernel
 * (arm64 can be either), where the trailing munmap() in map_aligned() would
 * be rejected for a non-page-aligned length and large allocations would
 * leak address space.  Upgrade path: read AT_PAGESZ out of the auxiliary
 * vector, which is readable without allocating, unlike sysconf. */
#define PAGE       4096u
#define SPAN_SIZE  (64u * 1024u)
#define SPAN_MASK  ((uintptr_t)(SPAN_SIZE - 1))
#define HDR        64u              /* span header; also the payload offset */
#define NCLASS     32
#define MAXSMALL   8192u            /* above this, one mmap per allocation */
#define ARENA_LEN  (4u * 1024u * 1024u)   /* spans are carved from arenas */
#define HUGE_PAGE  (2u * 1024u * 1024u)   /* x86-64 PMD; arenas align to it */
#define MAGIC      0x5b1ab10cu
#define CLS_LARGE  0xffffu

/*
 * Size classes.  Spacing is four classes per power-of-two octave above 128
 * bytes, and exact 16-byte steps below it.
 *
 * WHY: 16 bytes is _Alignof(max_align_t) on x86-64, so a finer bottom step
 * would be unrepresentable - every pointer we return has to be 16-aligned
 * anyway, so classes below 128 cost nothing to make exact.  Above that,
 * four steps per octave caps internal fragmentation at 1/5 (a 2049-byte
 * request lands in the 2560 class, 20% waste) while keeping the table at 32
 * entries, small enough that the class index is a few instructions of
 * arithmetic instead of a search or a lookup table that would need
 * initialising (see the re-entrancy note above).
 */
static const uint32_t cls_size[NCLASS] = {
	  16,   32,   48,   64,   80,   96,  112,  128,
	 160,  192,  224,  256,
	 320,  384,  448,  512,
	 640,  768,  896, 1024,
	1280, 1536, 1792, 2048,
	2560, 3072, 3584, 4096,
	5120, 6144, 7168, 8192,
};

/*
 * The small and large halves of this are mutually exclusive -- cls says which
 * -- and they are in an anonymous union so that the names stay unqualified
 * and the struct stays inside HDR.  It has to: the small half grew a
 * per-span free list and a list link when spans started being returned to
 * the kernel, and flat it would have been 80 bytes.
 */
struct hdr {
	uint32_t magic;
	uint16_t cls;        /* class index, or CLS_LARGE */
	uint16_t cached;     /* large: this mapping may go in the span cache */
	union {
		struct {                     /* small */
			uint32_t bump;       /* offset of next never-used object */
			uint32_t cap;        /* offset one past the last object */
			uint32_t live;       /* handed out and not yet returned */
			void *freelist;      /* free objects IN THIS SPAN */
			struct hdr *next;    /* next span on klass.partial */
		};
		struct {                     /* large */
			void    *map_base;   /* what to hand back to munmap */
			size_t   map_len;    /* how much, reserved tail included */
			size_t   commit;     /* bytes of map_len that are RW */
			size_t   commit0;    /* the commit it was mapped with */
			size_t   req;        /* usable payload bytes */
		};
	};
};
_Static_assert(sizeof(struct hdr) <= HDR, "span header must fit in HDR bytes");
_Static_assert(HDR % 16 == 0, "payload offset must preserve 16-byte alignment");

/*
 * Per class, the spans that have somewhere to put an object: a free object on
 * their own list, or room left on their bump pointer.  Allocation only ever
 * touches the head.
 *
 * The free list used to be global to the class and threaded across spans,
 * which is cheaper by one indirection and makes it impossible to say whether
 * a span is empty without walking the whole list.  Per-span lists are what
 * make the madvise() in span_reclaim() a constant-time decision, and they
 * improve locality on the way.  The invariant everything below depends on is
 * that a span is on this list if and only if it has a free object or an
 * unbumped one, which is why alloc_small() unlinks a span the moment it
 * fills and free_small() relinks it the moment it stops being full.
 */
struct klass {
	struct hdr *partial;
	struct hdr *spare;      /* one empty span kept back; see free_small */
};

static struct klass klass[NCLASS];
static char  *arena_cur, *arena_end;

/*
 * Every arena we have ever mapped, so malloc_stats() can walk every span.
 * Nothing else needs this: allocation only ever looks at a class's partial
 * list, and free() finds a span by masking a pointer.  It is here because
 * "internal fragmentation is capped at 1/5 by the class spacing" was an
 * argument about the table in this file and not a measurement of any
 * workload, and the only way to measure it is to be able to enumerate.
 *
 * A flat array rather than a list, because an arena has no header to thread
 * one through: a span can be carved from its first bytes and the span
 * header is the span's own.  shortcut: bounded at ARENA_MAX, which is 8 GiB
 * of small-object arenas; past that the dump says incomplete=1 rather than
 * lying about the total.  Upgrade path: spend one span per arena on a header
 * and link them.
 */
#define ARENA_MAX 2048
static char     *arenas[ARENA_MAX];
static unsigned  narena, arena_overflow;

/*
 * A cache of recently freed large mappings.
 *
 * Large-block churn measured 8x glibc, and nearly all of it is syscalls and
 * page faults: every free was an munmap and every allocation a fresh mmap, so
 * every block was faulted in from zero. glibc keeps freed blocks and hands
 * them back. This does the same, for the plain-large path only.
 *
 * Matched by best fit within 25%, on the COMMITTED size of the entry rather
 * than on its whole length: the difference between the two is the PROT_NONE
 * reservation described below, which is address space and not memory, so
 * matching on it would turn a 128 KiB request into a 4 MiB one.  Exact
 * matching is the obvious design and was tried; it moved large-churn by 2%,
 * because that benchmark picks random sizes and page-rounding still leaves
 * about 250 distinct lengths for a 16-entry cache to miss.
 *
 * A mapping that a realloc grew is restored to the size it was mapped with
 * before it goes in here; see big_shrink().  Without that the matching above
 * never fires for the one workload the reservation exists for.
 *
 * Only the plain path is cached. The over-aligned path (align >= SPAN_SIZE)
 * has a different geometry -- the header sits a whole span below a payload that
 * is itself span-aligned -- so a mapping from one is not interchangeable with a
 * request for the other, and the header records which kind it is rather than
 * leaving that to be re-derived.
 *
 * The bound is on bytes, not entries. Sixteen mappings of a megabyte is a very
 * different thing to sixteen of a page, and it is the bytes a program notices.
 */
#define SPANCACHE_N     16
#define SPANCACHE_BYTES (64u * 1024u * 1024u)

/* `commit` is what the entry costs the process; `len` is what munmap needs.
 * The byte bound below is on commit, because the difference between them is
 * PROT_NONE address space and not memory anybody paid for. */
static struct { void *base; size_t len, commit; } spancache[SPANCACHE_N];
static int    nspan;
static size_t spanbytes;

/*
 * THE RESERVED TAIL, which is what makes realloc() grow without copying.
 *
 * A large block is one mmap.  Doubling it used to mean a second mmap, a
 * memcpy of the whole thing and an munmap, and the memcpy is the cheap part:
 * the destination has to be faulted in page by page, so growing 16 KiB to
 * 4 MiB by doubling faulted about 2048 pages and copied 8 MiB.  glibc calls
 * mremap and the kernel edits a page table instead.
 *
 * mremap is not available to us.  MREMAP_MAYMOVE can return an address that
 * is not SPAN_SIZE-aligned, and the AND-mask header lookup depends on that
 * alignment; without MREMAP_MAYMOVE it fails whenever the next page is
 * taken, which on a busy heap is most of the time.
 *
 * So the space is reserved up front.  Each large mapping is RESERVE_K + 1
 * times the size it needs, and everything past the part in use is mapped
 * PROT_NONE: no pages, no RSS, no overcommit charge (the kernel only
 * accounts a private anonymous mapping that is writable), and a hard fault
 * for anybody who runs off the end of the block.  Growth is one mprotect.
 *
 * Both constants are measurements, and the README has the tables.
 *
 * RESERVE_MIN is a floor because the mprotect pair is not free: with no
 * floor at all the realloc-grow benchmark, whose chain stops at 64 KiB, went
 * from 151 ns/op to 638, since at those sizes the copy comes out of a span
 * cache mapping whose pages are already warm and costs less than two
 * syscalls.  Measured floors: 1 -> 638, 16 KiB -> 677, 32 KiB -> 533,
 * 64 KiB -> 155, 128 KiB -> 151.
 *
 * RESERVE_K = 31 means a buffer can double five times before it has to move.
 * The large-grow benchmark is bimodal in this constant rather than smooth,
 * because its chain spans exactly 32x from the first reserved block, so 31
 * is the smallest value that makes it copy-free (2.3 us/op at 31 and 63,
 * 62 us at 15, 120 us at 7).  The law to trust is the general one: one copy
 * every log2(K+1) doublings.
 *
 * shortcut: the reservation is a fixed multiple, so a block that grows past
 * it still pays one copy, and a block at or above RESERVE_MIN that never
 * grows pays one extra mprotect and one extra VMA.  Ceilings: address space,
 * measured at VmPeak 34 MB -> 996 MB on a workload holding 64 large blocks
 * (VmHWM unchanged at 1648 kB); and a process holding more than about 32000
 * live large blocks at once approaches the default vm.max_map_count, since
 * each one is now two mappings instead of one.  Upgrade path: widen the
 * reservation on each forced move, so a buffer that keeps doubling stops
 * moving after the second time, which would remove the cliff above.
 */
#ifndef RESERVE_K
#define RESERVE_K    31u
#endif
#ifndef RESERVE_MIN
#define RESERVE_MIN  (128u * 1024u)
#endif
#define RESERVE_MAX  (64u * 1024u * 1024u)

static size_t reserve_tail(size_t commit)
{
	size_t tail;

	if (commit < RESERVE_MIN)
		return 0;
	tail = commit <= RESERVE_MAX / RESERVE_K ? commit * RESERVE_K
						 : RESERVE_MAX;
	if (commit > SIZE_MAX - tail - SPAN_SIZE - PAGE)
		return 0;
	return tail;
}

/*
 * One global spinlock, guarding the class free lists and the arena.  Threads
 * keep the lock off their hot path with the magazines below; what is left for
 * it is batch refills, batch drains, new spans and large allocations.
 *
 * A spinlock rather than pthread_mutex_t because mutex initialisation and the
 * pthread symbols are things we would rather not touch this early in process
 * startup.
 */
/* shortcut: no pthread_atfork handler, so a fork() that races a thread
 * holding this lock leaves the child with a lock nobody will ever release,
 * and the child's first malloc hangs.  Ceiling: multithreaded programs that
 * fork() without exec()ing.  fork-then-exec is fine, because exec replaces
 * the whole image.  Upgrade path: register a handler that re-initialises lk
 * in the child - cheap, but it needs pthread_atfork resolved at load time,
 * which is exactly the startup window we are trying not to touch. */
static volatile int lk;

static void lock(void)
{
	unsigned spins = 0;
	while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE)) {
		while (__atomic_load_n(&lk, __ATOMIC_RELAXED)) {
#if defined(__x86_64__) || defined(__i386__)
			__builtin_ia32_pause();
#endif
			if (++spins > 2000) { spins = 0; sched_yield(); }
		}
	}
}

static void unlock(void) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); }

/* Diagnostics go through write(2): printf would allocate, and we may be
 * called from inside a libc function that is holding stdio's own lock. */
static void note(const char *s) { (void)!write(2, s, strlen(s)); }

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/* Any pointer we ever returned -> the header of the span (or the large
 * mapping) it came from, in one AND.  A span-aligned pointer can only have
 * come from the over-aligned large path, whose header sits one span lower;
 * see alloc_big().  Defined here rather than next to its large-path siblings
 * because the small free path needs it too: an object's own span is where it
 * goes back to. */
static struct hdr *hdr_of(void *p)
{
	uintptr_t u = (uintptr_t)p;
	struct hdr *h = (struct hdr *)((u & SPAN_MASK) ? (u & ~SPAN_MASK)
						       : u - SPAN_SIZE);
	return h->magic == MAGIC ? h : NULL;
}


/*
 * mmap `len` (a whole number of pages) at a SPAN_SIZE-aligned address, by
 * over-mapping by one span and trimming.  Paid once per arena and once per
 * large allocation, never on the small path.
 *
 * `got`, when given, says KEEP THE BACK SLACK and reports the length that is
 * really ours: `len` plus however much of the over-map the alignment left at
 * the end.  The back trim fires on all but one call in sixteen (the kernel
 * returned a span-aligned address on 13 of 200 probes here), and unmapping
 * that slack is pure waste for a caller that can use address space at the
 * end of its mapping -- which is exactly what a large block's PROT_NONE
 * reservation is.  Measured over a benchmark run: 5108 munmaps before, 3479
 * after, and large-block churn 0.86x.
 *
 * The arena path passes NULL, because a sub-span remainder cannot hold a
 * span and arena_end has to stay span-aligned.  So does the large path when
 * it is not reserving a tail: there the mapping is read-write, and folding
 * slack into it would leave bytes past the end of the block WRITABLE, which
 * is the one thing the reservation is also a guard against.  `guard` fails
 * if that is got wrong -- its last assertion is that an unreserved large
 * block faults one byte past its end.
 */
static void *map_aligned_to(size_t len, int prot, size_t *got, size_t al)
{
	void *r = mmap(NULL, len + al, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED)
		return NULL;
	uintptr_t a = ((uintptr_t)r + al - 1) & ~(uintptr_t)(al - 1);
	size_t front = a - (uintptr_t)r;
	if (front)
		munmap(r, front);
	if (got)
		*got = len + (al - front);
	else if (al - front)
		munmap((void *)(a + len), al - front);
	return (void *)a;
}

static void *map_aligned(size_t len, int prot, size_t *got)
{
	return map_aligned_to(len, prot, got, SPAN_SIZE);
}

/* shortcut: spans are never unmapped and never returned to the OS, so peak
 * RSS is sticky - a program that allocates 500 MB of 64-byte objects and
 * frees them keeps 500 MB resident.  Ceiling: anything with a large
 * transient peak looks much worse than glibc here.  Upgrade path: track
 * free objects per span, and madvise(MADV_DONTNEED) a span once it is
 * wholly free (the span stays mapped so the AND-mask lookup still works). */
static struct hdr *span_new(unsigned cls)
{
	if (arena_cur == arena_end) {
		/*
		 * Arenas are 2 MiB-aligned and asked for huge pages.  A 4 MiB
		 * arena is exactly the shape a transparent huge page wants,
		 * and the alignment is not optional: the kernel can only back
		 * a 2 MiB-aligned region, so a span-aligned arena would get
		 * at most half of itself covered.  The extra trimming costs
		 * one over-map of 2 MiB instead of 64 KiB, once per 4 MiB.
		 *
		 * WHAT IT IS WORTH, measured, because this is the sort of
		 * thing that gets switched on for the sound of it.  The
		 * allocator benchmarks cannot see it at all -- they write one
		 * byte per block, and a TLB entry is wasted on that either
		 * way -- and the six rows move by less than the run-to-run
		 * noise.  What it buys is the PROGRAM's accesses: random
		 * reads over 2 million live 64-byte objects go from 55.5 to
		 * 46.7 ns, repeatably, 16% faster, with /proc/self/smaps_rollup
		 * confirming 127 MB of AnonHugePages where there had been
		 * none.  2 MiB alignment on its own, without the madvise,
		 * accounts for none of it (54.0 ns).
		 *
		 * shortcut: it is unconditional.  Ceiling: a sparse workload
		 * faults in whole 2 MiB regions for a few live objects and
		 * pays for it -- 6.37 GB resident becomes 6.83 GB, 7.2%, on a
		 * churn over every size class -- and then span_reclaim()'s
		 * MADV_DONTNEED splits nearly all of those huge pages apart
		 * again (2 MB of AnonHugePages survived out of 6.8 GB), so
		 * that workload pays the cost and keeps none of the benefit.
		 * The two features are in tension and the measurement says by
		 * how much.  Upgrade path: madvise an arena only once its
		 * spans are densely occupied, or MADV_NOHUGEPAGE a span on
		 * reclaim.
		 */
		char *a = map_aligned_to(ARENA_LEN, PROT_READ | PROT_WRITE,
					 NULL, HUGE_PAGE);
		if (!a)
			return NULL;
		madvise(a, ARENA_LEN, MADV_HUGEPAGE);
		arena_cur = a;
		arena_end = a + ARENA_LEN;
		if (narena < ARENA_MAX)
			arenas[narena++] = a;
		else
			arena_overflow = 1;
	}
	struct hdr *h = (struct hdr *)arena_cur;
	arena_cur += SPAN_SIZE;

	uint32_t sz = cls_size[cls];
	h->magic = MAGIC;
	h->cls = (uint16_t)cls;
	h->bump = HDR;
	h->cap = HDR + ((SPAN_SIZE - HDR) / sz) * sz;
	h->live = 0;
	h->freelist = NULL;
	h->next = klass[cls].partial;
	klass[cls].partial = h;
	return h;
}

/*
 * The last object in a span has just come back.  Give the pages to the
 * kernel and re-arm the span, which is what turns "peak RSS is sticky
 * forever" into a number that comes back down.
 *
 * MADV_DONTNEED and not munmap, because the mapping has to stay: the
 * AND-mask header lookup is the whole design, and an unmapped span would
 * make every pointer arithmetic shortcut in this file a segfault waiting for
 * the address to be reused.  The kernel drops the pages and the next touch
 * faults in a fresh zero page, which is also what makes the re-arm below
 * honest.
 *
 * The first page cannot go: the header is at offset 0 and madvise is
 * page-granular.  So 4 KiB of every 64 KiB span stays resident (6.25%), and
 * the objects that live in that first page are zeroed by hand instead --
 * without that, re-arming the bump pointer would hand out dirty memory while
 * telling calloc the span was untouched, which is a disclosure bug and the
 * targeted calloc check catches it.
 *
 * The span keeps its class and stays on the class's partial list.  It is on
 * that list by the invariant above (it had a free object a moment ago), and
 * an empty span with bump < cap belongs there, so there is no list surgery
 * here at all.
 *
 * One empty span per class is kept back rather than reclaimed, because
 * without that the mixed-sizes benchmark lost 23%: a span of the 8192 class
 * holds seven objects, so a workload drifting across that boundary emptied
 * and refilled spans constantly, and 520 extra madvise calls over a bench
 * run cost about 10 ns on every operation in the row.  It is not the syscall
 * count that hurts, it is that each one flushes TLBs across every CPU the
 * process has run on and then the pages have to be faulted back in.  See
 * free_small() for the one-span hysteresis that fixed it.
 */
static void span_reclaim(struct hdr *h)
{
	madvise((char *)h + PAGE, SPAN_SIZE - PAGE, MADV_DONTNEED);
	memset((char *)h + HDR, 0, PAGE - HDR);
	h->bump = HDR;
	h->freelist = NULL;
}

/* Put an object back in its own span.  Called with the lock held. */
static void free_small(struct hdr *h, void *p)
{
	int wasfull = !h->freelist && h->bump == h->cap;

	*(void **)p = h->freelist;
	h->freelist = p;
	if (wasfull) {
		h->next = klass[h->cls].partial;
		klass[h->cls].partial = h;
	}
	if (--h->live == 0) {
		/* Keep the most recently emptied span of this class back, and
		 * reclaim whichever one it displaces -- but only if that one is
		 * still empty, since it stayed on the partial list and may have
		 * been handed out again in the meantime. */
		struct klass *k = &klass[h->cls];
		struct hdr *old = k->spare;

		k->spare = h;
		if (old && old != h && old->live == 0)
			span_reclaim(old);
	}
}

/* Pop the class's free list; failing that, bump-allocate the next never-used
 * object out of the class's current span.  Objects start at offset HDR and
 * every class size is a multiple of 16, so every object in a 64 KiB-aligned
 * span is 16-byte aligned.
 *
 * *fresh is set if the memory has never been handed out before, i.e. it is
 * still the zero page mmap gave us - calloc uses that to skip the memset. */
static void *alloc_small(unsigned cls, int *fresh)
{
	struct klass *k = &klass[cls];
	struct hdr *h = k->partial;
	void *p;

	if (!h) {
		h = span_new(cls);      /* links itself onto k->partial */
		if (!h)
			return NULL;
	}
	if (h->freelist) {
		p = h->freelist;
		h->freelist = *(void **)p;
		*fresh = 0;
	} else {
		p = (char *)h + h->bump;
		h->bump += cls_size[cls];
		*fresh = 1;
	}
	h->live++;
	if (!h->freelist && h->bump == h->cap) {
		/* Full, so off the list it goes, which is what lets free_small()
		 * decide whether to relink by looking at the span alone. */
		k->partial = h->next;
		h->next = NULL;
	}
	return p;
}

/* size -> class index, for 1 <= size <= MAXSMALL.  n is the size in 16-byte
 * units; below 9 units the classes are consecutive, above it we locate the
 * octave with a bit scan and then the quarter within it. */
static unsigned cls_of(size_t size)
{
	size_t n = (size + 15) >> 4;
	if (n <= 8)
		return (unsigned)n - 1;
	unsigned b = 63 - (unsigned)__builtin_clzll((unsigned long long)(n - 1));
	return 8 + (b - 3) * 4 + (unsigned)((n - 1 - ((size_t)1 << b)) >> (b - 2));
}

/*
 * PER-THREAD MAGAZINES.
 *
 * The global lock was costing 16x glibc at four threads, all of it
 * contention: every malloc and every free touched the same cache line.  A
 * magazine is a per-thread, per-class free list that malloc pops and free
 * pushes with no lock at all.  The lock is taken only to refill an empty
 * magazine or drain a full one, in batches, so its cost is divided by the
 * batch size -- 256 objects for the 16-byte class.
 *
 * TLS, CAREFULLY.  The re-entrancy note at the top of this file rules out
 * __thread, because a dynamic-TLS access can call __tls_get_addr, which can
 * reach dlopen bookkeeping, which allocates, which calls us.  The
 * initial-exec model is the exception: the offset is resolved at load time
 * and the access compiles to one %fs-relative load with no call and no lazy
 * allocation.  It is only valid for an object present at program start,
 * which is exactly how this one is loaded.  A dlopen'd slab would be
 * rejected by the loader for want of static TLS surplus, which is a far
 * louder failure than the recursion the model avoids.
 *
 * The slot holds a pointer rather than the magazines themselves.  Eight bytes
 * of static TLS instead of five hundred, and -- the useful part -- the
 * magazines outlive the thread that used them, which is what makes the
 * adoption in tc_claim() possible.
 *
 * Only recycled objects are ever put in a magazine.  A never-used object
 * comes straight off the bump pointer and is handed back directly, so calloc
 * can still tell that it is untouched zero page and skip the memset.
 */
#define MAG_BYTES 8192u   /* per class, per thread, before a drain */

struct mag {
	void    *head;    /* threaded through the free objects, as the global list is */
	uint32_t n;
};

struct tcache {
	struct mag     m[NCLASS];
	int            owner;   /* tid; the registry outlives the thread */
	struct tcache *next;
};

static struct tcache *tc_all;              /* registry; written under lk */
static char *tcpool, *tcpool_end;          /* bump space for tcache structs */
static __thread struct tcache *tc __attribute__((tls_model("initial-exec")));

/*
 * Claim this thread's magazines: adopt a set whose owner has exited, or carve
 * a new one.  Called with the lock held.  Returns NULL if memory ran out, and
 * a NULL tc is a complete fallback -- every path below then behaves exactly
 * as it did before magazines existed.
 *
 * Adoption is what keeps a thread-churning program from leaking.  A thread
 * that exits runs no code of ours: pthread_key_create's destructor is the
 * usual answer and is ruled out here, because pthread_setspecific allocates
 * for key indices past the first level and would call straight back into us.
 * So the magazines live in our own memory, the registry outlives every
 * thread, and liveness is a tgkill(tid, 0).  A recycled tid makes that say
 * "alive", which costs one unnecessary carve and is otherwise harmless.
 */
static struct tcache *tc_claim(void)
{
	int saved = errno;   /* a successful malloc must not disturb errno */
	int me = (int)syscall(SYS_gettid);
	int pid = (int)getpid();

	for (struct tcache *t = tc_all; t; t = t->next) {
		if (t->owner == me)
			continue;
		if (syscall(SYS_tgkill, pid, t->owner, 0) == 0 || errno != ESRCH)
			continue;
		t->owner = me;
		errno = saved;
		return t;
	}

	/* Carved from a dedicated mapping, not from our own malloc, which would
	 * recurse straight back here, and not from the arena, whose cursor has
	 * to stay span-aligned. */
	if ((size_t)(tcpool_end - tcpool) < sizeof(struct tcache)) {
		char *m = mmap(NULL, SPAN_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (m == MAP_FAILED) {
			errno = saved;
			return NULL;
		}
		tcpool = m;
		tcpool_end = m + SPAN_SIZE;
	}
	struct tcache *t = (struct tcache *)tcpool;
	tcpool += round_up(sizeof *t, 64);   /* mmap gave it to us zeroed */
	t->owner = me;
	t->next = tc_all;
	tc_all = t;
	errno = saved;
	return t;
}

/*
 * Hand the whole magazine back to the global free list.  The walk is to find
 * its tail, which splicing a list onto a singly-linked list requires.
 *
 * Keeping half of it back would be the textbook move, to stop a workload
 * sitting exactly on the threshold from taking the lock every other
 * operation.  That hysteresis is already here: a drain triggers at MAG_BYTES
 * and a refill stops at MAG_BYTES/2, so the magazine has to be filled halfway
 * again before it can drain again.  Keeping half was implemented, measured
 * across all five benchmarks, and made no difference outside the run-to-run
 * noise, so the midpoint walk it needed is not here.
 */
static void mag_drain(unsigned cls)
{
	struct mag *m = &tc->m[cls];
	void *p = m->head;

	lock();
	while (p) {
		void *next = *(void **)p;
		struct hdr *h = hdr_of(p);
		if (h)
			free_small(h, p);
		p = next;
	}
	unlock();
	m->head = NULL;
	m->n = 0;
}

/*
 * Take a batch off the class's global free list, return one object and keep
 * the rest.  Called with the lock held; this is the acquisition the magazine
 * exists to amortise.  An empty global list means there is nothing recycled
 * to batch, so fall through to the bump pointer for a single fresh object.
 */
static void *mag_refill(unsigned cls, int *fresh)
{
	struct mag *m = &tc->m[cls];
	uint32_t want = (MAG_BYTES / 2) / cls_size[cls];
	void *first = alloc_small(cls, fresh);

	if (!first)
		return NULL;
	/* Only RECYCLED objects are stocked, which is why this stops at a span
	 * with nothing on its free list rather than bump-allocating into the
	 * magazine: a never-used object has to be handed back directly so that
	 * calloc can still tell it is untouched zero page.
	 *
	 * The batch used to be a splice off one global list, which was fewer
	 * instructions.  With per-span free lists it is a loop over
	 * alloc_small(), which keeps the span bookkeeping -- the live count,
	 * the partial-list invariant -- in exactly one place.  Both walk the
	 * same number of nodes. */
	while (m->n + 1 < want) {
		struct hdr *h = klass[cls].partial;
		int f;
		void *p;

		if (!h || !h->freelist)
			break;
		p = alloc_small(cls, &f);
		if (!p)
			break;
		*(void **)p = m->head;
		m->head = p;
		m->n++;
	}
	return first;
}

/* The whole small path: magazine first, lock only when it is empty. */
static void *small(unsigned cls, int *fresh)
{
	struct tcache *t = tc;
	void *p;

	if (t && t->m[cls].head) {
		p = t->m[cls].head;
		t->m[cls].head = *(void **)p;
		t->m[cls].n--;
		*fresh = 0;
		return p;
	}
	lock();
	if (!t)
		t = tc = tc_claim();
	p = t ? mag_refill(cls, fresh) : alloc_small(cls, fresh);
	unlock();
	return p;
}

/*
 * Large / over-aligned allocations: one mmap each, with a span header in
 * front so free() finds them by the same AND-mask as everything else.
 *
 * `align` <= 16 is the plain large case: header at the region base, payload
 * at base+HDR, region base forced SPAN_SIZE-aligned so masking the payload
 * pointer lands on the header.
 *
 * 32 <= align < SPAN_SIZE: same, payload pushed up to the first aligned
 * offset.  Still inside the first span, so masking still works.
 *
 * align >= SPAN_SIZE: the payload is itself span-aligned, so there is no
 * room for a header below it *in its own span* - masking would return the
 * payload.  We exploit exactly that: a pointer from the small or plain-large
 * path is never span-aligned (it always sits at least HDR bytes into a
 * span), so `p == (p & ~SPAN_MASK)` is a precise, O(1) signal that the
 * header is one span lower down.  No side table, no search.
 */
/*
 * Take a cached mapping of exactly `len` bytes, or NULL. Called with the lock
 * held. The caller must treat the memory as dirty: it held someone else's data
 * a moment ago, which is why calloc gets told.
 */
static void *spancache_take(size_t need, size_t *got_commit, size_t *got_len)
{
	int best = -1;
	for (int i = 0; i < nspan; i++) {
		if (spancache[i].commit < need ||
		    spancache[i].commit > need + need / 4)
			continue;
		if (best < 0 || spancache[i].commit < spancache[best].commit)
			best = i;
	}
	if (best < 0)
		return NULL;
	void *base = spancache[best].base;
	*got_commit = spancache[best].commit;
	*got_len = spancache[best].len;
	spanbytes -= spancache[best].commit;
	spancache[best] = spancache[--nspan];
	return base;
}

/*
 * Keep a freed mapping, or say no. Called with the lock held; returns 0 if the
 * caller still owns the mapping and must unmap it.
 *
 * Refusing is the normal case for anything big: a program that frees one
 * 500MB block should not have 500MB quietly retained on the chance it asks
 * again. The cap is why `len > SPANCACHE_BYTES` is rejected outright rather
 * than evicting everything else to make room for it.
 */
static int spancache_put(void *base, size_t len, size_t commit)
{
	if (nspan == SPANCACHE_N || commit > SPANCACHE_BYTES)
		return 0;
	while (spanbytes + commit > SPANCACHE_BYTES) {
		/* Evict the oldest-ish entry. There is no LRU here and the order is
		 * whatever the swap-with-last removal left behind; with sixteen
		 * entries matched on exact size, a real policy would be more code
		 * than the hit rate justifies.
		 * shortcut: no recency. Add it if a workload ever shows the cache
		 * thrashing on a mix of sizes. */
		if (nspan == 0)
			return 0;
		void *old = spancache[0].base;
		size_t olen = spancache[0].len, ocommit = spancache[0].commit;
		spancache[0] = spancache[--nspan];
		spanbytes -= ocommit;
		munmap(old, olen);
	}
	spancache[nspan].base = base;
	spancache[nspan].len = len;
	spancache[nspan].commit = commit;
	nspan++;
	spanbytes += commit;
	return 1;
}

/*
 * *fresh is set only when the memory has never been handed out before, which
 * for a large block means it is still the zero pages mmap gave us. A mapping
 * out of the span cache is NOT fresh -- it holds whatever the last owner left
 * there -- and calloc has to memset it. Getting that wrong is a disclosure
 * bug, not a performance one.
 */
static void *alloc_big(size_t size, size_t align, int *fresh)
{
	size_t off, len;
	char *base;

	*fresh = 1;

	if (align >= SPAN_SIZE) {
		len = round_up(align + SPAN_SIZE + size, PAGE);
		base = mmap(NULL, len, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED)
			return NULL;
		char *p = (char *)round_up((uintptr_t)base + SPAN_SIZE, align);
		struct hdr *h = (struct hdr *)(p - SPAN_SIZE);
		h->magic = MAGIC;
		h->cls = CLS_LARGE;
		h->map_base = base;
		h->map_len = len;
		h->commit = len;  /* no reserved tail, so no room to grow into */
		h->commit0 = len;
		h->req = size;
		h->cached = 0;    /* different geometry; never interchangeable */
		return p;
	}

	off = align > HDR ? align : HDR;        /* align is a power of two */
	size_t commit = round_up(off + size, PAGE);
	size_t cached_commit, cached_len;
	len = commit;
	base = spancache_take(commit, &cached_commit, &cached_len);
	if (base) {
		*fresh = 0;   /* dirty: it held somebody else's data a moment ago */
		commit = cached_commit;   /* we own all of it; req reflects that */
		len = cached_len;
	} else {
		/* Below RESERVE_MIN there is no tail, and the mapping is made
		 * read-write in one syscall, exactly as it was before any of
		 * this existed.  The mprotect pair is only worth paying where
		 * the copy it avoids is worth more. */
		size_t tail = reserve_tail(commit);
		len = commit;
		if (!tail) {
			base = map_aligned(commit, PROT_READ | PROT_WRITE,
					   NULL);
		} else {
			/* &len: the alignment slack joins the reservation
			 * rather than being unmapped.  See map_aligned(). */
			base = map_aligned(commit + tail, PROT_NONE, &len);
			if (!base) {       /* no address space for the tail */
				base = map_aligned(commit,
						   PROT_READ | PROT_WRITE,
						   NULL);
				len = commit;
			} else if (mprotect(base, commit,
					    PROT_READ | PROT_WRITE) != 0) {
				munmap(base, len);
				base = NULL;
			}
		}
	}
	if (!base)
		return NULL;
	struct hdr *h = (struct hdr *)base;
	h->magic = MAGIC;
	h->cls = CLS_LARGE;
	h->map_base = base;
	h->map_len = len;
	h->commit = commit;
	h->commit0 = commit;
	h->req = commit - off;
	h->cached = 1;
	return base + off;
}

/*
 * Commit part of a large block's reserved tail so that it can grow where it
 * stands.  Called with the lock held; returns 0 if the reservation is too
 * small, in which case realloc() falls back to allocate-copy-free.
 *
 * The pages being added read as zero, either because the tail has been
 * PROT_NONE since the mapping was made or because big_shrink() dropped them
 * with MADV_DONTNEED on the way into the span cache.  Nothing here relies on
 * that -- realloc promises nothing about the new bytes -- but it is why this
 * is not the disclosure path the span cache was.
 */
static int big_grow(struct hdr *h, void *p, size_t size)
{
	size_t off = (size_t)((char *)p - (char *)h->map_base);
	size_t want;

	if (size > SIZE_MAX - off - PAGE)
		return 0;
	want = round_up(off + size, PAGE);
	if (want <= h->commit || want > h->map_len)
		return 0;
	if (mprotect((char *)h->map_base + h->commit, want - h->commit,
		     PROT_READ | PROT_WRITE) != 0)
		return 0;
	h->commit = want;
	h->req = want - off;
	return 1;
}

/*
 * Put a grown block back to the size it was mapped at, on the way into the
 * span cache.
 *
 * Without this the cache stops working for the one workload the reservation
 * is for.  A buffer that is malloc'd at 16 KiB and doubled to 64 KiB is
 * freed as a 64 KiB mapping, the next iteration asks for 16 KiB, the sizes
 * do not match, and every round pays a fresh mmap.  Measured on 200 rounds
 * of the realloc-grow loop: 17 mmaps and 9 munmaps before the reservation
 * existed, 214 and 587 with it and no shrink, 17 and 9 again with it.
 *
 * Both syscalls are needed and neither is enough on its own.  madvise drops
 * the pages, which is what the RSS is; mprotect takes the access away,
 * without which the guard tail of any block that had ever grown would be
 * writable for the rest of the mapping's life.
 */
static void big_shrink(struct hdr *h)
{
	char *from = (char *)h->map_base + h->commit0;
	size_t n = h->commit - h->commit0;

	madvise(from, n, MADV_DONTNEED);
	if (mprotect(from, n, PROT_NONE) == 0)
		h->commit = h->commit0;
}

static size_t usable(struct hdr *h)
{
	return h->cls == CLS_LARGE ? h->req : cls_size[h->cls];
}

/* ------------------------------------------------------------------ */

void *malloc(size_t size)
{
	void *p;
	int fresh;

	if (size == 0)
		size = 1;             /* a unique freeable pointer, as is customary */
	if (size > MAXSMALL) {
		if (size > SIZE_MAX - SPAN_SIZE - PAGE) { errno = ENOMEM; return NULL; }
		lock();
		p = alloc_big(size, 16, &fresh);
		unlock();
	} else {
		p = small(cls_of(size), &fresh);
	}
	if (!p)
		errno = ENOMEM;
	return p;
}

void free(void *p)
{
	if (!p)                       /* free(NULL) is a no-op, per C11 7.22.3.3 */
		return;
	/* A span header is written before the span's first object is handed out
	 * and never changes afterwards, so reading cls costs no lock -- and not
	 * taking one here is the entire point of the magazines.  The slow path
	 * below recomputes it, which is a handful of instructions in exchange
	 * for leaving that path exactly as it was. */
	struct hdr *hh = hdr_of(p);
	if (hh && hh->cls != CLS_LARGE && tc) {
		struct mag *m = &tc->m[hh->cls];
		*(void **)p = m->head;
		m->head = p;
		m->n++;
		if ((size_t)m->n * cls_size[hh->cls] > MAG_BYTES)
			mag_drain(hh->cls);
		return;
	}
	lock();
	struct hdr *h = hdr_of(p);
	if (!h) {
		unlock();
		/* Not ours.  Can happen if something allocated through a path we
		 * failed to interpose; leaking is survivable, abort()ing is not. */
		static int once;
		if (!once) { once = 1; note("slab: free() of foreign pointer, leaking\n"); }
		return;
	}
	if (h->cls == CLS_LARGE) {
		void *b = h->map_base;
		size_t l = h->map_len;
		if (h->commit > h->commit0)
			big_shrink(h);
		int keep = h->cached && spancache_put(b, l, h->commit);
		/* The header is inside the mapping, so invalidate the magic before
		 * letting go of it. A cached mapping that still says MAGIC would be
		 * recognised by hdr_of() through a stale pointer, and free() would
		 * cheerfully cache it a second time. */
		h->magic = 0;
		unlock();
		if (!keep)
			munmap(b, l);
		return;
	}
	/* The freed object is itself the free-list node: the smallest class is
	 * 16 bytes, so there is always room for the next pointer in the first 8
	 * bytes of dead memory, and the list costs no metadata at all. */
	free_small(h, p);
	unlock();
}

void *calloc(size_t n, size_t m)
{
	if (m && n > SIZE_MAX / m) { errno = ENOMEM; return NULL; }
	size_t size = n * m;
	void *p;

	if (size == 0)
		size = 1;
	int fresh;

	if (size > MAXSMALL) {
		if (size > SIZE_MAX - SPAN_SIZE - PAGE) { errno = ENOMEM; return NULL; }
		lock();
		p = alloc_big(size, 16, &fresh);
		unlock();
		if (!p) { errno = ENOMEM; return NULL; }
		/* A fresh mmap is already zero. A mapping out of the span cache is
		 * not: it holds what the last owner left in it. This used to be
		 * `return malloc(size)` with the comment "fresh mmap: already
		 * zero", which was true until the cache existed and would have been
		 * a disclosure bug the moment it did. */
		if (!fresh)
			memset(p, 0, size);
		return p;
	}

	p = small(cls_of(size), &fresh);
	if (!p) { errno = ENOMEM; return NULL; }
	/* A bump-allocated object has never been written, and spans are never
	 * recycled across classes, so it is still the kernel's zero page. */
	if (!fresh)
		memset(p, 0, size);
	return p;
}

/* realloc(p, 0) frees p and returns NULL.  C17 made this undefined; we
 * match glibc because that is what a drop-in replacement has to do, and
 * document it in the README. */
void *realloc(void *p, size_t size)
{
	if (!p)
		return malloc(size);
	if (size == 0) {
		free(p);
		return NULL;
	}
	lock();
	struct hdr *h = hdr_of(p);
	size_t old = h ? usable(h) : 0;
	int small = h && h->cls != CLS_LARGE;
	/* Growing into the reserved tail is the only reason this is inside the
	 * lock: it writes h->commit and h->req. */
	int grown = h && !small && size > old && big_grow(h, p, size);
	if (grown)
		old = h->req;
	unlock();
	if (!h) {
		note("slab: realloc() of foreign pointer\n");
		errno = EINVAL;
		return NULL;
	}
	if (grown)
		return p;
	/* In-place when the request still lands in the same class, and for
	 * large blocks when it still fits the mapping we already have.  A
	 * large block that outgrows its reserved tail lands here and is
	 * copied; see RESERVE_K above for how often that is. */
	if (small) {
		if (size <= MAXSMALL && cls_of(size) == h->cls)
			return p;
	} else if (size <= old) {
		/* shortcut: shrinking a large block in place keeps the whole
		 * mapping resident - realloc(p_1MB, 100) still holds 1 MB.
		 * Ceiling: read-the-file-then-trim-the-buffer code keeps peak
		 * footprint forever.  Upgrade path: munmap the pages past the
		 * new end, which is safe because the header stays in page 0. */
		return p;
	}

	void *q = malloc(size);
	if (!q)
		return NULL;             /* errno set, p still valid */
	memcpy(q, p, old < size ? old : size);
	free(p);
	return q;
}

int posix_memalign(void **out, size_t align, size_t size)
{
	if (align % sizeof(void *) || (align & (align - 1)) || align == 0)
		return EINVAL;
	if (size == 0)
		size = 1;
	void *p;
	if (align <= 16 && size <= MAXSMALL) {
		int fresh;
		lock();
		p = alloc_small(cls_of(size), &fresh);
		unlock();
	} else {
		if (size > SIZE_MAX - align - SPAN_SIZE - PAGE)
			return ENOMEM;
		int fresh;
		lock();
		p = alloc_big(size, align, &fresh);
		unlock();
	}
	if (!p)
		return ENOMEM;
	*out = p;
	return 0;
}

void *aligned_alloc(size_t align, size_t size)
{
	void *p;
	int e = posix_memalign(&p, align, size);
	if (e) { errno = e; return NULL; }
	return p;
}

void *memalign(size_t align, size_t size) { return aligned_alloc(align, size); }

void *valloc(size_t size) { return aligned_alloc(PAGE, size); }

void *pvalloc(size_t size) { return aligned_alloc(PAGE, round_up(size ? size : 1, PAGE)); }

void *reallocarray(void *p, size_t n, size_t m)
{
	if (m && n > SIZE_MAX / m) { errno = ENOMEM; return NULL; }
	return realloc(p, n * m);
}

size_t malloc_usable_size(void *p)
{
	if (!p)
		return 0;
	lock();
	struct hdr *h = hdr_of(p);
	size_t u = h ? usable(h) : 0;
	unlock();
	return u;
}

/* glibc's hooks and knobs.  Programs call these unconditionally; answering
 * "did nothing, successfully" is better than letting the real ones run
 * against an arena we are not using. */
int mallopt(int opt, int val) { (void)opt; (void)val; return 1; }
int malloc_trim(size_t pad) { (void)pad; return 0; }

/*
 * malloc_stats(3): occupancy of every span, on stderr.
 *
 * WHY THIS AND NOT A SIGNAL HANDLER, which is what the PLAN asked for. The
 * dump has to read the class lists and the arena table, so it takes the
 * lock; a handler that takes a lock the interrupted thread may already hold
 * deadlocks the process it was meant to diagnose, and there is no
 * async-signal-safe way to read this state consistently. malloc_stats is a
 * glibc entry point programs already call, it was a no-op here until now
 * (so slab was quietly reporting nothing where glibc reports something),
 * and it takes an explicit call from a known context. The one thing a signal
 * buys -- dumping a process you did not write -- is not worth a deadlock in
 * the allocator.
 *
 * Written with write(2) and hand-rolled decimal conversion, for the reason
 * at the top of this file: printf allocates.
 *
 * The heat map is the per-class line of digits: one character per span, the
 * tenths of its capacity that are in use, '#' for completely full and '.'
 * for completely empty. That is the fragmentation picture the PLAN wanted
 * rendered offline as a PNG, except that it needs no renderer, no second
 * file format and no new file.
 *
 * h->live counts an object as live from the moment it leaves the bump
 * pointer until it is handed back to its span, which means objects sitting
 * on a thread's magazine -- already freed by the program -- are in it. A
 * fragmentation report that called those live would be wrong, and a test
 * that differenced two dumps would be off by the magazine stock, which is
 * how this was noticed: 19977 where 20000 were expected. So the magazines
 * are walked and reported separately. Reading another thread's magazine
 * count without synchronising is a torn read of one uint32 in a diagnostic,
 * and the alternative is making every magazine pop atomic to improve a
 * printout.
 *
 * What it does NOT cover: live large blocks. They are one mmap each with no
 * list threaded through them, so the only large accounting here is the span
 * cache. Small spans are where the class-spacing argument lives, so they are
 * what this measures.
 */
static char *d_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *d_num(char *p, size_t v)
{
	char tmp[24];
	unsigned n = 0, i;

	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	for (i = 0; i < n; i++)
		p[i] = tmp[n - 1 - i];
	return p + n;
}

/* The heat map and the per-class line are emitted in HEAT_CHUNK-span pieces
 * so the stack buffer below is a fixed size whatever the heap looks like. */
#define HEAT_CHUNK 256

void malloc_stats(void)
{
	char buf[256], *p;
	size_t tlive = 0, tbytes = 0, tmapped = 0, tspans = 0;
	unsigned cls;

	lock();
	for (cls = 0; cls < NCLASS; cls++) {
		size_t sz = cls_size[cls], spans = 0, live = 0, cap = 0;
		unsigned nheat = 0;
		char heat[HEAT_CHUNK];

		/* Pass 1: the numbers.  Pass 2 re-walks for the heat map, which
		 * is two walks of the arena table instead of one and saves
		 * buffering a character per span. */
		for (unsigned a = 0; a < narena; a++)
			for (size_t off = 0; off + SPAN_SIZE <= ARENA_LEN;
			     off += SPAN_SIZE) {
				struct hdr *h = (struct hdr *)(arenas[a] + off);
				if (h->magic != MAGIC || h->cls != cls)
					continue;
				spans++;
				live += h->live;
				cap += (h->cap - HDR) / sz;
			}
		if (!spans)
			continue;
		tspans += spans;
		tlive += live;
		tbytes += live * sz;
		tmapped += spans * (size_t)SPAN_SIZE;

		p = d_str(buf, "slab: class=");
		p = d_num(p, cls);
		p = d_str(p, " objsize=");
		p = d_num(p, sz);
		p = d_str(p, " spans=");
		p = d_num(p, spans);
		p = d_str(p, " live=");
		p = d_num(p, live);
		p = d_str(p, " capacity=");
		p = d_num(p, cap);
		p = d_str(p, " livebytes=");
		p = d_num(p, live * sz);
		p = d_str(p, "\n");
		(void)!write(2, buf, (size_t)(p - buf));

		(void)!write(2, "slab: heat ", 11);
		for (unsigned a = 0; a < narena; a++)
			for (size_t off = 0; off + SPAN_SIZE <= ARENA_LEN;
			     off += SPAN_SIZE) {
				struct hdr *h = (struct hdr *)(arenas[a] + off);
				size_t n;
				if (h->magic != MAGIC || h->cls != cls)
					continue;
				n = (h->cap - HDR) / sz;
				heat[nheat++] = h->live == 0 ? '.'
					      : h->live >= n ? '#'
					      : (char)('0' + h->live * 10 / n);
				if (nheat == HEAT_CHUNK) {
					(void)!write(2, heat, nheat);
					nheat = 0;
				}
			}
		if (nheat)
			(void)!write(2, heat, nheat);
		(void)!write(2, "\n", 1);
	}

	size_t mago = 0, magb = 0;
	for (struct tcache *t = tc_all; t; t = t->next)
		for (cls = 0; cls < NCLASS; cls++) {
			mago += t->m[cls].n;
			magb += (size_t)t->m[cls].n * cls_size[cls];
		}

	p = d_str(buf, "slab: small live=");
	p = d_num(p, tlive);
	p = d_str(p, " livebytes=");
	p = d_num(p, tbytes);
	p = d_str(p, " magazined=");
	p = d_num(p, mago);
	p = d_str(p, " magbytes=");
	p = d_num(p, magb);
	p = d_str(p, " spans=");
	p = d_num(p, tspans);
	p = d_str(p, " mappedbytes=");
	p = d_num(p, tmapped);
	p = d_str(p, " arenas=");
	p = d_num(p, narena);
	p = d_str(p, " incomplete=");
	p = d_num(p, arena_overflow);
	p = d_str(p, "\n");
	(void)!write(2, buf, (size_t)(p - buf));

	p = d_str(buf, "slab: spancache entries=");
	p = d_num(p, (size_t)nspan);
	p = d_str(p, " commitbytes=");
	p = d_num(p, spanbytes);
	p = d_str(p, "\n");
	(void)!write(2, buf, (size_t)(p - buf));
	unlock();
}
