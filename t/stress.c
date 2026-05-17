/*
 * t/stress.c - the part of the oracle that a shell script cannot express.
 *
 * Mode `stress`: millions of random malloc/calloc/posix_memalign/realloc/
 * free over 2048 live slots, each block filled with a byte pattern derived
 * from a unique tag and verified on every subsequent touch.  This is what
 * actually catches overlapping allocations: if two live blocks share a byte,
 * one of them reads a pattern byte that belongs to the other.
 *
 * Mode `thread`: the same churn on 4 threads, with blocks handed between
 * threads through a shared pool so cross-thread free() is exercised against
 * the global free lists.
 *
 * Mode `tchurn`: create and join N threads one at a time, each allocating a
 * working set across many size classes and freeing all of it.  Nothing is
 * live when it ends, so peak RSS measures exactly what the allocator keeps
 * back per dead thread -- which is what a per-thread cache gets wrong when
 * the cache dies with its thread and the objects on it become unreachable.
 *
 * Mode `bench`: timing, printed as "name ns/op".  Five phases chosen to
 * separate where slab is competitive from where it is not.
 *
 * Mode `check`: semantic conformance.  Alignment of every size from 0 to
 * 4096 plus large and over-aligned cases, calloc zeroing, realloc content
 * preservation across every class boundary in both directions,
 * posix_memalign from 8 bytes to 4 MiB of alignment, free(NULL), malloc(0),
 * out-of-range sizes, and a direct test of the hard invariant: 30000 live
 * blocks at once, sorted by address, no block's payload reaching into the
 * next one's.
 *
 * Builds and passes against glibc malloc too - that is the point, it is an
 * oracle, not a slab test suite.  If a check fails under glibc then the
 * check is wrong, not the allocator.
 */

#define _GNU_SOURCE
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static int failures;

/* gcc knows what malloc/calloc/posix_memalign promise about alignment and
 * zeroing, and will constant-fold a test of those promises into "true".
 * Laundering every pointer through an asm barrier before inspecting it is
 * what keeps this file an oracle instead of a tautology. */
static void *launder(void *p)
{
	__asm__ volatile("" : "+r"(p) : : "memory");
	return p;
}

#define CHECK(cond, ...)                                                      \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL %s:%d: ", __func__, __LINE__);         \
			printf(__VA_ARGS__);                                  \
			putchar('\n');                                        \
			failures++;                                           \
		}                                                             \
	} while (0)

#define ALIGN _Alignof(max_align_t)

/* ---------------- byte pattern ---------------- */

static unsigned char pat(uint32_t tag, size_t i)
{
	return (unsigned char)(tag * 131u + (uint32_t)i * 17u + (uint32_t)(i >> 7) * 251u);
}

/* Blocks up to 8 KiB are filled and checked entirely.  Above that the
 * allocator serves each block from its own mmap, so interior overlap is
 * impossible; we check the first and last 4 KiB (the parts adjacent to
 * metadata and to neighbouring mappings) and skip the middle for speed. */
#define FULL 8192u
#define EDGE 4096u

static void fill(unsigned char *p, uint32_t tag, size_t n)
{
	size_t i;
	if (n <= FULL) {
		for (i = 0; i < n; i++)
			p[i] = pat(tag, i);
		return;
	}
	for (i = 0; i < EDGE; i++)
		p[i] = pat(tag, i);
	for (i = n - EDGE; i < n; i++)
		p[i] = pat(tag, i);
}

static int verify(const unsigned char *p, uint32_t tag, size_t n)
{
	size_t i;
	if (n <= FULL) {
		for (i = 0; i < n; i++)
			if (p[i] != pat(tag, i))
				return 0;
		return 1;
	}
	for (i = 0; i < EDGE; i++)
		if (p[i] != pat(tag, i))
			return 0;
	for (i = n - EDGE; i < n; i++)
		if (p[i] != pat(tag, i))
			return 0;
	return 1;
}

/* First n bytes only - used after realloc, which only promises to preserve
 * min(old, new) bytes. */
static int verify_prefix(const unsigned char *p, uint32_t tag, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i] != pat(tag, i))
			return 0;
	return 1;
}

