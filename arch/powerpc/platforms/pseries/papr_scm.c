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

// Debug macros for tracing
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

#define BIND_ANY_ADDR (~0ul)

#define PAPR_SCM_DIMM_CMD_MASK \
	((1ul << ND_CMD_GET_CONFIG_SIZE) | \
	 (1ul << ND_CMD_GET_CONFIG_DATA) | \
	 (1ul << ND_CMD_SET_CONFIG_DATA) | \
	 (1ul << ND_CMD_CALL))

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
	DBG_MID("About to start flush loop for drc_index=0x%x", p->drc_index);
	do {
		rc = plpar_hcall(H_SCM_FLUSH, ret_buf, p->drc_index, token);
		DBG_MID("plpar_hcall returned rc=%ld, token=%lx", rc, token);
		token = ret_buf[0];

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
		DBG_MID("Flush complete for drc_index=0x%x", p->drc_index);
	}

	DBG_EXIT("rc=%ld", rc);
	return rc;
}

static LIST_HEAD(papr_nd_regions);
static DEFINE_MUTEX(papr_ndr_lock);

static int drc_pmem_bind(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	uint64_t saved = 0;
	uint64_t token;
	int64_t rc;

	/*
	 * When the hypervisor cannot map all the requested memory in a single
	 * hcall it returns H_BUSY and we call again with the token until
	 * we get H_SUCCESS. Aborting the retry loop before getting H_SUCCESS
	 * leave the system in an undefined state, so we wait.
	 */
	token = 0;

	DBG_MID("Starting bind loop for drc_index=0x%x, blocks=%llu", p->drc_index, p->blocks);
	do {
		rc = plpar_hcall(H_SCM_BIND_MEM, ret, p->drc_index, 0,
				p->blocks, BIND_ANY_ADDR, token);
		DBG_MID("plpar_hcall returned rc=%lld, token=%lx, ret[0]=%lx, ret[1]=%lx", rc, token, ret[0], ret[1]);
		token = ret[0];
		if (!saved)
			saved = ret[1];
		cond_resched();
	} while (rc == H_BUSY);

	if (rc)
		DBG_MID("Bind failed with rc=%lld", rc);
	else
		DBG_MID("Bind succeeded, saved=0x%llx", saved);

	p->bound_addr = saved;
	dev_dbg(&p->pdev->dev, "bound drc 0x%x to 0x%lx\n",
		p->drc_index, (unsigned long)saved);
	DBG_EXIT("rc=%ld, bound_addr=0x%lx", rc, (unsigned long)saved);
	return rc;
}

