// SPDX-License-Identifier: GPL-2.0-only
/*
 * Microbenchmark for fully ordered RMWs: hwsync/hwsync vs lwsync/isync.
 *
 * The RFC claim is a ~2x win on back-to-back fully ordered RMWs on
 * POWER10. TCG does not model nest latency, so the speedup check is
 * skipped under QEMU emulation unless PPC_ATOMIC_BENCH_REQUIRE_SPEEDUP
 * is set.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "atomic-rmw-asm.h"
#include "utils.h"

#define ITERS		20000000UL
#define WARMUP		100000UL

static double nsec_since(struct timespec start, struct timespec end)
{
	return (end.tv_sec - start.tv_sec) * 1e9 +
	       (end.tv_nsec - start.tv_nsec);
}

static int running_under_qemu(void)
{
	char buf[4096];
	FILE *f;
	int qemu = 0;

	f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return 0;

	while (fgets(buf, sizeof(buf), f)) {
		if (strcasestr(buf, "qemu") || strcasestr(buf, "emulated")) {
			qemu = 1;
			break;
		}
	}
	fclose(f);
	return qemu;
}

static double bench(const char *name,
		    atomic_word_t (*fn)(atomic_word_t, atomic_word_t *),
		    unsigned long iters)
{
	struct timespec t0, t1;
	atomic_word_t v = 0;
	unsigned long i;
	double ns;

	if (bind_to_cpu(BIND_CPU_ANY) < 0) {
		perror("bind_to_cpu");
		return -1.0;
	}

	for (i = 0; i < WARMUP; i++)
		fn(1, &v);

	v = 0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < iters; i++)
		fn(1, &v);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (v != iters) {
		fprintf(stderr, "[FAIL] %s: counter %lu != %lu\n",
			name, (unsigned long)v, iters);
		return -1.0;
	}

	ns = nsec_since(t0, t1) / (double)iters;
	printf("%-24s %10.2f ns/op  (%lu ops)\n", name, ns, iters);
	fflush(stdout);
	return ns;
}

static int test_atomic_rmw_bench(void)
{
	double hwsync_ns, lwsync_ns, relaxed_ns;
	int require;
	int qemu;

	printf("powerpc fully-ordered RMW microbenchmark\n");
	printf("  hwsync ; larx/stcx. ; hwsync     (old)\n");
	printf("  lwsync ; larx/stcx. ; isync      (RFC)\n");

	relaxed_ns = bench("relaxed LL/SC",
			   atomic_add_return_relaxed_loop, ITERS);
	hwsync_ns = bench("hwsync + hwsync",
			  atomic_add_return_hwsync, ITERS);
	lwsync_ns = bench("lwsync + isync",
			  atomic_add_return_lwsync_isync, ITERS);
	FAIL_IF(relaxed_ns < 0 || hwsync_ns < 0 || lwsync_ns < 0);

	printf("speedup (hwsync / lwsync+isync) = %.2fx\n",
	       hwsync_ns / lwsync_ns);
	printf("relaxed baseline = %.2f ns/op\n", relaxed_ns);

	qemu = running_under_qemu();
	require = getenv("PPC_ATOMIC_BENCH_REQUIRE_SPEEDUP") != NULL;

	printf("Paste POWER8+ numbers in the cover letter. TCG is not a result.\n");
	if (qemu)
		printf("QEMU/TCG: nest hwsync cost is not modelled; ignore the ratio.\n");

	if (!require)
		return 0;

	FAIL_IF_MSG(lwsync_ns >= hwsync_ns,
		    "lwsync+isync was not faster than two hwsyncs");
	FAIL_IF_MSG(hwsync_ns / lwsync_ns < 1.2,
		    "expected at least 1.2x speedup on hardware");

	return 0;
}

int main(void)
{
	test_harness_set_timeout(300);
	return test_harness(test_atomic_rmw_bench, "atomic_rmw_bench");
}
