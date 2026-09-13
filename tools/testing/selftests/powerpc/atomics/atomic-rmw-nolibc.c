// SPDX-License-Identifier: GPL-2.0-only
/*
 * Freestanding functional + TB-tick microbench for lwsync/isync RMWs.
 * Builds with a cross gcc that has no ppc64le sysroot.
 */

#include "atomic-rmw-asm.h"

#define __NR_write	4
#define __NR_exit	1
#define BENCH_ITERS	2000000UL

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

	while (s[n])
		n++;
	return n;
}

static void print(const char *s)
{
	sys_write(1, s, strlen_s(s));
}

static void print_ul(unsigned long v)
{
	char buf[24];
	int i = 0, j;
	char tmp[24];

	if (!v) {
		print("0");
		return;
	}
	while (v) {
		tmp[i++] = '0' + (v % 10);
		v /= 10;
	}
	j = 0;
	while (i)
		buf[j++] = tmp[--i];
	buf[j] = 0;
	print(buf);
}

static int fail(const char *msg)
{
	print("[FAIL] ");
	print(msg);
	print("\n");
	return 1;
}

static unsigned long mftb(void)
{
	unsigned long t;

	asm volatile("mftb %0" : "=r" (t));
	return t;
}

static int test_single(void)
{
	atomic_word_t v = 40;
	atomic_word_t r;

	r = atomic_add_return_lwsync_isync(2, &v);
	if (r != 42 || v != 42)
		return fail("add_return single");

	r = cmpxchg_barriers(&v, 42, 99, ATOMIC_BARRIER_LWSYNC_ISYNC);
	if (r != 42 || v != 99)
		return fail("cmpxchg success");

	r = cmpxchg_barriers(&v, 42, 7, ATOMIC_BARRIER_LWSYNC_ISYNC);
	if (r != 99 || v != 99)
		return fail("cmpxchg fail must skip store");

	print("[PASS] single-thread add_return/cmpxchg\n");
	return 0;
}

static int bench_one(const char *name,
		     atomic_word_t (*fn)(atomic_word_t, atomic_word_t *),
		     unsigned long *ticks_out)
{
	atomic_word_t v = 0;
	unsigned long i, t0, t1;

	for (i = 0; i < 10000; i++)
		fn(1, &v);
	v = 0;
	t0 = mftb();
	for (i = 0; i < BENCH_ITERS; i++)
		fn(1, &v);
	t1 = mftb();
	if (v != BENCH_ITERS)
		return fail("bench counter");

	*ticks_out = t1 - t0;
	print(name);
	print("  ");
	print_ul(*ticks_out / BENCH_ITERS);
	print(".");
	print_ul((*ticks_out * 10 / BENCH_ITERS) % 10);
	print(" TB ticks/op\n");
	return 0;
}

static int test_bench(void)
{
	unsigned long hwsync_t, lwsync_t, relaxed_t;

	print("RMW microbench (TB ticks, ");
	print_ul(BENCH_ITERS);
	print(" ops)\n");
	if (bench_one("relaxed LL/SC", atomic_add_return_relaxed_loop,
		      &relaxed_t))
		return 1;
	if (bench_one("hwsync + hwsync", atomic_add_return_hwsync, &hwsync_t))
		return 1;
	if (bench_one("lwsync + isync", atomic_add_return_lwsync_isync,
		      &lwsync_t))
		return 1;

	print("speedup x10 (hwsync/lwsync) = ");
	print_ul((hwsync_t * 10) / (lwsync_t ? lwsync_t : 1));
	print("\n(TCG does not model nest hwsync cost; ");
	print("use atomic-rmw-bench on POWER hardware)\n");
	(void)relaxed_t;
	return 0;
}

void _start(void)
{
	int rc = 0;

	print("[PASS] freestanding atomic RMW start\n");
	rc |= test_single();
	rc |= test_bench();
	if (!rc)
		print("ok: atomic-rmw-nolibc\n");
	sys_exit(rc);
}
