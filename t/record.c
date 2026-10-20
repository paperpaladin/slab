/*
 * record - an LD_PRELOAD that writes down every allocator call a program
 * makes, and forwards the call to glibc's own allocator.
 *
 * The point is the corpus.  t/stress bench is six loops somebody wrote down
 * as a guess at how programs allocate; a trace taken out of gcc or CPython
 * is not a guess.  Recording it here and replaying it in t/replay separates
 * the allocator's cost from the program's own work, which timing the program
 * end to end cannot do: allocation is a few percent of what gcc spends its
 * time on, so an allocator twice as slow moves the wall clock by noise.
 *
 * It forwards to __libc_malloc and friends rather than dlsym(RTLD_NEXT),
 * because dlsym can allocate and this file is called from inside malloc.
 * They are public GLIBC_2.2.5 symbols; slab does not support other libcs
 * anyway (see PLAN's Non-goals).
 *
 * Nothing here may allocate, for the same reason slab.c may not: we are
 * holding a lock that malloc is about to need.  getenv, strtoull, open,
 * write and the atomics are all that is used.
 *
 * SLAB_TRACE IS A PREFIX, NOT A FILENAME, and that cost a wrong trace to
 * learn.  gcc is a driver that forks cc1 and as, and LD_PRELOAD is
 * inherited, so every one of them opened the same path with O_TRUNC and
 * wrote from offset 0.  cc1 wrote 673094 records; the driver finished
 * afterwards and overwrote the first 406 of them with its own, which is why
 * the trace then contained 254 frees of pointers nothing in it had
 * allocated.  The file is <prefix>.<pid> now, and t/trace.py refuses a file
 * with two trailers in it, because that is what this looked like from the
 * outside.
 *
 * WHAT IS NOT RECORDED, which matters when reading a replay number.  A
 * program reaching the allocator through a path we do not interpose still
 * reaches our free(), so the trace contains frees of pointers that were
 * never allocated in it.  t/trace.py counts those and prints the figure
 * rather than discarding them silently: a trace with many of them is a trace
 * whose lifetimes are wrong.
 *
 * Copyright (C) 2026  PLACEHOLDER.  Licensed under the GPLv3 (see LICENSE).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void  __libc_free(void *);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void *__libc_memalign(size_t, size_t);

enum { OP_END = 0, OP_MALLOC, OP_FREE, OP_CALLOC, OP_REALLOC, OP_MEMALIGN };

struct rec { uint64_t op, a, b, r; };

#define NBUF 8192
static struct rec buf[NBUF];
static unsigned nbuf;
static int trfd = -1, offline;
static uint64_t nrec, ndrop, cap = 2000000;
static volatile int lk;

static void lock(void)
{
	while (__atomic_exchange_n(&lk, 1, __ATOMIC_ACQUIRE))
		;
}

static void unlock(void) { __atomic_store_n(&lk, 0, __ATOMIC_RELEASE); }

/* Decimal, by hand: snprintf is stdio and stdio allocates. */
static size_t dec(char *out, uint64_t v)
{
	char tmp[24];
	size_t n = 0, i;

	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	for (i = 0; i < n; i++)
		out[i] = tmp[n - 1 - i];
	return n;
}

static void flush(void)
{
	if (nbuf && trfd >= 0)
		(void)!write(trfd, buf, nbuf * sizeof *buf);
	nbuf = 0;
}

static void emit(uint64_t op, uint64_t a, uint64_t b, uint64_t r)
{
	lock();
	if (offline) {
		unlock();
		return;
	}
	if (trfd < 0) {
		/* Opened on the first call rather than in a constructor: a
		 * constructor runs after some of libc's own startup
		 * allocation, and there is no reason to miss it. */
		const char *path = getenv("SLAB_TRACE");
		const char *max = getenv("SLAB_TRACE_MAX");
		char name[4096];
		size_t n = 0;

		if (!path) {
			offline = 1;      /* loaded with nowhere to write */
			unlock();
			return;
		}
		if (max)
			cap = strtoull(max, NULL, 10);
		while (path[n] && n < sizeof name - 24) {
			name[n] = path[n];
			n++;
		}
		name[n++] = '.';
		n += dec(name + n, (uint64_t)getpid());
		name[n] = 0;
		trfd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (trfd < 0) {
			offline = 1;
			unlock();
			return;
		}
	}
	if (nrec >= cap) {
		ndrop++;          /* counted, and reported in the trailer */
		unlock();
		return;
	}
	buf[nbuf].op = op;
	buf[nbuf].a = a;
	buf[nbuf].b = b;
	buf[nbuf].r = r;
	nrec++;
	if (++nbuf == NBUF)
		flush();
	unlock();
}

/* The trailer says how many records were written and how many were dropped
 * at the cap, so a truncated trace cannot be mistaken for a whole one. */
__attribute__((destructor)) static void fini(void)
{
	lock();
	if (trfd >= 0) {
		buf[nbuf].op = OP_END;
		buf[nbuf].a = nrec;
		buf[nbuf].b = ndrop;
		buf[nbuf].r = 0;
		nbuf++;
		flush();
		close(trfd);
		trfd = -1;
	}
	offline = 1;      /* frees after this point are past the trailer */
	unlock();
}

#define U(p) ((uint64_t)(uintptr_t)(p))

void *malloc(size_t n)
{
	void *p = __libc_malloc(n);
	emit(OP_MALLOC, n, 0, U(p));
	return p;
}

void free(void *p)
{
	emit(OP_FREE, U(p), 0, 0);
	__libc_free(p);
}

void *calloc(size_t n, size_t m)
{
	void *p = __libc_calloc(n, m);
	emit(OP_CALLOC, n, m, U(p));
	return p;
}

void *realloc(void *p, size_t n)
{
	void *q = __libc_realloc(p, n);
	emit(OP_REALLOC, U(p), n, U(q));
	return q;
}

void *reallocarray(void *p, size_t n, size_t m)
{
	void *q = __libc_realloc(p, n * m);
	emit(OP_REALLOC, U(p), n * m, U(q));
	return q;
}

int posix_memalign(void **out, size_t align, size_t n)
{
	void *p;

	if (align % sizeof(void *) || (align & (align - 1)) || align == 0)
		return 22;                      /* EINVAL */
	p = __libc_memalign(align, n);
	if (!p)
		return 12;                      /* ENOMEM */
	*out = p;
	emit(OP_MEMALIGN, align, n, U(p));
	return 0;
}

void *aligned_alloc(size_t align, size_t n)
{
	void *p = __libc_memalign(align, n);

	emit(OP_MEMALIGN, align, n, U(p));
	return p;
}

void *memalign(size_t align, size_t n) { return aligned_alloc(align, n); }
