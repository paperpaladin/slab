/*
 * bigloop - allocate and free one large block, N times, at a size the span
 * cache can match.
 *
 * It exists so the cache can be measured from outside the allocator. The claim
 * "freed large mappings are kept and reused" is a claim about syscalls, and
 * strace can count those; nothing visible in the program's own output can.
 *
 * The size is fixed and well above MAXSMALL so every iteration takes the large
 * path, and the block is written to, so a mapping that was never faulted in
 * cannot be mistaken for one that was.
 *
 * Copyright (C) 2026  PLACEHOLDER.  Licensed under the GPLv3 (see LICENSE).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The harness asserts this is above slab's MAXSMALL (8192), because the
 * span-cache check it drives cannot otherwise tell "the cache works" from
 * "the workload never reached the large path": glibc caches too, so both
 * munmap counts sit near zero either way. Printed rather than assumed. */
#define BIGLOOP_SIZE 300000

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 100;

	/* On stdout, which the harness captures separately from strace's
	 * stderr, so it can assert the workload is in the large path at all. */
	if (argc > 2 && argv[2][0] == 's') {
		printf("%u\n", (unsigned)BIGLOOP_SIZE);
		return 0;
	}

	for (long i = 0; i < n; i++) {
		void *p = malloc(BIGLOOP_SIZE);
		if (!p)
			return 1;
		memset(p, 0x5a, BIGLOOP_SIZE);
		free(p);
	}
	return 0;
}
