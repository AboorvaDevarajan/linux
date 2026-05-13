/* SPDX-License-Identifier: GPL-2.0 */
/*
 * htx_btt_repro.c -- Minimal reproducer that mimics the HTX hxestorage
 *                    workload defined by the user's default.ssd.
 *
 *   Phase 1 (rule_1, "bwrc"):  Sequential write + immediate read-compare
 *                               of the whole device, **looping forever**
 *                               (HTX skip=-99999999 = background).
 *                               4 threads, 256 KiB transfers, sections,
 *                               alternating UP/DOWN direction.
 *                               Runs concurrently with phase 3.
 *
 *   Phase 2 (rule_2, "S"):     Sleep (settle) -- gives phase 1 a head
 *                               start before readers begin.
 *
 *   Phase 3 (rule_3, "RC"):    Read-compare only.  ~nproc threads, random
 *                               seek, transfer sizes 4 KiB..256 KiB.
 *                               Runs concurrently with phase 1.
 *
 * The key insight: in HTX, stanza 1 has skip=-99999999, so it runs in
 * the background forever.  Stanza 3 (RC) starts after the 120s sleep
 * and reads while stanza 1 is still writing.  This concurrent
 * read+write is what triggers the BTT map/freelist race.
 *
 * Design notes:
 *
 *   - Phase 1 and phase 3 run CONCURRENTLY on the same device.
 *     Phase 1 writers keep looping over their sections with
 *     incrementing versions.  Phase 3 readers do random RC reads.
 *
 *   - Version tracking uses atomic load/store so readers and writers
 *     can access the version array concurrently without locks.
 *     The writer atomically stores the new version AFTER the pwrite
 *     completes.  The reader atomically loads the version BEFORE
 *     the pread.  Since the write may be in-flight, the reader
 *     accepts version == expected OR version == expected-1.
 *
 *   - Hard errors (DIFF_LBA, ALL_ZEROS, BAD_MAGIC) are reported
 *     regardless of version -- these indicate BTT map corruption,
 *     not a benign race.
 *
 *   - Each 4 KiB block carries a 64-byte header { magic, lba, version,
 *     tid }, followed by a deterministic body derived from (lba, version).
 *
 * Build:  gcc -O2 -Wall -pthread htx_btt_repro.c -o htx_btt_repro
 *
 * Usage:  htx_btt_repro [options] /dev/pmem0.4s
 *
 *   -t N      Phase-3 threads      (default: nproc)
 *   -i OPS    Phase-3 ops/thread   (default: 50000)
 *   -s SEC    Phase-2 sleep        (default: 5 seconds)
 *   -E LIMIT  Stop after LIMIT errors (default: 100; 0 = unlimited)
 *   -L DIR    Log/buffer directory (default: /tmp/htx_btt_repro)
 *   -v        Verbose progress
 *   -h        Help
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/fs.h>
#include <stdint.h>
#include <getopt.h>
#include <ctype.h>
#include <inttypes.h>
#include <time.h>

#define BLK_SIZE         4096u
#define HDR_SIZE         64u
#define HDR_MAGIC        0x484258545252504fULL  /* "HBXTRRPO" */
#define MAX_XFER_BLKS    64u                    /* 256 KiB */
#define PHASE1_XFER      (256u * 1024u)         /* 256 KiB sequential */
#define PHASE1_THREADS   4
#define DEFAULT_PHASE2_SLEEP   5
#define DEFAULT_OPS_PER_THREAD 50000
#define DEFAULT_MAX_ERRORS     100

struct blk_hdr {
	uint64_t magic;
	uint64_t lba;
	uint64_t version;
	uint32_t tid;
	uint32_t pad0;
	uint8_t  reserved[32];
};
_Static_assert(sizeof(struct blk_hdr) == HDR_SIZE, "blk_hdr must equal HDR_SIZE");

enum mis_type {
	MIS_NONE = 0,
	MIS_ALL_ZEROS,
	MIS_BAD_MAGIC,
	MIS_DIFF_LBA,
	MIS_DIFF_TID,
	MIS_OLD_VERSION,
	MIS_NEW_VERSION,
	MIS_BODY_MISMATCH,
};

