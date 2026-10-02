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
 * Mode `bench`: timing, printed as "name ns/op".  Six phases chosen to
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
#include <fcntl.h>
#include <sys/auxv.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

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

/* ---------------- huge mode ----------------
 *
 * SLAB-SPECIFIC, and the kernel is the oracle: AnonHugePages in
 * /proc/self/smaps_rollup is the kernel saying how much of this process is
 * actually backed by 2 MiB pages.  mmap and madvise returning 0 says
 * nothing -- MADV_HUGEPAGE is advice, and on a kernel with
 * transparent_hugepage=never it is advice that is ignored.
 *
 * That is also why the mode SKIPS rather than fails when THP is off or
 * set to "always": with "never" nothing can be backed, and with "always"
 * every allocator gets huge pages without asking and the check would be
 * about the kernel's default rather than about slab's madvise.
 */
static int thp_madvise_mode(void)
{
	FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
	char line[128] = "";

	if (!f)
		return 0;
	if (!fgets(line, sizeof line, f))
		line[0] = 0;
	fclose(f);
	return strstr(line, "[madvise]") != NULL;
}

static long smaps_rollup(const char *key)
{
	FILE *f = fopen("/proc/self/smaps_rollup", "r");
	char line[256];
	long v = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, strlen(key))) {
			if (sscanf(line + strlen(key), "%ld", &v) != 1)
				v = -1;
			break;
		}
	fclose(f);
	return v;
}

static void check_huge(long nobj)
{
	void **v;
	long i, before, after, want;

	if (!thp_madvise_mode()) {
		printf("huge: SKIP, transparent_hugepage is not in madvise "
		       "mode, so MADV_HUGEPAGE proves nothing either way\n");
		return;
	}
	v = malloc((size_t)nobj * sizeof *v);
	CHECK(v != NULL, "could not allocate the slot array");
	if (!v)
		return;
	memset(v, 0, (size_t)nobj * sizeof *v);
	before = smaps_rollup("AnonHugePages:");
	CHECK(before >= 0, "no AnonHugePages line in /proc/self/smaps_rollup");
	if (before < 0)
		return;
	for (i = 0; i < nobj; i++) {
		v[i] = malloc(64);
		if (!v[i]) {
			CHECK(0, "malloc(64) failed at object %ld", i);
			return;
		}
		*(unsigned char *)v[i] = 1;
	}
	after = smaps_rollup("AnonHugePages:");
	/* Three quarters of the bytes asked for.  Measured at 26624 KiB for
	 * 25000 KiB of objects, so 1.4x of headroom for a kernel that cannot
	 * find a free 2 MiB block for every arena -- and tight enough to
	 * catch the mutant that aligns arenas to a span instead of a huge
	 * page, which still gets 12288 KiB (49%) because a 4 MiB region
	 * happens to contain one aligned 2 MiB block wherever it starts.
	 * A quarter was the first threshold here and that mutant survived it. */
	want = nobj * 64 / 1024 * 3 / 4;
	printf("huge: AnonHugePages %ld -> %ld KiB over %ld KiB of 64-byte "
	       "objects\n", before, after, nobj * 64 / 1024);
	CHECK(after - before >= want,
	      "the kernel backed %ld KiB with huge pages where at least %ld "
	      "was expected: MADV_HUGEPAGE on the arenas is not taking effect",
	      after - before, want);
	for (i = 0; i < nobj; i++)
		free(v[i]);
	free(v);
}

/* ---------------- frag mode ----------------
 *
 * SLAB-SPECIFIC. The README argues that four size classes per octave above
 * 128 bytes "caps internal fragmentation at 1/5". That was an argument about
 * a table, not a measurement, and this mode turns it into both halves of a
 * measurement:
 *
 * 1. Exhaustively, for every request size from 1 to MAXSMALL:
 *    malloc_usable_size must be within 15 bytes of the request below 129
 *    (the classes are exact 16-byte steps there) and within a factor of 5/4
 *    above it. 8192 sizes, not a sample. glibc does not satisfy the first
 *    half -- usable(malloc(1)) is 24 there -- so this is slab's claim about
 *    slab's table.
 * 2. On a workload, against slab's own per-span accounting read back out of
 *    malloc_stats(). Two dumps, one before the workload and one with it
 *    live, so whatever the C library allocated at startup cancels out
 *    instead of being guessed at. The delta in live objects must equal the
 *    number allocated EXACTLY, and the delta in live bytes must equal the
 *    sum of malloc_usable_size over them exactly -- the same quantity
 *    reached by two unrelated paths, per-span counters against per-object
 *    lookups, so a disagreement is a bug in one of them.
 */
static void dump_stats(const char *path, int append)
{
	int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC),
		      0600);
	int saved;

	CHECK(fd >= 0, "could not open %s for the stats dump", path);
	if (fd < 0)
		return;
	saved = dup(2);
	fflush(NULL);
	dup2(fd, 2);
	malloc_stats();
	fflush(NULL);
	dup2(saved, 2);
	close(saved);
	close(fd);
}

/* The nth "slab: small ..." summary line in the dump.  `live` and `bytes`
 * come back net of the magazines, because an object on a magazine has been
 * freed by the program and only slab still thinks it is out. */
