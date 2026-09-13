// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Concurrent malloc/free hammer to exercise SLUB remote free / refill
 * (the 128-bit freelist_counters cmpxchg path on POWER8+).
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

#define NTHREADS	8
#define ITERS		50000

static void *worker(void *arg)
{
	unsigned long seed = (unsigned long)arg;
	unsigned int i;

	for (i = 0; i < ITERS; i++) {
		size_t sz = 8 + ((seed + i) % 2048);
		void *p = malloc(sz);

		if (!p)
			return (void *)1;
		memset(p, (unsigned char)(i & 0xff), sz);
		free(p);
	}
	return NULL;
}

static int test_malloc_stress(void)
{
	pthread_t threads[NTHREADS];
	int i, n = NTHREADS;
	long nproc;

	nproc = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc > 0 && nproc < n)
		n = (int)nproc;
	if (n < 2)
		n = 2;

	for (i = 0; i < n; i++) {
		if (pthread_create(&threads[i], NULL, worker,
				   (void *)(unsigned long)(i + 1))) {
			perror("pthread_create");
			return 1;
		}
	}

	for (i = 0; i < n; i++) {
		void *ret;

		if (pthread_join(threads[i], &ret) || ret)
			return 1;
	}

	return 0;
}

int main(void)
{
	return test_harness(test_malloc_stress, "cmpxchg128_slub_stress");
}