static void drc_pmem_unbind(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	uint64_t token = 0;
	int64_t rc;

	dev_dbg(&p->pdev->dev, "unbind drc 0x%x\n", p->drc_index);

	/* NB: unbind has the same retry requirements as drc_pmem_bind() */
	do {

		/* Unbind of all SCM resources associated with drcIndex */
		rc = plpar_hcall(H_SCM_UNBIND_ALL, ret, H_UNBIND_SCOPE_DRC,
				 p->drc_index, token);
		DBG_MID("plpar_hcall returned rc=%lld, token=%lx, ret[0]=%lx", rc, token, ret[0]);
		token = ret[0];

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
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
	unsigned long start_addr;
	unsigned long end_addr;
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	int64_t rc;

	DBG_MID("Querying block mem binding for start");
	rc = plpar_hcall(H_SCM_QUERY_BLOCK_MEM_BINDING, ret,
			 p->drc_index, 0);
	DBG_MID("plpar_hcall (start) rc=%lld, ret[0]=%lx", rc, ret[0]);
	if (rc)
		goto err_out;
	start_addr = ret[0];

	DBG_MID("Querying block mem binding for end");
	rc = plpar_hcall(H_SCM_QUERY_BLOCK_MEM_BINDING, ret,
			 p->drc_index, p->blocks - 1);
	DBG_MID("plpar_hcall (end) rc=%lld, ret[0]=%lx", rc, ret[0]);
	if (rc)
		goto err_out;
	end_addr = ret[0];

	DBG_MID("start_addr=0x%lx, end_addr=0x%lx, expected diff=0x%llx", start_addr, end_addr, (p->blocks - 1) * p->block_size);
	if ((end_addr - start_addr) != ((p->blocks - 1) * p->block_size)) {
		DBG_MID("Address range mismatch: (end-start)=0x%lx", end_addr - start_addr);
		goto err_out;
	}

	p->bound_addr = start_addr;
	dev_dbg(&p->pdev->dev, "bound drc 0x%x to 0x%lx\n", p->drc_index, start_addr);
	DBG_EXIT("rc=%ld, bound_addr=0x%lx", rc, (unsigned long)saved);
	return rc;

err_out:
	dev_info(&p->pdev->dev,
		 "Failed to query, trying an unbind followed by bind");
	DBG_MID("Calling drc_pmem_unbind and drc_pmem_bind");
	drc_pmem_unbind(p);
	rc = drc_pmem_bind(p);
	DBG_EXIT("rc=%ld (after unbind/bind)", rc);
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
	DBG_MID("plpar_hcall rc=%lld, ret[0]=%lx", rc, ret[0]);

	if (rc == H_PARTIAL) {
		dev_err(&p->pdev->dev,
			"Unknown performance stats, Err:0x%016lX\n", ret[0]);
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
			"Performance stats size %ld\n", ret[0]);
		DBG_MID("Queried stat buffer size: %ld", ret[0]);
		DBG_EXIT("rc=%ld", ret[0]);
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
	DBG_ENTRY("event config=%llu", event->attr.config);
	struct papr_scm_perf_stat *stat;
	struct papr_scm_perf_stats *stats;
	struct papr_scm_priv *p = dev_get_drvdata(dev);
	int rc, size;

	if (event->attr.config == 0 || event->attr.config >= ARRAY_SIZE(nvdimm_events_map)) {
		DBG_MID("Invalid event config: %llu");
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
	DBG_EXIT("rc=0, count=%llu", *count);
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
		DBG_MID("invalid event config: %llu", event->attr.config);
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
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
	struct nvdimm_pmu *nd_pmu;
	int rc, nodeid;

	nd_pmu = kzalloc(sizeof(*nd_pmu), GFP_KERNEL);
	if (!nd_pmu) {
		rc = -ENOMEM;
		DBG_MID("kzalloc failed");
		goto pmu_err_print;
	}
	if (!p->stat_buffer_len) {
		rc = -ENOENT;
		DBG_MID("stat_buffer_len is zero");
		goto pmu_check_events_err;
	}
	nd_pmu->pmu.task_ctx_nr = perf_invalid_context;
	nd_pmu->pmu.name = nvdimm_name(p->nvdimm);
	nd_pmu->pmu.event_init = papr_scm_pmu_event_init;
	nd_pmu->pmu.read = papr_scm_pmu_read;
	nd_pmu->pmu.add = papr_scm_pmu_add;
	nd_pmu->pmu.del = papr_scm_pmu_del;
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
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
	unsigned long ret[PLPAR_HCALL_BUFSIZE];
	u64 bitmap = 0;
	long rc;

	rc = plpar_hcall(H_SCM_HEALTH, ret, p->drc_index);
	DBG_MID("plpar_hcall rc=%ld", rc);
	if (rc == H_SUCCESS) {
		bitmap = ret[0] & ret[1];
		DBG_MID("Health hcall success, bitmap=0x%016lx", bitmap);
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
		"Queried dimm health info. Bitmap:0x%016lx Mask:0x%016lx\n",
		ret[0], ret[1]);
	DBG_EXIT("rc=0");
	return 0;
}

/* Min interval in seconds for assuming stable dimm health */
#define MIN_HEALTH_QUERY_INTERVAL 60

/* Query cached health info and if needed call drc_pmem_query_health */
static int drc_pmem_query_health(struct papr_scm_priv *p)
{
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
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
	DBG_ENTRY("drc_index=0x%x, in_offset=%lu, in_length=%lu", p->drc_index, hdr->in_offset, hdr->in_length);
	unsigned long data[PLPAR_HCALL_BUFSIZE];
	unsigned long offset, data_offset;
	int len, read;
	int64_t ret;

	if ((hdr->in_offset + hdr->in_length) > p->metadata_size) {
		DBG_MID("Input range out of bounds: in_offset=%lu, in_length=%lu, metadata_size=%d", hdr->in_offset, hdr->in_length, p->metadata_size);
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
		DBG_MID("plpar_hcall returned ret=%lld", ret);
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
	DBG_ENTRY("drc_index=0x%x, in_offset=%lu, in_length=%lu", p->drc_index, hdr->in_offset, hdr->in_length);
	unsigned long offset, data_offset;
	int len, wrote;
	unsigned long data;
	__be64 data_be;
	int64_t ret;

	if ((hdr->in_offset + hdr->in_length) > p->metadata_size) {
		DBG_MID("Input range out of bounds: in_offset=%lu, in_length=%lu, metadata_size=%d", hdr->in_offset, hdr->in_length, p->metadata_size);
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
		DBG_MID("plpar_hcall_norets returned ret=%lld", ret);
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
	DBG_ENTRY("cmd=%u, buf_len=%u", cmd, buf_len);
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
		dev_dbg(&p->pdev->dev, "Unsupported cmd=%u\n", cmd);
		DBG_MID("Unsupported cmd=%u", cmd);
		DBG_EXIT("rc=%d", -EINVAL);
		return -EINVAL;
	}
	if (cmd == ND_CMD_CALL) {
		if (!buf || buf_len < (sizeof(struct nd_cmd_pkg) + ND_PDSM_HDR_SIZE)) {
			dev_dbg(&p->pdev->dev, "Invalid pkg size=%u\n", buf_len);
			DBG_MID("Invalid pkg size=%u", buf_len);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
		nd_cmd = (struct nd_cmd_pkg *)buf;
		if (nd_cmd->nd_family != NVDIMM_FAMILY_PAPR) {
			dev_dbg(&p->pdev->dev, "Invalid pkg family=0x%llx\n", nd_cmd->nd_family);
			DBG_MID("Invalid pkg family=0x%llx", nd_cmd->nd_family);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
		pdsm = (enum papr_pdsm)nd_cmd->nd_command;
		if (pdsm <= PAPR_PDSM_MIN || pdsm >= PAPR_PDSM_MAX) {
			dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Invalid PDSM\n", pdsm);
			DBG_MID("PDSM[0x%x]: Invalid PDSM", pdsm);
			DBG_EXIT("rc=%d", -EINVAL);
			return -EINVAL;
		}
		if (nd_cmd->nd_size_out < ND_PDSM_HDR_SIZE) {
			dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Invalid payload\n", pdsm);
			DBG_MID("PDSM[0x%x]: Invalid payload", pdsm);
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
	DBG_ENTRY("");
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
	if (rc < 0) {
		dev_dbg(&p->pdev->dev, "Err(%d) fetching fuel gauge\n", rc);
		DBG_MID("drc_pmem_query_stats failed, rc=%d", rc);
		goto free_stats;
	}
	statval = be64_to_cpu(stat->stat_val);
	dev_dbg(&p->pdev->dev, "Fetched fuel-gauge %llu", statval);
	DBG_MID("Fetched fuel-gauge %llu", statval);
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
	DBG_ENTRY("");
	payload->health.extension_flags |= PDSM_DIMM_DSC_VALID;
	payload->health.dimm_dsc = p->dirty_shutdown_counter;
	DBG_EXIT("rc=%zu", sizeof(struct nd_papr_pdsm_health));
	return sizeof(struct nd_papr_pdsm_health);
}

/* Fetch the DIMM health info and populate it in provided package. */
static int papr_pdsm_health(struct papr_scm_priv *p,
			    union nd_pdsm_payload *payload)
{
	DBG_ENTRY("");
	int rc;

	rc = mutex_lock_interruptible(&p->health_mutex);
	if (rc) {
		DBG_MID("mutex_lock_interruptible failed, rc=%d", rc);
		goto out;
	}
	rc = __drc_pmem_query_health(p);
	if (rc) {
		DBG_MID("__drc_pmem_query_health failed, rc=%d", rc);
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
	DBG_ENTRY("");
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
	dev_dbg(&p->pdev->dev, "[Smart-inject] inject_mask=%#llx clear_mask=%#llx\n",
		inject_mask, clear_mask);
	DBG_MID("[Smart-inject] inject_mask=%#llx clear_mask=%#llx", inject_mask, clear_mask);
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
	DBG_EXIT("rc=%d", rc);
	return sizeof(struct nd_papr_pdsm_health);
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
	if (cmd >= 0 || cmd < ARRAY_SIZE(__pdsm_cmd_descriptors))
		return &__pdsm_cmd_descriptors[cmd];

	return NULL;
}

/*
 * For a given pdsm request call an appropriate service function.
 * Returns errors if any while handling the pdsm command package.
 */
static int papr_scm_service_pdsm(struct papr_scm_priv *p,
				 struct nd_cmd_pkg *pkg)
{
	/* Get the PDSM header and PDSM command */
	struct nd_pkg_pdsm *pdsm_pkg = (struct nd_pkg_pdsm *)pkg->nd_payload;
	enum papr_pdsm pdsm = (enum papr_pdsm)pkg->nd_command;
	const struct pdsm_cmd_desc *pdsc;
	int rc;

	/* Fetch corresponding pdsm descriptor for validation and servicing */
	pdsc = pdsm_cmd_desc(pdsm);

	/* Validate pdsm descriptor */
	/* Ensure that reserved fields are 0 */
	if (pdsm_pkg->reserved[0] || pdsm_pkg->reserved[1]) {
		dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Invalid reserved field\n",
			pdsm);
		return -EINVAL;
	}

	/* If pdsm expects some input, then ensure that the size_in matches */
	if (pdsc->size_in &&
	    pkg->nd_size_in != (pdsc->size_in + ND_PDSM_HDR_SIZE)) {
		dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Mismatched size_in=%d\n",
			pdsm, pkg->nd_size_in);
		return -EINVAL;
	}

	/* If pdsm wants to return data, then ensure that  size_out matches */
	if (pdsc->size_out &&
	    pkg->nd_size_out != (pdsc->size_out + ND_PDSM_HDR_SIZE)) {
		dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Mismatched size_out=%d\n",
			pdsm, pkg->nd_size_out);
		return -EINVAL;
	}

	/* Service the pdsm */
	if (pdsc->service) {
		dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Servicing..\n", pdsm);

		rc = pdsc->service(p, &pdsm_pkg->payload);

		if (rc < 0) {
			/* error encountered while servicing pdsm */
			pdsm_pkg->cmd_status = rc;
			pkg->nd_fw_size = ND_PDSM_HDR_SIZE;
		} else {
			/* pdsm serviced and 'rc' bytes written to payload */
			pdsm_pkg->cmd_status = 0;
			pkg->nd_fw_size = ND_PDSM_HDR_SIZE + rc;
		}
	} else {
		dev_dbg(&p->pdev->dev, "PDSM[0x%x]: Unsupported PDSM request\n",
			pdsm);
		pdsm_pkg->cmd_status = -ENOENT;
		pkg->nd_fw_size = ND_PDSM_HDR_SIZE;
	}

	return pdsm_pkg->cmd_status;
}

static int papr_scm_ndctl(struct nvdimm_bus_descriptor *nd_desc,
			  struct nvdimm *nvdimm, unsigned int cmd, void *buf,
			  unsigned int buf_len, int *cmd_rc)
{
	struct nd_cmd_get_config_size *get_size_hdr;
	struct nd_cmd_pkg *call_pkg = NULL;
	struct papr_scm_priv *p;
	int rc;

	rc = is_cmd_valid(nvdimm, cmd, buf, buf_len);
	if (rc) {
		pr_debug("Invalid cmd=0x%x. Err=%d\n", cmd, rc);
		return rc;
	}

	/* Use a local variable in case cmd_rc pointer is NULL */
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
		break;

	case ND_CMD_GET_CONFIG_DATA:
		*cmd_rc = papr_scm_meta_get(p, buf);
		break;

	case ND_CMD_SET_CONFIG_DATA:
		*cmd_rc = papr_scm_meta_set(p, buf);
		break;

	case ND_CMD_CALL:
		call_pkg = (struct nd_cmd_pkg *)buf;
		*cmd_rc = papr_scm_service_pdsm(p, call_pkg);
		break;

	default:
		dev_dbg(&p->pdev->dev, "Unknown command = %d\n", cmd);
		return -EINVAL;
	}

	dev_dbg(&p->pdev->dev, "returned with cmd_rc = %d\n", *cmd_rc);

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
	DBG_ENTRY("");
	struct seq_buf s;
	struct papr_scm_perf_stat *stat;
	struct papr_scm_perf_stats *stats;
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);
	ssize_t rc = 0;
	int i;

	if (!p->stat_buffer_len) {
		DBG_MID("stat_buffer_len is zero");
		DBG_EXIT("rc=0");
		return 0;
	}
	stats = kzalloc(p->stat_buffer_len, GFP_KERNEL);
	if (!stats) {
		DBG_MID("kzalloc failed");
		DBG_EXIT("rc=-ENOMEM");
		return -ENOMEM;
	}
	rc = drc_pmem_query_stats(p, stats, 0);
	if (rc < 0) {
		DBG_MID("drc_pmem_query_stats failed, rc=%zd", rc);
		kfree(stats);
		DBG_EXIT("rc=%zd", rc);
		return rc;
	}
	seq_buf_init(&s, buf, PAGE_SIZE);
	for (i = 0; i < be32_to_cpu(stats->num_statistics); i++) {
		stat = &stats->scm_statistic[i];
		seq_buf_printf(&s, "%8s: %llu\n",
			       stat->stat_id,
			       be64_to_cpu(stat->stat_val));
		DBG_MID("stat_id=%8s, stat_val=%llu", stat->stat_id, be64_to_cpu(stat->stat_val));
	}
	kfree(stats);
	DBG_EXIT("rc=%zd", s.len);
	return s.len;
}
static DEVICE_ATTR_ADMIN_RO(perf_stats);

static ssize_t flags_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);
	struct seq_buf s;
	ssize_t rc;

	seq_buf_init(&s, buf, PAGE_SIZE);
	seq_buf_printf(&s, "is_volatile: %d\n", p->is_volatile);
	seq_buf_printf(&s, "hcall_flush_required: %d\n", p->hcall_flush_required);
	DBG_MID("is_volatile=%d, hcall_flush_required=%d", p->is_volatile, p->hcall_flush_required);
	rc = s.len;
	DBG_EXIT("rc=%zd", rc);
	return rc;
}
DEVICE_ATTR_RO(flags);

static ssize_t dirty_shutdown_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	struct nvdimm *dimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(dimm);
	ssize_t rc;

	rc = sprintf(buf, "%llu\n", p->dirty_shutdown_counter);
	DBG_MID("dirty_shutdown_counter=%llu", p->dirty_shutdown_counter);
	DBG_EXIT("rc=%zd", rc);
	return rc;
}
DEVICE_ATTR_RO(dirty_shutdown);