static int read_small(const char *path, int nth, size_t *live, size_t *bytes,
		      size_t *spans, size_t *mapped)
{
	FILE *f = fopen(path, "r");
	char line[8192];
	int seen = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		size_t a, b, mo, mb, c, d;
		if (sscanf(line, "slab: small live=%zu livebytes=%zu "
			   "magazined=%zu magbytes=%zu spans=%zu "
			   "mappedbytes=%zu", &a, &b, &mo, &mb, &c, &d) != 6)
			continue;
		if (++seen != nth)
			continue;
		*live = a - mo; *bytes = b - mb; *spans = c; *mapped = d;
		fclose(f);
		return 1;
	}
	fclose(f);
	return 0;
}

/* The "slab: geometry ..." line of a dump. */
static int read_geometry(const char *path, size_t *pg, size_t *span)
{
	FILE *f = fopen(path, "r");
	char line[8192];
	int got = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		size_t a, b;
		if (sscanf(line, "slab: geometry page=%zu span=%zu",
			   &a, &b) != 2)
			continue;
		*pg = a; *span = b;
		got = 1;
		break;
	}
	fclose(f);
	return got;
}

/*
 * The page size slab is using, against the two places the C library will
 * say what it is.  4096 is no longer compiled in; it comes from AT_PAGESZ.
 *
 * This is the whole oracle available on an x86-64 machine and it is worth
 * being plain about that: every kernel this runs on has 4096-byte pages, so
 * what is checked is that slab agrees with glibc about the number, not that
 * it would behave on the 16 KiB- or 64 KiB-page arm64 kernel the change is
 * for.  A mutant that hardcodes a different number is caught; a kernel that
 * reports a different number cannot be tried here.
 */
static void check_geometry(const char *path)
{
	size_t pg = 0, span = 0;
	long sc = sysconf(_SC_PAGESIZE);
	unsigned long ax = getauxval(AT_PAGESZ);

	if (!read_geometry(path, &pg, &span)) {
		CHECK(0, "the dump has no 'slab: geometry' line");
		return;
	}
	printf("frag: page=%zu, span=%zu (sysconf says %ld, AT_PAGESZ says "
	       "%lu)\n", pg, span, sc, ax);
	CHECK(sc > 0 && pg == (size_t)sc,
	      "slab is rounding to a %zu-byte page where sysconf(_SC_PAGESIZE) "
	      "says %ld", pg, sc);
	CHECK(ax == 0 || pg == (size_t)ax,
	      "slab is rounding to a %zu-byte page where AT_PAGESZ says %lu",
	      pg, ax);
	CHECK(pg && span % pg == 0,
	      "a %zu-byte span is not a whole number of %zu-byte pages, so a "
	      "span cannot be given back to the kernel at all", span, pg);
}

static void check_frag(const char *path)
{
	enum { NOBJ = 20000 };
	size_t worst_num = 1, worst_den = 1, worst_at = 0, worst_slack = 0;
	size_t l0, b0, s0, m0, l1, b1, s1, m1;
	size_t requested = 0, usable = 0;
	void **v;
	size_t i;

	/* (1) every size, not a sample. */
	for (size_t sz = 1; sz <= 8192; sz++) {
		void *q = launder(malloc(sz));
		size_t u;
		CHECK(q != NULL, "malloc(%zu)", sz);
		if (!q)
			return;
		u = malloc_usable_size(q);
		CHECK(u >= sz, "usable %zu < requested %zu", u, sz);
		if (sz <= 128) {
			if (u - sz > worst_slack) {
				worst_slack = u - sz;
				if (u - sz > 15)
					worst_at = sz;
			}
			CHECK(u - sz <= 15,
			      "request %zu got %zu usable bytes: below 129 the "
			      "classes are exact 16-byte steps, so the slack "
			      "cannot exceed 15", sz, u);
		} else {
			CHECK(u * 4 < sz * 5,
			      "request %zu got %zu usable bytes, which is more "
			      "than the 5/4 the class spacing is supposed to "
			      "cap waste at", sz, u);
			if (u * worst_den > worst_num * sz) {
				worst_num = u; worst_den = sz;
			}
		}
		free(q);
	}
	/* Expressed as waste over the ALLOCATION, which is the denominator the
	 * README's "capped at 1/5" uses: a 4097-byte request lands in the 5120
	 * class, and 1023/5120 is 19.98%. */
	printf("frag: worst case over 8192 sizes: %zu bytes of slack at or "
	       "below 128, and %zu bytes requested in a %zu-byte class = "
	       "%zu.%02zu%% of the allocation wasted\n",
	       worst_slack, worst_den, worst_num,
	       (worst_num - worst_den) * 100 / worst_num,
	       (worst_num - worst_den) * 10000 / worst_num % 100);
	(void)worst_at;

	/* (2) a workload, against slab's own accounting. The two arrays are
	 * deliberately bigger than MAXSMALL so they take the large path and
	 * do not appear in the span totals below. */
	v = malloc(NOBJ * sizeof *v);
	CHECK(v != NULL, "could not allocate the slot array");
	if (!v)
		return;
	dump_stats(path, 0);
	if (!read_small(path, 1, &l0, &b0, &s0, &m0)) {
		CHECK(0, "malloc_stats() produced no 'slab: small' line, so "
		      "there is nothing to measure (not slab, or the dump "
		      "format moved)");
		return;
	}
	for (i = 0; i < NOBJ; i++) {
		size_t sz = 129 + (i * 2654435761u) % 8064;
		v[i] = malloc(sz);
		if (!v[i]) {
			CHECK(0, "malloc(%zu) failed at object %zu", sz, i);
			return;
		}
		*(unsigned char *)v[i] = 1;
		requested += sz;
		usable += malloc_usable_size(v[i]);
	}
	dump_stats(path, 1);
	if (!read_small(path, 2, &l1, &b1, &s1, &m1)) {
		CHECK(0, "the second malloc_stats() dump has no summary line");
		return;
	}
	CHECK(l1 - l0 == NOBJ,
	      "the span counters say %zu objects became live where %d were "
	      "allocated", l1 - l0, NOBJ);
	CHECK(b1 - b0 == usable,
	      "the span counters say %zu live bytes were added; summing "
	      "malloc_usable_size over the same blocks gives %zu",
	      b1 - b0, usable);
	CHECK(usable * 4 < requested * 5,
	      "%zu bytes requested occupy %zu bytes of class, which is worse "
	      "than the 5/4 the class spacing promises", requested, usable);
	printf("frag: %zu objects, %zu KiB requested, %zu KiB in classes "
	       "(%zu.%02zu%% of the allocation wasted), %zu spans holding "
	       "%zu KiB (%zu%% occupied by live objects)\n",
	       (size_t)NOBJ, requested / 1024, usable / 1024,
	       (usable - requested) * 100 / usable,
	       (usable - requested) * 10000 / usable % 100,
	       s1 - s0, (m1 - m0) / 1024,
	       m1 > m0 ? (b1 - b0) * 100 / (m1 - m0) : 0);
	for (i = 0; i < NOBJ; i++)
		free(v[i]);
	free(v);
	check_geometry(path);
}

