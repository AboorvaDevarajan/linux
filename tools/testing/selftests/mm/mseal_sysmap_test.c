// SPDX-License-Identifier: GPL-2.0
/*
 * Test that CONFIG_MSEAL_SYSTEM_MAPPINGS really seals the mappings the
 * kernel installs on behalf of a process (vdso, vvar, uprobes, and the
 * arm compat vectors/sigpage).
 *
 * Nothing in a freshly exec'd process calls mseal(), so every VMA that
 * reports the "sl" flag in /proc/self/smaps must have been sealed by the
 * kernel. The test discovers those VMAs and checks that the operations
 * mseal() is documented to forbid are in fact rejected with -EPERM.
 *
 * Each probe runs in a forked child: if a probe unexpectedly succeeds it
 * may well have unmapped the vdso, which would take the test process with
 * it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kselftest.h"

#define MAX_MAPS	128
#define VDSO_NAME	"[vdso]"

struct mapping {
	unsigned long start;
	unsigned long end;
	char name[64];
	bool sealed;
};

static struct mapping maps[MAX_MAPS];
static int nr_maps;
static unsigned long page_size;

enum probe_op {
	PROBE_MPROTECT,
	PROBE_MREMAP,
	PROBE_MUNMAP,
	PROBE_MUNMAP_PARTIAL,
	PROBE_MMAP_FIXED,
	PROBE_NR_OPS
};

static const char * const op_name[PROBE_NR_OPS] = {
	[PROBE_MPROTECT]	= "mprotect",
	[PROBE_MREMAP]		= "mremap",
	[PROBE_MUNMAP]		= "munmap",
	[PROBE_MUNMAP_PARTIAL]	= "partial munmap",
	[PROBE_MMAP_FIXED]	= "mmap MAP_FIXED over",
};

/*
 * VmFlags tokens are all two characters wide and both separated and
 * terminated by a space, so a plain substring search cannot alias.
 */
static bool vmflags_has(const char *line, const char *flag)
{
	char pattern[8];

	snprintf(pattern, sizeof(pattern), " %s ", flag);
	return strstr(line, pattern) != NULL;
}

/*
 * Collect the named and/or sealed VMAs of this process. smaps is used
 * rather than maps because only smaps reports VmFlags.
 */
static void scan_smaps(void)
{
	char line[512];
	struct mapping cur = {};
	bool have_cur = false;
	FILE *f;

	f = fopen("/proc/self/smaps", "r");
	if (!f)
		ksft_exit_fail_perror("cannot open /proc/self/smaps");

	while (fgets(line, sizeof(line), f)) {
		unsigned long start, end;
		int n;

		n = sscanf(line, "%lx-%lx %*s %*s %*s %*s %63s",
			   &start, &end, cur.name);
		if (n >= 2) {
			if (n < 3)
				cur.name[0] = '\0';
			cur.start = start;
			cur.end = end;
			cur.sealed = false;
			have_cur = true;
			continue;
		}

		if (!have_cur || strncmp(line, "VmFlags:", 8))
			continue;

		have_cur = false;
		cur.sealed = vmflags_has(line, "sl");
		if (!cur.sealed && cur.name[0] != '[')
			continue;

		if (nr_maps == MAX_MAPS)
			ksft_exit_fail_msg("more than %d mappings to track\n",
					   MAX_MAPS);
		if (!cur.name[0])
			snprintf(cur.name, sizeof(cur.name), "anon@0x%lx",
				 cur.start);
		maps[nr_maps++] = cur;
	}

	fclose(f);
}

static const struct mapping *find_map(const char *name)
{
	int i;

	for (i = 0; i < nr_maps; i++)
		if (!strcmp(maps[i].name, name))
			return &maps[i];
	return NULL;
}

/*
 * Runs in the child. Returns 0 when the operation was rejected with
 * EPERM as it should be, and non-zero otherwise.
 */