static const char *mis_name(enum mis_type t)
{
	switch (t) {
	case MIS_NONE:          return "NONE";
	case MIS_ALL_ZEROS:     return "ALL_ZEROS";
	case MIS_BAD_MAGIC:     return "BAD_MAGIC";
	case MIS_DIFF_LBA:      return "DIFF_LBA";
	case MIS_DIFF_TID:      return "DIFF_TID";
	case MIS_OLD_VERSION:   return "OLD_VERSION";
	case MIS_NEW_VERSION:   return "NEW_VERSION";
	case MIS_BODY_MISMATCH: return "BODY_MISMATCH";
	}
	return "UNKNOWN";
}

static volatile int stop_writers;

struct globals {
	const char *dev;
	int         fd;
	uint64_t    total_blocks;
	int         nthreads;       /* phase-3 threads */
	int         phase2_sleep;
	uint64_t    ops_per_thread;
	uint64_t    max_errors;
	const char *log_dir;
	int         verbose;
	uint64_t    g_err_count;    /* only inc'd under g_err_lock */
	uint64_t    p1_err_count;   /* phase-1 bwrc miscompares */
	uint64_t    g_newver_count; /* concurrent RC observed newer version */
};

static struct globals G;
static pthread_mutex_t g_err_lock = PTHREAD_MUTEX_INITIALIZER;

struct thread_ctx {
	int          tid;
	int          fd;
	uint64_t     section_start;
	uint64_t     section_nblks;
	uint64_t    *versions;      /* shared with writer; use atomics */
	uint64_t     ops_done;
	uint64_t     errors;
	unsigned int rng_state;
	uint8_t     *wbuf;
	uint8_t     *rbuf;
	uint8_t     *expected_blk;
};

/* ---------- block fill / verify ---------- */

static void fill_block(uint8_t *blk, uint64_t lba, uint64_t version,
		       uint32_t tid)
{
	struct blk_hdr *h;
	uint64_t *body;
	size_t nwords;
	size_t i;

	memset(blk, 0, BLK_SIZE);

	h = (struct blk_hdr *)blk;
	h->magic   = HDR_MAGIC;
	h->lba     = lba;
	h->version = version;
	h->tid     = tid;
	h->pad0    = 0;

	body = (uint64_t *)(blk + HDR_SIZE);
	nwords = (BLK_SIZE - HDR_SIZE) / sizeof(uint64_t);
	for (i = 0; i < nwords; i++)
		body[i] = (lba ^ version) + i;
}

/*
 * Classify a read block for a concurrent reader.
 * The writer may be in-flight, so we accept either expected_ver or
 * expected_ver-1 for version checks.  Hard errors (wrong LBA, zeros,
 * bad magic) are never tolerated.
 */
static enum mis_type classify_block_concurrent(const uint8_t *rbuf,
					       uint64_t lba,
					       uint64_t expected_ver,
					       uint32_t expected_tid)
{
	const struct blk_hdr *h;
	size_t i;
	int allzero = 1;

	for (i = 0; i < BLK_SIZE; i++) {
		if (rbuf[i]) {
			allzero = 0;
			break;
		}
	}
	if (allzero)
		return MIS_ALL_ZEROS;

	h = (const struct blk_hdr *)rbuf;
	if (h->magic != HDR_MAGIC)
		return MIS_BAD_MAGIC;
	if (h->lba != lba)
		return MIS_DIFF_LBA;
	if (h->tid != expected_tid)
		return MIS_DIFF_TID;

	/*
	 * Allow version to be expected_ver or expected_ver-1 (in-flight
	 * write from the concurrent phase-1 writer).  Anything older
	 * than expected_ver-1 is a real lost-write error.
	 */
	if (expected_ver >= 2 && h->version < expected_ver - 1)
		return MIS_OLD_VERSION;

	/*
	 * Version newer than expected is possible in concurrent mode:
	 * reader snapshots version N, then writer completes N+1 before
	 * the reader reaches this block.
	 */
	if (h->version > expected_ver)
		return MIS_NEW_VERSION;

	/*
	 * Header looks valid; skip body check since the writer may
	 * have just bumped the version and body is from the new or
	 * old version -- both are acceptable during concurrent access.
	 */
	return MIS_NONE;
}