/* ---------------- fork mode ----------------
 *
 * A multithreaded program that fork()s without exec()ing. POSIX says only
 * async-signal-safe things are legal in the child, and malloc is not one of
 * them -- but every allocator that wants to be dropped in has to survive it
 * anyway, because real programs do it and glibc's malloc handles it.
 *
 * The failure is a HANG, not a wrong answer: the child inherits a lock held
 * by a thread that does not exist there, and nothing will ever release it.
 * So the child arms alarm(2) and the parent treats a child that died of a
 * signal as the failure. Without that this check would not fail, it would
 * stop.
 *
 * The child's first allocation is a large one on purpose. A small one can
 * come off the magazine it inherited without touching the lock at all, and
 * the check would pass without ever reaching the thing it is about.
 *
 * The child then CREATES A THREAD and both of them churn with a verified
 * byte pattern, which reaches a second hazard the lock has nothing to do
 * with. Every magazine set in the registry the child inherited, including
 * the one the child's own thread is using, records a tid from the parent --
 * tids that do not exist here. A thread created in the child calls
 * tc_claim(), finds a set whose owner is gone, and adopts the one its
 * parent thread is still using. Two threads then push and pop the same
 * magazines with no lock between them and the same object comes out twice.
 * Without the second thread that mutant survives the whole suite.
 */
static long fork_child_ops = 3000;

static int fork_churn(uint64_t s)
{
	struct { void *p; uint32_t tag; size_t n; } v[128];
	int bad = 0;
	long i;
	unsigned k;

	memset(v, 0, sizeof v);
	for (i = 0; i < fork_child_ops; i++) {
		k = (unsigned)(rnd(&s) % 128);
		if (v[k].p) {
			if (!verify(v[k].p, v[k].tag, v[k].n))
				bad++;
			free(v[k].p);
			v[k].p = NULL;
		} else {
			size_t n = 16 + rnd(&s) % 2000;
			v[k].p = malloc(n);
			if (!v[k].p) {
				bad++;
				continue;
			}
			v[k].n = n;
			v[k].tag = (uint32_t)rnd(&s);
			fill(v[k].p, v[k].tag, n);
		}
	}
	for (k = 0; k < 128; k++)
		if (v[k].p) {
			if (!verify(v[k].p, v[k].tag, v[k].n))
				bad++;
			free(v[k].p);
		}
	return bad;
}

static void *fork_child_worker(void *arg)
{
	return (void *)(uintptr_t)fork_churn((uint64_t)(uintptr_t)arg + 101);
}

static volatile int fork_stop;

static void *fork_worker(void *arg)
{
	uint64_t s = (uint64_t)(uintptr_t)arg + 1;
	void *v[64] = {0};
	unsigned k;

	while (!fork_stop) {
		k = (unsigned)(rnd(&s) % 64);
		if (v[k]) {
			free(v[k]);
			v[k] = NULL;
		} else {
			v[k] = malloc(16 + rnd(&s) % 4000);
			if (v[k])
				*(unsigned char *)v[k] = 1;
		}
	}
	for (k = 0; k < 64; k++)
		free(v[k]);
	return NULL;
}

