CC      = gcc
# -fno-builtin-malloc and friends are not optional for an allocator.  gcc
# recognises malloc()+memset(0) as the calloc() idiom and rewrites it into a
# call to calloc - which, in this file, is a PLT call back into our own
# calloc.  That is an infinite tail-recursion, and it is how the first
# version of the small-object calloc below hung /usr/bin/wc.  The same
# reasoning applies to the other three: the compiler knows what these names
# mean, and inside their own implementation that knowledge is a liability.
CFLAGS  = -std=c11 -O2 -Wall -Wextra -fPIC \
	  -fno-builtin-malloc -fno-builtin-calloc \
	  -fno-builtin-realloc -fno-builtin-free
LDFLAGS = -shared

# The oracle must not be allowed to assume that malloc/calloc behave: gcc
# knows what they promise and will constant-fold the checks into "true".
TFLAGS  = -std=gnu11 -O2 -Wall -Wextra \
	  -fno-builtin-malloc -fno-builtin-calloc \
	  -fno-builtin-realloc -fno-builtin-free

all: libslab.so t/stress

libslab.so: slab.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

t/stress: t/stress.c
	$(CC) $(TFLAGS) -o $@ $< -lpthread

test: all
	@t/run.sh

clean:
	rm -f libslab.so t/stress

.PHONY: all test clean
