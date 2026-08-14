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

all: libslab.so t/stress t/librecord.so t/replay

libslab.so: slab.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

t/stress: t/stress.c
	$(CC) $(TFLAGS) -o $@ $< -lpthread

# The trace tools.  record is an LD_PRELOAD over the program being recorded,
# so it is built -fPIC -shared; replay is an ordinary program that runs under
# whichever allocator is being measured.
t/librecord.so: t/record.c
	$(CC) $(TFLAGS) -fPIC -shared -o $@ $<

t/replay: t/replay.c
	$(CC) $(TFLAGS) -o $@ $<

test: all
	@t/run.sh

# Five repetitions each; read the spread, not the first number.
bench: all
	@echo "== glibc malloc =="
	@for i in 1 2 3 4 5; do ./t/stress bench; echo; done
	@echo "== slab =="
	@for i in 1 2 3 4 5; do LD_PRELOAD=./libslab.so ./t/stress bench; echo; done

clean:
	rm -f libslab.so t/stress t/librecord.so t/replay

.PHONY: all test bench clean
