// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding POWER9 / POWER10 check: one static binary to copy onto
 * each machine. Skips unless AT_HWCAP2 has ISA 3.00 (CPU_FTR_ARCH_300).
 */

#include "atomic-rmw-asm.h"

#define __NR_write		4
#define __NR_exit		1
#define AT_NULL			0
#define AT_PLATFORM		15
#define AT_BASE_PLATFORM	24
#define AT_HWCAP2		26
#define PPC_FEATURE2_ARCH_3_00	0x00800000
#define PPC_FEATURE2_ARCH_3_1	0x00040000
#define PPC_FEATURE2_ARCH_3_2	0x00010000
#define KSFT_SKIP		4

#define PPC_INST_LWSYNC		0x7c2004ac
#define PPC_INST_SYNC		0x7c0004ac
#define PPC_INST_ISYNC		0x4c00012c
#define PPC_INST_BLR		0x4e800020
#define SCAN_MAX_INSNS		64

static long sys_write(int fd, const char *buf, unsigned long n)
{
	register long r0 asm("r0") = __NR_write;
	register long r3 asm("r3") = fd;
	register long r4 asm("r4") = (long)buf;
	register long r5 asm("r5") = n;

	asm volatile("sc" : "+r" (r3) : "r" (r0), "r" (r4), "r" (r5)
		     : "memory", "cr0", "ctr", "r6", "r7", "r8", "r9",
		       "r10", "r11", "r12");
	return r3;
}

static void sys_exit(int code)
{
	register long r0 asm("r0") = __NR_exit;
	register long r3 asm("r3") = code;

	asm volatile("sc" : "+r" (r3) : "r" (r0) : "memory");
	for (;;)
		;
}

static unsigned long strlen_s(const char *s)
{
	unsigned long n = 0;

	if (!s)
		return 0;
	while (s[n])
		n++;
	return n;
}

static void print(const char *s)
{
	sys_write(1, s, strlen_s(s));
}

static int fail(const char *msg)
{
	print("[FAIL] ");
	print(msg);
	print("\n");
	return 1;
}

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

static int scan_fn(const void *fn, unsigned int *insns, int *ninsns)
{
	const unsigned int *p = fn_code(fn);
	int i;

	for (i = 0; i < SCAN_MAX_INSNS; i++) {
		insns[i] = p[i];
		if (insns[i] == PPC_INST_BLR) {
			*ninsns = i + 1;
			return 0;
		}
	}
	return fail("no blr in scanned function");
}

static int find_op(const unsigned int *insns, int n, unsigned int op)
{
	int i;

	for (i = 0; i < n; i++) {
		if (insns[i] == op)
			return i;
	}
	return -1;
}

static int check_insns(void)
{
	unsigned int insns[SCAN_MAX_INSNS];
	int n, lwsync, isync, i, seen_bne = 0;

	if (scan_fn(p9_add_return, insns, &n))
		return 1;
	lwsync = find_op(insns, n, PPC_INST_LWSYNC);
	isync = find_op(insns, n, PPC_INST_ISYNC);
	if (lwsync < 0 || isync < 0 || isync <= lwsync)
		return fail("p9_add_return missing lwsync/isync order");
	if (find_op(insns, n, PPC_INST_SYNC) >= 0)
		return fail("p9_add_return used hwsync");
	print("[PASS] p9_add_return is lwsync ; larx/stcx. ; isync\n");

	if (scan_fn(p9_cmpxchg, insns, &n))
		return 1;
	isync = find_op(insns, n, PPC_INST_ISYNC);
	if (isync < 0 || find_op(insns, n, PPC_INST_LWSYNC) < 0)
		return fail("p9_cmpxchg missing lwsync/isync");
	for (i = 0; i < isync; i++) {
		unsigned int op = insns[i];
		int rel, target;

		if ((op & 0xfc000000) != 0x40000000)
			continue;
		if (op & 2)
			continue;
		rel = (short)(op & 0xfffc);
		target = i + (rel / 4);
		if (target <= isync)
			continue;
		seen_bne = 1;
		break;
	}
	if (!seen_bne)
		return fail("failed-CAS bne does not skip isync");
	print("[PASS] p9_cmpxchg failed CAS skips isync\n");
	return 0;
}

static int check_functional(void)
{
	atomic_word_t v = 40;
	atomic_word_t r;

	r = p9_add_return(2, &v);
	if (r != 42 || v != 42)
		return fail("add_return");
	r = p9_cmpxchg(&v, 42, 99);
	if (r != 42 || v != 99)
		return fail("cmpxchg success");
	r = p9_cmpxchg(&v, 42, 7);
	if (r != 99 || v != 99)
		return fail("cmpxchg fail must not store");
	print("[PASS] functional RMW on this CPU\n");
	return 0;
}

static const char *isa_label(unsigned long hwcap2)
{
	if (hwcap2 & PPC_FEATURE2_ARCH_3_2)
		return "POWER11+ (ISA 3.2)";
	if (hwcap2 & PPC_FEATURE2_ARCH_3_1)
		return "POWER10 (ISA 3.1)";
	if (hwcap2 & PPC_FEATURE2_ARCH_3_00)
		return "POWER9 (ISA 3.0)";
	return "pre-POWER9";
}

void _start(void)
{
	unsigned long *sp;
	unsigned long argc, hwcap2 = 0;
	unsigned long *p;
	const char *plat = "?";
	const char *base = "?";
	int rc;

	/*
	 * _start is a C function, so GCC has already allocated a frame.
	 * 0(r1) is the incoming SP; that is the ELF argc/argv/env/auxv.
	 */
	asm volatile("ld %0,0(1)" : "=r" (sp));
	argc = sp[0];
	p = sp + 1 + argc + 1;
	while (*p)
		p++;
	p++;
	for (; p[0] || p[1]; p += 2) {
		if (p[0] == AT_HWCAP2)
			hwcap2 = p[1];
		else if (p[0] == AT_PLATFORM)
			plat = (const char *)p[1];
		else if (p[0] == AT_BASE_PLATFORM)
			base = (const char *)p[1];
	}

	print("cpu: ");
	print(isa_label(hwcap2));
	print("\nAT_PLATFORM=");
	print(plat);
	print(" AT_BASE_PLATFORM=");
	print(base);
	print("\n");

	if (!(hwcap2 & PPC_FEATURE2_ARCH_3_00)) {
		print("[SKIP] need POWER9+ (ISA 3.00 / CPU_FTR_ARCH_300)\n");
		sys_exit(KSFT_SKIP);
	}

	rc = 0;
	rc |= check_insns();
	rc |= check_functional();
	if (!rc)
		print("ok: atomic-p9-p10-nolibc\n");
	sys_exit(rc);
}
