// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone POWER9 / POWER10 check for the fully ordered RMW sequence
 * (lwsync ; larx/stcx. ; isync).
 *
 * Kernel PPC_ATOMIC_* macros patch that sequence in at boot when
 * CPU_FTR_ARCH_300 is set (POWER9+). Userspace cannot apply those
 * fixups, so this binary compiles the P9+ body and:
 *
 *  1. SKIPs unless AT_HWCAP2 has PPC_FEATURE2_ARCH_3_00 (ISA 3.0 / P9+).
 *  2. Prints POWER9 vs POWER10 (vs later) from HWCAP2 + AT_PLATFORM.
 *  3. Scans the compiled instruction stream for lwsync / isync, and
 *     that a failed cmpxchg branches past isync.
 *  4. Runs the sequence on this CPU.
 *
 * Copy one binary onto a POWER9 and a POWER10 box and run it on each.
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>

#include "atomic-rmw-asm.h"
#include "utils.h"

#ifndef PPC_FEATURE2_ARCH_3_2
#define PPC_FEATURE2_ARCH_3_2	0x00010000
#endif

#define PPC_INST_LWSYNC	0x7c2004ac
#define PPC_INST_SYNC	0x7c0004ac
#define PPC_INST_ISYNC	0x4c00012c
#define PPC_INST_BLR	0x4e800020

#define SCAN_MAX_INSNS	64

static __attribute__((noinline)) atomic_word_t
p9_add_return(atomic_word_t a, atomic_word_t *v)
{
	atomic_word_t t;

	asm volatile(
	"	lwsync\n"
"1:	" PPC_LLARX "	%0,0,%3\n"
"	add	%0,%0,%2\n"
"	" PPC_STLCX "	%0,0,%3\n"
"	bne-	1b\n"
	"	isync\n"
	: "=&r" (t), "+m" (*v)
	: "r" (a), "r" (v)
	: "cc", "memory", "xer");

	return t;
}

static __attribute__((noinline)) atomic_word_t
p9_cmpxchg(atomic_word_t *p, atomic_word_t old, atomic_word_t new)
{
	atomic_word_t prev;

	asm volatile(
	"	lwsync\n"
"1:	" PPC_LLARX "	%0,0,%2\n"
"	" PPC_CMP "	0,%0,%3\n"
"	bne-	2f\n"
"	" PPC_STLCX "	%4,0,%2\n"
"	bne-	1b\n"
	"	isync\n"
"2:"
	: "=&r" (prev), "+m" (*p)
	: "r" (p), "r" (old), "r" (new)
	: "cc", "memory");

	return prev;
}

static const void *fn_code(const void *fn)
{
#if defined(_CALL_ELF) && _CALL_ELF == 1
	return *(const void * const *)fn;
#else
	return fn;
#endif
}

static const char *isa_label(void)
{
	if (have_hwcap2(PPC_FEATURE2_ARCH_3_2))
		return "POWER11+ (ISA 3.2)";
	if (have_hwcap2(PPC_FEATURE2_ARCH_3_1))
		return "POWER10 (ISA 3.1)";
	if (have_hwcap2(PPC_FEATURE2_ARCH_3_00))
		return "POWER9 (ISA 3.0)";
	return "pre-POWER9";
}

static int scan_fn(const char *name, const void *fn, uint32_t *insns, int *ninsns)
{
	const uint32_t *p = fn_code(fn);
	int i;

	for (i = 0; i < SCAN_MAX_INSNS; i++) {
		insns[i] = p[i];
		if (insns[i] == PPC_INST_BLR) {
			*ninsns = i + 1;
			return 0;
		}
	}

	fprintf(stderr, "[FAIL] %s: no blr in first %d insns\n", name,
		SCAN_MAX_INSNS);
	return 1;
}

static int find_op(const uint32_t *insns, int n, uint32_t op)
{
	int i;

	for (i = 0; i < n; i++) {
		if (insns[i] == op)
			return i;
	}
	return -1;
}

