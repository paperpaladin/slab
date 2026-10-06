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
 * mmap, munmap, memcpy, memset, write, strlen and sched_yield, none of
 * which allocate.  There is also no initialisation step at all: every table
 * below is either a compile-time constant or computed arithmetically, so
 * there is no "am I initialised yet" flag to race on and no init
 * re-entrancy window.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
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

struct hdr {
	uint32_t magic;
	uint16_t cls;        /* class index, or CLS_LARGE */
	uint16_t pad;
	uint32_t bump;       /* small: offset of next never-used object */
	uint32_t cap;        /* small: offset one past the last object */
	void    *map_base;   /* large: what to hand back to munmap */
	size_t   map_len;    /* large: how much */
	size_t   req;        /* large: usable payload bytes */
};
_Static_assert(sizeof(struct hdr) <= HDR, "span header must fit in HDR bytes");
_Static_assert(HDR % 16 == 0, "payload offset must preserve 16-byte alignment");

struct klass {
	void       *freelist;   /* singly linked through the free objects */
	struct hdr *cur;        /* span we are currently bump-allocating from */
};

static struct klass klass[NCLASS];
static char  *arena_cur, *arena_end;

/*
 * shortcut: one global spinlock for the whole allocator, no per-thread
 * caches.  Ceiling: throughput collapses once more than about two threads
 * allocate concurrently, because every operation serialises on one cache
 * line.  Upgrade path: a per-thread magazine of ~64 objects per class,
 * flushed to a global list in batches, which removes the lock from the hot
 * path entirely.  A spinlock rather than pthread_mutex_t because mutex
 * initialisation and the pthread symbols are things we would rather not
 * touch this early in process startup.
 */
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

/* mmap `len` (a whole number of pages) at a SPAN_SIZE-aligned address, by
 * over-mapping and trimming.  The two extra munmaps are paid once per arena
 * and once per large allocation, never on the small path. */
static void *map_aligned(size_t len)
{
	void *r = mmap(NULL, len + SPAN_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED)
		return NULL;
	uintptr_t a = ((uintptr_t)r + SPAN_MASK) & ~SPAN_MASK;
	size_t front = a - (uintptr_t)r;
	if (front)
		munmap(r, front);
	if (SPAN_SIZE - front)
		munmap((void *)(a + len), SPAN_SIZE - front);
	return (void *)a;
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
		char *a = map_aligned(ARENA_LEN);
		if (!a)
			return NULL;
		arena_cur = a;
		arena_end = a + ARENA_LEN;
	}
	struct hdr *h = (struct hdr *)arena_cur;
	arena_cur += SPAN_SIZE;

	uint32_t sz = cls_size[cls];
	h->magic = MAGIC;
	h->cls = (uint16_t)cls;
	h->bump = HDR;
	h->cap = HDR + ((SPAN_SIZE - HDR) / sz) * sz;
	return h;
}

/* Pop the class's free list; failing that, bump-allocate the next never-used
 * object out of the class's current span.  Objects start at offset HDR and
 * every class size is a multiple of 16, so every object in a 64 KiB-aligned
 * span is 16-byte aligned. */
static void *alloc_small(unsigned cls)
{
	struct klass *k = &klass[cls];
	void *p = k->freelist;
	if (p) {
		k->freelist = *(void **)p;
		return p;
	}
	struct hdr *h = k->cur;
	if (!h || h->bump == h->cap) {
		h = span_new(cls);
		if (!h)
			return NULL;
		k->cur = h;
	}
	p = (char *)h + h->bump;
	h->bump += cls_size[cls];
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

/* Large allocations keep the original scheme: one region each, header at the
 * span-aligned base, payload at base + HDR, so masking the payload pointer
 * lands on the header exactly as it does for a span of small objects. */
static void *alloc_big(size_t size)
{
	size_t len = round_up(HDR + size, PAGE);
	char *base = map_aligned(len);
	if (!base)
		return NULL;
	struct hdr *h = (struct hdr *)base;
	h->magic = MAGIC;
	h->cls = CLS_LARGE;
	h->map_base = base;
	h->map_len = len;
	h->req = len - HDR;
	return base + HDR;
}

static struct hdr *hdr_of(void *p)
{
	struct hdr *h = (struct hdr *)((uintptr_t)p & ~SPAN_MASK);
	return h->magic == MAGIC ? h : NULL;
}

static size_t usable(struct hdr *h)
{
	return h->cls == CLS_LARGE ? h->req : cls_size[h->cls];
}

/* ------------------------------------------------------------------ */

void *malloc(size_t size)
{
	void *p;

	if (size == 0)
		size = 1;             /* a unique freeable pointer, as is customary */
	if (size > MAXSMALL) {
		if (size > SIZE_MAX - SPAN_SIZE - PAGE) { errno = ENOMEM; return NULL; }
		lock();
		p = alloc_big(size);
		unlock();
	} else {
		lock();
		p = alloc_small(cls_of(size));
		unlock();
	}
	if (!p)
		errno = ENOMEM;
	return p;
}

void free(void *p)
{
	if (!p)                       /* free(NULL) is a no-op, per C11 7.22.3.3 */
		return;
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
		unlock();
		munmap(b, l);
		return;
	}
	/* The freed object is itself the free-list node: the smallest class is
	 * 16 bytes, so there is always room for the next pointer in the first 8
	 * bytes of dead memory, and the list costs no metadata at all. */
	*(void **)p = klass[h->cls].freelist;
	klass[h->cls].freelist = p;
	unlock();
}

void *calloc(size_t n, size_t m)
{
	if (m && n > SIZE_MAX / m) { errno = ENOMEM; return NULL; }
	size_t size = n * m;
	void *p = malloc(size);
	if (p && size <= MAXSMALL)
		memset(p, 0, size);   /* large blocks are fresh mmap, already zero */
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
	unlock();
	if (!h) {
		note("slab: realloc() of foreign pointer\n");
		errno = EINVAL;
		return NULL;
	}
	/* In-place when the request still lands in the same class, and for
	 * large blocks when it still fits the mapping we already have. */
	if (small) {
		if (size <= MAXSMALL && cls_of(size) == h->cls)
			return p;
	} else if (size <= old) {
		return p;
	}

	void *q = malloc(size);
	if (!q)
		return NULL;             /* errno set, p still valid */
	memcpy(q, p, old < size ? old : size);
	free(p);
	return q;
}