/* ---------- miscompare reporting ---------- */

static void hex16(FILE *fp, const uint8_t *buf, size_t off)
{
	size_t i;
	int c;

	fprintf(fp, "%08zx: ", off);
	for (i = 0; i < 16; i++)
		fprintf(fp, "%02x%s", buf[off + i], (i == 7) ? "  " : " ");
	fprintf(fp, " |");
	for (i = 0; i < 16; i++) {
		c = buf[off + i];
		fputc(isprint(c) ? c : '.', fp);
	}
	fprintf(fp, "|\n");
}

static void hex_dump(FILE *fp, const char *tag, const uint8_t *buf,
		     size_t bytes)
{
	size_t off;

	fprintf(fp, "%s (first %zu bytes):\n", tag, bytes);
	for (off = 0; off + 16 <= bytes; off += 16)
		hex16(fp, buf, off);
}

static void report_miscompare(struct thread_ctx *ctx, uint64_t lba,
			      const char *op, enum mis_type type,
			      const uint8_t *expected,
			      const uint8_t *actual,
			      uint64_t expected_ver,
			      uint64_t io_start_lba, uint32_t io_nblks)
{
	const struct blk_hdr *ah = (const struct blk_hdr *)actual;
	char wpath[512], rpath[512], tpath[512];
	uint64_t my_idx;
	FILE *fp;
	int fd;

	pthread_mutex_lock(&g_err_lock);
	my_idx = ++G.g_err_count;
	pthread_mutex_unlock(&g_err_lock);

	snprintf(wpath, sizeof(wpath), "%s/htxbtt_mis_%" PRIu64 ".wbuf",
		 G.log_dir, my_idx);
	snprintf(rpath, sizeof(rpath), "%s/htxbtt_mis_%" PRIu64 ".rbuf",
		 G.log_dir, my_idx);
	snprintf(tpath, sizeof(tpath), "%s/htxbtt_mis_%" PRIu64 ".txt",
		 G.log_dir, my_idx);

	fd = open(wpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, expected, BLK_SIZE) != (ssize_t)BLK_SIZE)
			fprintf(stderr, "warn: short write to %s\n", wpath);
		close(fd);
	}
	fd = open(rpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, actual, BLK_SIZE) != (ssize_t)BLK_SIZE)
			fprintf(stderr, "warn: short write to %s\n", rpath);
		close(fd);
	}

	fp = fopen(tpath, "w");
	if (!fp) {
		fprintf(stderr, "warn: cannot open %s: %s\n", tpath,
			strerror(errno));
		return;
	}

	fprintf(fp, "=== htx_btt_repro miscompare report #%" PRIu64 " ===\n",
		my_idx);
	fprintf(fp, "Type:                %s\n", mis_name(type));
	fprintf(fp, "LBA:                 0x%" PRIx64 "\n", lba);
	fprintf(fp, "Operation:           %s\n", op);
	fprintf(fp, "Thread:              %d\n", ctx->tid);
	fprintf(fp, "Expected version:    0x%" PRIx64 "\n", expected_ver);
	fprintf(fp, "I/O start LBA:       0x%" PRIx64 "\n", io_start_lba);
	fprintf(fp, "I/O blocks:          %u (%u bytes)\n",
		io_nblks, io_nblks * BLK_SIZE);
	fprintf(fp, "Block offset in I/O: %" PRIu64 " (%" PRIu64 " bytes)\n",
		lba - io_start_lba, (lba - io_start_lba) * BLK_SIZE);
	fprintf(fp, "Section base LBA:    0x%" PRIx64 "\n",
		ctx->section_start);
	fprintf(fp, "Section blocks:      %" PRIu64 "\n", ctx->section_nblks);

	fprintf(fp, "\nActual header:\n");
	if (type == MIS_ALL_ZEROS) {
		fprintf(fp, "  (block is entirely zero)\n");
	} else {
		fprintf(fp, "  magic=0x%016" PRIx64
			" (expected 0x%016" PRIx64 ")\n",
			ah->magic, (uint64_t)HDR_MAGIC);
		fprintf(fp, "  lba=0x%" PRIx64 "  (expected 0x%" PRIx64 ")\n",
			ah->lba, lba);
		fprintf(fp, "  ver=0x%" PRIx64 "  (expected 0x%" PRIx64 ")\n",
			ah->version, expected_ver);
		fprintf(fp, "  tid=%u           (expected %u)\n",
			ah->tid, (uint32_t)ctx->tid);
	}

	fprintf(fp, "\n");
	hex_dump(fp, "Expected (at snap version)", expected, 256);
	fprintf(fp, "\n");
	hex_dump(fp, "Actual",   actual,   256);
	fclose(fp);

	fprintf(stderr,
		"MISCOMPARE #%" PRIu64 ": %-13s lba=0x%-10" PRIx64
		" op=%-5s tid=%2d expver=0x%-8" PRIx64
		" io=[0x%" PRIx64 "+%u] -> %s\n",
		my_idx, mis_name(type), lba, op, ctx->tid,
		expected_ver, io_start_lba, io_nblks, tpath);
}

