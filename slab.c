/*
 * slab - a size-class memory allocator, drop-in for malloc(3) via LD_PRELOAD.
 *
 * This first cut has no size classes at all: every allocation gets its own
 * mmap, with a 64-byte header in front of the payload.  It is correct, it is
 * thread-safe for free (there is no shared mutable state), and it is
 * grotesquely wasteful - the smallest possible malloc costs a whole page,
 * plus four syscalls to get it and one to give it back.  What it does
 * establish is the thing every later commit depends on: how free() finds an
 * allocation's metadata.
 *
 * WHY THE SPAN-ALIGNED HEADER:  the obvious scheme is a header immediately
 * in front of the payload, found by subtracting a constant.  On x86-64 that
 * costs 16 bytes per object once you account for keeping the payload
 * 16-byte aligned, which doubles the footprint of a 16-byte allocation.  So
 * instead every region we map is aligned to a 64 KiB SPAN, the header lives
 * at the base of the span, and the lookup is `p & ~0xffff`.  Right now that
 * buys nothing, because there is one allocation per region.  Once a span
 * holds many objects of one size class (next commit) the same single AND
 * instruction covers all of them, and the per-object cost drops to zero.
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
 * mmap, munmap, memcpy, write and strlen, none of which allocate.  There is
 * also no initialisation step: nothing to race on, and no "am I initialised
 * yet" window for a re-entrant first call to fall into.
 */

#define _GNU_SOURCE
#include <errno.h>
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
#define HDR        64u              /* region header; also the payload offset */
#define MAGIC      0x5b1ab10cu

struct hdr {
	uint32_t magic;
	uint32_t pad;
	void    *map_base;   /* what to hand back to munmap */
	size_t   map_len;    /* how much */
	size_t   req;        /* usable payload bytes */
};
_Static_assert(sizeof(struct hdr) <= HDR, "region header must fit in HDR bytes");
_Static_assert(HDR % 16 == 0, "payload offset must preserve 16-byte alignment");

/* Diagnostics go through write(2): printf would allocate, and we may be
 * called from inside a libc function that is holding stdio's own lock. */
static void note(const char *s) { (void)!write(2, s, strlen(s)); }

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/* mmap `len` (a whole number of pages) at a SPAN_SIZE-aligned address, by
 * over-mapping and trimming. */
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

/* One region per allocation: header at the span-aligned base, payload at
 * base + HDR, so masking the payload pointer lands on the header. */
static void *alloc_map(size_t size)
{
	size_t len = round_up(HDR + size, PAGE);
	char *base = map_aligned(len);
	if (!base)
		return NULL;
	struct hdr *h = (struct hdr *)base;
	h->magic = MAGIC;
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

/* ------------------------------------------------------------------ */

void *malloc(size_t size)
{
	if (size == 0)
		size = 1;             /* a unique freeable pointer, as is customary */
	if (size > SIZE_MAX - SPAN_SIZE - PAGE) { errno = ENOMEM; return NULL; }
	void *p = alloc_map(size);
	if (!p)
		errno = ENOMEM;
	return p;
}

void free(void *p)
{
	if (!p)                       /* free(NULL) is a no-op, per C11 7.22.3.3 */
		return;
	struct hdr *h = hdr_of(p);
	if (!h) {
		/* Not ours.  Can happen if something allocated through a path we
		 * failed to interpose; leaking is survivable, abort()ing is not. */
		static int once;
		if (!once) { once = 1; note("slab: free() of foreign pointer, leaking\n"); }
		return;
	}
	munmap(h->map_base, h->map_len);
}

void *calloc(size_t n, size_t m)
{
	if (m && n > SIZE_MAX / m) { errno = ENOMEM; return NULL; }
	/* Every allocation is a fresh mmap, which the kernel has already
	 * zeroed, so there is nothing to memset. */
	return malloc(n * m);
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
	struct hdr *h = hdr_of(p);
	if (!h) {
		note("slab: realloc() of foreign pointer\n");
		errno = EINVAL;
		return NULL;
	}
	if (size <= h->req)
		return p;
	void *q = malloc(size);
	if (!q)
		return NULL;             /* errno set, p still valid */
	memcpy(q, p, h->req);
	free(p);
	return q;
}