static umode_t papr_nd_attribute_visible(struct kobject *kobj,
					 struct attribute *attr, int n)
{
	DBG_ENTRY("");
	struct device *dev = kobj_to_dev(kobj);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	struct papr_scm_priv *p = nvdimm_provider_data(nvdimm);
	umode_t ret = attr->mode;

	/* For if perf-stats not available remove perf_stats sysfs */
	if (attr == &dev_attr_perf_stats.attr && p->stat_buffer_len == 0)
		ret = 0;

	DBG_EXIT("ret=%u", ret);
	return ret;
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
	DBG_ENTRY("drc_index=0x%x", p->drc_index);
	struct device *dev = &p->pdev->dev;
	struct nd_mapping_desc mapping;
	struct nd_region_desc ndr_desc;
	int rc;

	memset(&mapping, 0, sizeof(mapping));
	memset(&ndr_desc, 0, sizeof(ndr_desc));
	mapping.npfn = p->blocks;
	mapping.start = p->bound_addr >> PAGE_SHIFT;
	mapping.end = mapping.start + mapping.npfn - 1;
	mapping.target_node = dev_to_node(dev);
	mapping.flags = 0;
	ndr_desc.numa_node = mapping.target_node;
	ndr_desc.res = &p->res;
	ndr_desc.provider_data = p;
	ndr_desc.mapping = &mapping;
	ndr_desc.num_mappings = 1;
	ndr_desc.nd_set = &p->nd_set;
	ndr_desc.attr_groups = papr_nd_attr_groups;
	ndr_desc.dax = 0;
	ndr_desc.nvdimm = p->nvdimm;
	ndr_desc.flags = 0;
	ndr_desc.pmem_flush = papr_scm_pmem_flush;
	ndr_desc.bus = p->bus;
	ndr_desc.dev = dev;
	ndr_desc.nd_region_name = dev_name(dev);
	ndr_desc.nd_region_type = ND_REGION_PMEM;
	ndr_desc.nd_region_flags = 0;
	ndr_desc.nd_region_id = 0;
	ndr_desc.nd_region_size = p->blocks * p->block_size;
	ndr_desc.nd_region_alignment = PAGE_SIZE;
	ndr_desc.nd_region_interleave_ways = 1;
	ndr_desc.nd_region_interleave_granularity = PAGE_SIZE;
	ndr_desc.nd_region_interleave_offset = 0;
	ndr_desc.nd_region_interleave_index = 0;
	ndr_desc.nd_region_interleave_set = NULL;
	ndr_desc.nd_region_interleave_set_size = 0;
	ndr_desc.nd_region_interleave_set_offset = 0;
	ndr_desc.nd_region_interleave_set_index = 0;
	ndr_desc.nd_region_interleave_set_stride = 0;
	ndr_desc.nd_region_interleave_set_type = 0;
	ndr_desc.nd_region_interleave_set_flags = 0;
	ndr_desc.nd_region_interleave_set_id = 0;
	ndr_desc.nd_region_interleave_set_name = NULL;
	ndr_desc.nd_region_interleave_set_bus = NULL;
	ndr_desc.nd_region_interleave_set_dev = NULL;
	ndr_desc.nd_region_interleave_set_provider_data = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups = NULL;
	ndr_desc.nd_region_interleave_set_dax = 0;
	ndr_desc.nd_region_interleave_set_nvdimm = NULL;
	ndr_desc.nd_region_interleave_set_flags2 = 0;
	ndr_desc.nd_region_interleave_set_alignment = 0;
	ndr_desc.nd_region_interleave_set_size2 = 0;
	ndr_desc.nd_region_interleave_set_offset2 = 0;
	ndr_desc.nd_region_interleave_set_index2 = 0;
	ndr_desc.nd_region_interleave_set_stride2 = 0;
	ndr_desc.nd_region_interleave_set_type2 = 0;
	ndr_desc.nd_region_interleave_set_flags2_2 = 0;
	ndr_desc.nd_region_interleave_set_id2 = 0;
	ndr_desc.nd_region_interleave_set_name2 = NULL;
	ndr_desc.nd_region_interleave_set_bus2 = NULL;
	ndr_desc.nd_region_interleave_set_dev2 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data2 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups2 = NULL;
	ndr_desc.nd_region_interleave_set_dax2 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm2 = NULL;
	ndr_desc.nd_region_interleave_set_flags3 = 0;
	ndr_desc.nd_region_interleave_set_alignment2 = 0;
	ndr_desc.nd_region_interleave_set_size3 = 0;
	ndr_desc.nd_region_interleave_set_offset3 = 0;
	ndr_desc.nd_region_interleave_set_index3 = 0;
	ndr_desc.nd_region_interleave_set_stride3 = 0;
	ndr_desc.nd_region_interleave_set_type3 = 0;
	ndr_desc.nd_region_interleave_set_flags4 = 0;
	ndr_desc.nd_region_interleave_set_id3 = 0;
	ndr_desc.nd_region_interleave_set_name3 = NULL;
	ndr_desc.nd_region_interleave_set_bus3 = NULL;
	ndr_desc.nd_region_interleave_set_dev3 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data3 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups3 = NULL;
	ndr_desc.nd_region_interleave_set_dax3 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm3 = NULL;
	ndr_desc.nd_region_interleave_set_flags5 = 0;
	ndr_desc.nd_region_interleave_set_alignment3 = 0;
	ndr_desc.nd_region_interleave_set_size4 = 0;
	ndr_desc.nd_region_interleave_set_offset4 = 0;
	ndr_desc.nd_region_interleave_set_index4 = 0;
	ndr_desc.nd_region_interleave_set_stride4 = 0;
	ndr_desc.nd_region_interleave_set_type4 = 0;
	ndr_desc.nd_region_interleave_set_flags6 = 0;
	ndr_desc.nd_region_interleave_set_id4 = 0;
	ndr_desc.nd_region_interleave_set_name4 = NULL;
	ndr_desc.nd_region_interleave_set_bus4 = NULL;
	ndr_desc.nd_region_interleave_set_dev4 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data4 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups4 = NULL;
	ndr_desc.nd_region_interleave_set_dax4 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm4 = NULL;
	ndr_desc.nd_region_interleave_set_flags7 = 0;
	ndr_desc.nd_region_interleave_set_alignment4 = 0;
	ndr_desc.nd_region_interleave_set_size5 = 0;
	ndr_desc.nd_region_interleave_set_offset5 = 0;
	ndr_desc.nd_region_interleave_set_index5 = 0;
	ndr_desc.nd_region_interleave_set_stride5 = 0;
	ndr_desc.nd_region_interleave_set_type5 = 0;
	ndr_desc.nd_region_interleave_set_flags8 = 0;
	ndr_desc.nd_region_interleave_set_id5 = 0;
	ndr_desc.nd_region_interleave_set_name5 = NULL;
	ndr_desc.nd_region_interleave_set_bus5 = NULL;
	ndr_desc.nd_region_interleave_set_dev5 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data5 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups5 = NULL;
	ndr_desc.nd_region_interleave_set_dax5 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm5 = NULL;
	ndr_desc.nd_region_interleave_set_flags9 = 0;
	ndr_desc.nd_region_interleave_set_alignment5 = 0;
	ndr_desc.nd_region_interleave_set_size6 = 0;
	ndr_desc.nd_region_interleave_set_offset6 = 0;
	ndr_desc.nd_region_interleave_set_index6 = 0;
	ndr_desc.nd_region_interleave_set_stride6 = 0;
	ndr_desc.nd_region_interleave_set_type6 = 0;
	ndr_desc.nd_region_interleave_set_flags10 = 0;
	ndr_desc.nd_region_interleave_set_id6 = 0;
	ndr_desc.nd_region_interleave_set_name6 = NULL;
	ndr_desc.nd_region_interleave_set_bus6 = NULL;
	ndr_desc.nd_region_interleave_set_dev6 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data6 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups6 = NULL;
	ndr_desc.nd_region_interleave_set_dax6 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm6 = NULL;
	ndr_desc.nd_region_interleave_set_flags11 = 0;
	ndr_desc.nd_region_interleave_set_alignment6 = 0;
	ndr_desc.nd_region_interleave_set_size7 = 0;
	ndr_desc.nd_region_interleave_set_offset7 = 0;
	ndr_desc.nd_region_interleave_set_index7 = 0;
	ndr_desc.nd_region_interleave_set_stride7 = 0;
	ndr_desc.nd_region_interleave_set_type7 = 0;
	ndr_desc.nd_region_interleave_set_flags12 = 0;
	ndr_desc.nd_region_interleave_set_id7 = 0;
	ndr_desc.nd_region_interleave_set_name7 = NULL;
	ndr_desc.nd_region_interleave_set_bus7 = NULL;
	ndr_desc.nd_region_interleave_set_dev7 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data7 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups7 = NULL;
	ndr_desc.nd_region_interleave_set_dax7 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm7 = NULL;
	ndr_desc.nd_region_interleave_set_flags13 = 0;
	ndr_desc.nd_region_interleave_set_alignment7 = 0;
	ndr_desc.nd_region_interleave_set_size8 = 0;
	ndr_desc.nd_region_interleave_set_offset8 = 0;
	ndr_desc.nd_region_interleave_set_index8 = 0;
	ndr_desc.nd_region_interleave_set_stride8 = 0;
	ndr_desc.nd_region_interleave_set_type8 = 0;
	ndr_desc.nd_region_interleave_set_flags14 = 0;
	ndr_desc.nd_region_interleave_set_id8 = 0;
	ndr_desc.nd_region_interleave_set_name8 = NULL;
	ndr_desc.nd_region_interleave_set_bus8 = NULL;
	ndr_desc.nd_region_interleave_set_dev8 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data8 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups8 = NULL;
	ndr_desc.nd_region_interleave_set_dax8 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm8 = NULL;
	ndr_desc.nd_region_interleave_set_flags15 = 0;
	ndr_desc.nd_region_interleave_set_alignment8 = 0;
	ndr_desc.nd_region_interleave_set_size9 = 0;
	ndr_desc.nd_region_interleave_set_offset9 = 0;
	ndr_desc.nd_region_interleave_set_index9 = 0;
	ndr_desc.nd_region_interleave_set_stride9 = 0;
	ndr_desc.nd_region_interleave_set_type9 = 0;
	ndr_desc.nd_region_interleave_set_flags16 = 0;
	ndr_desc.nd_region_interleave_set_id9 = 0;
	ndr_desc.nd_region_interleave_set_name9 = NULL;
	ndr_desc.nd_region_interleave_set_bus9 = NULL;
	ndr_desc.nd_region_interleave_set_dev9 = NULL;
	ndr_desc.nd_region_interleave_set_provider_data9 = NULL;
	ndr_desc.nd_region_interleave_set_attr_groups9 = NULL;
	ndr_desc.nd_region_interleave_set_dax9 = 0;
	ndr_desc.nd_region_interleave_set_nvdimm9 = NULL;