static uint64_t rnd(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

static size_t pick_size(uint64_t *s)
{
	uint64_t r = rnd(s);
	switch ((r >> 32) % 100) {
	case 0 ... 69:  return 1 + r % 128;
	case 70 ... 89: return 129 + r % 896;
	case 90 ... 97: return 1025 + r % 7168;
	default:        return 8193 + r % 253952;
	}
}

/* ---------------- check mode ---------------- */

static void check_alignment(void)
{
	for (size_t s = 0; s <= 4096; s++) {
		void *p = launder(malloc(s));
		CHECK(p != NULL, "malloc(%zu) returned NULL", s);
		CHECK(((uintptr_t)p & (ALIGN - 1)) == 0,
		      "malloc(%zu) = %p not %zu-aligned", s, p, (size_t)ALIGN);
		if (p)
			memset(p, 0x5a, s);  /* must be writable for s bytes */
		free(p);
	}
	size_t big[] = {8191, 8192, 8193, 65536, 65600, 1u << 20, (1u << 22) + 7};
	for (unsigned i = 0; i < sizeof big / sizeof *big; i++) {
		void *p = launder(malloc(big[i]));
		CHECK(p != NULL, "malloc(%zu) returned NULL", big[i]);
		CHECK(((uintptr_t)p & (ALIGN - 1)) == 0,
		      "malloc(%zu) = %p misaligned", big[i], p);
		if (p)
			memset(p, 0x5a, big[i]);
		free(p);
	}
}

static void check_calloc(void)
{
	/* Dirty a block of each size first, then free it, so a calloc that
	 * forgets to zero recycled memory is caught rather than passing by
	 * accident on fresh pages. */
	size_t sizes[] = {1, 8, 16, 17, 64, 129, 255, 1000, 4096, 8192, 8193, 70000};
	for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
		size_t s = sizes[i];
		unsigned char *d = malloc(s);
		CHECK(d != NULL, "malloc(%zu)", s);
		if (d)
			memset(d, 0xaa, s);
		free(d);
		/* volatile: gcc knows calloc() returns zeroed memory and will
		 * delete this loop outright if it can see through the call.
		 * This was found by mutation-testing the oracle - a slab build
		 * whose calloc never zeroed anything passed until the reads
		 * were made volatile. */
		volatile unsigned char *p = calloc(1, s);
		CHECK(p != NULL, "calloc(1,%zu)", s);
		if (!p)
			continue;
		size_t bad = (size_t)-1;
		for (size_t j = 0; j < s; j++)
			if (p[j]) { bad = j; break; }
		CHECK(bad == (size_t)-1, "calloc(1,%zu) byte %zu = 0x%02x",
		      s, bad, bad == (size_t)-1 ? 0 : p[bad]);
		free((void *)p);
	}
	/* volatile so gcc cannot fold these into -Walloc-size warnings */
	volatile size_t huge = SIZE_MAX;
	CHECK(calloc(huge / 2, 4) == NULL, "calloc overflow not rejected");
	CHECK(calloc(huge, huge) == NULL, "calloc overflow not rejected");
	void *z = calloc(0, 0);
	free(z);                    /* must be freeable whatever it returns */
}

