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
	ok "$name ($(wc -c <"$T/a.out") bytes identical)"
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

# Footprint.  slab never returns memory to the OS (see the shortcut note in
# slab.c), so this is the ceiling made measurable rather than a pass we
# expect to be tight.
rss=$(python3 - "$SLAB" <<'PY'
import os, resource, subprocess, sys
prog = ["python3", "-c",
        "d=[bytearray(i%4096) for i in range(60000)]\n"
        "d=[x for i,x in enumerate(d) if i%3]\n"
        "print(len(d))"]
def run(env):
    e = dict(os.environ); e.update(env)
    subprocess.run(prog, env=e, stdout=subprocess.DEVNULL)
    return resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
a = run({})
b = run({"LD_PRELOAD": sys.argv[1]})
print("%.2f" % (b / a))
PY
)
if python3 -c "import sys; sys.exit(0 if float('$rss') < 3.0 else 1)"; then
	ok "peak RSS ${rss}x glibc on a churn workload (limit 3.0x)"
else
	bad "peak RSS ${rss}x glibc, over the 3.0x limit"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
