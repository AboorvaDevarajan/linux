#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Source-level checks that fully ordered powerpc atomics use lwsync/isync
# on POWER9+ (CPU_FTR_ARCH_300), that smp_mb() is still hwsync, and that
# failed cmpxchg skips the exit barrier. Skip if the kernel tree is not
# next to the installed test.

set -eu

ksft_skip=4

find_srctree()
{
	if [ -n "${KBUILD_SRC:-}" ] &&
	   [ -f "$KBUILD_SRC/arch/powerpc/include/asm/synch.h" ]; then
		printf '%s\n' "$KBUILD_SRC"
		return 0
	fi

	d=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
	i=0
	while [ "$i" -lt 8 ]; do
		d=$(dirname "$d")
		if [ -f "$d/arch/powerpc/include/asm/synch.h" ]; then
			printf '%s\n' "$d"
			return 0
		fi
		i=$((i + 1))
	done
	return 1
}

fail()
{
	printf '[FAIL] %s\n' "$1" >&2
	exit 1
}

pass()
{
	printf '[PASS] %s\n' "$1"
}

if ! srctree=$(find_srctree); then
	echo "skip: kernel source not found (need synch.h)"
	exit $ksft_skip
fi

synch="$srctree/arch/powerpc/include/asm/synch.h"
atomic="$srctree/arch/powerpc/include/asm/atomic.h"
cmpxchg="$srctree/arch/powerpc/include/asm/cmpxchg.h"
barrier="$srctree/arch/powerpc/include/asm/barrier.h"

# 64-bit SMP: POWER9+ FTR alternative lwsync/isync, else sync/sync.
# 32-bit SMP keeps hwsync.
grep -q 'BEGIN_FTR_SECTION_NESTED(80)' "$synch" ||
	fail "64-bit ENTRY must use an FTR alternative"
grep -q 'BEGIN_FTR_SECTION_NESTED(81)' "$synch" ||
	fail "64-bit EXIT must use an FTR alternative"
grep -q '0x20000' "$synch" || fail "FTR mask 0x20000 (CPU_FTR_ARCH_300) missing"
grep -q lwsync "$synch" || fail "P9+ ENTRY body must be lwsync"
grep -q isync "$synch" || fail "P9+ EXIT body must be isync"
grep -q 'CPU_FTR_ARCH_300' "$synch" || fail "comment/error must name CPU_FTR_ARCH_300"
pass "64-bit ENTRY/EXIT are POWER9+ (ARCH_300) lwsync/isync vs sync"

entry32=$(awk '
	/^#ifdef __powerpc64__/ { p=1; next }
	p && /^#else/ { p=2; next }
	p==2 && /^#endif/ { exit }
	p==2 && /^#define PPC_ATOMIC_ENTRY_BARRIER/ { print; exit }
' "$synch")
exit32=$(awk '
	/^#ifdef __powerpc64__/ { p=1; next }
	p && /^#else/ { p=2; next }
	p==2 && /^#endif/ { exit }
	p==2 && /^#define PPC_ATOMIC_EXIT_BARRIER/ { print; exit }
' "$synch")
[ -n "$entry32" ] || fail "32-bit PPC_ATOMIC_ENTRY_BARRIER missing"
[ -n "$exit32" ] || fail "32-bit PPC_ATOMIC_EXIT_BARRIER missing"
case "$entry32" in
*"stringify_in_c(sync)"*) pass "32-bit ENTRY still hwsync" ;;
*) fail "32-bit ENTRY should remain sync, got: $entry32" ;;
esac
case "$exit32" in
*"stringify_in_c(sync)"*) pass "32-bit EXIT still hwsync" ;;
*) fail "32-bit EXIT should remain sync, got: $exit32" ;;
esac

grep -q '__powerpc64__' "$synch" || fail "FTR alternatives must be under __powerpc64__"

grep -q 'define __smp_mb()' "$barrier" || fail "__smp_mb missing"
grep -E -q '#define __smp_mb\(\)[[:space:]]+__mb\(\)' "$barrier" ||
	fail "smp_mb() must remain __mb() (hwsync), not lwsync"
grep -E -q '#define __mb\(\)[[:space:]]+__asm__ __volatile__ \("sync"' "$barrier" ||
	fail "__mb() must remain hwsync (sync)"
pass "smp_mb()/__mb() still hwsync"

grep -q '__atomic_pre_full_fence' "$atomic" ||
	fail "atomic_add_return path must use __atomic_pre_full_fence (not leftover smp_mb)"
grep -q '__atomic_post_full_fence' "$atomic" ||
	fail "atomic_add_return path must use __atomic_post_full_fence"
grep -q 'PPC_ATOMIC_ENTRY_BARRIER' "$atomic" ||
	fail "__atomic_pre_full_fence must use PPC_ATOMIC_ENTRY_BARRIER"
grep -q 'PPC_ATOMIC_EXIT_BARRIER' "$atomic" ||
	fail "__atomic_post_full_fence must use PPC_ATOMIC_EXIT_BARRIER"