static void check_realloc(void)
{
	/* realloc(NULL, n) == malloc(n) */
	unsigned char *p = realloc(NULL, 100);
	CHECK(p != NULL, "realloc(NULL,100) returned NULL");
	fill(p, 7, 100);

	/* grow across many class boundaries, content preserved each time */
	size_t cur = 100;
	size_t steps[] = {101, 128, 129, 300, 1000, 4000, 8192, 8193, 40000, 300000};
	for (unsigned i = 0; i < sizeof steps / sizeof *steps; i++) {
		unsigned char *q = realloc(p, steps[i]);
		CHECK(q != NULL, "realloc to %zu returned NULL", steps[i]);
		if (!q)
			return;
		p = q;
		size_t keep = cur < steps[i] ? cur : steps[i];
		if (keep > EDGE)
			keep = EDGE;
		CHECK(verify_prefix(p, 7, keep),
		      "realloc %zu -> %zu lost content", cur, steps[i]);
		cur = steps[i];
		fill(p, 7, cur);
	}
	/* shrink, content preserved up to the new size */
	size_t shrink[] = {100000, 9000, 8192, 500, 64, 1};
	for (unsigned i = 0; i < sizeof shrink / sizeof *shrink; i++) {
		unsigned char *q = realloc(p, shrink[i]);
		CHECK(q != NULL, "realloc shrink to %zu returned NULL", shrink[i]);
		if (!q)
			return;
		p = q;
		size_t keep = shrink[i] < EDGE ? shrink[i] : EDGE;
		CHECK(verify_prefix(p, 7, keep),
		      "realloc shrink %zu -> %zu lost content", cur, shrink[i]);
		cur = shrink[i];
		fill(p, 7, cur);
	}
	/* realloc(p, 0): documented to free p and return NULL, as glibc does */
	CHECK(realloc(p, 0) == NULL, "realloc(p,0) did not return NULL");

	/* malloc_usable_size must not under-report */
	for (size_t s = 1; s <= 8192; s++) {
		unsigned char *a = malloc(s);
		size_t u = malloc_usable_size(a);
		CHECK(u >= s, "usable_size(%zu) = %zu < %zu", s, u, s);
		memset(a, 0x33, u);        /* writing all of it must be safe */
		free(a);
	}
}

static void check_free_null(void)
{
	/* CHECK(1, ...) cannot fail, and said so in the count. The real test
	 * is that the two free(NULL) calls above do not abort -- reaching this
	 * line IS the result -- but an assertion whose condition is the
	 * literal 1 inflates the check count without adding a check. Allocate
	 * after them instead: a free(NULL) that corrupted the allocator shows
	 * up as the next malloc failing or returning something unusable. */
	void *p;

	free(NULL);
	free(NULL);
	p = malloc(32);
	CHECK(p != NULL, "free(NULL) left the allocator usable");
	if (p) {
		memset(p, 0xab, 32);
		CHECK(((unsigned char *)p)[31] == 0xab,
		      "...and the block it then returned is writable");
		free(p);
	}
}

static void check_memalign(void)
{
	for (size_t a = sizeof(void *); a <= (1u << 22); a *= 2) {
		for (size_t s = 1; s <= 70000; s = s * 7 + 3) {
			void *p = NULL;
			int e = posix_memalign(&p, a, s);
			p = launder(p);
			CHECK(e == 0, "posix_memalign(%zu,%zu) = %d", a, s, e);
			if (e)
				continue;
			CHECK(((uintptr_t)p & (a - 1)) == 0,
			      "posix_memalign(%zu,%zu) = %p misaligned", a, s, p);
			memset(p, 0x77, s);
			free(p);
		}
	}
	void *p = NULL;
	CHECK(posix_memalign(&p, 3, 10) != 0, "align 3 accepted");
	CHECK(posix_memalign(&p, 24, 10) != 0, "align 24 accepted");
	CHECK(posix_memalign(&p, 0, 10) != 0, "align 0 accepted");
	for (size_t a = 16; a <= (1u << 20); a *= 4) {
		void *q = launder(aligned_alloc(a, a * 2));
		CHECK(q != NULL, "aligned_alloc(%zu)", a);
		CHECK(((uintptr_t)q & (a - 1)) == 0, "aligned_alloc(%zu) misaligned", a);
		if (q)
			memset(q, 1, a * 2);
		free(q);
	}
}

static void check_malloc0(void)
{
	void *a = launder(malloc(0)), *b = launder(malloc(0));
	CHECK(a != NULL && b != NULL, "malloc(0) returned NULL");
	CHECK(a != b, "malloc(0) returned the same pointer twice");
	free(a);
	free(b);
	volatile size_t huge = SIZE_MAX;
	CHECK(malloc(huge) == NULL, "malloc(SIZE_MAX) succeeded");
	CHECK(malloc(huge - 4096) == NULL, "absurd malloc succeeded");
}

/* The hard invariant, tested directly: hold a few tens of thousands of live
 * blocks of every interesting size at once, sort them by address, and assert
 * that no block's payload reaches into the next one's. */
struct ext { unsigned char *p; size_t n; };

static int by_addr(const void *a, const void *b)
{
	const struct ext *x = a, *y = b;
	return x->p < y->p ? -1 : x->p > y->p ? 1 : 0;
}