/* ---------- I/O helpers ---------- */

static int do_pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
	ssize_t n;

	while (len) {
		n = pwrite(fd, buf, len, off);
		if (n <= 0)
			return -1;
		buf = (const char *)buf + n;
		len -= n;
		off += n;
	}
	return 0;
}

static int do_pread_full(int fd, void *buf, size_t len, off_t off)
{
	ssize_t n;

	while (len) {
		n = pread(fd, buf, len, off);
		if (n <= 0)
			return -1;
		buf = (char *)buf + n;
		len -= n;
		off += n;
	}
	return 0;
}

/* ---------- phase-3 read-verify (concurrent with phase 1) ---------- */

static int do_read_verify_concurrent(struct thread_ctx *ctx,
				     uint64_t start_lba, uint32_t nblks,
				     const char *op_name)
{
	uint32_t i;
	int errors = 0;

	if (do_pread_full(ctx->fd, ctx->rbuf, nblks * BLK_SIZE,
			  (off_t)start_lba * BLK_SIZE) < 0) {
		fprintf(stderr,
			"[T%d] pread failed lba=0x%" PRIx64 " n=%u: %s\n",
			ctx->tid, start_lba, nblks, strerror(errno));
		return -1;
	}

	for (i = 0; i < nblks; i++) {
		uint64_t lba = start_lba + i;
		uint64_t idx = lba - ctx->section_start;
		uint64_t snap_ver = __atomic_load_n(&ctx->versions[idx],
						    __ATOMIC_ACQUIRE);
		const uint8_t *rblk = ctx->rbuf + i * BLK_SIZE;
		enum mis_type t;

		if (snap_ver == 0)
			continue;

		t = classify_block_concurrent(rblk, lba, snap_ver,
					      (uint32_t)ctx->tid);
		if (t == MIS_NONE)
			continue;

		/*
		 * NEW_VERSION is expected under concurrent writer+reader
		 * workload and is not a data-integrity failure. Track it
		 * separately as an informational event.
		 */
		if (t == MIS_NEW_VERSION) {
			pthread_mutex_lock(&g_err_lock);
			G.g_newver_count++;
			pthread_mutex_unlock(&g_err_lock);
			continue;
		}

		fill_block(ctx->expected_blk, lba, snap_ver,
			   (uint32_t)ctx->tid);
		report_miscompare(ctx, lba, op_name, t,
				  ctx->expected_blk, rblk,
				  snap_ver, start_lba, nblks);
		errors++;
		ctx->errors++;
		if (G.max_errors && G.g_err_count >= G.max_errors)
			break;
	}

	return errors;
}

/* ---------- phase 1: looping sequential bwrc (background) ---------- */

struct phase1_arg {
	int       tid;
	int       fd;
	uint64_t  base_lba;
	uint64_t  nblks;
	int       direction;     /* 0 = UP, 1 = DOWN */
	uint64_t *versions;      /* shared version array; use atomics */
	uint8_t  *wbuf;          /* aligned PHASE1_XFER */
	uint8_t  *rbuf;          /* aligned PHASE1_XFER */
	uint64_t  errors;
	uint64_t  passes;        /* full-device passes completed */
};

