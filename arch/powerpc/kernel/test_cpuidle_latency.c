// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Measure how long a CPU takes to wake up from an idle state when it is
 * woken by a directed IPI.
 *
 * Userspace can time a timer wakeup on its own, by sleeping on a pinned
 * thread and seeing how late it was woken, so there is no reason for the
 * kernel to help with that. What userspace cannot do is aim a single IPI
 * at one specific idle CPU, which is the only reason this module exists.
 *
 * The interface is in debugfs, under powerpc/latency_test/:
 *
 *	ipi_cpu_dest	Write a CPU number to send that CPU an IPI from the
 *			writing CPU. Reads back the CPU used last time.
 *	ipi_cpu_src	CPU the last IPI was sent from.
 *	ipi_latency_ns	Time from sending the last IPI to running the
 *			handler on the destination CPU.
 *
 * The IPI is sent from the CPU that issues the write(), so a caller picks
 * the source by binding itself to it, for example with taskset. The write
 * blocks until the handler has run, so the results are stable by the time
 * it returns.
 *
 * The latency includes the cost of sending the IPI, so a caller needs a
 * baseline from a CPU known to be awake to tell the two apart.
 */

#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/smp.h>

/* One set of results, so only let one measurement run at a time. */
static DEFINE_MUTEX(latency_mutex);
static struct dentry *latency_dir;

struct ipi_latency {
	unsigned int	src_cpu;
	unsigned int	dest_cpu;
	ktime_t		time_start;
	u64		latency_ns;
};

static struct ipi_latency ipi_wakeup;

static void ipi_latency_handler(void *info)
{
	struct ipi_latency *ipi = info;

	ipi->latency_ns = ktime_to_ns(ktime_sub(ktime_get(), ipi->time_start));
}

static int run_ipi_test(unsigned int cpu)
{
	int ret;

	/*
	 * Pin to the writing CPU so that the timestamp and the IPI both
	 * come from the CPU the caller asked to measure.
	 */
	ipi_wakeup.src_cpu = get_cpu();
	ipi_wakeup.dest_cpu = cpu;
	ipi_wakeup.latency_ns = 0;
	ipi_wakeup.time_start = ktime_get();
	ret = smp_call_function_single(cpu, ipi_latency_handler, &ipi_wakeup, 1);
	put_cpu();

	return ret;
}

static int ipi_cpu_dest_get(void *data, u64 *val)
{
	*val = ipi_wakeup.dest_cpu;

	return 0;
}

static int ipi_cpu_dest_set(void *data, u64 val)
{
	if (val >= nr_cpu_ids)
		return -EINVAL;

	guard(mutex)(&latency_mutex);

	return run_ipi_test(val);
}
DEFINE_DEBUGFS_ATTRIBUTE(ipi_cpu_dest_fops, ipi_cpu_dest_get, ipi_cpu_dest_set,
			 "%llu\n");

static int __init latency_test_init(void)
{
	latency_dir = debugfs_create_dir("latency_test", arch_debugfs_dir);

	debugfs_create_file_unsafe("ipi_cpu_dest", 0644, latency_dir, NULL,
				   &ipi_cpu_dest_fops);
	debugfs_create_u32("ipi_cpu_src", 0444, latency_dir, &ipi_wakeup.src_cpu);
	debugfs_create_u64("ipi_latency_ns", 0444, latency_dir,
			   &ipi_wakeup.latency_ns);

	return 0;
}

static void __exit latency_test_exit(void)
{
	debugfs_remove_recursive(latency_dir);
}

module_init(latency_test_init);
module_exit(latency_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pratik R. Sampat <psampat@linux.ibm.com>");
MODULE_AUTHOR("Aboorva Devarajan <aboorvad@linux.ibm.com>");
MODULE_DESCRIPTION("Measure cpuidle wakeup latency for a directed IPI");