static int check_add_sequence(void)
{
	uint32_t insns[SCAN_MAX_INSNS];
	int n, lwsync, isync, sync;

	FAIL_IF(scan_fn("p9_add_return", p9_add_return, insns, &n));

	lwsync = find_op(insns, n, PPC_INST_LWSYNC);
	isync = find_op(insns, n, PPC_INST_ISYNC);
	sync = find_op(insns, n, PPC_INST_SYNC);

	FAIL_IF_MSG(lwsync < 0, "p9_add_return missing lwsync");
	FAIL_IF_MSG(isync < 0, "p9_add_return missing isync");
	FAIL_IF_MSG(isync <= lwsync, "isync is not after lwsync");
	FAIL_IF_MSG(sync >= 0, "p9_add_return used hwsync (pre-P9 sequence)");

	printf("  p9_add_return: lwsync @+%d isync @+%d (%d insns)\n",
	       lwsync, isync, n);
	return 0;
}

static int check_cmpxchg_fail_skips_isync(void)
{
	uint32_t insns[SCAN_MAX_INSNS];
	int n, isync, i, seen_bne = 0;

	FAIL_IF(scan_fn("p9_cmpxchg", p9_cmpxchg, insns, &n));
	isync = find_op(insns, n, PPC_INST_ISYNC);
	FAIL_IF_MSG(isync < 0, "p9_cmpxchg missing isync");
	FAIL_IF_MSG(find_op(insns, n, PPC_INST_LWSYNC) < 0,
		    "p9_cmpxchg missing lwsync");

	/*
	 * First bne (failed compare) should target an address after isync.
	 * Encoding: BO/BI in bits, relative LI in bits 16-29, AA=0, LK=0.
	 * bc 4,2,target is bne. Accept any bc whose target PC is > isync.
	 */
	for (i = 0; i < isync; i++) {
		uint32_t op = insns[i];
		int32_t rel;
		int target;

		if ((op & 0xfc000000) != 0x40000000)
			continue;
		/* bc; relative, not absolute */
		if (op & 2)
			continue;
		rel = (int16_t)(op & 0xfffc);
		target = i + (rel / 4);
		if (target <= isync)
			continue;
		seen_bne = 1;
		printf("  p9_cmpxchg: bne @+%d -> +%d (after isync @+%d)\n",
		       i, target, isync);
		break;
	}

	FAIL_IF_MSG(!seen_bne, "failed-CAS bne does not skip isync");
	return 0;
}

static int check_functional(void)
{
	atomic_word_t v = 40;
	atomic_word_t r, prev;

	r = p9_add_return(2, &v);
	FAIL_IF(r != 42);
	FAIL_IF(v != 42);

	prev = p9_cmpxchg(&v, 42, 99);
	FAIL_IF(prev != 42);
	FAIL_IF(v != 99);

	prev = p9_cmpxchg(&v, 42, 7);
	FAIL_IF(prev != 99);
	FAIL_IF(v != 99);

	return 0;
}

static int test_atomic_p9_p10(void)
{
	const char *plat = auxv_platform();
	const char *base = auxv_base_platform();

	SKIP_IF_MSG(!have_hwcap2(PPC_FEATURE2_ARCH_3_00),
		    "need POWER9+ (ISA 3.00 / CPU_FTR_ARCH_300)");

	printf("  cpu: %s\n", isa_label());
	printf("  AT_PLATFORM=%s AT_BASE_PLATFORM=%s\n",
	       plat ? plat : "?", base ? base : "?");
	printf("  HWCAP2 ARCH_3_00=%d ARCH_3_1=%d ARCH_3_2=%d\n",
	       have_hwcap2(PPC_FEATURE2_ARCH_3_00),
	       have_hwcap2(PPC_FEATURE2_ARCH_3_1),
	       have_hwcap2(PPC_FEATURE2_ARCH_3_2));

	FAIL_IF(check_add_sequence());
	FAIL_IF(check_cmpxchg_fail_skips_isync());
	FAIL_IF(check_functional());

	return 0;
}

int main(void)
{
	return test_harness(test_atomic_p9_p10, "atomic_p9_p10");
}
