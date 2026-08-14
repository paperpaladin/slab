#!/usr/bin/env python3
"""t/trace.py - turn a raw allocator trace into a slot trace, and say what is in it.

t/record.c writes raw records of (op, a, b, result) where a, b and result
are the real pointers and sizes a program used.  Replaying that needs a map
from "the pointer the trace mentions" to "the pointer we just got back",
which in C is a hash table with deletions inside the loop being timed.  Here
it is a dict, run once, offline, and the replay driver becomes an array
index.

So this pass assigns every live block a SLOT, reusing a slot as soon as its
block is freed.  The output record is (op, slot, arg1, arg2) and t/replay.c
needs no bookkeeping beyond one array of void *.

It also prints what the trace contains, because a replay number is only
worth as much as the trace behind it:

  - dropped records, if the recorder hit SLAB_TRACE_MAX.  A truncated trace
    is still usable; one that does not say it was truncated is not.
  - frees of pointers the trace never allocated.  The recorder interposes
    malloc, calloc, realloc and the aligned family, and anything that
    reaches the real allocator some other way still comes back through our
    free().  Those frees are dropped from the slot trace and COUNTED, since
    a trace full of them has lifetimes that do not mean anything.
  - peak live blocks, total bytes requested, and the op mix, which is the
    part worth comparing against the synthetic benchmark's guess.
"""
import struct
import sys

RAW = struct.Struct("<4Q")
SLOT = struct.Struct("<IIQQ")
OP_END, OP_MALLOC, OP_FREE, OP_CALLOC, OP_REALLOC, OP_MEMALIGN = range(6)
NAME = {OP_MALLOC: "malloc", OP_FREE: "free", OP_CALLOC: "calloc",
        OP_REALLOC: "realloc", OP_MEMALIGN: "memalign"}


def main(src, dst):
    raw = open(src, "rb").read()
    if len(raw) % RAW.size:
        sys.exit("%s: %d bytes is not a whole number of %d-byte records"
                 % (src, len(raw), RAW.size))

    where = {}          # trace pointer -> slot
    free_slots = []
    nslots = 0
    out = bytearray()
    counts = dict.fromkeys(NAME, 0)
    nops = 0
    orphan_free = 0
    orphan_realloc = 0
    bytes_req = 0
    live = 0
    peak_live = 0
    declared = dropped = None
    truncated_mid_record = len(raw) % RAW.size

    def take():
        nonlocal nslots
        if free_slots:
            return free_slots.pop()
        nslots += 1
        return nslots - 1

    for off in range(0, len(raw) - truncated_mid_record, RAW.size):
        op, a, b, r = RAW.unpack_from(raw, off)
        if op == OP_END:
            if declared is not None:
                # Two trailers means two processes wrote this file. That is
                # exactly what happened the first time a gcc trace was taken:
                # the driver, cc1 and as all inherited LD_PRELOAD, all opened
                # the same path with O_TRUNC, and the driver's 406 records
                # landed on top of cc1's. The recorder writes <prefix>.<pid>
                # now; refusing here is the check that it still does.
                sys.exit("%s: two trailers (records %d and %d) - more than "
                         "one process wrote this file, so the trace is a "
                         "mixture and its lifetimes are wrong"
                         % (src, declared, a))
            declared, dropped = a, b
            continue
        if op not in NAME:
            sys.exit("%s: unknown op %d at record %d" % (src, op, off // RAW.size))
        counts[op] += 1

        if op == OP_FREE:
            if a == 0:
                continue            # free(NULL) is a no-op everywhere
            slot = where.pop(a, None)
            if slot is None:
                orphan_free += 1
                continue
            free_slots.append(slot)
            live -= 1
            out += SLOT.pack(OP_FREE, slot, 0, 0)
            nops += 1
            continue

        if op == OP_REALLOC:
            size = b
            old = where.pop(a, None)
            if a and old is None:
                orphan_realloc += 1
                old = None          # replay it as a plain malloc
            if size == 0:
                # glibc's realloc(p, 0) frees and returns NULL.
                if old is not None:
                    free_slots.append(old)
                    live -= 1
                    out += SLOT.pack(OP_FREE, old, 0, 0)
                    nops += 1
                continue
            if r == 0:
                if old is not None:     # failed: the old block is still live
                    where[a] = old
                continue
            if old is None:
                slot = take()
                live += 1
                out += SLOT.pack(OP_MALLOC, slot, size, 0)
            else:
                slot = old
                out += SLOT.pack(OP_REALLOC, slot, size, 0)
            where[r] = slot
            bytes_req += size
            nops += 1
            peak_live = max(peak_live, live)
            continue

        # malloc, calloc, memalign
        if r == 0:
            continue                    # the allocation failed; nothing to map
        slot = take()
        live += 1
        peak_live = max(peak_live, live)
        where[r] = slot
        if op == OP_CALLOC:
            size = a * b
            out += SLOT.pack(OP_CALLOC, slot, a, b)
        elif op == OP_MEMALIGN:
            size = b
            out += SLOT.pack(OP_MEMALIGN, slot, a, b)
        else:
            size = a
            out += SLOT.pack(OP_MALLOC, slot, size, 0)
        bytes_req += size
        nops += 1

    with open(dst, "wb") as f:
        # Header, so the replay can size its slot array and check that it
        # read as many records as were written.
        f.write(SLOT.pack(OP_END, nslots, nops, 0))
        f.write(out)

    nraw = sum(counts.values())
    print("trace: %s -> %s" % (src, dst))
    print("trace: %d raw records (%s)"
          % (nraw, ", ".join("%s %d" % (NAME[o], counts[o])
                             for o in sorted(counts) if counts[o])))
    if declared is None:
        print("trace: NO TRAILER - the recorder did not run its destructor, "
              "so this trace is cut off at an unknown point")
    else:
        print("trace: recorder wrote %d, dropped %d at the cap" % (declared, dropped))
        if declared != nraw:
            print("trace: MISMATCH - trailer says %d records, %d were read"
                  % (declared, nraw))
    if truncated_mid_record:
        print("trace: %d trailing bytes were a partial record" % truncated_mid_record)
    print("trace: %d replayable ops, %d slots (peak live blocks), "
          "%d KiB requested" % (nops, nslots, bytes_req // 1024))
    print("trace: %d frees and %d reallocs of pointers this trace never "
          "allocated (paths the recorder does not interpose)"
          % (orphan_free, orphan_realloc))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: trace.py RAWTRACE SLOTTRACE")
    sys.exit(main(sys.argv[1], sys.argv[2]))