static void *phase1_thread(void *arg)
{
	struct phase1_arg *a = arg;
	const uint32_t blks_per_xfer = PHASE1_XFER / BLK_SIZE;
	uint64_t version = 1;

	if (G.verbose)
		printf("[Phase1 T%d] base=0x%" PRIx64 " nblks=%" PRIu64
		       " dir=%s (looping)\n",
		       a->tid, a->base_lba, a->nblks,
		       a->direction ? "DOWN" : "UP");

	while (!stop_writers) {
		uint64_t done = 0;
		uint64_t total = a->nblks;

		while (done < total && !stop_writers) {
			uint32_t this_blks = blks_per_xfer;
			uint64_t lba;
			uint32_t i;

			if (G.max_errors && G.g_err_count >= G.max_errors)
				goto out;

			if (this_blks > total - done)
				this_blks = (uint32_t)(total - done);

			if (a->direction == 0)
				lba = a->base_lba + done;
			else
				lba = a->base_lba + (total - done - this_blks);

			for (i = 0; i < this_blks; i++)
				fill_block(a->wbuf + i * BLK_SIZE,
					   lba + i, version,
					   (uint32_t)a->tid);

			if (do_pwrite_full(a->fd, a->wbuf,
					   this_blks * BLK_SIZE,
					   (off_t)lba * BLK_SIZE) < 0) {
				fprintf(stderr,
					"[Phase1 T%d] pwrite failed "
					"lba=0x%" PRIx64 " n=%u: %s\n",
					a->tid, lba, this_blks,
					strerror(errno));
				return (void *)1;
			}

			/*
			 * Update version array AFTER pwrite completes.
			 * Use release semantics so readers see the version
			 * bump only after the data is on disk.
			 */
			for (i = 0; i < this_blks; i++) {
				uint64_t idx = (lba + i) - a->base_lba;
				__atomic_store_n(&a->versions[idx], version,
						__ATOMIC_RELEASE);
			}

			/* bwrc: immediate read-compare */
			if (do_pread_full(a->fd, a->rbuf,
					  this_blks * BLK_SIZE,
					  (off_t)lba * BLK_SIZE) < 0) {
				fprintf(stderr,
					"[Phase1 T%d] pread failed "
					"lba=0x%" PRIx64 " n=%u: %s\n",
					a->tid, lba, this_blks,
					strerror(errno));
				return (void *)1;
			}

			for (i = 0; i < this_blks; i++) {
				const uint8_t *rblk =
					a->rbuf + i * BLK_SIZE;
				const uint8_t *wblk =
					a->wbuf + i * BLK_SIZE;

				if (memcmp(rblk, wblk, BLK_SIZE) != 0) {
					const struct blk_hdr *rh =
						(const struct blk_hdr *)rblk;
					const struct blk_hdr *wh =
						(const struct blk_hdr *)wblk;

					pthread_mutex_lock(&g_err_lock);
					uint64_t idx = ++G.g_err_count;
					G.p1_err_count++;
					pthread_mutex_unlock(&g_err_lock);

					a->errors++;

					fprintf(stderr,
						"PHASE1_MISMATCH #%" PRIu64
						": T%d lba=0x%" PRIx64
						" ver=0x%" PRIx64
						" wrote_lba=0x%" PRIx64
						" read_lba=0x%" PRIx64
						" wrote_tid=%u read_tid=%u"
						" read_ver=0x%" PRIx64
						" read_magic=0x%" PRIx64
						"\n",
						idx, a->tid, lba + i,
						version,
						wh->lba, rh->lba,
						wh->tid, rh->tid,
						rh->version, rh->magic);

					if (G.max_errors &&
					    G.g_err_count >= G.max_errors)
						goto out;
				}
			}

			done += this_blks;
		}

		a->passes++;
		version++;

		if (G.verbose)
			printf("[Phase1 T%d] pass %" PRIu64
			       " done (ver=%" PRIu64 "), errs=%" PRIu64
			       "\n", a->tid, a->passes, version - 1,
			       a->errors);
	}

out:
	if (G.verbose)
		printf("[Phase1 T%d] stopping after %" PRIu64
		       " passes, %" PRIu64 " errors\n",
		       a->tid, a->passes, a->errors);
	return NULL;
}

/* ---------- phase 3: read-only random verify (RC, concurrent) ------- */

