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
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 100;

	for (long i = 0; i < n; i++) {
		void *p = malloc(300000);
		if (!p)
			return 1;
		memset(p, 0x5a, 300000);
		free(p);
	}
	return 0;
}
