// SPDX-License-Identifier: GPL-2.0-only
/*
 * Functional checks for the fully ordered powerpc RMW sequence
 * (lwsync ; l{w,d}arx/st{w,d}cx. ; isync) and for failed cmpxchg
 * skipping the exit barrier.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "atomic-rmw-asm.h"
#include "utils.h"

#define NTHREADS	8
#define NITERS		200000

struct add_arg {
	atomic_word_t *counter;
	int iters;
};

static void *add_thread(void *arg)
{
	struct add_arg *a = arg;
	int i;

	for (i = 0; i < a->iters; i++)
		atomic_add_return_barriers(1, a->counter,
					   ATOMIC_BARRIER_LWSYNC_ISYNC);

	return NULL;
}

static int test_add_return_single(void)
{
	atomic_word_t v = 40;
	atomic_word_t r;

	r = atomic_add_return_barriers(2, &v, ATOMIC_BARRIER_LWSYNC_ISYNC);
	FAIL_IF(r != 42);
	FAIL_IF(v != 42);

	r = atomic_add_return_lwsync_isync(8, &v);
	FAIL_IF(r != 50);
	FAIL_IF(v != 50);

	return 0;
}

static int test_add_return_threaded(void)
{
	pthread_t tids[NTHREADS];
	struct add_arg arg;
	atomic_word_t v = 0;
	int i;

	arg.counter = &v;
	arg.iters = NITERS;

	for (i = 0; i < NTHREADS; i++)
		FAIL_IF(pthread_create(&tids[i], NULL, add_thread, &arg));

	for (i = 0; i < NTHREADS; i++)
		FAIL_IF(pthread_join(tids[i], NULL));

	FAIL_IF_MSG(v != (atomic_word_t)NTHREADS * NITERS,
		    "threaded atomic_add_return lost updates");

	return 0;
}

static int test_cmpxchg_success_fail(void)
{
	atomic_word_t v = 0x1111;
	atomic_word_t prev;

	prev = cmpxchg_barriers(&v, 0x1111, 0x2222,
				ATOMIC_BARRIER_LWSYNC_ISYNC);
	FAIL_IF(prev != 0x1111);
	FAIL_IF(v != 0x2222);

	/* Fail: value unchanged, returned current, no store. */
	prev = cmpxchg_barriers(&v, 0x1111, 0x3333,
				ATOMIC_BARRIER_LWSYNC_ISYNC);
	FAIL_IF(prev != 0x2222);
	FAIL_IF(v != 0x2222);

	prev = cmpxchg_barriers(&v, 0x2222, 0x3333,
				ATOMIC_BARRIER_LWSYNC_ISYNC);
	FAIL_IF(prev != 0x2222);
	FAIL_IF(v != 0x3333);

	return 0;
}

static int test_cmpxchg_fail_does_not_store(void)
{
	atomic_word_t v = 7;
	int i;

	for (i = 0; i < 10000; i++) {
		atomic_word_t prev;

		prev = cmpxchg_barriers(&v, 0, 99, ATOMIC_BARRIER_LWSYNC_ISYNC);
		FAIL_IF(prev != 7);
		FAIL_IF(v != 7);
	}

	return 0;
}

static int test_hwsync_sequence_still_correct(void)
{
	atomic_word_t v = 0;
	atomic_word_t r;

	r = atomic_add_return_hwsync(1, &v);
	FAIL_IF(r != 1);
	FAIL_IF(v != 1);

	return 0;
}

static int test_atomic_rmw(void)
{
	FAIL_IF(test_add_return_single());
	FAIL_IF(test_add_return_threaded());
	FAIL_IF(test_cmpxchg_success_fail());
	FAIL_IF(test_cmpxchg_fail_does_not_store());
	FAIL_IF(test_hwsync_sequence_still_correct());

	return 0;
}

int main(void)
{
	return test_harness(test_atomic_rmw, "atomic_rmw");
}