static void *phase3_thread(void *arg)
{
	struct thread_ctx *ctx = arg;
	uint64_t op;

	if (G.verbose)
		printf("[Phase3 T%d] section base=0x%" PRIx64
		       " nblks=%" PRIu64 " ops=%" PRIu64
		       " (concurrent with writers)\n",
		       ctx->tid, ctx->section_start, ctx->section_nblks,
		       G.ops_per_thread);

	for (op = 0; op < G.ops_per_thread; op++) {
		uint32_t nblks;
		uint64_t off_in_section;
		uint64_t start_lba;

		if (G.max_errors && G.g_err_count >= G.max_errors)
			break;

		nblks = 1u + (rand_r(&ctx->rng_state) % MAX_XFER_BLKS);
		if (nblks > ctx->section_nblks)
			nblks = (uint32_t)ctx->section_nblks;

		off_in_section = (uint64_t)rand_r(&ctx->rng_state) << 16;
		off_in_section ^= (uint64_t)rand_r(&ctx->rng_state);
		off_in_section %= (ctx->section_nblks - nblks + 1);
		start_lba = ctx->section_start + off_in_section;

		do_read_verify_concurrent(ctx, start_lba, nblks, "rc");

		ctx->ops_done++;
		if (G.verbose && (op + 1) % 1000 == 0)
			printf("[Phase3 T%d] %" PRIu64 "/%" PRIu64
			       " ops, errs=%" PRIu64 "\n",
			       ctx->tid, op + 1, G.ops_per_thread,
			       ctx->errors);
	}

	return NULL;
}

/* ---------- main ---------- */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [-t threads] [-i ops] [-s sleep] [-E maxerrs]\n"
		"           [-L logdir] [-v] /dev/pmemXs\n"
		"\n"
		"  -t N      Phase-3 threads      (default: nproc)\n"
		"  -i OPS    Phase-3 ops/thread   (default: %d)\n"
		"  -s SEC    Phase-2 sleep        (default: %d)\n"
		"  -E LIMIT  Stop after N errors  (default: %d, 0=unlimited)\n"
		"  -L DIR    Log/buffer dir       (default: /tmp/htx_btt_repro)\n"
		"  -v        Verbose progress\n"
		"  -h        This help\n",
		argv0, DEFAULT_OPS_PER_THREAD, DEFAULT_PHASE2_SLEEP,
		DEFAULT_MAX_ERRORS);
}

static uint64_t *alloc_versions(uint64_t n)
{
	uint64_t *v = calloc((size_t)n, sizeof(uint64_t));
	if (!v) {
		fprintf(stderr,
			"calloc(versions, %" PRIu64 ") failed: %s\n",
			n, strerror(errno));
		exit(1);
	}
	return v;
}