static void check_no_overlap(void)
{
	enum { N = 30000 };
	struct ext *v = malloc(N * sizeof *v);
	uint64_t s = 99;
	for (int i = 0; i < N; i++) {
		v[i].n = pick_size(&s);
		v[i].p = launder(malloc(v[i].n));
		CHECK(v[i].p != NULL, "malloc(%zu)", v[i].n);
		fill(v[i].p, (uint32_t)i, v[i].n);
	}
	for (int i = 0; i < N; i++)
		CHECK(verify(v[i].p, (uint32_t)i, v[i].n),
		      "block %d (%zu bytes) clobbered", i, v[i].n);
	qsort(v, N, sizeof *v, by_addr);
	int overlaps = 0;
	for (int i = 1; i < N; i++)
		if (v[i - 1].p + v[i - 1].n > v[i].p)
			overlaps++;
	CHECK(overlaps == 0, "%d overlapping live allocations", overlaps);
	for (int i = 0; i < N; i++)
		free(v[i].p);
	free(v);
}

/* ---------------- stress mode ---------------- */

struct slot { unsigned char *p; size_t n; uint32_t tag; };

static void churn(struct slot *v, unsigned nslots, long iters, uint64_t seed,
		  struct slot *shared, unsigned nshared, pthread_mutex_t *mu)
{
	uint64_t s = seed | 1;
	uint32_t tag = (uint32_t)seed;

	for (long it = 0; it < iters; it++) {
		unsigned i = (unsigned)(rnd(&s) % nslots);
		struct slot *b = &v[i];

		if (!b->p) {
			b->n = pick_size(&s);
			b->tag = ++tag;
			unsigned mode = (unsigned)(rnd(&s) % 10);
			if (mode == 0) {
				b->p = calloc(1, b->n);
				if (b->p) {
					volatile unsigned char *z = b->p;
					for (size_t j = 0; j < b->n; j++)
						if (z[j]) {
							printf("  FAIL calloc(%zu) not zeroed at %zu\n", b->n, j);
							failures++;
							break;
						}
				}
			} else if (mode == 1) {
				void *q = NULL;
				size_t a = (size_t)16 << (rnd(&s) % 8);
				if (posix_memalign(&q, a, b->n) == 0) {
					q = launder(q);
					if ((uintptr_t)q & (a - 1)) {
						printf("  FAIL memalign %zu misaligned\n", a);
						failures++;
					}
					b->p = q;
				}
			} else {
				b->p = malloc(b->n);
			}
			if (!b->p) {
				printf("  FAIL out of memory at iter %ld\n", it);
				failures++;
				return;
			}
			fill(b->p, b->tag, b->n);
			continue;
		}

		if (!verify(b->p, b->tag, b->n)) {
			printf("  FAIL block %u (%zu bytes, tag %u) clobbered at iter %ld\n",
			       i, b->n, b->tag, it);
			failures++;
			b->p = NULL;        /* stop cascading on the same block */
			continue;
		}

		unsigned act = (unsigned)(rnd(&s) % 10);
		if (act < 4) {
			free(b->p);
			b->p = NULL;
		} else if (act < 8) {
			size_t nn = pick_size(&s);
			unsigned char *q = realloc(b->p, nn);
			if (!q) {
				printf("  FAIL realloc(%zu -> %zu) failed\n", b->n, nn);
				failures++;
				return;
			}
			size_t keep = b->n < nn ? b->n : nn;
			if (keep > EDGE)
				keep = EDGE;
			if (!verify_prefix(q, b->tag, keep)) {
				printf("  FAIL realloc %zu -> %zu lost content\n", b->n, nn);
				failures++;
			}
			b->p = q;
			b->n = nn;
			fill(b->p, b->tag, b->n);
		} else if (shared) {
			/* Hand this block to whichever thread takes the slot
			 * next, and adopt whatever was there - so a block is
			 * routinely freed by a thread other than the one that
			 * allocated it, which is the case a per-thread cache
			 * would get wrong and a global free list must not. */
			unsigned j = (unsigned)(rnd(&s) % nshared);
			pthread_mutex_lock(mu);
			struct slot t = shared[j];
			shared[j] = *b;
			pthread_mutex_unlock(mu);
			if (t.p) {
				if (!verify(t.p, t.tag, t.n)) {
					printf("  FAIL shared block clobbered\n");
					failures++;
				}
				free(t.p);
			}
			b->p = NULL;
		}
	}
	for (unsigned i = 0; i < nslots; i++)
		if (v[i].p) {
			if (!verify(v[i].p, v[i].tag, v[i].n)) {
				printf("  FAIL block %u clobbered at drain\n", i);
				failures++;
			}
			free(v[i].p);
			v[i].p = NULL;
		}
}

