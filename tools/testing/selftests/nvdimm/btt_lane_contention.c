// SPDX-License-Identifier: GPL-2.0
/*
 * btt_lane_contention.c - Test BTT lane serialization under preemption
 *
 * Verifies that concurrent I/O to a sector-mode (BTT-backed) pmem device
 * does not corrupt data.  Pins pairs of processes to the same CPU so they
 * compute the same BTT lane, writes a unique pattern per process to
 * exclusive device regions, and checks for miscompares on readback.
 *
 * Any corruption proves a kernel BTT lane serialization bug, since no
 * userspace data overlap exists.
 *
 * Requires a sector-mode pmem namespace (/dev/pmemXs).
 * Skips gracefully if no BTT device is available.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/fs.h>
#include "../kselftest.h"

#define IO_SIZE		(256 * 1024)
#define DEF_NPROCS	16
#define DEF_ITERS	100

static void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

static int child(const char *dev, int id, int nprocs,
		 off_t dev_size, int iters, int cpu, int lblock_size)
{
	off_t region = (dev_size / nprocs) & ~((off_t)IO_SIZE - 1);
	off_t base = (off_t)id * region;
	size_t nblocks = region / IO_SIZE;
	unsigned char *wbuf, *rbuf;
	int fd, rc = 0;

	pin_to_cpu(cpu);

	wbuf = aligned_alloc(lblock_size, IO_SIZE);
	rbuf = aligned_alloc(lblock_size, IO_SIZE);
	fd = open(dev, O_RDWR | O_DIRECT);
	if (fd < 0)
		fd = open(dev, O_RDWR);
	if (!wbuf || !rbuf || fd < 0) {
		perror("child setup");
		free(wbuf);
		free(rbuf);
		return 1;
	}

	memset(wbuf, 0x40 + id, IO_SIZE);

	for (int i = 0; i < iters && !rc; i++) {
		for (size_t b = 0; b < nblocks && !rc; b++) {
			off_t off = base + (off_t)b * IO_SIZE;

			if (pwrite(fd, wbuf, IO_SIZE, off) != IO_SIZE) {
				ksft_print_msg("[proc %d] pwrite failed at 0x%lx: %s\n",
					       id, (unsigned long)off, strerror(errno));
				rc = 1;
				break;
			}

			if (pread(fd, rbuf, IO_SIZE, off) != IO_SIZE) {
				ksft_print_msg("[proc %d] pread failed at 0x%lx: %s\n",
					       id, (unsigned long)off, strerror(errno));
				rc = 1;
				break;
			}

			if (memcmp(wbuf, rbuf, IO_SIZE) == 0)
				continue;

			/*
			 * Find the first corrupted byte and count the
			 * total damage.
			 */
			size_t first = 0, bad = 0;

			for (size_t j = 0; j < IO_SIZE; j++) {
				if (wbuf[j] != rbuf[j]) {
					if (!bad)
						first = j;
					bad++;
				}
			}

			unsigned char got = rbuf[first];
			int other = got - 0x40;

			ksft_print_msg("[proc %d] MISCOMPARE iter=%d block=%zu off=0x%lx\n",
				       id, i, b, (unsigned long)off);
			ksft_print_msg("[proc %d]   byte %zu: exp 0x%02x got 0x%02x (%zu/%d bad)\n",
				       id, first, wbuf[first], got, bad, IO_SIZE);

			if (other >= 0 && other < nprocs)
				ksft_print_msg("[proc %d]   from proc %d (shared CPU %d)\n",
					       id, other, other % (nprocs / 2));

			rc = 1;
		}
	}

	close(fd);
	free(wbuf);
	free(rbuf);
	return rc;
}

static int find_btt_device(char *path, size_t len)
{
	struct dirent *entry;
	DIR *dir;

	dir = opendir("/dev");
	if (!dir)
		return 0;

	while ((entry = readdir(dir))) {
		if (strncmp(entry->d_name, "pmem", 4) == 0 &&
		    strchr(entry->d_name, 's')) {
			snprintf(path, len, "/dev/%s", entry->d_name);
			closedir(dir);
			return 1;
		}
	}
	closedir(dir);
	return 0;
}

