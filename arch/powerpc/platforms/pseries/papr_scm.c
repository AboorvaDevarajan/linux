// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)	"papr-scm: " fmt

#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/ndctl.h>
#include <linux/sched.h>
#include <linux/libnvdimm.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/seq_buf.h>
#include <linux/nd.h>

#include <asm/plpar_wrappers.h>
#include <uapi/linux/papr_pdsm.h>
#include <linux/papr_scm.h>
#include <asm/mce.h>
#include <linux/unaligned.h>
#include <linux/perf_event.h>

#define BIND_ANY_ADDR (~0ul)

#define PAPR_SCM_DIMM_CMD_MASK \
	((1ul << ND_CMD_GET_CONFIG_SIZE) | \
	 (1ul << ND_CMD_GET_CONFIG_DATA) | \
	 (1ul << ND_CMD_SET_CONFIG_DATA) | \
	 (1ul << ND_CMD_CALL))

// Debug macros for tracing
#ifndef DBG_ENTRY
#define DEBUG_TRACE_ENTRY_EXIT 1
#if DEBUG_TRACE_ENTRY_EXIT
#define DBG_ENTRY(fmt, ...) pr_debug("%s: ENTRY: " fmt "\n", __func__, ##__VA_ARGS__)
#define DBG_EXIT(fmt, ...) pr_debug("%s: EXIT: " fmt "\n", __func__, ##__VA_ARGS__)
#define DBG_MID(fmt, ...) pr_debug("%s: " fmt "\n", __func__, ##__VA_ARGS__)
#else
#define DBG_ENTRY(fmt, ...)
#define DBG_EXIT(fmt, ...)
#define DBG_MID(fmt, ...)
#endif
#endif

/* Struct holding a single performance metric */
struct papr_scm_perf_stat {
	u8 stat_id[8];
	__be64 stat_val;
} __packed;

/* Struct exchanged between kernel and PHYP for fetching drc perf stats */
struct papr_scm_perf_stats {
	u8 eye_catcher[8];
	/* Should be PAPR_SCM_PERF_STATS_VERSION */
	__be32 stats_version;
	/* Number of stats following */
	__be32 num_statistics;
	/* zero or more performance matrics */
	struct papr_scm_perf_stat scm_statistic[];
} __packed;

/* private struct associated with each region */
struct papr_scm_priv {
	struct platform_device *pdev;
	struct device_node *dn;
	uint32_t drc_index;
	uint64_t blocks;
	uint64_t block_size;
	int metadata_size;
	bool is_volatile;
	bool hcall_flush_required;

	uint64_t bound_addr;

	struct nvdimm_bus_descriptor bus_desc;
	struct nvdimm_bus *bus;
	struct nvdimm *nvdimm;
	struct resource res;
	struct nd_region *region;
	struct nd_interleave_set nd_set;
	struct list_head region_list;

	/* Protect dimm health data from concurrent read/writes */
	struct mutex health_mutex;

	/* Last time the health information of the dimm was updated */
	unsigned long lasthealth_jiffies;

	/* Health information for the dimm */
	u64 health_bitmap;

	/* Holds the last known dirty shutdown counter value */
	u64 dirty_shutdown_counter;

	/* length of the stat buffer as expected by phyp */
	size_t stat_buffer_len;

	/* The bits which needs to be overridden */
	u64 health_bitmap_inject_mask;
};

static int papr_scm_pmem_flush(struct nd_region *nd_region,
			       struct bio *bio __maybe_unused)
{
	DBG_ENTRY("");
	struct papr_scm_priv *p = nd_region_provider_data(nd_region);
	unsigned long ret_buf[PLPAR_HCALL_BUFSIZE], token = 0;
	long rc;

	dev_dbg(&p->pdev->dev, "flush drc 0x%x", p->drc_index);
	DBG_MID("About to start flush loop for drc_index=0x%x", (unsigned int)p->drc_index);
	do {
		rc = plpar_hcall(H_SCM_FLUSH, ret_buf, p->drc_index, token);
		DBG_MID("plpar_hcall returned rc=%ld, token=%lx", rc, token);
		token = ret_buf[0];

		/* Check if we are stalled for some time */
		if (H_IS_LONG_BUSY(rc)) {
			DBG_MID("H_IS_LONG_BUSY detected, sleeping for %d ms", get_longbusy_msecs(rc));
			msleep(get_longbusy_msecs(rc));
			rc = H_BUSY;
		} else if (rc == H_BUSY) {
			DBG_MID("H_BUSY detected, calling cond_resched()");
			cond_resched();
		}
	} while (rc == H_BUSY);

	if (rc) {
		dev_err(&p->pdev->dev, "flush error: %ld", rc);
		DBG_MID("Flush error: %ld", rc);
		rc = -EIO;
	} else {
		dev_dbg(&p->pdev->dev, "flush drc 0x%x complete", p->drc_index);
		DBG_MID("Flush complete for drc_index=0x%x", (unsigned int)p->drc_index);
	}

	DBG_EXIT("rc=%ld", rc);
	return rc;
}

static LIST_HEAD(papr_nd_regions);
static DEFINE_MUTEX(papr_ndr_lock);

static int drc_pmem_bind(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	uint64_t saved = 0;
	uint64_t token;
	int64_t rc;

	token = 0;
	DBG_MID("Starting bind loop for drc_index=0x%x, blocks=%llu", (unsigned int)p->drc_index, (unsigned long long)p->blocks);
	do {
		rc = plpar_hcall(H_SCM_BIND_MEM, ret, p->drc_index, 0,
				p->blocks, BIND_ANY_ADDR, token);
		DBG_MID("plpar_hcall returned rc=%lld, token=%llx, ret[0]=%llx, ret[1]=%llx", rc, (unsigned long long)token, (unsigned long long)ret[0], (unsigned long long)ret[1]);
		token = ret[0];
		if (!saved)
			saved = ret[1];
		cond_resched();
	} while (rc == H_BUSY);

	if (rc)
		DBG_MID("Bind failed with rc=%lld", rc);
	else
		DBG_MID("Bind succeeded, saved=0x%llx", (unsigned long long)saved);

	p->bound_addr = saved;
	dev_dbg(&p->pdev->dev, "bound drc 0x%x to 0x%llx\n", p->drc_index, (unsigned long long)saved);
	DBG_EXIT("rc=%lld, bound_addr=0x%llx", rc, (unsigned long long)saved);
	return rc;
}

static void drc_pmem_unbind(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	uint64_t token = 0;
	int64_t rc;

	dev_dbg(&p->pdev->dev, "unbind drc 0x%x\n", p->drc_index);

	/* NB: unbind has the same retry requirements as drc_pmem_bind() */
	do {

		/* Unbind of all SCM resources associated with drcIndex */
		rc = plpar_hcall(H_SCM_UNBIND_ALL, ret, H_UNBIND_SCOPE_DRC,
				 p->drc_index, token);
		DBG_MID("plpar_hcall returned rc=%lld, token=%llx, ret[0]=%llx", rc, (unsigned long long)token, (unsigned long long)ret[0]);
		token = ret[0];

		/* Check if we are stalled for some time */
		if (H_IS_LONG_BUSY(rc)) {
			msleep(get_longbusy_msecs(rc));
			rc = H_BUSY;
		} else if (rc == H_BUSY) {
			cond_resched();
		}

	} while (rc == H_BUSY);

	if (rc)
		dev_err(&p->pdev->dev, "unbind error: %lld\n", rc);
	else
		dev_dbg(&p->pdev->dev, "unbind drc 0x%x complete\n",
			p->drc_index);

	DBG_EXIT("");
	return;
}

