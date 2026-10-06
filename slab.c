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
 * caches.  Ceiling: measured 16x slower than glibc at 4 threads, and 6x
 * slower than slab's own single-threaded number, because every operation
 * serialises on one cache line (see README Benchmarks).  Upgrade path: a
 * per-thread magazine of ~64 objects per class, flushed to a global list in
 * batches, which removes the lock from the hot path entirely.  A spinlock rather than pthread_mutex_t because mutex
 * initialisation and the pthread symbols are things we would rather not
 * touch this early in process startup.
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
 * span is 16-byte aligned.
 *
 * *fresh is set if the memory has never been handed out before, i.e. it is
 * still the zero page mmap gave us - calloc uses that to skip the memset. */
static void *alloc_small(unsigned cls, int *fresh)
{
	struct klass *k = &klass[cls];
	void *p = k->freelist;
	if (p) {
		k->freelist = *(void **)p;
		*fresh = 0;
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
	*fresh = 1;
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
static void *alloc_big(size_t size, size_t align)
{
	size_t off, len;
	char *base;

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
		h->req = size;
		return p;
	}

	off = align > HDR ? align : HDR;        /* align is a power of two */
	len = round_up(off + size, PAGE);
	base = map_aligned(len);
	if (!base)
		return NULL;
	struct hdr *h = (struct hdr *)base;
	h->magic = MAGIC;
	h->cls = CLS_LARGE;
	h->map_base = base;
	h->map_len = len;
	h->req = len - off;
	return base + off;
}

static struct hdr *hdr_of(void *p)
{
	uintptr_t u = (uintptr_t)p;
	struct hdr *h = (struct hdr *)((u & SPAN_MASK) ? (u & ~SPAN_MASK)
						       : u - SPAN_SIZE);
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
	int fresh;

	if (size == 0)
		size = 1;             /* a unique freeable pointer, as is customary */
	if (size > MAXSMALL) {
		if (size > SIZE_MAX - SPAN_SIZE - PAGE) { errno = ENOMEM; return NULL; }
		lock();
		p = alloc_big(size, 16);
		unlock();
	} else {
		lock();
		p = alloc_small(cls_of(size), &fresh);
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
	void *p;

	if (size == 0)
		size = 1;
	if (size > MAXSMALL)
		return malloc(size);       /* fresh mmap: already zero */

	int fresh;
	lock();
	p = alloc_small(cls_of(size), &fresh);
	unlock();
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
	unlock();
	if (!h) {
		note("slab: realloc() of foreign pointer\n");
		errno = EINVAL;
		return NULL;
	}
	/* In-place when the request still lands in the same class, and for
	 * large blocks when it still fits the mapping we already have.
	 * shortcut: no mremap for the large grow case - mremap(MREMAP_MAYMOVE)
	 * can return an address that is not SPAN_SIZE-aligned, which would
	 * break the AND-mask header lookup.  Ceiling: growing a 1 MB buffer by
	 * doubling costs a full copy each time where glibc gets a page-table
	 * remap.  Upgrade path: reserve a PROT_NONE guard tail per large
	 * mapping and mprotect it in, so growth never needs to move. */
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
		lock();
		p = alloc_big(size, align);
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
void malloc_stats(void) { }
