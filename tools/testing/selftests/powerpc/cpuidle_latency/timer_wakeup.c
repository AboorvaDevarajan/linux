// SPDX-License-Identifier: GPL-2.0
/*
 * Measure how late a sleeping thread is woken on one CPU.
 *
 * Bind to the given CPU, sleep until an absolute deadline, and print how
 * much later than that deadline we actually got back, in nanoseconds.
 * Sleeping is what lets the CPU enter an idle state, so with a deadline
 * past the target residency of the state under test, the overshoot
 * includes the cost of leaving it.
 *
 * A single wakeup is far too noisy to report on its own, so the sleep is
 * repeated and the smallest, middle and largest overshoot are printed on
 * one line. Repeating in this process rather than by being run again keeps
 * the samples on one thread that is already bound and already warm, and
 * leaves the spread visible to the caller instead of hidden in an average.
 *
 * The deadline is absolute so that a signal cannot shorten the sleep, and
 * the affinity is set here rather than by the caller so that the thread is
 * known to have slept and woken on the CPU being measured. Timer slack is
 * dropped because the 50us a task gets by default is larger than the
 * wakeup being measured.
 *
 * Author: Aboorva Devarajan <aboorvad@linux.ibm.com>
 */

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

#define NSEC_PER_SEC	1000000000LL

/* Keep a stray argument from turning into a very long sleep. */
#define MAX_SLEEP_NS	NSEC_PER_SEC
#define MAX_SAMPLES	10000

static long long timespec_to_ns(const struct timespec *ts)
{
	return (long long)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

static void ns_to_timespec(long long ns, struct timespec *ts)
{
	ts->tv_sec = ns / NSEC_PER_SEC;
	ts->tv_nsec = ns % NSEC_PER_SEC;
}

static int pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);

	return sched_setaffinity(0, sizeof(set), &set);
}

/*
 * strtoll() on its own accepts an empty string, and stops at the first
 * character it does not like without saying so, either of which would turn
 * a mistyped argument into a measurement of something else.
 */
static int parse_ll(const char *arg, long long min, long long max,
		    long long *res)
{
	long long val;
	char *rest;

	errno = 0;
	val = strtoll(arg, &rest, 10);
	if (rest == arg || *rest || errno || val < min || val > max)
		return -1;

	*res = val;
	return 0;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a;
	long long y = *(const long long *)b;

	return (x > y) - (x < y);
}

static int measure_once(const char *prog, int cpu, long long sleep_ns,
			long long *res)
{
	struct timespec start, deadline, end;
	int ret;

	if (clock_gettime(CLOCK_MONOTONIC, &start)) {
		fprintf(stderr, "%s: clock_gettime: %s\n", prog,
			strerror(errno));
		return -1;
	}
	ns_to_timespec(timespec_to_ns(&start) + sleep_ns, &deadline);

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				      &deadline, NULL);
	} while (ret == EINTR);

	if (ret) {
		fprintf(stderr, "%s: clock_nanosleep: %s\n", prog,
			strerror(ret));
		return -1;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &end)) {
		fprintf(stderr, "%s: clock_gettime: %s\n", prog,
			strerror(errno));
		return -1;
	}

	/*
	 * Affinity is pinned, so waking anywhere else means the measurement
	 * is not about the CPU that was asked for.
	 */
	if (sched_getcpu() != cpu) {
		fprintf(stderr, "%s: woke on CPU %d, wanted %d\n", prog,
			sched_getcpu(), cpu);
		return -1;
	}

	*res = timespec_to_ns(&end) - timespec_to_ns(&deadline);
	return 0;
}

int main(int argc, char *argv[])
{
	static long long samples[MAX_SAMPLES];
	long long sleep_ns, median, value;
	long long count = 1;
	int cpu, i;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s <cpu> <sleep-ns> [samples]\n",
			argv[0]);
		return 2;
	}

	if (parse_ll(argv[1], 0, INT_MAX, &value)) {
		fprintf(stderr, "%s: bad CPU '%s'\n", argv[0], argv[1]);
		return 2;
	}
	cpu = value;

	if (parse_ll(argv[2], 1, MAX_SLEEP_NS, &sleep_ns)) {
		fprintf(stderr, "%s: sleep must be 1..%lld ns, got '%s'\n",
			argv[0], MAX_SLEEP_NS, argv[2]);
		return 2;
	}

	if (argc == 4 && parse_ll(argv[3], 1, MAX_SAMPLES, &count)) {
		fprintf(stderr, "%s: samples must be 1..%d, got '%s'\n",
			argv[0], MAX_SAMPLES, argv[3]);
		return 2;
	}

	/*
	 * hrtimer_nanosleep() lets a timer fire up to current->timer_slack_ns
	 * late, and the 50us default is an order of magnitude more than the
	 * idle wakeup this is trying to see. Ask for the tightest the kernel
	 * will give us.
	 */
	if (prctl(PR_SET_TIMERSLACK, 1UL)) {
		fprintf(stderr, "%s: cannot drop timer slack: %s\n", argv[0],
			strerror(errno));
		return 1;
	}

	if (pin_to_cpu(cpu)) {
		fprintf(stderr, "%s: cannot bind to CPU %d: %s\n", argv[0], cpu,
			strerror(errno));
		return 1;
	}

	/* Give up the CPU so that the new affinity has taken effect. */
	sched_yield();
	if (sched_getcpu() != cpu) {
		fprintf(stderr, "%s: running on CPU %d, wanted %d\n", argv[0],
			sched_getcpu(), cpu);
		return 1;
	}

	for (i = 0; i < count; i++) {
		if (measure_once(argv[0], cpu, sleep_ns, &samples[i]))
			return 1;
	}

	qsort(samples, count, sizeof(samples[0]), cmp_ll);
	if (count % 2)
		median = samples[count / 2];
	else
		median = (samples[count / 2 - 1] + samples[count / 2]) / 2;

	printf("%lld %lld %lld\n", samples[0], median, samples[count - 1]);

	return 0;
}