static int do_probe(const struct mapping *map, enum probe_op op)
{
	void *addr = (void *)map->start;
	size_t len = map->end - map->start;
	long ret;

	errno = 0;
	switch (op) {
	case PROBE_MPROTECT:
		/*
		 * PROT_READ needs only VM_MAYREAD, which every system
		 * mapping carries, so an EPERM here can only come from the
		 * seal. Asking for PROT_WRITE would earn an EACCES from the
		 * read-only mappings instead.
		 */
		ret = mprotect(addr, len, PROT_READ);
		break;
	case PROBE_MREMAP:
		ret = mremap(addr, len, len, MREMAP_MAYMOVE) == MAP_FAILED
			? -1 : 0;
		break;
	case PROBE_MUNMAP:
		ret = munmap(addr, len);
		break;
	case PROBE_MUNMAP_PARTIAL:
		/* Unmapping the tail forces a split of a sealed VMA. */
		ret = munmap((char *)addr + page_size, len - page_size);
		break;
	case PROBE_MMAP_FIXED:
		ret = mmap(addr, len, PROT_READ,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			   -1, 0) == MAP_FAILED ? -1 : 0;
		break;
	default:
		return 1;
	}

	if (ret == 0) {
		ksft_print_msg("%s: %s unexpectedly succeeded\n",
			       map->name, op_name[op]);
		return 1;
	}
	if (errno != EPERM) {
		ksft_print_msg("%s: %s failed with %s, expected EPERM\n",
			       map->name, op_name[op], strerror(errno));
		return 1;
	}
	return 0;
}

static void probe(const struct mapping *map, enum probe_op op)
{
	int status;
	pid_t pid;

	/* Or the child would flush a copy of the pending TAP output. */
	fflush(stdout);

	pid = fork();
	if (pid < 0) {
		ksft_test_result_fail("%s: %s: fork: %s\n", map->name,
				      op_name[op], strerror(errno));
		return;
	}
	if (pid == 0)
		exit(do_probe(map, op));

	if (waitpid(pid, &status, 0) < 0) {
		ksft_test_result_fail("%s: %s: waitpid: %s\n", map->name,
				      op_name[op], strerror(errno));
		return;
	}

	/*
	 * A child that dies on a signal has most likely lost a mapping it
	 * still needed, which is itself a failure of the seal.
	 */
	if (WIFSIGNALED(status)) {
		ksft_test_result_fail("%s: %s: child killed by signal %d\n",
				      map->name, op_name[op],
				      WTERMSIG(status));
		return;
	}

	ksft_test_result(WIFEXITED(status) && WEXITSTATUS(status) == 0,
			 "%s rejects %s\n", map->name, op_name[op]);
}

/*
 * An architecture gets the vvar mapping sealed for free from the generic
 * vdso_install_vvar_mapping() helper, but has to pass VM_SEALED_SYSMAP
 * to its own _install_special_mapping() call for the vdso. Sealing one
 * but not the other means the architecture enabled the option without
 * finishing the job.
 */
static void test_vdso_is_sealed(void)
{
	const struct mapping *vdso = find_map(VDSO_NAME);

	if (!vdso) {
		ksft_test_result_skip("no %s mapping in this process\n",
				      VDSO_NAME);
		return;
	}

	ksft_test_result(vdso->sealed,
			 "%s is sealed while other system mappings are\n",
			 VDSO_NAME);
}

static bool probe_applies(const struct mapping *map, enum probe_op op)
{
	/* A single page VMA cannot be split. */
	if (op == PROBE_MUNMAP_PARTIAL)
		return map->end - map->start > page_size;
	return true;
}

int main(void)
{
	int i, op, nr_sealed = 0, plan = 1;

	ksft_print_header();

	page_size = sysconf(_SC_PAGESIZE);
	scan_smaps();

	for (i = 0; i < nr_maps; i++) {
		if (!maps[i].sealed)
			continue;
		nr_sealed++;
		for (op = 0; op < PROBE_NR_OPS; op++)
			if (probe_applies(&maps[i], op))
				plan++;
	}

	if (!nr_sealed) {
		ksft_print_msg("needs a 64-bit kernel built with CONFIG_MSEAL_SYSTEM_MAPPINGS=y\n");
		ksft_exit_skip("no sealed system mappings found\n");
	}

	ksft_set_plan(plan);
	ksft_print_msg("found %d sealed system mapping(s)\n", nr_sealed);

	test_vdso_is_sealed();

	for (i = 0; i < nr_maps; i++) {
		if (!maps[i].sealed)
			continue;
		for (op = 0; op < PROBE_NR_OPS; op++)
			if (probe_applies(&maps[i], op))
				probe(&maps[i], op);
	}

	ksft_finished();
}