	p->region = nvdimm_pmem_region_create(p->bus, &ndr_desc);
	if (!p->region) {
		dev_err(dev, "Failed to create nvdimm region\n");
		DBG_EXIT("rc=-ENOMEM");
		return -ENOMEM;
	}
	list_add(&p->region_list, &papr_nd_regions);
	DBG_EXIT("rc=0");
	return 0;
}

static void papr_scm_add_badblock(struct nd_region *region,
				  struct nvdimm_bus *bus, u64 phys_addr)
{
	DBG_ENTRY("phys_addr=0x%llx", phys_addr);
	struct nd_region_data *ndrd = nd_region_provider_data(region);
	struct badblocks *bb = &ndrd->bb;
	int rc;

	rc = badblocks_set(bb, (phys_addr - region->ndr_start) >> SECTOR_SHIFT, 1);
	DBG_MID("badblocks_set rc=%d", rc);
	DBG_EXIT("");
}

static int handle_mce_ue(struct notifier_block *nb, unsigned long val,
			 void *data)
{
	DBG_ENTRY("");
	struct machine_check_event *evt = data;
	struct papr_scm_priv *p;
	u64 phys_addr;
	bool found = false;

	if (evt->error_type != MCE_ERROR_TYPE_UE)
		return NOTIFY_DONE;

	if (list_empty(&papr_nd_regions))
		return NOTIFY_DONE;

	/*
	 * The physical address obtained here is PAGE_SIZE aligned, so get the
	 * exact address from the effective address
	 */
	phys_addr = evt->u.ue_error.physical_address +
			(evt->u.ue_error.effective_address & ~PAGE_MASK);
	list_for_each_entry(p, &papr_nd_regions, region_list) {
		if (phys_addr >= p->res.start && phys_addr < p->res.end) {
			papr_scm_add_badblock(p->region, p->bus, phys_addr);
			found = true;
			break;
		}
	}
	DBG_EXIT("ret=%d", found ? NOTIFY_OK : NOTIFY_DONE);
	return found ? NOTIFY_OK : NOTIFY_DONE;
}

