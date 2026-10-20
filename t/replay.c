/*
 * replay - reissue a recorded allocator trace against whatever allocator is
 * loaded, and time it.
 *
 * The trace comes from t/record.c by way of t/trace.py, which has already
 * turned every pointer the recorded program used into a slot index.  So
 * there is no hash table and no bookkeeping inside the loop being timed:
 * one array of void *, one switch, and the clock.
 *
 * FAITHFULNESS IS CHECKED, not assumed, because a replay that quietly
 * dropped half the trace would report a lovely number.  Every free must find
 * its slot occupied, every allocation must find it empty, and the number of
 * records read must equal the number the header says were written.  Any
 * mismatch is a failure and the count is printed.  Those three are what make
 * the timing worth reading; see t/run.sh, which plants a replay that skips
 * one op in sixteen and requires this to reject it.
 *
 * Its own memory comes from mmap, not from malloc: the trace is mapped and
 * the slot array is mapped, so the allocator under test is only ever asked
 * for the trace's own allocations.
 *
 * Each block gets its first and last byte written, which is an
 * approximation and the honest limit of this tool.  The recorded program
 * touched its memory in some pattern of its own that the trace does not
 * carry, so page-fault and cache behaviour here is the replay's, not the
 * program's.  What does carry over exactly is the sequence of sizes and the
 * lifetimes, which is what the synthetic benchmark was guessing at.
 *
 * Copyright (C) 2026  PLACEHOLDER.  Licensed under the GPLv3 (see LICENSE).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { OP_END = 0, OP_MALLOC, OP_FREE, OP_CALLOC, OP_REALLOC, OP_MEMALIGN };

struct rec {
	uint32_t op, slot;
	uint64_t a, b;
};

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void touch(void *p, size_t n)
{
	if (p && n) {
		((volatile unsigned char *)p)[0] = 1;
		((volatile unsigned char *)p)[n - 1] = 1;
	}
}

int main(int argc, char **argv)
{
	const char *path;
	long reps;
	int fd;
	struct stat st;
	const struct rec *rec;
	size_t nrec, i;
	long r;
	uint32_t nslots, declared;
	void **slot;
	size_t *len;
	unsigned long bad_free = 0, bad_fill = 0, nullret = 0, ops = 0;
	double t;

	if (argc < 2) {
		fprintf(stderr, "usage: replay SLOTTRACE [reps]\n");
		return 2;
	}
	path = argv[1];
	reps = argc > 2 ? atol(argv[2]) : 1;
	if (reps < 1)
		reps = 1;

	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof *rec) {
		fprintf(stderr, "replay: cannot read %s\n", path);
		return 2;
	}
	rec = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (rec == MAP_FAILED) {
		fprintf(stderr, "replay: cannot map %s\n", path);
		return 2;
	}
	nrec = (size_t)st.st_size / sizeof *rec;
	if (rec[0].op != OP_END) {
		fprintf(stderr, "replay: %s has no header record\n", path);
		return 2;
	}
	nslots = rec[0].slot;
	declared = (uint32_t)rec[0].a;
	if (declared != nrec - 1) {
		fprintf(stderr, "replay: header says %u ops, the file holds "
			"%zu\n", declared, nrec - 1);
		return 1;
	}
	if (!nslots) {
		fprintf(stderr, "replay: the trace allocates nothing\n");
		return 1;
	}

	/* Mapped, not malloc'd: the allocator under test must only see the
	 * trace's own calls. */
	slot = mmap(NULL, (size_t)nslots * sizeof *slot, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	len = mmap(NULL, (size_t)nslots * sizeof *len, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (slot == MAP_FAILED || len == MAP_FAILED) {
		fprintf(stderr, "replay: out of address space for %u slots\n",
			nslots);
		return 2;
	}

	t = now();
	for (r = 0; r < reps; r++) {
		for (i = 1; i < nrec; i++) {
			uint32_t s = rec[i].slot;
			size_t n;
			void *p;

			switch (rec[i].op) {
			case OP_MALLOC:
				if (slot[s])
					bad_fill++;
				n = (size_t)rec[i].a;
				p = malloc(n);
				if (!p)
					nullret++;
				touch(p, n);
				slot[s] = p;
				len[s] = n;
				break;
			case OP_CALLOC:
				if (slot[s])
					bad_fill++;
				n = (size_t)rec[i].a * (size_t)rec[i].b;
				p = calloc((size_t)rec[i].a, (size_t)rec[i].b);
				if (!p)
					nullret++;
				touch(p, n);
				slot[s] = p;
				len[s] = n;
				break;
			case OP_MEMALIGN:
				if (slot[s])
					bad_fill++;
				n = (size_t)rec[i].b;
				p = NULL;
				if (posix_memalign(&p, (size_t)rec[i].a, n) != 0)
					nullret++;
				touch(p, n);
				slot[s] = p;
				len[s] = n;
				break;
			case OP_REALLOC:
				if (!slot[s])
					bad_free++;
				n = (size_t)rec[i].a;
				p = realloc(slot[s], n);
				if (!p)
					nullret++;
				else
					touch(p, n);
				slot[s] = p;
				len[s] = n;
				break;
			case OP_FREE:
				if (!slot[s])
					bad_free++;
				free(slot[s]);
				slot[s] = NULL;
				len[s] = 0;
				break;
			default:
				fprintf(stderr, "replay: unknown op %u at "
					"record %zu\n", rec[i].op, i);
				return 1;
			}
			ops++;
		}
		/* A trace usually ends with blocks still live.  Clear them, so
		 * the next repetition starts from the same state and the
		 * bad_fill check above stays meaningful. */
		for (i = 0; i < nslots; i++) {
			free(slot[i]);
			slot[i] = NULL;
		}
	}
	t = now() - t;

	printf("replay: %lu ops in %.3fs = %.1f ns/op (%ld rep%s, %u slots)\n",
	       ops, t, t / (double)ops * 1e9, reps, reps == 1 ? "" : "s",
	       nslots);
	if (bad_free || bad_fill || nullret) {
		printf("replay: FAIL %lu frees of an empty slot, %lu "
		       "allocations into an occupied slot, %lu allocation "
		       "failures\n", bad_free, bad_fill, nullret);
		return 1;
	}
	return 0;
}