static void check_fork(long nfork)
{
	enum { NT = 4, NCHILD = 200 };
	/* 3 from a child whose byte patterns did not survive, 2 from one that
	 * could not allocate at all. */
	pthread_t th[NT];
	long i;
	int hung = 0, failed = 0;

	fflush(NULL);          /* nothing buffered for a child to duplicate */
	for (i = 0; i < NT; i++)
		if (pthread_create(&th[i], NULL, fork_worker,
				   (void *)(uintptr_t)i) != 0) {
			CHECK(0, "pthread_create failed at %ld", i);
			return;
		}
	for (i = 0; i < nfork; i++) {
		pid_t c = fork();
		int st = 0;

		if (c == 0) {
			struct itimerval deadline = {{0, 0}, {0, 500000}};
			void *big, *q[NCHILD];
			/* NT + 2 threads, not one. The child inherits a
			 * magazine set per thread the parent had, and all of
			 * them except the one this thread is using are
			 * genuinely free, so the first few threads created
			 * here adopt those and never reach it. The mutant
			 * that leaves this thread's set stamped with a dead
			 * tid survives entirely if the child creates fewer
			 * threads than the parent had. */
			pthread_t ct[NT + 2];
			void *r2 = NULL;
			int j, bad, nc = 0;

			/* 500 ms, not alarm(1), because the whole cost of
			 * this check is the mutants: a child that deadlocks
			 * waits out the timer, and three mutants hanging
			 * half their children at ten seconds each took
			 * `make test` from 36 seconds to 382. A healthy
			 * child here takes 14 ms, so this is 35x of
			 * headroom. */
			setitimer(ITIMER_REAL, &deadline, NULL);
			big = malloc(100000);     /* always takes the lock */
			if (!big)
				_exit(2);
			*(unsigned char *)big = 1;
			for (j = 0; j < NCHILD; j++) {
				q[j] = malloc(32 + (size_t)j * 7);
				if (!q[j])
					_exit(2);
				*(unsigned char *)q[j] = 1;
			}
			for (j = 0; j < NCHILD; j++)
				free(q[j]);
			free(big);
			for (j = 0; j < NT + 2; j++)
				if (pthread_create(&ct[j], NULL,
						   fork_child_worker,
						   (void *)(uintptr_t)(i + j))
				    == 0)
					nc++;
			if (nc != NT + 2)
				_exit(2);
			bad = fork_churn((uint64_t)i + 7);
			for (j = 0; j < nc; j++) {
				pthread_join(ct[j], &r2);
				if (r2)
					bad++;
			}
			_exit(bad ? 3 : 0);
		}
		if (c < 0 || waitpid(c, &st, 0) != c) {
			failed++;
			continue;
		}
		if (WIFSIGNALED(st))
			hung++;
		else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
			failed++;
	}
	fork_stop = 1;
	for (i = 0; i < NT; i++)
		pthread_join(th[i], NULL);

	printf("fork: %ld children allocated inside a fork of a %d-thread "
	       "process, %d hung, %d failed\n", nfork, NT, hung, failed);
	CHECK(hung == 0,
	      "%d of %ld children died of a signal, which here means the "
	      "alarm fired or something faulted: either the child's first "
	      "malloc found the lock held by a thread that does not exist "
	      "there, or two of its threads are sharing a magazine set",
	      hung, nfork);
	CHECK(failed == 0, "%d of %ld children could not allocate", failed,
	      nfork);
}

/* ---------------- sweep mode ----------------
 *
 * SLAB-SPECIFIC. A thread that exits runs none of slab's code, so whatever
 * is left on its per-class magazines is stranded. Adoption bounds that by
 * the PEAK number of threads rather than by the number created, which is
 * enough for a pool and not for a program that peaks at many threads and
 * then runs one: those sets are never asked for again.
 *
 * The observable is malloc_stats()'s magazined figure, which is exactly the
 * memory in question, and the measurement is a difference across a dump
 * taken while all those threads are dead and a dump taken after the main
 * thread has allocated enough to make slab want another arena -- which is
 * the only moment slab spends a syscall per live thread looking for dead
 * ones.
 *
 * The threads run CONCURRENTLY on purpose. Run one at a time they are
 * adopted, one set is reused for all of them, and there is nothing to
 * sweep; `stress tchurn` already covers that case and this one would be
 * vacuous without the overlap.
 */
#define SWEEP_THREADS 64
#define SWEEP_FILL     8000u    /* just under slab's 8192-byte drain point */

static pthread_barrier_t sweep_bar;

static void *sweep_worker(void *arg)
{
	void *v[SWEEP_FILL / 16 + 1];

	(void)arg;
	/* Just under a drain's worth in every class, allocated and freed, so
	 * all of it is sitting on this thread's magazines when it exits.
	 * Free one more than fits and the magazine drains to the spans
	 * instead, and there is nothing left to strand. */
	for (size_t sz = 16; sz <= 8192; sz = sz + (sz < 128 ? 16 : sz / 4)) {
		size_t k = SWEEP_FILL / sz, i;

		if (!k)
			k = 1;
		for (i = 0; i < k; i++) {
			v[i] = malloc(sz);
			if (v[i])
				*(unsigned char *)v[i] = 1;
		}
		for (i = 0; i < k; i++)
			free(v[i]);
	}
	/* Nobody exits until everybody has filled, so there are
	 * SWEEP_THREADS distinct magazine sets rather than one adopted set
	 * handed around. Without this the check measured 189 KiB stranded
	 * where it expected megabytes, because threads that had already
	 * exited were being adopted (and, once this commit existed, swept)
	 * while the later ones were still starting. */
	pthread_barrier_wait(&sweep_bar);
	return NULL;
}

static int read_mag(const char *path, int nth, size_t *objs, size_t *bytes)
{
	FILE *f = fopen(path, "r");
	char line[8192];
	int seen = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		size_t a, b, mo, mb;
		if (sscanf(line, "slab: small live=%zu livebytes=%zu "
			   "magazined=%zu magbytes=%zu", &a, &b, &mo, &mb) != 4)
			continue;
		if (++seen != nth)
			continue;
		*objs = mo; *bytes = mb;
		fclose(f);
		return 1;
	}
	fclose(f);
	return 0;
}