static int drc_pmem_query_n_bind(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	unsigned long start_addr;
	unsigned long end_addr;
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	int64_t rc;

	DBG_MID("Querying block mem binding for start");
	rc = plpar_hcall(H_SCM_QUERY_BLOCK_MEM_BINDING, ret,
			 p->drc_index, 0);
	DBG_MID("plpar_hcall (start) rc=%lld, ret[0]=%llx", rc, (unsigned long long)ret[0]);
	if (rc)
		goto err_out;
	start_addr = ret[0];

	DBG_MID("Querying block mem binding for end");
	rc = plpar_hcall(H_SCM_QUERY_BLOCK_MEM_BINDING, ret,
			 p->drc_index, p->blocks - 1);
	DBG_MID("plpar_hcall (end) rc=%lld, ret[0]=%llx", rc, (unsigned long long)ret[0]);
	if (rc)
		goto err_out;
	end_addr = ret[0];

	DBG_MID("start_addr=0x%llx, end_addr=0x%llx, expected diff=0x%llx", (unsigned long long)start_addr, (unsigned long long)end_addr, (unsigned long long)((p->blocks - 1) * p->block_size));
	if ((end_addr - start_addr) != ((p->blocks - 1) * p->block_size)) {
		DBG_MID("Address range mismatch: (end-start)=0x%llx", (unsigned long long)(end_addr - start_addr));
		goto err_out;
	}

	p->bound_addr = start_addr;
	dev_dbg(&p->pdev->dev, "bound drc 0x%x to 0x%llx\n", p->drc_index, (unsigned long long)start_addr);
	DBG_EXIT("rc=%lld, bound_addr=0x%llx", rc, (unsigned long long)start_addr);
	return rc;

err_out:
	dev_info(&p->pdev->dev,
		 "Failed to query, trying an unbind followed by bind");
	DBG_MID("Calling drc_pmem_unbind and drc_pmem_bind");
	drc_pmem_unbind(p);
	rc = drc_pmem_bind(p);
	DBG_EXIT("rc=%lld (after unbind/bind)", rc);
	return rc;
}

/*
 * Query the Dimm performance stats from PHYP and copy them (if returned) to
 * provided struct papr_scm_perf_stats instance 'stats' that can hold atleast
 * (num_stats + header) bytes.
 * - If buff_stats == NULL the return value is the size in bytes of the buffer
 * needed to hold all supported performance-statistics.
 * - If buff_stats != NULL and num_stats == 0 then we copy all known
 * performance-statistics to 'buff_stat' and expect to be large enough to
 * hold them.
 * - if buff_stats != NULL and num_stats > 0 then copy the requested
 * performance-statistics to buff_stats.
 */
static ssize_t drc_pmem_query_stats(struct papr_scm_priv *p,
				    struct papr_scm_perf_stats *buff_stats,
				    unsigned int num_stats)
{
	DBG_ENTRY("drc_index=0x%x, num_stats=%u, buff_stats=%p", p->drc_index, num_stats, buff_stats);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	size_t size;
	s64 rc;

	if (buff_stats) {
		DBG_MID("Setting up out buffer: stats_version=%d, num_statistics=%u", PAPR_SCM_PERF_STATS_VERSION, num_stats);
		memcpy(buff_stats->eye_catcher,
		       PAPR_SCM_PERF_STATS_EYECATCHER, 8);
		buff_stats->stats_version =
			cpu_to_be32(PAPR_SCM_PERF_STATS_VERSION);
		buff_stats->num_statistics =
			cpu_to_be32(num_stats);

		if (num_stats)
			size = sizeof(struct papr_scm_perf_stats) +
				num_stats * sizeof(struct papr_scm_perf_stat);
		else
			size = p->stat_buffer_len;
		DBG_MID("Buffer size calculated: %zu", size);
	} else {
		size = 0;
		DBG_MID("No out buffer, size=0");
	}

	DBG_MID("Calling plpar_hcall for performance stats, size=%zu", size);
	rc = plpar_hcall(H_SCM_PERFORMANCE_STATS, ret, p->drc_index,
			 buff_stats ? virt_to_phys(buff_stats) : 0,
			 size);
	DBG_MID("plpar_hcall rc=%lld, ret[0]=%llx", rc, (unsigned long long)ret[0]);

	if (rc == H_PARTIAL) {
		dev_err(&p->pdev->dev,
			"Unknown performance stats, Err:0x%016llX\n", (unsigned long long)ret[0]);
		DBG_MID("H_PARTIAL error, returning -ENOENT");
		DBG_EXIT("rc=%d", -ENOENT);
		return -ENOENT;
	} else if (rc == H_AUTHORITY) {
		dev_info(&p->pdev->dev,
			 "Permission denied while accessing performance stats");
		DBG_MID("H_AUTHORITY error, returning -EPERM");
		DBG_EXIT("rc=%d", -EPERM);
		return -EPERM;
	} else if (rc == H_UNSUPPORTED) {
		dev_dbg(&p->pdev->dev, "Performance stats unsupported\n");
		DBG_MID("H_UNSUPPORTED error, returning -EOPNOTSUPP");
		DBG_EXIT("rc=%d", -EOPNOTSUPP);
		return -EOPNOTSUPP;
	} else if (rc != H_SUCCESS) {
		dev_err(&p->pdev->dev,
			"Failed to query performance stats, Err:%lld\n", rc);
		DBG_MID("General error, returning -EIO");
		DBG_EXIT("rc=%d", -EIO);
		return -EIO;
	} else if (!size) {
		dev_dbg(&p->pdev->dev,
			"Performance stats size %lld\n", (long long)ret[0]);
		DBG_MID("Queried stat buffer size: %lld", (long long)ret[0]);
		DBG_EXIT("rc=%lld", (long long)ret[0]);
		return ret[0];
	}

	dev_dbg(&p->pdev->dev,
		"Performance stats returned %d stats\n",
		be32_to_cpu(buff_stats->num_statistics));
	DBG_MID("Performance stats successfully fetched");
	DBG_EXIT("rc=0");
	return 0;
}

#ifdef CONFIG_PERF_EVENTS
#define to_nvdimm_pmu(_pmu)	container_of(_pmu, struct nvdimm_pmu, pmu)

static const char * const nvdimm_events_map[] = {
	[1] = "CtlResCt",
	[2] = "CtlResTm",
	[3] = "PonSecs ",
	[4] = "MemLife ",
	[5] = "CritRscU",
	[6] = "HostLCnt",
	[7] = "HostSCnt",
	[8] = "HostSDur",
	[9] = "HostLDur",
	[10] = "MedRCnt ",
	[11] = "MedWCnt ",
	[12] = "MedRDur ",
	[13] = "MedWDur ",
	[14] = "CchRHCnt",
	[15] = "CchWHCnt",
	[16] = "FastWCnt",
};