#define NSLOT 2048
#define NSHARED 128

static struct slot shared[NSHARED];
static pthread_mutex_t shmu = PTHREAD_MUTEX_INITIALIZER;
static long thr_iters;

static void *worker(void *arg)
{
	struct slot *v = calloc(NSLOT, sizeof *v);
	churn(v, NSLOT, thr_iters, (uint64_t)(uintptr_t)arg * 0x9e3779b97f4a7c15ull + 1,
	      shared, NSHARED, &shmu);
	free(v);
	return NULL;
}

/* ---------------- bench mode ---------------- */

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void report(const char *name, double secs, long ops)
{
	printf("%-22s %8.1f ns/op\n", name, secs / ops * 1e9);
}

static void bench_small(long iters)
{
	void *v[1024] = {0};
	uint64_t s = 1;
	double t0 = now();
	for (long i = 0; i < iters; i++) {
		unsigned k = (unsigned)(rnd(&s) % 1024);
		if (v[k]) { free(v[k]); v[k] = NULL; }
		else { v[k] = malloc(8 + rnd(&s) % 248); *(char *)v[k] = 1; }
	}
	double t = now() - t0;
	for (unsigned k = 0; k < 1024; k++)
		free(v[k]);
	report("small-churn", t, iters);
}

static void bench_mixed(long iters)
{
	void *v[512] = {0};
	size_t n[512] = {0};
	uint64_t s = 2;
	double t0 = now();
	for (long i = 0; i < iters; i++) {
		unsigned k = (unsigned)(rnd(&s) % 512);
		if (v[k]) { free(v[k]); v[k] = NULL; }
		else { n[k] = pick_size(&s); v[k] = malloc(n[k]);
		       memset(v[k], 1, n[k] < 64 ? n[k] : 64); }
	}
	double t = now() - t0;
	for (unsigned k = 0; k < 512; k++)
		free(v[k]);
	report("mixed-sizes", t, iters);
}

static void bench_grow(long iters)
{
	double t0 = now();
	long ops = 0;
	for (long i = 0; i < iters; i++) {
		size_t n = 8;
		char *p = malloc(n);
		while (n < 65536) {
			size_t m = n * 2;
			p = realloc(p, m);
			p[m - 1] = 1;
			n = m;
			ops++;
		}
		free(p);
	}
	report("realloc-grow", now() - t0, ops);
}

static void bench_large(long iters)
{
	void *v[64] = {0};
	uint64_t s = 3;
	double t0 = now();
	for (long i = 0; i < iters; i++) {
		unsigned k = (unsigned)(rnd(&s) % 64);
		if (v[k]) { free(v[k]); v[k] = NULL; }
		else { size_t n = 16384 + rnd(&s) % (1u << 20);
		       v[k] = malloc(n); *(char *)v[k] = 1; }
	}
	double t = now() - t0;
	for (unsigned k = 0; k < 64; k++)
		free(v[k]);
	report("large-churn", t, iters);
}

static long mt_iters;

static void *mt_worker(void *arg)
{
	void *v[256] = {0};
	uint64_t s = (uint64_t)(uintptr_t)arg + 7;
	for (long i = 0; i < mt_iters; i++) {
		unsigned k = (unsigned)(rnd(&s) % 256);
		if (v[k]) { free(v[k]); v[k] = NULL; }
		else { v[k] = malloc(8 + rnd(&s) % 248); *(char *)v[k] = 1; }
	}
	for (unsigned k = 0; k < 256; k++)
		free(v[k]);
	return NULL;
}