static void check_sweep(const char *path)
{
	pthread_t th[SWEEP_THREADS];
	size_t o0, b0, o1, b1;          /* magazined objects and bytes */
	size_t n0, nb0, ns0, nm0;       /* live, NET of the magazines */
	size_t n1, nb1, ns1, nm1;
	int i;
	enum { NFORCE = 12000 };          /* 1 KiB each: 12 MiB, several arenas */
	void **v;

	pthread_barrier_init(&sweep_bar, NULL, SWEEP_THREADS + 1);
	for (i = 0; i < SWEEP_THREADS; i++)
		if (pthread_create(&th[i], NULL, sweep_worker, NULL) != 0) {
			CHECK(0, "pthread_create failed at %d", i);
			return;
		}
	pthread_barrier_wait(&sweep_bar);
	for (i = 0; i < SWEEP_THREADS; i++)
		pthread_join(th[i], NULL);
	pthread_barrier_destroy(&sweep_bar);

	dump_stats(path, 0);
	if (!read_small(path, 1, &n0, &nb0, &ns0, &nm0) ||
	    !read_mag(path, 1, &o0, &b0)) {
		CHECK(0, "malloc_stats() produced no 'slab: small' line");
		return;
	}
	CHECK(b0 > 2u * 1024 * 1024,
	      "%zu dead threads left only %zu KiB on their magazines, which is "
	      "too little for this check to mean anything",
	      (size_t)SWEEP_THREADS, b0 / 1024);

	v = malloc(NFORCE * sizeof *v);
	CHECK(v != NULL, "could not allocate the slot array");
	if (!v)
		return;
	for (i = 0; i < NFORCE; i++) {
		v[i] = malloc(1024);
		if (!v[i]) {
			CHECK(0, "malloc(1024) failed at %d", i);
			return;
		}
		*(unsigned char *)v[i] = 1;
	}
	dump_stats(path, 1);
	if (!read_small(path, 2, &n1, &nb1, &ns1, &nm1) ||
	    !read_mag(path, 2, &o1, &b1)) {
		CHECK(0, "the second malloc_stats() dump has no summary line");
		return;
	}
	printf("sweep: %zu dead threads held %zu KiB on their magazines, "
	       "%zu KiB left after the next arena was wanted\n",
	       (size_t)SWEEP_THREADS, b0 / 1024, b1 / 1024);
	/* The main thread's own magazines are in the second figure and cannot
	 * be subtracted from outside; they are at most 32 classes by the 8 KiB
	 * drain threshold, so 256 KiB, and a tenth of the starting figure is
	 * comfortably above that while being far below it. */
	CHECK(b1 * 10 < b0,
	      "%zu KiB was still on dead threads' magazines after an arena was "
	      "wanted, down from %zu KiB: they are only being reclaimed by "
	      "adoption", b1 / 1024, b0 / 1024);
	/* And the objects have to arrive somewhere. `live` here is net of the
	 * magazines, so a sweep that emptied the magazine heads without
	 * handing the objects back to their spans would leave those %zu
	 * objects counted live and owned by nobody -- the magazined figure
	 * above would look perfect and the memory would be gone for good.
	 * Only NFORCE objects were allocated between the two dumps. */
	CHECK(n1 - n0 <= (size_t)NFORCE + 1000,
	      "%zu objects became live between the dumps where only %d were "
	      "allocated: the sweep is dropping magazines rather than "
	      "returning them (%zu came off the magazines)",
	      n1 - n0, NFORCE, o0 - o1);
	for (i = 0; i < NFORCE; i++)
		free(v[i]);
	free(v);
}

/* ---------------- reclaim mode ----------------
 *
 * Does a transient peak come back down?  Not a slab-specific question: glibc
 * and every other allocator worth comparing against return large runs of
 * freed memory to the kernel, so this is a conformance check and t/run.sh
 * requires BOTH to pass it.  It is in its own mode rather than in `check`
 * only because it wants a few hundred megabytes.
 *
 * Resident size is read out of /proc/self/statm, so the number is the
 * kernel's rather than ours, and it is read three times: before the
 * allocation, at the peak, and after everything has been freed.  The reading
 * at the peak is not decoration -- it is the guard that stops this being a
 * test of nothing.  An allocator (or a mistake in the loop below) that never
 * made the memory resident in the first place would satisfy the "it came
 * back down" assertion trivially, so the peak has to be at least half the
 * bytes requested before the drop is believed.
 *
 * 56 bytes, not 64: glibc rounds a 56-byte request to a 64-byte chunk and
 * slab puts it in the 64-byte class, so neither side is paying more
 * per-object overhead than the other and the comparison is about reclaiming
 * rather than about headers.
 */
static size_t vmrss(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	unsigned long total = 0, res = 0;

	if (!f)
		return 0;
	if (fscanf(f, "%lu %lu", &total, &res) != 2)
		res = 0;
	fclose(f);
	return (size_t)res * 4096;
}