static int papr_scm_pmu_get_value(struct perf_event *event, struct device *dev, u64 *count)
{
	DBG_ENTRY("event config=%llu", (unsigned long long)event->attr.config);
	struct papr_scm_perf_stat *stat;
	struct papr_scm_perf_stats *stats;
	struct papr_scm_priv *p = dev_get_drvdata(dev);
	int rc, size;

	if (event->attr.config == 0 || event->attr.config >= ARRAY_SIZE(nvdimm_events_map)) {
		DBG_MID("Invalid event config: %llu", (unsigned long long)event->attr.config);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}

	size = sizeof(struct papr_scm_perf_stats) + sizeof(struct papr_scm_perf_stat);
	if (!p) {
		DBG_MID("Provider data is NULL");
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}

	stats = kzalloc(size, GFP_KERNEL);
	if (!stats) {
		DBG_MID("kzalloc failed");
		DBG_EXIT("rc=%d", -ENOMEM);
		return -ENOMEM;
	}

	stat = &stats->scm_statistic[0];
	memcpy(&stat->stat_id, nvdimm_events_map[event->attr.config], sizeof(stat->stat_id));
	stat->stat_val = 0;

	rc = drc_pmem_query_stats(p, stats, 1);
	if (rc < 0) {
		DBG_MID("drc_pmem_query_stats failed, rc=%d", rc);
		kfree(stats);
		DBG_EXIT("rc=%d", rc);
		return rc;
	}

	*count = be64_to_cpu(stat->stat_val);
	kfree(stats);
	DBG_EXIT("rc=0, count=0x%llx", (unsigned long long)*count);
	return 0;
}

