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
 * which allocate (plus gettid and tgkill, reached through syscall(2) because
 * glibc's wrappers for them are recent).  There is also no initialisation step at all: every table
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
	uint16_t cached;     /* large: this mapping may go in the span cache */
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
 * A cache of recently freed large mappings.
 *
 * Large-block churn measured 8x glibc, and nearly all of it is syscalls and
 * page faults: every free was an munmap and every allocation a fresh mmap, so
 * every block was faulted in from zero. glibc keeps freed blocks and hands
 * them back. This does the same, for the plain-large path only.
 *
 * Matched on exact length. Large requests are rounded up to a page, so lengths
 * cluster hard in practice, and exact matching keeps the lookup a short scan
 * with no policy to get wrong. shortcut: a best-fit or size-class match would
 * hit more often on a workload whose sizes drift; the ceiling is that a program
 * cycling through many distinct sizes gets no reuse at all.
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

static struct { void *base; size_t len; } spancache[SPANCACHE_N];
static int    nspan;
static size_t spanbytes;

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
	void *tail = m->head;

	for (uint32_t i = 1; i < m->n; i++)
		tail = *(void **)tail;
	lock();
	*(void **)tail = klass[cls].freelist;
	klass[cls].freelist = m->head;
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
	void *first = klass[cls].freelist, *last;
	uint32_t got = 1;

	if (!first)
		return alloc_small(cls, fresh);
	if (!want)
		want = 1;
	last = first;
	while (got < want && *(void **)last) {
		last = *(void **)last;
		got++;
	}
	klass[cls].freelist = *(void **)last;
	*(void **)last = NULL;
	m->head = *(void **)first;   /* NULL when got == 1, since last == first */
	m->n = got - 1;
	*fresh = 0;
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
static void *spancache_take(size_t len, size_t *got)
{
	int best = -1;
	for (int i = 0; i < nspan; i++) {
		if (spancache[i].len < len || spancache[i].len > len + len / 4)
			continue;
		if (best < 0 || spancache[i].len < spancache[best].len)
			best = i;
	}
	if (best < 0)
		return NULL;
	void *base = spancache[best].base;
	*got = spancache[best].len;
	spanbytes -= spancache[best].len;
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
static int spancache_put(void *base, size_t len)
{
	if (nspan == SPANCACHE_N || len > SPANCACHE_BYTES)
		return 0;
	while (spanbytes + len > SPANCACHE_BYTES) {
		/* Evict the oldest-ish entry. There is no LRU here and the order is
		 * whatever the swap-with-last removal left behind; with sixteen
		 * entries matched on exact size, a real policy would be more code
		 * than the hit rate justifies.
		 * shortcut: no recency. Add it if a workload ever shows the cache
		 * thrashing on a mix of sizes. */
		if (nspan == 0)
			return 0;
		void *old = spancache[0].base;
		size_t olen = spancache[0].len;
		spancache[0] = spancache[--nspan];
		spanbytes -= olen;
		munmap(old, olen);
	}
	spancache[nspan].base = base;
	spancache[nspan].len = len;
	nspan++;
	spanbytes += len;
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
		h->req = size;
		h->cached = 0;    /* different geometry; never interchangeable */
		return p;
	}

	off = align > HDR ? align : HDR;        /* align is a power of two */
	len = round_up(off + size, PAGE);
	size_t got = len;
	base = spancache_take(len, &got);
	if (base) {
		*fresh = 0;
		len = got;   /* we own all of it, and req below reflects that */
	} else {
		base = map_aligned(len);
	}
	if (!base)
		return NULL;
	struct hdr *h = (struct hdr *)base;
	h->magic = MAGIC;
	h->cls = CLS_LARGE;
	h->map_base = base;
	h->map_len = len;
	h->req = len - off;
	h->cached = 1;
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
		int keep = h->cached && spancache_put(b, l);
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
void malloc_stats(void) { }