static void check_reclaim(size_t bytes)
{
	const size_t sz = 56;
	size_t n = bytes / sz, i, base, peak, after;
	void **v = malloc(n * sizeof *v);

	CHECK(v != NULL, "could not allocate the %zu-entry slot array", n);
	if (!v)
		return;
	memset(v, 0, n * sizeof *v);   /* resident before the baseline reading */
	base = vmrss();

	for (i = 0; i < n; i++) {
		v[i] = malloc(sz);
		if (!v[i]) {
			CHECK(0, "malloc(%zu) failed at object %zu", sz, i);
			break;
		}
		memset(v[i], 0xaa, sz);   /* every byte of every page, dirty */
	}
	peak = vmrss();
	for (i = 0; i < n; i++)
		free(v[i]);
	after = vmrss();

	/* Printed unconditionally, because the three numbers are the result and
	 * t/run.sh puts them in its own output.  A pass/fail alone would hide
	 * the difference between "gave back nearly all of it" and "squeaked
	 * under the threshold". */
	/* Clamped at both ends on purpose. RSS can end up ABOVE the peak
	 * reading -- it does under ASan, whose quarantine and shadow grow
	 * while the frees happen -- and the unsigned subtraction then printed
	 * 18446744073709551608%, which is how this was noticed. */
	size_t kept = after > base ? after - base : 0;
	size_t grew = peak > base ? peak - base : 1;
	printf("reclaim: RSS %zu -> %zu -> %zu KiB, %zu KiB requested, %zu%% "
	       "of the peak given back\n",
	       base / 1024, peak / 1024, after / 1024, bytes / 1024,
	       kept >= grew ? (size_t)0 : 100 - kept * 100 / grew);
	CHECK(peak > base + bytes / 2,
	      "the workload never became resident: RSS went from %zu to %zu "
	      "KiB while allocating %zu KiB, so there was nothing to give back",
	      base / 1024, peak / 1024, bytes / 1024);
	CHECK(after < base + (peak - base) / 4,
	      "%zu KiB of %zu KiB allocated is still resident after freeing all "
	      "of it (RSS %zu -> %zu -> %zu KiB)",
	      (after - base) / 1024, (peak - base) / 1024,
	      base / 1024, peak / 1024, after / 1024);

	/* AND THE DISCLOSURE HALF.  Reclaiming a span re-arms its bump
	 * pointer, which tells calloc the memory is untouched zero page.  That
	 * is true for the fifteen pages MADV_DONTNEED threw away and false for
	 * the first one, which holds the span header and so cannot be given
	 * back; slab zeroes that page's objects by hand instead.  Every byte
	 * above was dirtied with 0xaa, so a calloc out of a re-armed span that
	 * skipped the hand-zeroing hands back 0xaa.
	 *
	 * `check` cannot see this.  Its frees go onto a magazine and the
	 * magazine does not drain in 4 KiB of traffic, so no span there ever
	 * reaches zero live objects and no reclaim ever happens. */
	enum { NZ = 20000 };
	void **z = malloc(NZ * sizeof *z);
	CHECK(z != NULL, "could not allocate the zero-check slots");
	if (z) {
		size_t bad = (size_t)-1, badoff = 0;
		unsigned char badval = 0;
		for (i = 0; i < NZ; i++) {
			/* volatile for the same reason check_calloc() needs it:
			 * gcc knows calloc returns zeroed memory and will
			 * delete a loop that only looks for a nonzero byte. */
			volatile unsigned char *q = calloc(1, sz);
			z[i] = (void *)q;
			if (!q)
				continue;
			for (size_t j = 0; j < sz; j++)
				if (q[j]) {
					bad = i; badoff = j; badval = q[j];
					break;
				}
			if (bad != (size_t)-1)
				break;
		}
		CHECK(bad == (size_t)-1,
		      "calloc out of a reclaimed span returned byte %zu = 0x%02x "
		      "of object %zu: the first page of a re-armed span is not "
		      "being zeroed", badoff, badval, bad);
		for (i = 0; i < NZ; i++)
			free(z[i]);
		free(z);
	}
	free(v);
}

/* ---------------- guard mode ----------------
 *
 * SLAB-SPECIFIC, and the only mode here that is.  Everything above is an
 * oracle that glibc also has to pass; this asks three questions that are
 * about slab's reserved PROT_NONE tail and nothing else, so t/run.sh runs it
 * under the preload only and no claim is made about any other allocator.
 *
 * 1. Did growth happen WHERE THE BLOCK ALREADY WAS?  realloc returning the
 *    same pointer for a large block that quadrupled is the observable, and
 *    it is here because a reservation that is never actually grown into is
 *    invisible in every other check: the suite would pass with mprotect
 *    deleted and realloc copying as before.
 * 2. Is the tail really unreadable?  Asked of the kernel twice over: once
 *    by reading the permissions back out of /proc/self/maps, and once by
 *    writing a byte past the end of the block in a forked child and
 *    requiring that child to die of SIGSEGV.  mprotect returning 0 is not
 *    evidence; a fault is.
 * 3b. Does realloc DOWN give the tail back?  Resident size out of
 *    /proc/self/statm before and after, the pointer unchanged, the content
 *    kept, and the guard moved down to the block's new end -- that last one
 *    is what separates giving the pages back from also taking the access
 *    away, and there is a mutant for each.
 * 3. Does a grown block go back to its natural size when freed?  The span
 *    cache matches on that size, so without the restore a buffer that is
 *    malloc'd small and doubled is never reused, which measured as 200
 *    fresh mmaps where there had been none.  Observable as: free a grown
 *    block, ask for the original size again, get the same address, the same
 *    usable size, and a tail that faults again.
 */

