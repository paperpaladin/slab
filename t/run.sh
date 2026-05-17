#!/bin/sh
#
# t/run.sh - the oracle for slab.
#
# Tier 1 is the only tier that really matters: take real system binaries,
# run them twice - once normally, once with LD_PRELOAD=libslab.so - and
# require that stdout, stderr and the exit status are byte-identical.  A
# program whose output does not change is a program whose memory was not
# corrupted, and these programs (ls, cat, grep, sort, wc, sh, python3, gcc)
# exercise far more allocation patterns than any suite we would write.
#
# Tier 3 checks the oracle itself, by mutation: build a deliberately broken
# slab and require the tier-2 check that owns that behaviour to reject it.
#
# Tier 2 is t/stress, which checks the things output comparison cannot see:
# alignment, calloc zeroing, realloc content preservation, and - via a byte
# pattern per block - that no two live allocations overlap.
#
set -u

cd "$(dirname "$0")/.." || exit 2
SLAB="$PWD/libslab.so"
T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT

pass=0
fail=0
skipped=0
ok()  { pass=$((pass + 1)); printf 'ok    %s\n' "$*"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n' "$*"; }

[ -f "$SLAB" ]  || { echo "libslab.so missing; run make"; exit 2; }
[ -x ./t/stress ] || { echo "t/stress missing; run make"; exit 2; }

# ---------------------------------------------------------------- tier 1
# A deterministic input file big enough to push sort and grep into real
# allocation: no /dev/urandom, because the oracle must be reproducible.
python3 - "$T/data" <<'PY'
import sys
s = 88172645463325252
with open(sys.argv[1], "w") as f:
    for i in range(200000):
        s ^= (s << 13) & 0xffffffffffffffff
        s ^= s >> 7
        s ^= (s << 17) & 0xffffffffffffffff
        f.write("%08x %s %d\n" % (s & 0xffffffff, "ab/cd"[: (s % 5) + 1], i % 997))
PY

# same <name> <shell command>: run it with and without the preload and
# demand identical bytes out of both.
# ------------------------------------------------- is slab even loaded?
#
# This runs FIRST because every other check in the file is satisfied by a
# shared object that loads cleanly and interposes nothing. Verified by
# building one -- `gcc -shared -fPIC` over a single unused variable, copied
# over libslab.so -- which gives "31 passed, 0 failed" on this suite.
#
# tier 1 compares a command's output with and without LD_PRELOAD and demands
# they match; tier 2 runs t/stress under preload and demands exit 0; and the
# suite separately requires both to pass against glibc malloc, because an
# oracle glibc fails is a wrong oracle. All three hold when nothing is
# interposed. The RSS ratios come out near 1.00 against limits of 3.0 and
# 1.5, the munmap counts match because both sides are glibc, and tier 3's
# mutants are built from slab.c rather than from the .so.
#
# The two values are COMPARED rather than pinned: slab's header is 16 bytes
# for a 1-byte request and this glibc's is 24, but another libc may answer
# differently and the property is "ours is not theirs".
g_usable=$(./t/stress interposed 2>/dev/null)
s_usable=$(LD_PRELOAD="$SLAB" ./t/stress interposed 2>/dev/null)
if [ -z "$s_usable" ] || [ -z "$g_usable" ]; then
	bad "interposition: could not read malloc_usable_size" \
	    "glibc='$g_usable' slab='$s_usable'"
elif [ "$s_usable" = "$g_usable" ]; then
	bad "interposition: LD_PRELOAD changes nothing" \
	    "malloc_usable_size(malloc(1)) is $g_usable either way -- $SLAB is not providing malloc, and every check below passes anyway"
else
	ok "interposition: LD_PRELOAD replaces malloc (usable(malloc(1)) $g_usable -> $s_usable)"
fi

same() {
	name=$1
	cmd=$2
	sh -c "$cmd" >"$T/a.out" 2>"$T/a.err"
	ra=$?
	LD_PRELOAD="$SLAB" sh -c "$cmd" >"$T/b.out" 2>"$T/b.err"
	rb=$?

	if [ "$ra" != "$rb" ]; then
		bad "$name: exit status $ra vs $rb (slab)"
		return
	fi
	if ! cmp -s "$T/a.out" "$T/b.out"; then
		bad "$name: stdout differs ($(wc -c <"$T/a.out") vs $(wc -c <"$T/b.out") bytes)"
		cmp "$T/a.out" "$T/b.out" 2>&1 | head -2 | sed 's/^/        /'
		return
	fi
	if ! cmp -s "$T/a.err" "$T/b.err"; then
		bad "$name: stderr differs"
		head -3 "$T/b.err" | sed 's/^/        /'
		return
	fi
	# Any diagnostic from slab itself means we hit a path we do not
	# handle, even if the program happened to survive it.
	if grep -q '^slab:' "$T/b.err"; then
		bad "$name: slab printed a diagnostic"
		grep '^slab:' "$T/b.err" | head -2 | sed 's/^/        /'
		return
	fi
	# Two identical failures compare equal. A command that does not exist
	# produces empty stdout on both sides, matching stderr, and no `slab:`
	# diagnostic -- so it reached "ok <name> (0 bytes identical)". Every
	# line in this family produces output today (the smallest is 5 bytes),
	# so nothing is currently vacuous; a renamed python3 or gcc would have
	# turned four of the heaviest checks into passes.
	_nb=$(wc -c <"$T/a.out")
	if [ "$_nb" -eq 0 ] && [ ! -s "$T/a.err" ]; then
		bad "$name: produced no output at all, so there was nothing to compare"
		return
	fi
	ok "$name ($_nb bytes identical)"
}

same "ls -la /usr/bin"        "ls -la /usr/bin"
same "ls -R /usr/include"     "ls -R /usr/include"
same "ls error path"         "ls /nonexistent-zz; echo rc=\$?"
same "cat /etc/services"      "cat /etc/services"
same "cat big file"           "cat '$T/data'"
same "grep /etc/services"     "grep -n tcp /etc/services"
same "grep -r /usr/include"   "grep -rl malloc /usr/include 2>/dev/null | sort"
same "grep -c big file"       "grep -c 'b' '$T/data'"
same "sort big file"          "sort '$T/data'"
same "sort -u | wc"           "sort -u '$T/data' | wc -lwc"
same "sort -k2 /etc/services" "sort -k2,2 -k1,1 /etc/services"
same "wc -lwc"                "wc -lwc '$T/data'"
same "shell pipeline"         "cut -d' ' -f2,3 '$T/data' | sort | uniq -c | sort -rn | head -40"
same "python3 json churn"     "python3 -c \"
import json
d = [{'i': i, 's': 'x' * (i % 500), 'n': [i, i*2, i*3]} for i in range(40000)]
b = json.dumps(d)
e = json.loads(b)
print(len(b), sum(len(x['s']) for x in e), e[-1]['n'])
\""
same "python3 dict/sort"      "python3 -c \"
import collections
c = collections.Counter()
for i in range(300000):
    c[str(i * 7919 % 10007)] += 1
print(len(c), sum(c.values()), sorted(c.items())[:3])
\""
same "python3 str building"   "python3 -c \"
s = ''
for i in range(20000):
    s += '%d,' % i
print(len(s), hash(s) is not None, s[-10:])
\""
same "gcc -S the stress test" "gcc -O2 -S -o - t/stress.c 2>&1 | wc -l"
same "nested shell + subproc" "for i in 1 2 3; do ls /usr/lib | wc -l; done"

# ---------------------------------------------------------------- tier 2
unit() {
	name=$1
	shift
	if LD_PRELOAD="$SLAB" "$@" >"$T/u.out" 2>&1; then
		ok "$name"
	else
		bad "$name"
		head -6 "$T/u.out" | sed 's/^/        /'
	fi
}

unit "semantics (alignment, calloc, realloc, memalign, no-overlap)" \
	./t/stress check
unit "stress: 3M random ops, byte pattern verified"  ./t/stress stress 3000000
unit "stress: 4 threads, 400k ops each, cross-thread free" \
	./t/stress thread 400000

# The same two must also pass against glibc malloc: if they do not, the
# oracle is wrong rather than slab.
if ./t/stress check >"$T/g.out" 2>&1 && ./t/stress stress 300000 >>"$T/g.out" 2>&1; then
	ok "oracle agrees with glibc malloc"
else
	bad "oracle fails against glibc malloc - the test is wrong, not slab"
	head -4 "$T/g.out" | sed 's/^/        /'
fi

# rss_ratio LIB CMD...  -- peak RSS of CMD under LIB, over peak RSS without.
#
# wait4(), not getrusage(RUSAGE_CHILDREN).  That reports the maximum over
# every child already reaped rather than the last one, so running glibc first
# and the preload second and dividing yields max(a,b)/a: a number that can
# never fall below 1.00, and which therefore reported exactly 1.00 for any
# workload where slab was the better of the two.  It had teeth only in the
# direction of slab being worse.  wait4 reports the rusage of one named child.
rss_ratio() {
	_lib=$1
	shift
	python3 - "$_lib" "$@" <<'PY'
import os, sys
lib, args = sys.argv[1], sys.argv[2:]
def run(env):
    e = dict(os.environ); e.update(env)
    pid = os.fork()
    if pid == 0:
        fd = os.open(os.devnull, os.O_WRONLY)
        os.dup2(fd, 1)
        os.dup2(fd, 2)
        try:
            os.execvpe(args[0], args, e)
        finally:
            os._exit(127)
    _, st, ru = os.wait4(pid, 0)
    if st:
        sys.exit("child exited with status %d" % st)
    return ru.ru_maxrss
print("%.2f" % (run({"LD_PRELOAD": lib}) / run({})))
PY
}

# Footprint.  slab never returns memory to the OS (see the shortcut note in
# slab.c), so this is the ceiling made measurable rather than a pass we
# expect to be tight.
cat > "$T/churn.py" <<'PY'
d = [bytearray(i % 4096) for i in range(60000)]
d = [x for i, x in enumerate(d) if i % 3]
print(len(d))
PY
rss=$(rss_ratio "$SLAB" python3 "$T/churn.py")
if python3 -c "import sys; sys.exit(0 if float('$rss') < 3.0 else 1)"; then
	ok "peak RSS ${rss}x glibc on a churn workload (limit 3.0x)"
else
	bad "peak RSS ${rss}x glibc, over the 3.0x limit"
fi

# Thread churn.  Magazines belong to a thread, and a thread that exits runs
# none of our code, so every object still on its magazines becomes
# unreachable unless something reclaims them.  A thousand threads created and
# joined one at a time, with nothing live when it ends, makes peak RSS a
# direct measurement of what is retained per dead thread.
unit "stress: 1000 sequential threads, nothing live at the end" \
	./t/stress tchurn 1000
tcr=$(rss_ratio "$SLAB" ./t/stress tchurn 1000)
if python3 -c "import sys; sys.exit(0 if float('$tcr') < 1.5 else 1)"; then
	ok "peak RSS ${tcr}x glibc after 1000 dead threads (limit 1.5x)"
else
	bad "peak RSS ${tcr}x glibc after 1000 dead threads - magazines leaking?"
fi

# The span cache, measured from outside. "Freed large mappings are kept and
# reused" is a claim about syscalls, and nothing in a program's own output can
# see it, so strace counts them. 200 alloc/free cycles of one large size:
# without the cache that is 200 munmaps for the frees plus two trimming
# munmaps per map_aligned, and it measured 601. With it, 3.
#
# glibc's number is printed alongside and asserted small too, because if the
# workload were wrong -- a size that never reached the large path, say -- both
# would be near zero and the assertion would pass for nothing.
if command -v strace >/dev/null 2>&1; then
	cc -O1 -o "$T/bigloop" t/bigloop.c 2>/dev/null || {
		bad "could not build t/bigloop.c"; }
	if [ -x "$T/bigloop" ]; then
		mu() { LD_PRELOAD="$1" strace -c -e trace=munmap "$T/bigloop" 200 2>&1 \
		       >/dev/null | awk '$NF=="munmap"{print $4}'; }
		ours=$(mu "$SLAB"); ours=${ours:-0}
		theirs=$(LD_PRELOAD= strace -c -e trace=munmap "$T/bigloop" 200 2>&1 \
		         >/dev/null | awk '$NF=="munmap"{print $4}'); theirs=${theirs:-0}
		# Is the workload in the large path at all? The `theirs -gt 20`
		# guard below cannot answer that: glibc caches large frees too,
		# so a size that never reaches slab's large path gives
		# ours=1 theirs=1 and reads as a working cache. Changing
		# bigloop's malloc(300000) to malloc(300) produced exactly
		# that -- "ok span cache: 200 large cycles cost 1 munmaps
		# (glibc 1)". The size is reported by the program and checked
		# against slab's MAXSMALL here.
		bigsz=$("$T/bigloop" 1 size)
		maxsmall=$(sed -n 's/^#define MAXSMALL *\([0-9]*\)u.*/\1/p' slab.c)
		if [ -z "$bigsz" ] || [ -z "$maxsmall" ]; then
			bad "span cache: could not read the workload size or MAXSMALL"
		elif [ "$bigsz" -le "$maxsmall" ]; then
			bad "span cache: bigloop allocates $bigsz, at or under MAXSMALL ($maxsmall), so it never reaches the large path"
		elif [ "$theirs" -gt 20 ]; then
			bad "span cache: glibc made $theirs munmaps, so the workload is wrong"
		elif [ "$ours" -le 20 ]; then
			ok "span cache: 200 large cycles cost $ours munmaps (glibc $theirs)"
		else
			bad "span cache: 200 large cycles cost $ours munmaps, want <= 20 (glibc $theirs)"
		fi
	fi
else
	# Counted, not just printed: this is the one skippable check and the
	# suite has no skip counter, so "31 passed, 0 failed" with strace
	# missing was indistinguishable from a 31-check suite that ran
	# everything. prof does this correctly; slab did not.
	skipped=$((skipped + 1))
	printf 'SKIP span cache munmap count (strace not installed)\n'
fi

# ---------------------------------------------------------------- tier 3
# Does tier 2 have teeth?  Build deliberately broken slabs and require the
# oracle to reject each one.  A suite that cannot fail is not a suite, and
# the only way to find out which it is, is to try.
#
# Each mutant names the mode that is *supposed* to catch it, not just "any
# mode".  A broken calloc has to be caught by the targeted calloc check, not
# incidentally by the random churn - otherwise the named check could be dead
# code and the suite would still look green.
#
# Mutants are compiled with the same -fno-builtin flags as the real library,
# so this tests the oracle and not the build.
MUT="-std=c11 -O2 -fPIC -shared -fno-builtin-malloc -fno-builtin-calloc
     -fno-builtin-realloc -fno-builtin-free"

mutant() {
	name=$1
	mode=$2
	expr=$3
	sed "$expr" slab.c >"$T/mut.c"
	if cmp -s "$T/mut.c" slab.c; then
		bad "self-check $name: sed did not apply (slab.c changed shape)"
	elif ! gcc $MUT -o "$T/mut.so" "$T/mut.c" 2>"$T/m.err"; then
		bad "self-check $name: mutant did not compile"
		head -2 "$T/m.err" | sed 's/^/        /'
	elif LD_PRELOAD="$T/mut.so" ./t/stress $mode >/dev/null 2>&1; then
		bad "self-check: 'stress $mode' ACCEPTED $name"
	else
		ok "self-check: 'stress $mode' rejects $name"
	fi
}

mutant "objects that overlap by 8 bytes" "stress 200000" \
	's|\th->bump += cls_size\[cls\];|\th->bump += cls_size[cls] - 8;|'
mutant "a calloc that never zeroes" "check" \
	's|\t\tmemset(p, 0, size);|\t\t(void)0;|'
mutant "a realloc that copies half the bytes" "check" \
	's|\tmemcpy(q, p, old < size ? old : size);|\tmemcpy(q, p, (old < size ? old : size) / 2);|'
mutant "a class index one too small for one octave" "check" \
	's|>> (b - 2));|>> (b - 2)) - (b == 5);|'

# The two RSS limits are measurements rather than exit statuses, so `mutant`
# cannot drive them: an allocator that leaks a megabyte per dead thread still
# exits 0.  Build the no-adoption slab by hand and require the thread-churn
# limit to throw it out.
sed 's|if (syscall(SYS_tgkill, pid, t->owner, 0) == 0 |if (1 |' slab.c >"$T/na.c"
if cmp -s "$T/na.c" slab.c; then
	bad "self-check no-adoption: sed did not apply (slab.c changed shape)"
elif ! gcc $MUT -o "$T/na.so" "$T/na.c" 2>"$T/m.err"; then
	bad "self-check no-adoption: mutant did not compile"
	head -2 "$T/m.err" | sed 's/^/        /'
else
	nar=$(rss_ratio "$T/na.so" ./t/stress tchurn 1000)
	if python3 -c "import sys; sys.exit(0 if float('$nar') >= 1.5 else 1)"; then
		ok "self-check: thread-churn RSS rejects magazines with no adoption (${nar}x)"
	else
		bad "self-check: no-adoption magazines measured only ${nar}x, limit has no teeth"
	fi
fi

printf '\n%d passed, %d failed, %d skipped\n' "$pass" "$fail" "$skipped"
[ "$fail" -eq 0 ]