static void bench_threads(long iters, int nthreads)
{
	pthread_t th[8];
	mt_iters = iters;
	double t0 = now();
	for (int i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, mt_worker, (void *)(uintptr_t)i);
	for (int i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	report("4-thread-churn", now() - t0, iters * nthreads);
}

/* ---------------- thread-churn mode ---------------- */

/*
 * Touch a wide spread of size classes and free every byte of it.  Spread,
 * because a per-thread cache is per class: a worker that used one class
 * would leave only one magazine behind and hide the cost of the other 31.
 */
static void *tchurn_worker(void *arg)
{
	enum { NB = 400 };
	static __thread void *v[NB];
	(void)arg;

	for (int round = 0; round < 3; round++) {
		for (int i = 0; i < NB; i++) {
			v[i] = malloc(16 + (size_t)((i * 37) % 8100));
			if (v[i])
				memset(v[i], 0xa5, 16);
		}
		for (int i = 0; i < NB; i++)
			free(v[i]);
	}
	return NULL;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "check";
	long n = argc > 2 ? atol(argv[2]) : 0;

	/* Is the allocator under test actually ours?
	 *
	 * NOTHING ELSE IN THIS SUITE ASKS. Tier 1 compares a command's output
	 * with and against LD_PRELOAD and demands they match; tier 2 runs this
	 * binary under preload and demands exit 0; and the suite separately
	 * REQUIRES that both also pass against glibc malloc, because an oracle
	 * that glibc fails is a wrong oracle. All three are satisfied by a
	 * shared object that loads cleanly and defines no malloc at all.
	 *
	 * Verified by building one: `gcc -shared -fPIC` over a file containing
	 * a single unused variable, copied over libslab.so, gives
	 *
	 *     31 passed, 0 failed
	 *
	 * The RSS ratios come out at about 1.00 against limits of 3.0 and 1.5,
	 * the munmap counts match because both sides are glibc, and tier 3's
	 * mutants are compiled from slab.c rather than from the .so, so they
	 * cannot notice either. The whole suite was green on a functionless
	 * library.
	 *
	 * malloc_usable_size is the cheapest observable that separates them:
	 * slab's header is 16 bytes for a 1-byte request where glibc's is 24.
	 * The number is not asserted here -- `interposed` prints it and
	 * t/run.sh compares the preloaded value against the un-preloaded one,
	 * so the check survives a libc whose own answer is different. */
	if (!strcmp(mode, "interposed")) {
		void *p = malloc(1);
		printf("%zu\n", malloc_usable_size(p));
		free(p);
		return 0;
	}

	if (!strcmp(mode, "check")) {
		check_free_null();
		check_alignment();
		check_calloc();
		check_realloc();
		check_memalign();
		check_malloc0();
		check_no_overlap();
	} else if (!strcmp(mode, "stress")) {
		static struct slot v[NSLOT];
		churn(v, NSLOT, n ? n : 2000000, 0xfeedfaceull, NULL, 0, NULL);
	} else if (!strcmp(mode, "thread")) {
		pthread_t th[4];
		thr_iters = n ? n : 400000;
		for (int i = 0; i < 4; i++)
			pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)(i + 1));
		for (int i = 0; i < 4; i++)
			pthread_join(th[i], NULL);
		for (unsigned i = 0; i < NSHARED; i++)
			if (shared[i].p) {
				if (!verify(shared[i].p, shared[i].tag, shared[i].n)) {
					printf("  FAIL shared residue clobbered\n");
					failures++;
				}
				free(shared[i].p);
			}
	} else if (!strcmp(mode, "tchurn")) {
		long threads = n ? n : 300;
		for (long i = 0; i < threads; i++) {
			pthread_t t;
			if (pthread_create(&t, NULL, tchurn_worker, NULL) != 0) {
				printf("  FAIL pthread_create at %ld\n", i);
				failures++;
				break;
			}
			pthread_join(t, NULL);
		}
	} else if (!strcmp(mode, "bench")) {
		long it = n ? n : 2000000;
		bench_small(it);
		bench_mixed(it / 4);
		bench_grow(it / 200);
		bench_large(it / 500);
		bench_threads(it / 4, 4);
		return 0;
	} else {
		fprintf(stderr,
			"usage: stress check|stress|thread|tchurn|bench [n]\n");
		return 2;
	}

	if (failures)
		printf("  %d failure(s) in mode %s\n", failures, mode);
	return failures ? 1 : 0;
}