/* Write one byte at p[off] in a child process and report how it died.
 * Returns the signal, or 0 if the child survived the write. */
static int fault_on_write(volatile unsigned char *p, size_t off)
{
	fflush(NULL);                /* no buffered output duplicated by fork */
	pid_t c = fork();
	if (c == 0) {
		p[off] = 0x5a;
		_exit(0);            /* the write was allowed: that is the bug */
	}
	int st = 0;
	if (c < 0 || waitpid(c, &st, 0) != c)
		return -1;
	return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
}

/* Permissions the kernel reports for the mapping that starts at `addr`, or
 * NULL if no mapping starts exactly there. */
static int maps_perms_at(unsigned long addr, char out[5])
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int found = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		unsigned long a, b;
		char perms[8];
		if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3)
			continue;
		if (a == addr) {
			memcpy(out, perms, 4);
			out[4] = 0;
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

static void check_guard(void)
{
	/* Above RESERVE_MIN in slab.c, so this block gets a reserved tail.
	 * A value that is not a round number of pages on purpose: the tail
	 * starts at the page boundary past the payload, not at the payload. */
	const size_t n = 300000;
	unsigned char *p = launder(malloc(n));
	CHECK(p != NULL, "malloc(%zu)", n);
	if (!p)
		return;
	size_t up = malloc_usable_size(p);
	CHECK(up >= n, "usable %zu < requested %zu", up, n);
	fill(p, 0x1234, n);

	/* (2a) the kernel's own account of the tail */
	char perms[5] = "????";
	int got = maps_perms_at((unsigned long)p + up, perms);
	CHECK(got, "no mapping starts at the end of the payload (%p + %zu)",
	      (void *)p, up);
	CHECK(got && !strcmp(perms, "---p"),
	      "tail mapping at %p is %s, wanted ---p", (void *)(p + up), perms);

	/* (2b) and what the hardware does about it */
	int sig = fault_on_write(p, up);
	CHECK(sig == SIGSEGV, "writing p[%zu], one byte past the block, died of "
	      "signal %d, wanted SIGSEGV (%d)", up, sig, SIGSEGV);

	/* (1) growth in place: same address, content kept, more room */
	unsigned char *q = launder(realloc(p, 4 * n));
	CHECK(q != NULL, "realloc(p, %zu)", 4 * n);
	if (!q)
		return;
	CHECK(q == p, "realloc grew %zu -> %zu by moving the block (%p -> %p), "
	      "so the reservation was not used", n, 4 * n, (void *)p, (void *)q);
	CHECK(verify(q, 0x1234, n), "content lost growing in place");
	size_t uq = malloc_usable_size(q);
	CHECK(uq >= 4 * n, "usable after growth %zu < %zu", uq, 4 * n);

	/* the guard moved up with the end of the block */
	sig = fault_on_write(q, uq);
	CHECK(sig == SIGSEGV, "after growing, writing q[%zu] died of signal %d, "
	      "wanted SIGSEGV", uq, sig);

	/* (3b) realloc down hands the tail back. 64 MiB so the measurement is
	 * well clear of everything else the process is holding. */
	{
		/* 8 MiB and not 64: big enough that the resident drop is
		 * unmistakable, small enough that the mapping still fits the
		 * span cache, which the last assertion here needs. */
		const size_t big = 8u << 20;
		unsigned char *w = launder(malloc(big));
		unsigned char *x;
		size_t before, after, ux;

		CHECK(w != NULL, "malloc(%zu)", big);
		if (!w)
			return;
		memset(w, 0xbb, big);
		before = vmrss();
		x = launder(realloc(w, 1000));
		after = vmrss();
		CHECK(x != NULL, "realloc(p, 1000) returned NULL");
		if (!x)
			return;
		CHECK(x == w, "realloc down from %zu to 1000 moved the block "
		      "(%p -> %p)", big, (void *)w, (void *)x);
		CHECK(x[0] == 0xbb && x[999] == 0xbb,
		      "content lost shrinking in place");
		CHECK(before > after + big / 2,
		      "shrinking a %zu KiB block to 1000 bytes gave back %zu "
		      "KiB of resident memory (RSS %zu -> %zu KiB)",
		      big / 1024, (before - after) / 1024, before / 1024,
		      after / 1024);
		ux = malloc_usable_size(x);
		CHECK(ux < big / 2,
		      "after shrinking to 1000 bytes the block still reports "
		      "%zu usable", ux);
		sig = fault_on_write(x, ux);
		CHECK(sig == SIGSEGV,
		      "after shrinking, writing x[%zu] died of signal %d: the "
		      "pages were dropped but the access was not taken away",
		      ux, sig);
		/* And the trim must not cost the mapping its place in the
		 * span cache. Freed and asked for again at the size it was
		 * made at, it has to come back -- otherwise a single realloc
		 * down throws the mapping away and the next request pays a
		 * fresh mmap and several hundred page faults, which measured
		 * as 33% on the recorded CPython replay. */
		free(x);
		{
			unsigned char *y = launder(malloc(big));

			CHECK(y == w,
			      "after a realloc down and a free, asking for "
			      "%zu again gave %p rather than the mapping that "
			      "was just released (%p): a trimmed block is "
			      "going into the span cache at its trimmed size",
			      big, (void *)y, (void *)w);
			free(y);
		}
	}

	/* (3) freed at four times its natural size, reusable at its natural
	 * size, with the guard back where it started */
	free(q);
	unsigned char *r = launder(malloc(n));
	CHECK(r != NULL, "malloc(%zu) after the grown block was freed", n);
	if (!r)
		return;
	CHECK(r == p, "the grown mapping was not recycled for an equal request "
	      "(%p, wanted %p): freeing it did not restore its natural size",
	      (void *)r, (void *)p);
	size_t ur = malloc_usable_size(r);
	CHECK(ur == up, "recycled block reports %zu usable bytes, wanted the "
	      "original %zu", ur, up);
	sig = fault_on_write(r, ur);
	CHECK(sig == SIGSEGV, "after the grown block was freed and re-taken, "
	      "writing r[%zu] died of signal %d, wanted SIGSEGV", ur, sig);
	free(r);

	/* A large block too small for a reservation still ends where it says
	 * it ends: there is no tail, so one byte past it is simply unmapped. */
	const size_t m = 20000;
	unsigned char *t = launder(malloc(m));
	CHECK(t != NULL, "malloc(%zu)", m);
	if (!t)
		return;
	size_t ut = malloc_usable_size(t);
	sig = fault_on_write(t, ut);
	CHECK(sig == SIGSEGV, "unreserved large block: writing t[%zu] died of "
	      "signal %d, wanted SIGSEGV", ut, sig);
	free(t);
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

/*
 * Mode `tlb`: random reads over a large live working set, which is the only
 * shape in which a huge page can pay for itself and the one `bench` cannot
 * see.  Every row of `bench` writes one byte per block and moves on, so a
 * TLB entry is wasted on it either way and MADV_HUGEPAGE moves those six
 * numbers by less than their run-to-run spread.  This moves by 16%.
 *
 * Not asserted, because it is a timing and this suite asserts none.  It
 * exists so the number in the README can be re-measured:
 *
 *     ./t/stress tlb                       (glibc)
 *     LD_PRELOAD=./libslab.so ./t/stress tlb
 */
static void check_tlb(long nobj, long naccess)
{
	unsigned char **v = malloc((size_t)nobj * sizeof *v);
	unsigned long s = 12345, acc = 0;
	long i;
	double t0;

	CHECK(v != NULL, "could not allocate the slot array");
	if (!v)
		return;
	for (i = 0; i < nobj; i++) {
		v[i] = malloc(64);
		if (!v[i]) {
			CHECK(0, "malloc(64) failed at object %ld", i);
			return;
		}
		memset(v[i], (int)i, 64);
	}
	t0 = now();
	for (i = 0; i < naccess; i++) {
		s = s * 6364136223846793005UL + 1442695040888963407UL;
		acc += v[(s >> 20) % (unsigned long)nobj][(s >> 3) & 63];
	}
	printf("tlb: %.1f ns/access over %ld live 64-byte objects, "
	       "AnonHugePages %ld KiB, Rss %ld KiB (checksum %lu)\n",
	       (now() - t0) / (double)naccess * 1e9, nobj,
	       smaps_rollup("AnonHugePages:"), smaps_rollup("Rss:"), acc);
	for (i = 0; i < nobj; i++)
		free(v[i]);
	free(v);
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

/*
 * Growing a large buffer by doubling, 16 KiB up to 4 MiB.
 *
 * bench_grow above stops at 64 KiB, so only two of its thirteen steps are
 * large-to-large; this one is eight out of eight, which is the case a
 * grow-in-place reservation exists for.  Only the last byte of each size is
 * touched, so the number is allocator work rather than the cost of filling
 * a buffer.
 */
static void bench_biggrow(long iters)
{
	double t0 = now();
	long ops = 0;
	for (long i = 0; i < iters; i++) {
		size_t n = 16384;
		char *p = malloc(n);
		p[n - 1] = 1;
		while (n < 4u * 1024 * 1024) {
			size_t m = n * 2;
			p = realloc(p, m);
			p[m - 1] = 1;
			n = m;
			ops++;
		}
		free(p);
	}
	report("large-grow", now() - t0, ops);
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

	if (!strcmp(mode, "guard")) {
		check_guard();
	} else if (!strcmp(mode, "tlb")) {
		check_tlb(n ? n : 2000000, 20000000);
	} else if (!strcmp(mode, "huge")) {
		check_huge(n ? n : 400000);
	} else if (!strcmp(mode, "fork")) {
		check_fork(n ? n : 200);
	} else if (!strcmp(mode, "sweep")) {
		check_sweep(argc > 2 ? argv[2] : "sweep.txt");
	} else if (!strcmp(mode, "frag")) {
		check_frag(argc > 2 ? argv[2] : "frag.txt");
	} else if (!strcmp(mode, "reclaim")) {
		check_reclaim((size_t)(n ? n : 200) * 1024 * 1024);
	} else if (!strcmp(mode, "check")) {
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
		bench_biggrow(it / 20000 ? it / 20000 : 1);
		bench_large(it / 500);
		bench_threads(it / 4, 4);
		return 0;
	} else {
		fprintf(stderr,
			"usage: stress check|guard|reclaim|huge|tlb|fork|"
			"frag PATH|sweep PATH|stress|thread|tchurn|bench [n]\n");
		return 2;
	}

	if (failures)
		printf("  %d failure(s) in mode %s\n", failures, mode);
	return failures ? 1 : 0;
}