int main(int argc, char **argv)
{
	int nprocs = DEF_NPROCS, iters = DEF_ITERS, fail = 0;
	int cpus[CPU_SETSIZE], ncpus = 0, cpus_used;
	char dev_path[256];
	const char *dev;
	off_t dev_size;
	cpu_set_t set;
	pid_t *pids;
	int fd, lblock_size;

	ksft_print_header();
	ksft_set_plan(1);

	if (argc >= 2) {
		dev = argv[1];
	} else {
		if (!find_btt_device(dev_path, sizeof(dev_path)))
			ksft_exit_skip("No BTT device found (need /dev/pmemXs)\n");
		dev = dev_path;
	}

	if (argc > 2)
		nprocs = atoi(argv[2]);
	if (argc > 3)
		iters = atoi(argv[3]);

	if (nprocs < 2)
		ksft_exit_skip("Need at least 2 processes to test lane sharing\n");

	fd = open(dev, O_RDONLY);
	if (fd < 0)
		ksft_exit_skip("Cannot open %s: %s\n", dev, strerror(errno));

	if (ioctl(fd, BLKGETSIZE64, &dev_size) < 0) {
		close(fd);
		ksft_exit_skip("Cannot get size of %s\n", dev);
	}

	if (ioctl(fd, BLKSSZGET, &lblock_size) < 0) {
		close(fd);
		ksft_exit_skip("Cannot get logical block size of %s\n", dev);
	}
	close(fd);

	if (dev_size / nprocs < IO_SIZE)
		ksft_exit_skip("Device %s too small for %d processes\n",
			       dev, nprocs);

	if (lblock_size <= 0 || (IO_SIZE % lblock_size) != 0)
		ksft_exit_skip("Unsupported logical block size %d for IO_SIZE=%d\n",
			       lblock_size, IO_SIZE);

	sched_getaffinity(0, sizeof(set), &set);
	for (int c = 0; c < CPU_SETSIZE; c++)
		if (CPU_ISSET(c, &set))
			cpus[ncpus++] = c;

	cpus_used = nprocs / 2;
	if (cpus_used > ncpus)
		cpus_used = ncpus;
	if (cpus_used < 1)
		cpus_used = 1;

	ksft_print_msg("device: %s (%ld MB)\n", dev,
		       (long)(dev_size / (1024 * 1024)));
	ksft_print_msg("processes: %d (%d per CPU across %d CPUs)\n",
		       nprocs, nprocs / cpus_used, cpus_used);
	ksft_print_msg("iterations: %d per process\n", iters);
	ksft_print_msg("I/O size: %d KB\n", IO_SIZE / 1024);
	ksft_print_msg("logical block size: %d bytes\n", lblock_size);

	pids = calloc(nprocs, sizeof(pid_t));
	if (!pids)
		ksft_exit_fail_msg("calloc failed\n");

	for (int i = 0; i < nprocs; i++) {
		pids[i] = fork();
		if (pids[i] < 0)
			ksft_exit_fail_msg("fork failed: %s\n",
					   strerror(errno));
		if (pids[i] == 0)
			_exit(child(dev, i, nprocs, dev_size,
				    iters, cpus[i % cpus_used],
				    lblock_size));
	}

	for (int i = 0; i < nprocs; i++) {
		int st;

		waitpid(pids[i], &st, 0);
		if (WIFEXITED(st)) {
			fail += WEXITSTATUS(st);
		} else {
			ksft_print_msg("proc %d terminated abnormally\n", i);
			fail++;
		}
	}

	if (fail)
		ksft_test_result_fail("BTT lane contention: %d process(es) saw corruption\n",
				      fail);
	else
		ksft_test_result_pass("BTT lane contention: all data verified\n");

	free(pids);
	ksft_finished();
}

