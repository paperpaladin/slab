CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -fPIC
LDFLAGS = -shared

all: libslab.so

libslab.so: slab.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f libslab.so

.PHONY: all clean