pass "__atomic_pre/post_full_fence route through PPC_ATOMIC_* macros"

# Failed CAS: exit barrier must sit before label 2 in __cmpxchg_u64.
awk '
	/^__cmpxchg_u64\(/ { in_fn = 1 }
	in_fn && /PPC_ATOMIC_EXIT_BARRIER/ { exit_line = NR }
	in_fn && /2:"/ { label_line = NR }
	in_fn && /^__cmpxchg_u64_local/ { exit }
	END {
		if (!exit_line || !label_line) {
			print "missing EXIT_BARRIER or label 2 in __cmpxchg_u64"
			exit 1
		}
		if (!(exit_line < label_line)) {
			print "PPC_ATOMIC_EXIT_BARRIER is not before label 2; failed CAS would isync"
			exit 1
		}
	}
' "$cmpxchg" || fail "failed cmpxchg must skip the exit barrier"
pass "__cmpxchg_u64 skips exit barrier on the bne fail path"

# This compiles the RFC instruction sequence, not vmlinux. Fully ordered
# atomics are inline, so there is no atomic_add_return symbol to dump.
# The kernel source checks above are the in-tree proof that vmlinux
# will use these macros.
cross=${CROSS_COMPILE:-}
cc=${cross}gcc
if ! command -v powerpc64le-linux-gnu-gcc >/dev/null 2>&1 &&
   ! command -v "$cc" >/dev/null 2>&1; then
	if [ "$(uname -m)" != ppc64le ] && [ "$(uname -m)" != ppc64 ]; then
		pass "skip objdump: no powerpc compiler"
		echo "ok: powerpc atomic barriers"
		exit 0
	fi
fi

if command -v powerpc64le-linux-gnu-gcc >/dev/null 2>&1; then
	cc=powerpc64le-linux-gnu-gcc
	objdump=powerpc64le-linux-gnu-objdump
elif command -v "${cross}gcc" >/dev/null 2>&1; then
	cc=${cross}gcc
	objdump=${cross}objdump
else
	cc=gcc
	objdump=objdump
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/seq.c" <<'EOF'
unsigned long add_lwsync(unsigned long a, unsigned long *v)
{
	unsigned long t;
	asm volatile(
	"	lwsync\n"
"1:	ldarx	%0,0,%3\n"
"	add	%0,%0,%2\n"
"	stdcx.	%0,0,%3\n"
"	bne-	1b\n"
	"	isync\n"
	: "=&r" (t), "+m" (*v)
	: "r" (a), "r" (v)
	: "cc", "memory", "xer");
	return t;
}
unsigned long cmpxchg_lwsync(unsigned long *p, unsigned long old, unsigned long new)
{
	unsigned long prev;
	asm volatile(
	"	lwsync\n"
"1:	ldarx	%0,0,%2\n"
"	cmpd	0,%0,%3\n"
"	bne-	2f\n"
"	stdcx.	%4,0,%2\n"
"	bne-	1b\n"
	"	isync\n"
"2:"
	: "=&r" (prev), "+m" (*p)
	: "r" (p), "r" (old), "r" (new)
	: "cc", "memory");
	return prev;
}
EOF

if ! "$cc" -O2 -c -ffreestanding -o "$tmp/seq.o" "$tmp/seq.c"; then
	fail "could not compile barrier sequence with $cc"
fi
dump=$("$objdump" -d "$tmp/seq.o")
echo "$dump" | grep -q lwsync || fail "objdump missing lwsync"
echo "$dump" | grep -q isync || fail "objdump missing isync"
echo "$dump" | grep -q ldarx || fail "objdump missing ldarx"

isync_off=$(printf '%s\n' "$dump" | awk '
	/<cmpxchg_lwsync>:/ { take=1; next }
	take && /<[a-zA-Z0-9_]+>:/ { exit }
	take && $0 ~ /[[:space:]]isync/ {
		split($1, a, ":")
		print a[1]
		exit
	}
')
bne_tgt=$(printf '%s\n' "$dump" | awk '
	/<cmpxchg_lwsync>:/ { take=1; next }
	take && /<[a-zA-Z0-9_]+>:/ { exit }
	take && $0 ~ /bne/ {
		for (i = 1; i <= NF; i++) {
			if ($i ~ /^bne/) {
				print $(i+1)
				exit
			}
		}
	}
')
[ -n "$isync_off" ] && [ -n "$bne_tgt" ] || fail "could not parse cmpxchg objdump (isync=$isync_off bne=$bne_tgt)"
isync_dec=$(printf '%d' "0x$isync_off")
bne_dec=$(printf '%d' "0x$bne_tgt")
[ "$bne_dec" -gt "$isync_dec" ] ||
	fail "failed CAS bne target 0x$bne_tgt is not after isync 0x$isync_off"
pass "objdump: lwsync/isync sequence; failed CAS skips isync"

echo "ok: powerpc atomic barriers"
exit 0