static struct notifier_block mce_ue_nb = {
	.notifier_call = handle_mce_ue
};

static int papr_scm_probe(struct platform_device *pdev)
{
	DBG_ENTRY("");
	struct device_node *dn = pdev->dev.of_node;
	u32 drc_index, metadata_size;
	u64 blocks, block_size;
	struct papr_scm_priv *p;
	u8 uuid_raw[UUID_SIZE];
	const char *uuid_str;
	ssize_t stat_size;
	uuid_t uuid;
	int rc;

	/* check we have all the required DT properties */
	if (of_property_read_u32(dn, "ibm,my-drc-index", &drc_index)) {
		dev_err(&pdev->dev, "%pOF: missing drc-index!\n", dn);
		return -ENODEV;
	}

	if (of_property_read_u64(dn, "ibm,block-size", &block_size)) {
		dev_err(&pdev->dev, "%pOF: missing block-size!\n", dn);
		return -ENODEV;
	}

	if (of_property_read_u64(dn, "ibm,number-of-blocks", &blocks)) {
		dev_err(&pdev->dev, "%pOF: missing number-of-blocks!\n", dn);
		return -ENODEV;
	}

	if (of_property_read_string(dn, "ibm,unit-guid", &uuid_str)) {
		dev_err(&pdev->dev, "%pOF: missing unit-guid!\n", dn);
		return -ENODEV;
	}

	/*
	 * open firmware platform device create won't update the NUMA 
	 * distance table. For PAPR SCM devices we use numa_map_to_online_node()
	 * to find the nearest online NUMA node and that requires correct
	 * distance table information.
	 */
	update_numa_distance(dn);

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	/* Initialize the dimm mutex */
	mutex_init(&p->health_mutex);

	/* optional DT properties */
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

	/* We just need to ensure that set cookies are unique across */
	uuid_parse(uuid_str, &uuid);

	/*
	 * The cookie1 and cookie2 are not really little endian.
	 * We store a raw buffer representation of the
	 * uuid string so that we can compare this with the label
	 * area cookie irrespective of the endian configuration
	 * with which the kernel is built.
	 *
	 * Historically we stored the cookie in the below format.
	 * for a uuid string 72511b67-0b3b-42fd-8d1d-5be3cae8bcaa
	 *	cookie1 was 0xfd423b0b671b5172
	 *	cookie2 was 0xaabce8cae35b1d8d
	 */
	export_uuid(uuid_raw, &uuid);
	p->nd_set.cookie1 = get_unaligned_le64(&uuid_raw[0]);
	p->nd_set.cookie2 = get_unaligned_le64(&uuid_raw[8]);

	/* might be zero */
	p->metadata_size = metadata_size;
	p->pdev = pdev;

	/* request the hypervisor to bind this region to somewhere in memory */
	rc = drc_pmem_bind(p);

	/* If phyp says drc memory still bound then force unbound and retry */
	if (rc == H_OVERLAP)
		rc = drc_pmem_query_n_bind(p);

	if (rc != H_SUCCESS) {
		dev_err(&p->pdev->dev, "bind err: %d\n", rc);
		rc = -ENXIO;
		goto err;
	}

	/* setup the resource for the newly bound range */
	p->res.start = p->bound_addr;
	p->res.end   = p->bound_addr + p->blocks * p->block_size - 1;
	p->res.name  = pdev->name;
	p->res.flags = IORESOURCE_MEM;

	/* Try retrieving the stat buffer and see if its supported */
	stat_size = drc_pmem_query_stats(p, NULL, 0);
	if (stat_size > 0) {
		p->stat_buffer_len = stat_size;
		dev_dbg(&p->pdev->dev, "Max perf-stat size %lu-bytes\n",
			p->stat_buffer_len);
	}

	rc = papr_scm_nvdimm_init(p);
	if (rc)
		goto err2;

	platform_set_drvdata(pdev, p);
	papr_scm_pmu_register(p);

	return 0;

err2:	drc_pmem_unbind(p);
err:	kfree(p);
	DBG_EXIT("rc=%d", rc);
	return rc;
}

static void papr_scm_remove(struct platform_device *pdev)
{
	DBG_ENTRY("");
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