static int papr_scm_pmu_event_init(struct perf_event *event)
{
	DBG_ENTRY("");
	struct nvdimm_pmu *nd_pmu = to_nvdimm_pmu(event->pmu);
	struct papr_scm_priv *p;

	if (!nd_pmu) {
		DBG_MID("nd_pmu is NULL");
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (event->attr.type != event->pmu->type) {
		DBG_MID("event type mismatch");
		DBG_EXIT("rc=%d", -ENOENT);
		return -ENOENT;
	}
	if (is_sampling_event(event)) {
		DBG_MID("sampling event not supported");
		DBG_EXIT("rc=%d", -EOPNOTSUPP);
		return -EOPNOTSUPP;
	}
	if (has_branch_stack(event)) {
		DBG_MID("branch stack not supported");
		DBG_EXIT("rc=%d", -EOPNOTSUPP);
		return -EOPNOTSUPP;
	}
	p = (struct papr_scm_priv *)nd_pmu->dev->driver_data;
	if (!p) {
		DBG_MID("provider data is NULL");
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (event->attr.config == 0 || event->attr.config > 16) {
		DBG_MID("invalid event config: %llu", (unsigned long long)event->attr.config);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	DBG_EXIT("rc=0");
	return 0;
}

static int papr_scm_pmu_add(struct perf_event *event, int flags)
{
	DBG_ENTRY("flags=0x%x", flags);
	u64 count;
	int rc;
	struct nvdimm_pmu *nd_pmu = to_nvdimm_pmu(event->pmu);

	if (!nd_pmu) {
		DBG_MID("nd_pmu is NULL");
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (flags & PERF_EF_START) {
		rc = papr_scm_pmu_get_value(event, nd_pmu->dev, &count);
		if (rc) {
			DBG_MID("papr_scm_pmu_get_value failed, rc=%d", rc);
			DBG_EXIT("rc=%d", rc);
			return rc;
		}
		local64_set(&event->hw.prev_count, count);
	}
	DBG_EXIT("rc=0");
	return 0;
}

static void papr_scm_pmu_read(struct perf_event *event)
{
	DBG_ENTRY("");
	u64 prev, now;
	int rc;
	struct nvdimm_pmu *nd_pmu = to_nvdimm_pmu(event->pmu);

	if (!nd_pmu) {
		DBG_MID("nd_pmu is NULL");
		DBG_EXIT("");
		return;
	}
	rc = papr_scm_pmu_get_value(event, nd_pmu->dev, &now);
	if (rc) {
		DBG_MID("papr_scm_pmu_get_value failed, rc=%d", rc);
		DBG_EXIT("");
		return;
	}
	prev = local64_xchg(&event->hw.prev_count, now);
	local64_add(now - prev, &event->count);
	DBG_EXIT("");
}

static void papr_scm_pmu_del(struct perf_event *event, int flags)
{
	DBG_ENTRY("flags=0x%x", flags);
	papr_scm_pmu_read(event);
	DBG_EXIT("");
}

static void papr_scm_pmu_register(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	struct nvdimm_pmu *nd_pmu;
	int rc, nodeid;

	nd_pmu = kzalloc(sizeof(*nd_pmu), GFP_KERNEL);
	if (!nd_pmu) {
		DBG_MID("kzalloc failed");
		rc = -ENOMEM;
		goto pmu_err_print;
	}
	if (!p->stat_buffer_len) {
		DBG_MID("stat_buffer_len is zero");
		rc = -ENOENT;
		goto pmu_check_events_err;
	}
	nd_pmu->dev = &p->pdev->dev;
	nd_pmu->pmu = (struct pmu) {
		.name = dev_name(&p->pdev->dev),
		.module = THIS_MODULE,
		.attr_groups = NULL,
		.task_ctx_nr = perf_invalid_context,
		.event_init = papr_scm_pmu_event_init,
		.add = papr_scm_pmu_add,
		.del = papr_scm_pmu_del,
		.start = NULL,
		.stop = NULL,
		.read = papr_scm_pmu_read,
	};
	nd_pmu->pmu.capabilities = PERF_PMU_CAP_NO_INTERRUPT |
				PERF_PMU_CAP_NO_EXCLUDE;
	nodeid = numa_map_to_online_node(dev_to_node(&p->pdev->dev));
	nd_pmu->arch_cpumask = *cpumask_of_node(nodeid);
	rc = register_nvdimm_pmu(nd_pmu, p->pdev);
	if (rc) {
		DBG_MID("register_nvdimm_pmu failed, rc=%d", rc);
		goto pmu_check_events_err;
	}
	p->pdev->archdata.priv = nd_pmu;
	DBG_EXIT("");
	return;

pmu_check_events_err:
	kfree(nd_pmu);
pmu_err_print:
	dev_info(&p->pdev->dev, "nvdimm pmu didn't register rc=%d\n", rc);
	DBG_EXIT("");
}

#else
static void papr_scm_pmu_register(struct papr_scm_priv *p) { }
#endif

/*
 * Issue hcall to retrieve dimm health info and populate papr_scm_priv with the
 * health information.
 */
static int __drc_pmem_query_health(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	u64 bitmap = 0;
	long rc;

	rc = plpar_hcall(H_SCM_HEALTH, ret, p->drc_index);
	DBG_MID("plpar_hcall rc=%ld", rc);
	if (rc == H_SUCCESS) {
		bitmap = ret[0] & ret[1];
		DBG_MID("Health hcall success, bitmap=0x%016llx", (unsigned long long)bitmap);
	} else if (rc == H_FUNCTION) {
		dev_info_once(&p->pdev->dev,
					  "Hcall H_SCM_HEALTH not implemented, assuming empty health bitmap");
		DBG_MID("H_FUNCTION: not implemented, assuming empty health bitmap");
	} else {
		dev_err(&p->pdev->dev,
			"Failed to query health information, Err:%ld\n", rc);
		DBG_MID("Failed to query health, rc=%ld", rc);
		DBG_EXIT("rc=%d", -ENXIO);
		return -ENXIO;
	}

	p->lasthealth_jiffies = jiffies;
	if (p->health_bitmap_inject_mask)
		bitmap = (bitmap & ~p->health_bitmap_inject_mask) |
			p->health_bitmap_inject_mask;
	WRITE_ONCE(p->health_bitmap, bitmap);
	dev_dbg(&p->pdev->dev,
		"Queried dimm health info. Bitmap:0x%016llx Mask:0x%016llx\n",
		(unsigned long long)ret[0], (unsigned long long)ret[1]);
	DBG_EXIT("rc=0");
	return 0;
}

/* Min interval in seconds for assuming stable dimm health */
#define MIN_HEALTH_QUERY_INTERVAL 60

/* Query cached health info and if needed call drc_pmem_query_health */
static int drc_pmem_query_health(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	unsigned long cache_timeout;
	int rc;

	rc = mutex_lock_interruptible(&p->health_mutex);
	if (rc) {
		DBG_MID("mutex_lock_interruptible failed, rc=%d", rc);
		DBG_EXIT("rc=%d", rc);
		return rc;
	}

	cache_timeout = p->lasthealth_jiffies +
		secs_to_jiffies(MIN_HEALTH_QUERY_INTERVAL);
	DBG_MID("cache_timeout=%lu, jiffies=%lu", cache_timeout, jiffies);
	if (time_after(jiffies, cache_timeout)) {
		rc = __drc_pmem_query_health(p);
		DBG_MID("Queried new health info, rc=%d", rc);
	} else {
		rc = 0;
		DBG_MID("Using cached health info");
	}

	mutex_unlock(&p->health_mutex);
	DBG_EXIT("rc=%d", rc);
	return rc;
}

static int papr_scm_meta_get(struct papr_scm_priv *p,
			     struct nd_cmd_get_config_data_hdr *hdr)
{
	DBG_ENTRY("drc_index=0x%x, in_offset=%lu, in_length=%lu", (unsigned int)p->drc_index, (unsigned long)hdr->in_offset, (unsigned long)hdr->in_length);
	unsigned long data[PLPAR_HCALL_BUFSIZE];
	unsigned long offset, data_offset;
	int len, read;
	int64_t ret;

	if ((hdr->in_offset + hdr->in_length) > p->metadata_size) {
		DBG_MID("Input range out of bounds: in_offset=%lu, in_length=%lu, metadata_size=%d", (unsigned long)hdr->in_offset, (unsigned long)hdr->in_length, p->metadata_size);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}

	for (len = hdr->in_length; len; len -= read) {
		data_offset = hdr->in_length - len;
		offset = hdr->in_offset + data_offset;
		DBG_MID("Reading len=%d, data_offset=%lu, offset=%lu", len, data_offset, offset);
		if (len >= 8)
			read = 8;
		else if (len >= 4)
			read = 4;
		else if (len >= 2)
			read = 2;
		else
			read = 1;

		DBG_MID("Calling plpar_hcall H_SCM_READ_METADATA, read=%d", read);
		ret = plpar_hcall(H_SCM_READ_METADATA, data, p->drc_index,
				  offset, read);
		DBG_MID("plpar_hcall returned ret=%lld", (long long)ret);
		if (ret == H_PARAMETER) {
			DBG_MID("bad DRC index");
			DBG_EXIT("rc=%d", -ENODEV);
			return -ENODEV;
		}
		if (ret) {
			DBG_MID("other invalid parameter");
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}

		switch (read) {
		case 8:
			DBG_MID("Copying 8 bytes to out_buf+%lu", data_offset);
			*(uint64_t *)(hdr->out_buf + data_offset) = be64_to_cpu(data[0]);
			break;
		case 4:
			DBG_MID("Copying 4 bytes to out_buf+%lu", data_offset);
			*(uint32_t *)(hdr->out_buf + data_offset) = be32_to_cpu(data[0] & 0xffffffff);
			break;
		case 2:
			DBG_MID("Copying 2 bytes to out_buf+%lu", data_offset);
			*(uint16_t *)(hdr->out_buf + data_offset) = be16_to_cpu(data[0] & 0xffff);
			break;
		case 1:
			DBG_MID("Copying 1 byte to out_buf+%lu", data_offset);
			*(uint8_t *)(hdr->out_buf + data_offset) = (data[0] & 0xff);
			break;
		}
	}
	DBG_EXIT("rc=0");
	return 0;
}

static int papr_scm_meta_set(struct papr_scm_priv *p,
			     struct nd_cmd_set_config_hdr *hdr)
{
	DBG_ENTRY("drc_index=0x%x, in_offset=%lu, in_length=%lu", (unsigned int)p->drc_index, (unsigned long)hdr->in_offset, (unsigned long)hdr->in_length);
	unsigned long offset, data_offset;
	int len, wrote;
	unsigned long data;
	__be64 data_be;
	int64_t ret;

	if ((hdr->in_offset + hdr->in_length) > p->metadata_size) {
		DBG_MID("Input range out of bounds: in_offset=%lu, in_length=%lu, metadata_size=%d", (unsigned long)hdr->in_offset, (unsigned long)hdr->in_length, p->metadata_size);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}

	for (len = hdr->in_length; len; len -= wrote) {
		data_offset = hdr->in_length - len;
		offset = hdr->in_offset + data_offset;
		DBG_MID("Writing len=%d, data_offset=%lu, offset=%lu", len, data_offset, offset);
		if (len >= 8) {
			data = *(uint64_t *)(hdr->in_buf + data_offset);
			data_be = cpu_to_be64(data);
			wrote = 8;
		} else if (len >= 4) {
			data = *(uint32_t *)(hdr->in_buf + data_offset);
			data &= 0xffffffff;
			data_be = cpu_to_be32(data);
			wrote = 4;
		} else if (len >= 2) {
			data = *(uint16_t *)(hdr->in_buf + data_offset);
			data &= 0xffff;
			data_be = cpu_to_be16(data);
			wrote = 2;
		} else {
			data_be = *(uint8_t *)(hdr->in_buf + data_offset);
			data_be &= 0xff;
			wrote = 1;
		}

		DBG_MID("Calling plpar_hcall_norets H_SCM_WRITE_METADATA, wrote=%d", wrote);
		ret = plpar_hcall_norets(H_SCM_WRITE_METADATA, p->drc_index,
					 offset, data_be, wrote);
		DBG_MID("plpar_hcall_norets returned ret=%lld", (long long)ret);
		if (ret == H_PARAMETER) {
			DBG_MID("bad DRC index");
			DBG_EXIT("rc=%d", -ENODEV);
			return -ENODEV;
		}
		if (ret) {
			DBG_MID("other invalid parameter");
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
	}
	DBG_EXIT("rc=0");
	return 0;
}

/*
 * Do a sanity checks on the inputs args to dimm-control function and return
 * '0' if valid. Validation of PDSM payloads happens later in
 * papr_scm_service_pdsm.
 */
static int is_cmd_valid(struct nvdimm *nvdimm, unsigned int cmd, void *buf,
			unsigned int buf_len)
{
	DBG_ENTRY("cmd=0x%x, buf_len=%u", cmd, buf_len);
	unsigned long cmd_mask = PAPR_SCM_DIMM_CMD_MASK;
	struct nd_cmd_pkg *nd_cmd;
	struct papr_scm_priv *p;
	enum papr_pdsm pdsm;

	if (!nvdimm) {
		DBG_MID("nvdimm is NULL");
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}

	p = nvdimm_provider_data(nvdimm);

	if (!test_bit(cmd, &cmd_mask)) {
		DBG_MID("Unsupported cmd=%u", cmd);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}

	if (cmd == ND_CMD_CALL) {
		if (!buf || buf_len < (sizeof(struct nd_cmd_pkg) + ND_PDSM_HDR_SIZE)) {
			DBG_MID("Invalid pkg size=%u", buf_len);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
		nd_cmd = (struct nd_cmd_pkg *)buf;
		if (nd_cmd->nd_family != NVDIMM_FAMILY_PAPR) {
			DBG_MID("Invalid pkg family=0x%llx", nd_cmd->nd_family);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
		pdsm = (enum papr_pdsm)nd_cmd->nd_command;
		if (pdsm <= PAPR_PDSM_MIN || pdsm >= PAPR_PDSM_MAX) {
			DBG_MID("Invalid PDSM: 0x%x", pdsm);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
		if (nd_cmd->nd_size_out < ND_PDSM_HDR_SIZE) {
			DBG_MID("Invalid payload for PDSM[0x%x]", pdsm);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
	}
	DBG_EXIT("rc=0");
	return 0;
}

static int papr_pdsm_fuel_gauge(struct papr_scm_priv *p,
				union nd_pdsm_payload *payload)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	int rc, size;
	u64 statval;
	struct papr_scm_perf_stat *stat;
	struct papr_scm_perf_stats *stats;

	if (!p->stat_buffer_len) {
		DBG_MID("stat_buffer_len is zero");
		DBG_EXIT("rc=0");
		return 0;
	}

	size = sizeof(struct papr_scm_perf_stats) + sizeof(struct papr_scm_perf_stat);
	stats = kzalloc(size, GFP_KERNEL);
	if (!stats) {
		DBG_MID("kzalloc failed");
		DBG_EXIT("rc=%d", -ENOMEM);
		return -ENOMEM;
	}

	stat = &stats->scm_statistic[0];
	memcpy(&stat->stat_id, "MemLife ", sizeof(stat->stat_id));
	stat->stat_val = 0;

	rc = drc_pmem_query_stats(p, stats, 1);
	DBG_MID("drc_pmem_query_stats rc=%d", rc);
	if (rc < 0) {
		dev_dbg(&p->pdev->dev, "Err(%d) fetching fuel gauge\n", rc);
		goto free_stats;
	}

	statval = be64_to_cpu(stat->stat_val);
	dev_dbg(&p->pdev->dev, "Fetched fuel-gauge %llu", statval);
	payload->health.extension_flags |= PDSM_DIMM_HEALTH_RUN_GAUGE_VALID;
	payload->health.dimm_fuel_gauge = statval;

	rc = sizeof(struct nd_papr_pdsm_health);

free_stats:
	kfree(stats);
	DBG_EXIT("rc=%d", rc);
	return rc;
}

/* Add the dirty-shutdown-counter value to the pdsm */
static int papr_pdsm_dsc(struct papr_scm_priv *p,
			 union nd_pdsm_payload *payload)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	payload->health.extension_flags |= PDSM_DIMM_DSC_VALID;
	payload->health.dimm_dsc = p->dirty_shutdown_counter;
	int rc = sizeof(struct nd_papr_pdsm_health);
	DBG_EXIT("rc=%d", rc);
	return rc;
}

/* Fetch the DIMM health info and populate it in provided package. */
static int papr_pdsm_health(struct papr_scm_priv *p,
			    union nd_pdsm_payload *payload)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	int rc;

	rc = mutex_lock_interruptible(&p->health_mutex);
	if (rc)
		goto out;

	rc = __drc_pmem_query_health(p);
	DBG_MID("__drc_pmem_query_health rc=%d", rc);
	if (rc) {
		mutex_unlock(&p->health_mutex);
		goto out;
	}

	payload->health = (struct nd_papr_pdsm_health) {
		.extension_flags = 0,
		.dimm_unarmed = !!(p->health_bitmap & PAPR_PMEM_UNARMED_MASK),
		.dimm_bad_shutdown = !!(p->health_bitmap & PAPR_PMEM_BAD_SHUTDOWN_MASK),
		.dimm_bad_restore = !!(p->health_bitmap & PAPR_PMEM_BAD_RESTORE_MASK),
		.dimm_scrubbed = !!(p->health_bitmap & PAPR_PMEM_SCRUBBED_AND_LOCKED),
		.dimm_locked = !!(p->health_bitmap & PAPR_PMEM_SCRUBBED_AND_LOCKED),
		.dimm_encrypted = !!(p->health_bitmap & PAPR_PMEM_ENCRYPTED),
		.dimm_health = PAPR_PDSM_DIMM_HEALTHY,
	};

	if (p->health_bitmap & PAPR_PMEM_HEALTH_FATAL)
		payload->health.dimm_health = PAPR_PDSM_DIMM_FATAL;
	else if (p->health_bitmap & PAPR_PMEM_HEALTH_CRITICAL)
		payload->health.dimm_health = PAPR_PDSM_DIMM_CRITICAL;
	else if (p->health_bitmap & PAPR_PMEM_HEALTH_UNHEALTHY)
		payload->health.dimm_health = PAPR_PDSM_DIMM_UNHEALTHY;

	mutex_unlock(&p->health_mutex);
	papr_pdsm_fuel_gauge(p, payload);
	papr_pdsm_dsc(p, payload);

	rc = sizeof(struct nd_papr_pdsm_health);

out:
	DBG_EXIT("rc=%d", rc);
	return rc;
}

/* Inject a smart error Add the dirty-shutdown-counter value to the pdsm */
static int papr_pdsm_smart_inject(struct papr_scm_priv *p,
				  union nd_pdsm_payload *payload)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	int rc;
	u32 supported_flags = 0;
	u64 inject_mask = 0, clear_mask = 0;
	u64 mask;

	if (payload->smart_inject.flags & PDSM_SMART_INJECT_HEALTH_FATAL) {
		supported_flags |= PDSM_SMART_INJECT_HEALTH_FATAL;
		if (payload->smart_inject.fatal_enable)
			inject_mask |= PAPR_PMEM_HEALTH_FATAL;
		else
			clear_mask |= PAPR_PMEM_HEALTH_FATAL;
	}

	if (payload->smart_inject.flags & PDSM_SMART_INJECT_BAD_SHUTDOWN) {
		supported_flags |= PDSM_SMART_INJECT_BAD_SHUTDOWN;
		if (payload->smart_inject.unsafe_shutdown_enable)
			inject_mask |= PAPR_PMEM_SHUTDOWN_DIRTY;
		else
			clear_mask |= PAPR_PMEM_SHUTDOWN_DIRTY;
	}

	DBG_MID("inject_mask=0x%llx, clear_mask=0x%llx", (unsigned long long)inject_mask, (unsigned long long)clear_mask);
	rc = mutex_lock_interruptible(&p->health_mutex);
	if (rc) {
		DBG_MID("mutex_lock_interruptible failed, rc=%d", rc);
		DBG_EXIT("rc=%d", rc);
		return rc;
	}
	mask = READ_ONCE(p->health_bitmap_inject_mask);
	mask = (mask & ~clear_mask) | inject_mask;
	WRITE_ONCE(p->health_bitmap_inject_mask, mask);
	p->lasthealth_jiffies = 0;
	mutex_unlock(&p->health_mutex);
	payload->smart_inject.flags = supported_flags;
	rc = sizeof(struct nd_papr_pdsm_health);
	DBG_EXIT("rc=%d", rc);
	return rc;
}

/*
 * 'struct pdsm_cmd_desc'
 * Identifies supported PDSMs' expected length of in/out payloads
 * and pdsm service function.
 *
 * size_in	: Size of input payload if any in the PDSM request.
 * size_out	: Size of output payload if any in the PDSM request.
 * service	: Service function for the PDSM request. Return semantics:
 *		  rc < 0 : Error servicing PDSM and rc indicates the error.
 *		  rc >=0 : Serviced successfully and 'rc' indicate number of
 *			bytes written to payload.
 */
struct pdsm_cmd_desc {
	u32 size_in;
	u32 size_out;
	int (*service)(struct papr_scm_priv *dimm,
		       union nd_pdsm_payload *payload);
};

/* Holds all supported PDSMs' command descriptors */
static const struct pdsm_cmd_desc __pdsm_cmd_descriptors[] = {
	[PAPR_PDSM_MIN] = {
		.size_in = 0,
		.size_out = 0,
		.service = NULL,
	},
	/* New PDSM command descriptors to be added below */

	[PAPR_PDSM_HEALTH] = {
		.size_in = 0,
		.size_out = sizeof(struct nd_papr_pdsm_health),
		.service = papr_pdsm_health,
	},

	[PAPR_PDSM_SMART_INJECT] = {
		.size_in = sizeof(struct nd_papr_pdsm_smart_inject),
		.size_out = sizeof(struct nd_papr_pdsm_smart_inject),
		.service = papr_pdsm_smart_inject,
	},
	/* Empty */
	[PAPR_PDSM_MAX] = {
		.size_in = 0,
		.size_out = 0,
		.service = NULL,
	},
};

/* Given a valid pdsm cmd return its command descriptor else return NULL */
static inline const struct pdsm_cmd_desc *pdsm_cmd_desc(enum papr_pdsm cmd)
{
	DBG_ENTRY("cmd=%d", cmd);
	if (cmd >= 0 || cmd < ARRAY_SIZE(__pdsm_cmd_descriptors)) {
		DBG_EXIT("returning descriptor");
		return &__pdsm_cmd_descriptors[cmd];
	}
	DBG_EXIT("returning NULL");
	return NULL;
}

/*
 * For a given pdsm request call an appropriate service function.
 * Returns errors if any while handling the pdsm command package.
 */
static int papr_scm_service_pdsm(struct papr_scm_priv *p,
				 struct nd_cmd_pkg *pkg)
{
	DBG_ENTRY("drc_index=0x%x, nd_command=0x%x", (unsigned int)p->drc_index, (unsigned int)pkg->nd_command);
	struct nd_pkg_pdsm *pdsm_pkg = (struct nd_pkg_pdsm *)pkg->nd_payload;
	enum papr_pdsm pdsm = (enum papr_pdsm)pkg->nd_command;
	const struct pdsm_cmd_desc *pdsc;
	int rc;

	pdsc = pdsm_cmd_desc(pdsm);
	if (pdsm_pkg->reserved[0] || pdsm_pkg->reserved[1]) {
		DBG_MID("PDSM[0x%x]: Invalid reserved field", pdsm);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (pdsc->size_in && pkg->nd_size_in != (pdsc->size_in + ND_PDSM_HDR_SIZE)) {
		DBG_MID("PDSM[0x%x]: Mismatched size_in=%d", pdsm, pkg->nd_size_in);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (pdsc->size_out && pkg->nd_size_out != (pdsc->size_out + ND_PDSM_HDR_SIZE)) {
		DBG_MID("PDSM[0x%x]: Mismatched size_out=%d", pdsm, pkg->nd_size_out);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (pdsc->service) {
		DBG_MID("PDSM[0x%x]: Servicing..", pdsm);
		rc = pdsc->service(p, &pdsm_pkg->payload);
		if (rc < 0) {
			pdsm_pkg->cmd_status = rc;
			pkg->nd_fw_size = ND_PDSM_HDR_SIZE;
			DBG_MID("PDSM[0x%x]: Service error rc=%d", pdsm, rc);
		} else {
			pdsm_pkg->cmd_status = 0;
			pkg->nd_fw_size = ND_PDSM_HDR_SIZE + rc;
			DBG_MID("PDSM[0x%x]: Service success, bytes written=%d", pdsm, rc);
		}
	} else {
		DBG_MID("PDSM[0x%x]: Unsupported PDSM request", pdsm);
		pdsm_pkg->cmd_status = -ENOENT;
		pkg->nd_fw_size = ND_PDSM_HDR_SIZE;
	}
	DBG_EXIT("rc=%d", pdsm_pkg->cmd_status);
	return pdsm_pkg->cmd_status;
}

static int papr_scm_ndctl(struct nvdimm_bus_descriptor *nd_desc,
			  struct nvdimm *nvdimm, unsigned int cmd, void *buf,
			  unsigned int buf_len, int *cmd_rc)
{
	DBG_ENTRY("cmd=0x%x, buf_len=%u", cmd, buf_len);
	struct nd_cmd_get_config_size *get_size_hdr;
	struct nd_cmd_pkg *call_pkg = NULL;
	struct papr_scm_priv *p;
	int rc;

	rc = is_cmd_valid(nvdimm, cmd, buf, buf_len);
	if (rc) {
		DBG_MID("is_cmd_valid failed, rc=%d", rc);
		DBG_EXIT("rc=%d", rc);
		return rc;
	}
	if (!cmd_rc)
		cmd_rc = &rc;
	p = nvdimm_provider_data(nvdimm);
	switch (cmd) {
	case ND_CMD_GET_CONFIG_SIZE:
		get_size_hdr = buf;
		get_size_hdr->status = 0;
		get_size_hdr->max_xfer = 8;
		get_size_hdr->config_size = p->metadata_size;
		*cmd_rc = 0;
		DBG_MID("ND_CMD_GET_CONFIG_SIZE: metadata_size=%d", p->metadata_size);
		break;
	case ND_CMD_GET_CONFIG_DATA:
		*cmd_rc = papr_scm_meta_get(p, buf);
		DBG_MID("ND_CMD_GET_CONFIG_DATA: rc=%d", *cmd_rc);
		break;
	case ND_CMD_SET_CONFIG_DATA:
		*cmd_rc = papr_scm_meta_set(p, buf);
		DBG_MID("ND_CMD_SET_CONFIG_DATA: rc=%d", *cmd_rc);
		break;
	case ND_CMD_CALL:
		call_pkg = (struct nd_cmd_pkg *)buf;
		*cmd_rc = papr_scm_service_pdsm(p, call_pkg);
		DBG_MID("ND_CMD_CALL: rc=%d", *cmd_rc);
		break;
	default:
		DBG_MID("Unknown command = %d", cmd);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	DBG_MID("returned with cmd_rc = %d", *cmd_rc);
	DBG_EXIT("rc=0");
	return 0;
}

static ssize_t health_bitmap_inject_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);

	return sprintf(buf, "%#llx\n",
		       READ_ONCE(p->health_bitmap_inject_mask));
}

static DEVICE_ATTR_ADMIN_RO(health_bitmap_inject);

static ssize_t perf_stats_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	int index;
	ssize_t rc;
	struct seq_buf s;
	struct papr_scm_perf_stat *stat;
	struct papr_scm_perf_stats *stats;
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);

	if (!p->stat_buffer_len)
		return -ENOENT;

	/* Allocate the buffer for phyp where stats are written */
	stats = kzalloc(p->stat_buffer_len, GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	/* Ask phyp to return all dimm perf stats */
	rc = drc_pmem_query_stats(p, stats, 0);
	if (rc)
		goto free_stats;
	/*
	 * Go through the returned output buffer and print stats and
	 * values. Since stat_id is essentially a char string of
	 * 8 bytes, simply use the string format specifier to print it.
	 */
	seq_buf_init(&s, buf, PAGE_SIZE);
	for (index = 0, stat = stats->scm_statistic;
	     index < be32_to_cpu(stats->num_statistics);
	     ++index, ++stat) {
		seq_buf_printf(&s, "%.8s = 0x%016llX\n",
			       stat->stat_id,
			       be64_to_cpu(stat->stat_val));
	}

free_stats:
	kfree(stats);
	return rc ? rc : (ssize_t)seq_buf_used(&s);
}
static DEVICE_ATTR_ADMIN_RO(perf_stats);

static ssize_t flags_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);
	struct seq_buf s;
	u64 health;
	int rc;

	rc = drc_pmem_query_health(p);
	if (rc)
		return rc;

	/* Copy health_bitmap locally, check masks & update out buffer */
	health = READ_ONCE(p->health_bitmap);

	seq_buf_init(&s, buf, PAGE_SIZE);
	if (health & PAPR_PMEM_UNARMED_MASK)
		seq_buf_printf(&s, "not_armed ");

	if (health & PAPR_PMEM_BAD_SHUTDOWN_MASK)
		seq_buf_printf(&s, "flush_fail ");

	if (health & PAPR_PMEM_BAD_RESTORE_MASK)
		seq_buf_printf(&s, "restore_fail ");

	if (health & PAPR_PMEM_ENCRYPTED)
		seq_buf_printf(&s, "encrypted ");

	if (health & PAPR_PMEM_SMART_EVENT_MASK)
		seq_buf_printf(&s, "smart_notify ");

	if (health & PAPR_PMEM_SCRUBBED_AND_LOCKED)
		seq_buf_printf(&s, "scrubbed locked ");

	if (seq_buf_used(&s))
		seq_buf_printf(&s, "\n");

	return seq_buf_used(&s);
}
DEVICE_ATTR_RO(flags);

static ssize_t dirty_shutdown_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);

	return sysfs_emit(buf, "%llu\n", p->dirty_shutdown_counter);
}
DEVICE_ATTR_RO(dirty_shutdown);

static umode_t papr_nd_attribute_visible(struct kobject *kobj,
					 struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(nvdimm);

	/* For if perf-stats not available remove perf_stats sysfs */
	if (attr == &dev_attr_perf_stats.attr && p->stat_buffer_len == 0)
		return 0;

	return attr->mode;
}

/* papr_scm specific dimm attributes */
static struct attribute *papr_nd_attributes[] = {
	&dev_attr_flags.attr,
	&dev_attr_perf_stats.attr,
	&dev_attr_dirty_shutdown.attr,
	&dev_attr_health_bitmap_inject.attr,
	NULL,
};

static const struct attribute_group papr_nd_attribute_group = {
	.name = "papr",
	.is_visible = papr_nd_attribute_visible,
	.attrs = papr_nd_attributes,
};

static const struct attribute_group *papr_nd_attr_groups[] = {
	&papr_nd_attribute_group,
	NULL,
};

static int papr_scm_nvdimm_init(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", (unsigned int)p->drc_index);
	struct device *dev = &p->pdev->dev;
	struct nd_mapping_desc mapping;
	struct nd_region_desc ndr_desc;
	unsigned long dimm_flags;
	int target_nid, online_nid;

	p->bus_desc.ndctl = papr_scm_ndctl;
	p->bus_desc.module = THIS_MODULE;
	p->bus_desc.of_node = p->pdev->dev.of_node;
	p->bus_desc.provider_name = kstrdup(p->pdev->name, GFP_KERNEL);
	set_bit(NVDIMM_FAMILY_PAPR, &p->bus_desc.dimm_family_mask);
	if (!p->bus_desc.provider_name) {
		DBG_MID("provider_name allocation failed");
		DBG_EXIT("rc=%d", -ENOMEM);
		return -ENOMEM;
	}
	p->bus = nvdimm_bus_register(NULL, &p->bus_desc);
	if (!p->bus) {
		dev_err(dev, "Error creating nvdimm bus %pOF\n", p->dn);
		kfree(p->bus_desc.provider_name);
		DBG_EXIT("rc=%d", -ENXIO);
		return -ENXIO;
	}
	dimm_flags = 0;
	set_bit(NDD_LABELING, &dimm_flags);
	__drc_pmem_query_health(p);
	if (p->health_bitmap & PAPR_PMEM_UNARMED_MASK)
		set_bit(NDD_UNARMED, &dimm_flags);
	p->nvdimm = nvdimm_create(p->bus, p, papr_nd_attr_groups,
				  dimm_flags, PAPR_SCM_DIMM_CMD_MASK, 0, NULL);
	if (!p->nvdimm) {
		dev_err(dev, "Error creating DIMM object for %pOF\n", p->dn);
		DBG_EXIT("rc=%d", -ENXIO);
		goto err;
	}
	if (nvdimm_bus_check_dimm_count(p->bus, 1))
		goto err;
	memset(&mapping, 0, sizeof(mapping));
	mapping.nvdimm = p->nvdimm;
	mapping.start = 0;
	mapping.size = p->blocks * p->block_size;
	memset(&ndr_desc, 0, sizeof(ndr_desc));
	target_nid = dev_to_node(&p->pdev->dev);
	online_nid = numa_map_to_online_node(target_nid);
	ndr_desc.numa_node = online_nid;
	ndr_desc.target_node = target_nid;
	ndr_desc.res = &p->res;
	ndr_desc.of_node = p->dn;
	ndr_desc.provider_data = p;
	ndr_desc.mapping = &mapping;
	ndr_desc.num_mappings = 1;
	ndr_desc.nd_set = &p->nd_set;
	if (p->hcall_flush_required) {
		set_bit(ND_REGION_ASYNC, &ndr_desc.flags);
		ndr_desc.flush = papr_scm_pmem_flush;
	}
	if (p->is_volatile)
		p->region = NULL; // Not handled here
	else {
		set_bit(ND_REGION_PERSIST_MEMCTRL, &ndr_desc.flags);
		p->region = nvdimm_pmem_region_create(p->bus, &ndr_desc);
	}
	if (!p->region) {
		dev_err(dev, "Error registering region %pR from %pOF\n",
				ndr_desc.res, p->dn);
		DBG_EXIT("rc=%d", -ENXIO);
		goto err;
	}
	if (target_nid != online_nid)
		dev_info(dev, "Region registered with target node %d and online node %d",
				 target_nid, online_nid);
	mutex_lock(&papr_ndr_lock);
	list_add_tail(&p->region_list, &papr_nd_regions);
	mutex_unlock(&papr_ndr_lock);
	DBG_EXIT("rc=0");
	return 0;
err:
	nvdimm_bus_unregister(p->bus);
	kfree(p->bus_desc.provider_name);
	DBG_EXIT("rc=%d", -ENXIO);
	return -ENXIO;
}

static void papr_scm_add_badblock(struct nd_region *region,
				  struct nvdimm_bus *bus, u64 phys_addr)
{
	DBG_ENTRY("phys_addr=0x%llx", (unsigned long long)phys_addr);
	u64 aligned_addr = ALIGN_DOWN(phys_addr, L1_CACHE_BYTES);
	if (nvdimm_bus_add_badrange(bus, aligned_addr, L1_CACHE_BYTES)) {
		pr_err("Bad block registration for 0x%llx failed\n", phys_addr);
		DBG_MID("Bad block registration failed for 0x%llx", (unsigned long long)phys_addr);
		DBG_EXIT("");
		return;
	}
	pr_debug("Add memory range (0x%llx - 0x%llx) as bad range\n",
		 aligned_addr, aligned_addr + L1_CACHE_BYTES);
	DBG_MID("Added bad range: 0x%llx - 0x%llx", (unsigned long long)aligned_addr, (unsigned long long)(aligned_addr + L1_CACHE_BYTES));
	nvdimm_region_notify(region, NVDIMM_REVALIDATE_POISON);
	DBG_EXIT("");
}

static int handle_mce_ue(struct notifier_block *nb, unsigned long val,
			 void *data)
{
	DBG_ENTRY("val=%lu", val);
	struct machine_check_event *evt = data;
	struct papr_scm_priv *p;
	u64 phys_addr;
	bool found = false;
	if (evt->error_type != MCE_ERROR_TYPE_UE) {
		DBG_MID("Not a UE error type");
		DBG_EXIT("NOTIFY_DONE");
		return NOTIFY_DONE;
	}
	if (list_empty(&papr_nd_regions)) {
		DBG_MID("papr_nd_regions list is empty");
		DBG_EXIT("NOTIFY_DONE");
		return NOTIFY_DONE;
	}
	phys_addr = evt->u.ue_error.physical_address +
				(evt->u.ue_error.effective_address & ~PAGE_MASK);
	if (!evt->u.ue_error.physical_address_provided ||
	    !is_zone_device_page(pfn_to_page(phys_addr >> PAGE_SHIFT))) {
		DBG_MID("Physical address not provided or not a zone device page");
		DBG_EXIT("NOTIFY_DONE");
		return NOTIFY_DONE;
	}
	mutex_lock(&papr_ndr_lock);
	list_for_each_entry(p, &papr_nd_regions, region_list) {
		if (phys_addr >= p->res.start && phys_addr <= p->res.end) {
			found = true;
			break;
		}
	}
	if (found) {
		DBG_MID("Adding badblock for phys_addr=0x%llx", (unsigned long long)phys_addr);
		papr_scm_add_badblock(p->region, p->bus, phys_addr);
	}
	mutex_unlock(&papr_ndr_lock);
	if (found) {
		DBG_EXIT("NOTIFY_OK");
		return NOTIFY_OK;
	} else {
		DBG_EXIT("NOTIFY_DONE");
		return NOTIFY_DONE;
	}
}

static struct notifier_block mce_ue_nb = {
	.notifier_call = handle_mce_ue
};

static int papr_scm_probe(struct platform_device *pdev)
{
	DBG_ENTRY("pdev=%p", pdev);
	struct device_node *dn = pdev->dev.of_node;
	u32 drc_index, metadata_size;
	u64 blocks, block_size;
	struct papr_scm_priv *p;
	u8 uuid_raw[UUID_SIZE];
	const char *uuid_str;
	ssize_t stat_size;
	uuid_t uuid;
	int rc;
	if (of_property_read_u32(dn, "ibm,my-drc-index", &drc_index)) {
		DBG_MID("missing drc-index");
		DBG_EXIT("rc=%d", -ENODEV);
		return -ENODEV;
	}
	if (of_property_read_u64(dn, "ibm,block-size", &block_size)) {
		DBG_MID("missing block-size");
		DBG_EXIT("rc=%d", -ENODEV);
		return -ENODEV;
	}
	if (of_property_read_u64(dn, "ibm,number-of-blocks", &blocks)) {
		DBG_MID("missing number-of-blocks");
		DBG_EXIT("rc=%d", -ENODEV);
		return -ENODEV;
	}
	if (of_property_read_string(dn, "ibm,unit-guid", &uuid_str)) {
		DBG_MID("missing unit-guid");
		DBG_EXIT("rc=%d", -ENODEV);
		return -ENODEV;
	}
	update_numa_distance(dn);
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		DBG_MID("kzalloc failed");
		DBG_EXIT("rc=%d", -ENOMEM);
		return -ENOMEM;
	}
	mutex_init(&p->health_mutex);
	of_property_read_u32(dn, "ibm,metadata-size", &metadata_size);
	p->dn = dn;
	p->drc_index = drc_index;
	p->block_size = block_size;
	p->blocks = blocks;
	p->is_volatile = !of_property_read_bool(dn, "ibm,cache-flush-required");
	p->hcall_flush_required = of_property_read_bool(dn, "ibm,hcall-flush-required");
	if (of_property_read_u64(dn, "ibm,persistence-failed-count",
							 &p->dirty_shutdown_counter))
		p->dirty_shutdown_counter = 0;
	uuid_parse(uuid_str, &uuid);
	export_uuid(uuid_raw, &uuid);
	p->nd_set.cookie1 = get_unaligned_le64(&uuid_raw[0]);
	p->nd_set.cookie2 = get_unaligned_le64(&uuid_raw[8]);
	p->metadata_size = metadata_size;
	p->pdev = pdev;
	rc = drc_pmem_bind(p);
	if (rc == H_OVERLAP)
		rc = drc_pmem_query_n_bind(p);
	if (rc != H_SUCCESS) {
		DBG_MID("bind err: %d", rc);
		DBG_EXIT("rc=%d", -ENXIO);
		rc = -ENXIO;
		goto err;
	}
	p->res.start = p->bound_addr;
	p->res.end   = p->bound_addr + p->blocks * p->block_size - 1;
	p->res.name  = pdev->name;
	p->res.flags = IORESOURCE_MEM;
	stat_size = drc_pmem_query_stats(p, NULL, 0);
	if (stat_size > 0) {
		p->stat_buffer_len = stat_size;
		DBG_MID("Max perf-stat size %lu-bytes", p->stat_buffer_len);
	}
	rc = papr_scm_nvdimm_init(p);
	if (rc)
		goto err2;
	platform_set_drvdata(pdev, p);
	papr_scm_pmu_register(p);
	DBG_EXIT("rc=0");
	return 0;
err2:
	drc_pmem_unbind(p);
err:
	kfree(p);
	DBG_EXIT("rc=%d", rc);
	return rc;
}

static void papr_scm_remove(struct platform_device *pdev)
{
	DBG_ENTRY("pdev=%p", pdev);
	struct papr_scm_priv *p = platform_get_drvdata(pdev);
	mutex_lock(&papr_ndr_lock);
	list_del(&p->region_list);
	mutex_unlock(&papr_ndr_lock);
	nvdimm_bus_unregister(p->bus);
	drc_pmem_unbind(p);
	if (pdev->archdata.priv)
		unregister_nvdimm_pmu(pdev->archdata.priv);
	pdev->archdata.priv = NULL;
	kfree(p->bus_desc.provider_name);
	kfree(p);
	DBG_EXIT("");
}

static const struct of_device_id papr_scm_match[] = {
	{ .compatible = "ibm,pmemory" },
	{ .compatible = "ibm,pmemory-v2" },
	{ },
};

static struct platform_driver papr_scm_driver = {
	.probe = papr_scm_probe,
	.remove = papr_scm_remove,
	.driver = {
		.name = "papr_scm",
		.of_match_table = papr_scm_match,
	},
};

static int __init papr_scm_init(void)
{
	DBG_ENTRY("");
	int ret;
	ret = platform_driver_register(&papr_scm_driver);
	if (!ret)
		mce_register_notifier(&mce_ue_nb);
	DBG_EXIT("rc=%d", ret);
	return ret;
}
module_init(papr_scm_init);

static void __exit papr_scm_exit(void)
{
	DBG_ENTRY("");
	mce_unregister_notifier(&mce_ue_nb);
	platform_driver_unregister(&papr_scm_driver);
	DBG_EXIT("");
}
module_exit(papr_scm_exit);

MODULE_DEVICE_TABLE(of, papr_scm_match);
MODULE_DESCRIPTION("PAPR Storage Class Memory interface driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("IBM Corporation");