int main(int argc, char **argv)
{
	int    opt;
	int    rc = 0;
	uint64_t dev_size_bytes;
	uint64_t section_blks;
	int    i;
	struct phase1_arg p1[PHASE1_THREADS];
	pthread_t p1_th[PHASE1_THREADS];
	int    p1_launched = 0;
	pthread_t *p3_th = NULL;
	struct thread_ctx *p3ctx = NULL;
	int    p3_nthreads;
	uint64_t *versions[PHASE1_THREADS];
	long   nproc;

	memset(&G, 0, sizeof(G));
	G.phase2_sleep   = DEFAULT_PHASE2_SLEEP;
	G.ops_per_thread = DEFAULT_OPS_PER_THREAD;
	G.max_errors     = DEFAULT_MAX_ERRORS;
	G.log_dir        = "/tmp/htx_btt_repro";
	G.verbose        = 0;
	nproc = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc < 1)
		nproc = 4;
	G.nthreads = (int)nproc;

	while ((opt = getopt(argc, argv, "t:i:s:E:L:vh")) != -1) {
		switch (opt) {
		case 't': G.nthreads = atoi(optarg); break;
		case 'i': G.ops_per_thread = strtoull(optarg, NULL, 0); break;
		case 's': G.phase2_sleep = atoi(optarg); break;
		case 'E': G.max_errors = strtoull(optarg, NULL, 0); break;
		case 'L': G.log_dir = optarg; break;
		case 'v': G.verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}
	G.dev = argv[optind];

	if (G.nthreads < 1) {
		fprintf(stderr, "thread count must be >= 1\n");
		return 2;
	}

	if (mkdir(G.log_dir, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "mkdir(%s) failed: %s\n", G.log_dir,
			strerror(errno));
		return 1;
	}

	G.fd = open(G.dev, O_RDWR | O_DIRECT);
	if (G.fd < 0) {
		fprintf(stderr, "open(%s) failed: %s\n", G.dev,
			strerror(errno));
		return 1;
	}
	if (ioctl(G.fd, BLKGETSIZE64, &dev_size_bytes) < 0) {
		fprintf(stderr, "BLKGETSIZE64(%s) failed: %s\n",
			G.dev, strerror(errno));
		close(G.fd);
		return 1;
	}
	G.total_blocks = dev_size_bytes / BLK_SIZE;

	section_blks = G.total_blocks / (uint64_t)PHASE1_THREADS;
	if (section_blks == 0) {
		fprintf(stderr, "device too small for %d threads\n",
			PHASE1_THREADS);
		close(G.fd);
		return 1;
	}

	p3_nthreads = G.nthreads;

	printf("=== htx_btt_repro configuration ===\n");
	printf("Device:           %s\n", G.dev);
	printf("Size:             %" PRIu64 " bytes (%" PRIu64
	       " blocks of %u)\n",
	       dev_size_bytes, G.total_blocks, BLK_SIZE);
	printf("Phase-1 threads:  %d (bwrc, looping, concurrent)\n",
	       PHASE1_THREADS);
	printf("Phase-3 threads:  %d (RC, random read-verify, concurrent)\n",
	       p3_nthreads);
	printf("Phase-3 ops/thr:  %" PRIu64 "\n", G.ops_per_thread);
	printf("Phase-2 sleep:    %d sec\n", G.phase2_sleep);
	printf("Max errors:       %" PRIu64 " (0 = unlimited)\n",
	       G.max_errors);
	printf("Log dir:          %s\n", G.log_dir);
	printf("====================================\n\n");

	/* Allocate shared version arrays (one per phase-1 section) */
	for (i = 0; i < PHASE1_THREADS; i++)
		versions[i] = alloc_versions(
			(i == PHASE1_THREADS - 1)
			? (G.total_blocks - (uint64_t)i * section_blks)
			: section_blks);

	/*
	 * ---- Phase 1: launch looping bwrc writers (background) ----
	 * These run until stop_writers is set.
	 */
	printf("=== Phase 1: launching %d background bwrc writers ===\n",
	       PHASE1_THREADS);

	memset(p1, 0, sizeof(p1));
	stop_writers = 0;

	for (i = 0; i < PHASE1_THREADS; i++) {
		p1[i].tid       = i;
		p1[i].fd        = G.fd;
		p1[i].base_lba  = (uint64_t)i * section_blks;
		p1[i].nblks     = (i == PHASE1_THREADS - 1)
			? (G.total_blocks - p1[i].base_lba) : section_blks;
		p1[i].direction = i & 1;
		p1[i].versions  = versions[i];

		if (posix_memalign((void **)&p1[i].wbuf, BLK_SIZE,
				   PHASE1_XFER) ||
		    posix_memalign((void **)&p1[i].rbuf, BLK_SIZE,
				   PHASE1_XFER)) {
			perror("posix_memalign(p1)");
			rc = 1;
			goto out;
		}
		if (pthread_create(&p1_th[i], NULL,
				   phase1_thread, &p1[i]) != 0) {
			perror("pthread_create(p1)");
			rc = 1;
			goto out;
		}
		p1_launched = i + 1;
	}

	printf("Phase 1 writers launched (looping in background).\n\n");

	/* ---- Phase 2: settle (writers keep running) ---- */

	printf("=== Phase 2: settle (%d seconds, writers active) ===\n",
	       G.phase2_sleep);
	fflush(stdout);
	sleep(G.phase2_sleep);
	printf("Phase 2 done.\n\n");

	/* ---- Phase 3: concurrent random read-verify (RC) ---- */

	printf("=== Phase 3: launching %d concurrent RC readers ===\n",
	       p3_nthreads);
	printf("%d threads, %" PRIu64 " ops/thread, xfer 1..%u "
	       "blocks (%u..%u KiB)\n\n",
	       p3_nthreads, G.ops_per_thread, MAX_XFER_BLKS,
	       BLK_SIZE / 1024, MAX_XFER_BLKS * BLK_SIZE / 1024);

	p3ctx = calloc((size_t)p3_nthreads, sizeof(*p3ctx));
	p3_th = calloc((size_t)p3_nthreads, sizeof(*p3_th));
	if (!p3ctx || !p3_th) {
		perror("calloc(p3)");
		rc = 1;
		goto out;
	}

	for (i = 0; i < p3_nthreads; i++) {
		int owner = i % PHASE1_THREADS;
		uint64_t sec_start = (uint64_t)owner * section_blks;
		uint64_t sec_nblks = (owner == PHASE1_THREADS - 1)
			? (G.total_blocks - sec_start) : section_blks;

		p3ctx[i].tid           = owner;
		p3ctx[i].fd            = G.fd;
		p3ctx[i].section_start = sec_start;
		p3ctx[i].section_nblks = sec_nblks;
		p3ctx[i].versions      = versions[owner];
		p3ctx[i].rng_state     =
			(unsigned int)(0xDEAD ^ i ^ time(NULL));
		p3ctx[i].ops_done      = 0;
		p3ctx[i].errors        = 0;

		if (posix_memalign((void **)&p3ctx[i].rbuf, BLK_SIZE,
				   MAX_XFER_BLKS * BLK_SIZE) ||
		    posix_memalign((void **)&p3ctx[i].expected_blk,
				   BLK_SIZE, BLK_SIZE)) {
			fprintf(stderr, "posix_memalign p3 %d\n", i);
			rc = 1;
			goto out;
		}
	}

	for (i = 0; i < p3_nthreads; i++) {
		if (pthread_create(&p3_th[i], NULL,
				   phase3_thread, &p3ctx[i]) != 0) {
			perror("pthread_create(p3)");
			rc = 1;
			goto out;
		}
	}

	/* Wait for all phase-3 readers to finish */
	for (i = 0; i < p3_nthreads; i++) {
		void *r;
		pthread_join(p3_th[i], &r);
		(void)r;
	}

	printf("Phase 3 readers done.\n");

	/* ---- Stop phase-1 writers ---- */

	printf("Stopping phase-1 writers...\n");
	__atomic_store_n(&stop_writers, 1, __ATOMIC_RELEASE);

out:
	/* Join phase-1 writers if they were launched */
	for (i = 0; i < p1_launched; i++) {
		void *r;
		__atomic_store_n(&stop_writers, 1, __ATOMIC_RELEASE);
		pthread_join(p1_th[i], &r);
		if (r != NULL) {
			fprintf(stderr,
				"[Phase1] thread %d reported I/O error\n", i);
			rc = 1;
		}
		printf("[Phase1 T%d] passes=%" PRIu64 " errors=%" PRIu64 "\n",
		       i, p1[i].passes, p1[i].errors);
		free(p1[i].wbuf);
		free(p1[i].rbuf);
	}

	printf("\n=== Summary ===\n");
	printf("Phase-1 bwrc miscompares:  %" PRIu64 "\n", G.p1_err_count);
	printf("Phase-3 NEW_VERSION seen:  %" PRIu64 " (informational)\n",
	       G.g_newver_count);
	printf("Total miscompares:         %" PRIu64 "\n", G.g_err_count);
	printf("Reports under:             %s/htxbtt_mis_*\n", G.log_dir);
	printf("===============\n");

	if (G.g_err_count) {
		fprintf(stderr,
			"\nRESULT: FAIL (%" PRIu64 " miscompares).\n"
			"Reports saved under %s\n",
			G.g_err_count, G.log_dir);
		rc = 1;
	} else {
		printf("\nRESULT: PASS\n");
	}

	/* cleanup */
	if (p3ctx) {
		for (i = 0; i < p3_nthreads; i++) {
			free(p3ctx[i].rbuf);
			free(p3ctx[i].expected_blk);
		}
		free(p3ctx);
	}
	free(p3_th);
	for (i = 0; i < PHASE1_THREADS; i++)
		free(versions[i]);
	close(G.fd);
	return rc;
}
