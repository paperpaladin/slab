/*
 * t/stress.c - the part of the oracle that a shell script cannot express.
 *
 * Mode `stress`: millions of random malloc/calloc/posix_memalign/realloc/
 * free over 2048 live slots, each block filled with a byte pattern derived
 * from a unique tag and verified on every subsequent touch.  This is what
 * actually catches overlapping allocations: if two live blocks share a byte,
 * one of them reads a pattern byte that belongs to the other.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

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
		void *p = malloc(s);
		CHECK(p != NULL, "malloc(%zu) returned NULL", s);
		CHECK(((uintptr_t)p & (ALIGN - 1)) == 0,
		      "malloc(%zu) = %p not %zu-aligned", s, p, (size_t)ALIGN);
		if (p)
			memset(p, 0x5a, s);  /* must be writable for s bytes */
		free(p);
	}
	size_t big[] = {8191, 8192, 8193, 65536, 65600, 1u << 20, (1u << 22) + 7};
	for (unsigned i = 0; i < sizeof big / sizeof *big; i++) {
		void *p = malloc(big[i]);
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
		unsigned char *p = calloc(1, s);
		CHECK(p != NULL, "calloc(1,%zu)", s);
		if (!p)
			continue;
		size_t bad = (size_t)-1;
		for (size_t j = 0; j < s; j++)
			if (p[j]) { bad = j; break; }
		CHECK(bad == (size_t)-1, "calloc(1,%zu) byte %zu = 0x%02x",
		      s, bad, bad == (size_t)-1 ? 0 : p[bad]);
		free(p);
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
	free(NULL);
	free(NULL);
	CHECK(1, "free(NULL) survived");
}

static void check_memalign(void)
{
	for (size_t a = sizeof(void *); a <= (1u << 22); a *= 2) {
		for (size_t s = 1; s <= 70000; s = s * 7 + 3) {
			void *p = NULL;
			int e = posix_memalign(&p, a, s);
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
		void *q = aligned_alloc(a, a * 2);
		CHECK(q != NULL, "aligned_alloc(%zu)", a);
		CHECK(((uintptr_t)q & (a - 1)) == 0, "aligned_alloc(%zu) misaligned", a);
		if (q)
			memset(q, 1, a * 2);
		free(q);
	}
}

static void check_malloc0(void)
{
	void *a = malloc(0), *b = malloc(0);
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
		v[i].p = malloc(v[i].n);
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

static void churn(struct slot *v, unsigned nslots, long iters, uint64_t seed)
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
					for (size_t j = 0; j < b->n; j++)
						if (b->p[j]) {
							printf("  FAIL calloc(%zu) not zeroed at %zu\n", b->n, j);
							failures++;
							break;
						}
				}
			} else if (mode == 1) {
				void *q = NULL;
				size_t a = (size_t)16 << (rnd(&s) % 8);
				if (posix_memalign(&q, a, b->n) == 0) {
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

		if (rnd(&s) % 2) {
			free(b->p);
			b->p = NULL;
		} else {
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

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "check";
	long n = argc > 2 ? atol(argv[2]) : 0;

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
		churn(v, NSLOT, n ? n : 2000000, 0xfeedfaceull);
	} else {
		fprintf(stderr, "usage: stress check|stress [iters]\n");
		return 2;
	}

	if (failures)
		printf("  %d failure(s) in mode %s\n", failures, mode);
	return failures ? 1 : 0;
}
