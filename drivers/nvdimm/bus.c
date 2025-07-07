// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/libnvdimm.h>
#include <linux/sched/mm.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/fcntl.h>
#include <linux/async.h>
#include <linux/ndctl.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/nd.h>
#include "nd-core.h"
#include "nd.h"
#include "pfn.h"

int nvdimm_major;
static int nvdimm_bus_major;
static DEFINE_IDA(nd_ida);

static const struct class nd_class = {
	.name = "nd",
};

static int to_nd_device_type(const struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	if (is_nvdimm(dev))
		{printf(KERN_INFO "%s: EXIT: ND_DEVICE_DIMM pid=%d\n", __func__, current->pid); return ND_DEVICE_DIMM;}
	else if (is_memory(dev))
		{printf(KERN_INFO "%s: EXIT: ND_DEVICE_REGION_PMEM pid=%d\n", __func__, current->pid); return ND_DEVICE_REGION_PMEM;}
	else if (is_nd_dax(dev))
		{printf(KERN_INFO "%s: EXIT: ND_DEVICE_DAX_PMEM pid=%d\n", __func__, current->pid); return ND_DEVICE_DAX_PMEM;}
	else if (is_nd_region(dev->parent))
		{printf(KERN_INFO "%s: EXIT: nstype=%d pid=%d\n", __func__, nd_region_to_nstype(to_nd_region(dev->parent)), current->pid); return nd_region_to_nstype(to_nd_region(dev->parent));}

	printf(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;
}

static int nvdimm_bus_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, env=%p pid=%d\n", __func__, dev, env, current->pid);
	int ret = add_uevent_var(env, "MODALIAS=" ND_DEVICE_MODALIAS_FMT,
			to_nd_device_type(dev));
	printf(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static struct module *to_bus_provider(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	if (is_nd_region(dev)) {
		struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
		printf(KERN_INFO "%s: EXIT: module=%p pid=%d\n", __func__, nvdimm_bus->nd_desc->module, current->pid);
		return nvdimm_bus->nd_desc->module;
	}
	printf(KERN_INFO "%s: EXIT: NULL pid=%d\n", __func__, current->pid);
	return NULL;
}

static void nvdimm_bus_probe_start(struct nvdimm_bus *nvdimm_bus)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p pid=%d\n", __func__, nvdimm_bus, current->pid);
	nvdimm_bus_lock(&nvdimm_bus->dev);
	nvdimm_bus->probe_active++;
	nvdimm_bus_unlock(&nvdimm_bus->dev);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nvdimm_bus_probe_end(struct nvdimm_bus *nvdimm_bus)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p pid=%d\n", __func__, nvdimm_bus, current->pid);
	nvdimm_bus_lock(&nvdimm_bus->dev);
	if (--nvdimm_bus->probe_active == 0)
		wake_up(&nvdimm_bus->wait);
	nvdimm_bus_unlock(&nvdimm_bus->dev);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static int nvdimm_bus_probe(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nd_device_driver *nd_drv = to_nd_device_driver(dev->driver);
	struct module *provider = to_bus_provider(dev);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
	int rc;

	if (!try_module_get(provider)) {
		printf(KERN_INFO "%s: EXIT: -ENXIO pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	dev_dbg(&nvdimm_bus->dev, "START: %s.probe(%s)\n",
			dev->driver->name, dev_name(dev));

	nvdimm_bus_probe_start(nvdimm_bus);
	rc = nd_drv->probe(dev);
	if ((rc == 0 || rc == -EOPNOTSUPP) &&
			dev->parent && is_nd_region(dev->parent))
		nd_region_advance_seeds(to_nd_region(dev->parent), dev);
	nvdimm_bus_probe_end(nvdimm_bus);

	dev_dbg(&nvdimm_bus->dev, "END: %s.probe(%s) = %d\n", dev->driver->name,
			dev_name(dev), rc);

	if (rc != 0)
		module_put(provider);
	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static void nvdimm_bus_remove(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nd_device_driver *nd_drv = to_nd_device_driver(dev->driver);
	struct module *provider = to_bus_provider(dev);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);

	if (nd_drv->remove)
		nd_drv->remove(dev);

	dev_dbg(&nvdimm_bus->dev, "%s.remove(%s)\n", dev->driver->name,
			dev_name(dev));
	module_put(provider);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nvdimm_bus_shutdown(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
	struct nd_device_driver *nd_drv = NULL;

	if (dev->driver)
		nd_drv = to_nd_device_driver(dev->driver);

	if (nd_drv && nd_drv->shutdown) {
		nd_drv->shutdown(dev);
		dev_dbg(&nvdimm_bus->dev, "%s.shutdown(%s)\n",
				dev->driver->name, dev_name(dev));
	}
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

void nd_device_notify(struct device *dev, enum nvdimm_event event)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, event=%d pid=%d\n", __func__, dev, event, current->pid);
	device_lock(dev);
	if (dev->driver) {
		struct nd_device_driver *nd_drv;

		nd_drv = to_nd_device_driver(dev->driver);
		if (nd_drv->notify)
			nd_drv->notify(dev, event);
	}
	device_unlock(dev);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL(nd_device_notify);

void nvdimm_region_notify(struct nd_region *nd_region, enum nvdimm_event event)
{
	printf(KERN_INFO "%s: ENTRY: nd_region=%p, event=%d pid=%d\n", __func__, nd_region, event, current->pid);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(&nd_region->dev);

	if (!nvdimm_bus) {
		printf(KERN_INFO "%s: EXIT: nvdimm_bus=NULL pid=%d\n", __func__, current->pid);
		return;
	}

	/* caller is responsible for holding a reference on the device */
	nd_device_notify(&nd_region->dev, event);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL_GPL(nvdimm_region_notify);

struct clear_badblocks_context {
	resource_size_t phys, cleared;
};

static int nvdimm_clear_badblocks_region(struct device *dev, void *data)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, data=%p pid=%d\n", __func__, dev, data, current->pid);
	struct clear_badblocks_context *ctx = data;
	struct nd_region *nd_region;
	resource_size_t ndr_end;
	sector_t sector;

	/* make sure device is a region */
	if (!is_memory(dev)) {
		printf(KERN_INFO "%s: EXIT: not memory pid=%d\n", __func__, current->pid);
		return 0;
	}

	nd_region = to_nd_region(dev);
	ndr_end = nd_region->ndr_start + nd_region->ndr_size - 1;

	/* make sure we are in the region */
	if (ctx->phys < nd_region->ndr_start ||
	    (ctx->phys + ctx->cleared - 1) > ndr_end) {
		printf(KERN_INFO "%s: EXIT: not in region pid=%d\n", __func__, current->pid);
		return 0;
	}

	sector = (ctx->phys - nd_region->ndr_start) / 512;
	badblocks_clear(&nd_region->bb, sector, ctx->cleared / 512);

	if (nd_region->bb_state)
		sysfs_notify_dirent(nd_region->bb_state);

	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
	return 0;
}

static void nvdimm_clear_badblocks_regions(struct nvdimm_bus *nvdimm_bus,
		phys_addr_t phys, u64 cleared)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p, phys=%pa, cleared=%llu pid=%d\n", __func__, nvdimm_bus, &phys, cleared, current->pid);
	struct clear_badblocks_context ctx = {
		.phys = phys,
		.cleared = cleared,
	};

	device_for_each_child(&nvdimm_bus->dev, &ctx,
			nvdimm_clear_badblocks_region);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nvdimm_account_cleared_poison(struct nvdimm_bus *nvdimm_bus,
		phys_addr_t phys, u64 cleared)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p, phys=%pa, cleared=%llu pid=%d\n", __func__, nvdimm_bus, &phys, cleared, current->pid);
	if (cleared > 0)
		badrange_forget(&nvdimm_bus->badrange, phys, cleared);

	if (cleared > 0 && cleared / 512)
		nvdimm_clear_badblocks_regions(nvdimm_bus, phys, cleared);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

long nvdimm_clear_poison(struct device *dev, phys_addr_t phys,
		unsigned int len)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, phys=%pa, len=%u pid=%d\n", __func__, dev, &phys, len, current->pid);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc;
	struct nd_cmd_clear_error clear_err;
	struct nd_cmd_ars_cap ars_cap;
	u32 clear_err_unit, mask;
	unsigned int noio_flag;
	int cmd_rc, rc;

	if (!nvdimm_bus)
		return -ENXIO;

	nd_desc = nvdimm_bus->nd_desc;
	/*
	 * if ndctl does not exist, it's PMEM_LEGACY and
	 * we want to just pretend everything is handled.
	 */
	if (!nd_desc->ndctl)
		return len;

	memset(&ars_cap, 0, sizeof(ars_cap));
	ars_cap.address = phys;
	ars_cap.length = len;
	noio_flag = memalloc_noio_save();
	rc = nd_desc->ndctl(nd_desc, NULL, ND_CMD_ARS_CAP, &ars_cap,
			sizeof(ars_cap), &cmd_rc);
	memalloc_noio_restore(noio_flag);
	if (rc < 0)
		return rc;
	if (cmd_rc < 0)
		return cmd_rc;
	clear_err_unit = ars_cap.clear_err_unit;
	if (!clear_err_unit || !is_power_of_2(clear_err_unit))
		return -ENXIO;

	mask = clear_err_unit - 1;
	if ((phys | len) & mask)
		return -ENXIO;
	memset(&clear_err, 0, sizeof(clear_err));
	clear_err.address = phys;
	clear_err.length = len;
	noio_flag = memalloc_noio_save();
	rc = nd_desc->ndctl(nd_desc, NULL, ND_CMD_CLEAR_ERROR, &clear_err,
			sizeof(clear_err), &cmd_rc);
	memalloc_noio_restore(noio_flag);
	if (rc < 0)
		return rc;
	if (cmd_rc < 0)
		return cmd_rc;

	nvdimm_account_cleared_poison(nvdimm_bus, phys, clear_err.cleared);

	printf(KERN_INFO "%s: EXIT: cleared=%lld pid=%d\n", __func__, (long long)clear_err.cleared, current->pid);
	return clear_err.cleared;
}
EXPORT_SYMBOL_GPL(nvdimm_clear_poison);

static int nvdimm_bus_match(struct device *dev, const struct device_driver *drv);

static const struct bus_type nvdimm_bus_type = {
	.name = "nd",
	.uevent = nvdimm_bus_uevent,
	.match = nvdimm_bus_match,
	.probe = nvdimm_bus_probe,
	.remove = nvdimm_bus_remove,
	.shutdown = nvdimm_bus_shutdown,
};

static void nvdimm_bus_release(struct device *dev)
{
	struct nvdimm_bus *nvdimm_bus;

	nvdimm_bus = container_of(dev, struct nvdimm_bus, dev);
	ida_free(&nd_ida, nvdimm_bus->id);
	kfree(nvdimm_bus);
}

static const struct device_type nvdimm_bus_dev_type = {
	.release = nvdimm_bus_release,
	.groups = nvdimm_bus_attribute_groups,
};

bool is_nvdimm_bus(struct device *dev)
{
	return dev->type == &nvdimm_bus_dev_type;
}

struct nvdimm_bus *walk_to_nvdimm_bus(struct device *nd_dev)
{
	struct device *dev;

	for (dev = nd_dev; dev; dev = dev->parent)
		if (is_nvdimm_bus(dev))
			break;
	dev_WARN_ONCE(nd_dev, !dev, "invalid dev, not on nd bus\n");
	if (dev)
		return to_nvdimm_bus(dev);
	return NULL;
}

struct nvdimm_bus *to_nvdimm_bus(struct device *dev)
{
	struct nvdimm_bus *nvdimm_bus;

	nvdimm_bus = container_of(dev, struct nvdimm_bus, dev);
	WARN_ON(!is_nvdimm_bus(dev));
	return nvdimm_bus;
}
EXPORT_SYMBOL_GPL(to_nvdimm_bus);

struct nvdimm_bus *nvdimm_to_bus(struct nvdimm *nvdimm)
{
	return to_nvdimm_bus(nvdimm->dev.parent);
}
EXPORT_SYMBOL_GPL(nvdimm_to_bus);

static struct lock_class_key nvdimm_bus_key;

struct nvdimm_bus *nvdimm_bus_register(struct device *parent,
		struct nvdimm_bus_descriptor *nd_desc)
{
	printf(KERN_INFO "%s: ENTRY: parent=%p, nd_desc=%p pid=%d\n", __func__, parent, nd_desc, current->pid);
	struct nvdimm_bus *nvdimm_bus;
	int rc;

	nvdimm_bus = kzalloc(sizeof(*nvdimm_bus), GFP_KERNEL);
	if (!nvdimm_bus)
		return NULL;
	INIT_LIST_HEAD(&nvdimm_bus->list);
	INIT_LIST_HEAD(&nvdimm_bus->mapping_list);
	init_waitqueue_head(&nvdimm_bus->wait);
	nvdimm_bus->id = ida_alloc(&nd_ida, GFP_KERNEL);
	if (nvdimm_bus->id < 0) {
		kfree(nvdimm_bus);
		return NULL;
	}
	mutex_init(&nvdimm_bus->reconfig_mutex);
	badrange_init(&nvdimm_bus->badrange);
	nvdimm_bus->nd_desc = nd_desc;
	nvdimm_bus->dev.parent = parent;
	nvdimm_bus->dev.type = &nvdimm_bus_dev_type;
	nvdimm_bus->dev.groups = nd_desc->attr_groups;
	nvdimm_bus->dev.bus = &nvdimm_bus_type;
	nvdimm_bus->dev.of_node = nd_desc->of_node;
	device_initialize(&nvdimm_bus->dev);
	lockdep_set_class(&nvdimm_bus->dev.mutex, &nvdimm_bus_key);
	device_set_pm_not_required(&nvdimm_bus->dev);
	rc = dev_set_name(&nvdimm_bus->dev, "ndbus%d", nvdimm_bus->id);
	if (rc)
		goto err;

	rc = device_add(&nvdimm_bus->dev);
	if (rc) {
		dev_dbg(&nvdimm_bus->dev, "registration failed: %d\n", rc);
		printf(KERN_INFO "%s: EXIT: device_add failed rc=%d pid=%d\n", __func__, rc, current->pid);
		goto err;
	}
	printf(KERN_INFO "%s: EXIT: nvdimm_bus=%p pid=%d\n", __func__, nvdimm_bus, current->pid);
	return nvdimm_bus;
 err:
	put_device(&nvdimm_bus->dev);
	return NULL;
}
EXPORT_SYMBOL_GPL(nvdimm_bus_register);

void nvdimm_bus_unregister(struct nvdimm_bus *nvdimm_bus)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p pid=%d\n", __func__, nvdimm_bus, current->pid);
	if (!nvdimm_bus) {
		printf(KERN_INFO "%s: EXIT: nvdimm_bus is NULL pid=%d\n", __func__, current->pid);
		return;
	}
	device_unregister(&nvdimm_bus->dev);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL_GPL(nvdimm_bus_unregister);

static int child_unregister(struct device *dev, void *data)
{
	/*
	 * the singular ndctl class device per bus needs to be
	 * "device_destroy"ed, so skip it here
	 *
	 * i.e. remove classless children
	 */
	if (dev->class)
		return 0;

	if (is_nvdimm(dev))
		nvdimm_delete(to_nvdimm(dev));
	else
		nd_device_unregister(dev, ND_SYNC);

	return 0;
}

static void free_badrange_list(struct list_head *badrange_list)
{
	printf(KERN_INFO "%s: ENTRY: badrange_list=%p pid=%d\n", __func__, badrange_list, current->pid);
	struct badrange_entry *bre, *next;

	list_for_each_entry_safe(bre, next, badrange_list, list) {
		list_del(&bre->list);
		kfree(bre);
	}
	list_del_init(badrange_list);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nd_bus_remove(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);

	mutex_lock(&nvdimm_bus_list_mutex);
	list_del_init(&nvdimm_bus->list);
	mutex_unlock(&nvdimm_bus_list_mutex);

	wait_event(nvdimm_bus->wait,
			atomic_read(&nvdimm_bus->ioctl_active) == 0);

	nd_synchronize();
	device_for_each_child(&nvdimm_bus->dev, NULL, child_unregister);

	spin_lock(&nvdimm_bus->badrange.lock);
	free_badrange_list(&nvdimm_bus->badrange.list);
	spin_unlock(&nvdimm_bus->badrange.lock);

	nvdimm_bus_destroy_ndctl(nvdimm_bus);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static int nd_bus_probe(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	int rc;

	rc = nvdimm_bus_create_ndctl(nvdimm_bus);
	if (rc) {
		printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	mutex_lock(&nvdimm_bus_list_mutex);
	list_add_tail(&nvdimm_bus->list, &nvdimm_bus_list);
	mutex_unlock(&nvdimm_bus_list_mutex);

	/* enable bus provider attributes to look up their local context */
	dev_set_drvdata(dev, nvdimm_bus->nd_desc);

	printf(KERN_INFO "%s: EXIT: rc=0 pid=%d\n", __func__, current->pid);
	return 0;
}

static struct nd_device_driver nd_bus_driver = {
	.probe = nd_bus_probe,
	.remove = nd_bus_remove,
	.drv = {
		.name = "nd_bus",
		.suppress_bind_attrs = true,
		.bus = &nvdimm_bus_type,
		.owner = THIS_MODULE,
		.mod_name = KBUILD_MODNAME,
	},
};

static int nvdimm_bus_match(struct device *dev, const struct device_driver *drv)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, drv=%p pid=%d\n", __func__, dev, drv, current->pid);
	const struct nd_device_driver *nd_drv = to_nd_device_driver(drv);

	if (is_nvdimm_bus(dev) && nd_drv == &nd_bus_driver) {
		printf(KERN_INFO "%s: EXIT: 1 pid=%d\n", __func__, current->pid);
		return true;
	}

	int ret = !!test_bit(to_nd_device_type(dev), &nd_drv->type);
	printf(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static ASYNC_DOMAIN_EXCLUSIVE(nd_async_domain);

void nd_synchronize(void)
{
	printf(KERN_INFO "%s: ENTRY pid=%d\n", __func__, current->pid);
	async_synchronize_full_domain(&nd_async_domain);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL_GPL(nd_synchronize);

static void nd_async_device_register(void *d, async_cookie_t cookie)
{
	printf(KERN_INFO "%s: ENTRY: d=%p, cookie=%lu pid=%d\n", __func__, d, (unsigned long)cookie, current->pid);
	struct device *dev = d;

	if (device_add(dev) != 0) {
		dev_err(dev, "%s: failed\n", __func__);
		put_device(dev);
	}
	put_device(dev);
	if (dev->parent)
		put_device(dev->parent);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nd_async_device_unregister(void *d, async_cookie_t cookie)
{
	printf(KERN_INFO "%s: ENTRY: d=%p, cookie=%lu pid=%d\n", __func__, d, (unsigned long)cookie, current->pid);
	struct device *dev = d;

	/* flush bus operations before delete */
	nvdimm_bus_lock(dev);
	nvdimm_bus_unlock(dev);

	device_unregister(dev);
	put_device(dev);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void __nd_device_register(struct device *dev, bool sync)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, sync=%d pid=%d\n", __func__, dev, sync, current->pid);
	if (!dev) {
		printf(KERN_INFO "%s: EXIT: dev is NULL pid=%d\n", __func__, current->pid);
		return;
	}

	/*
	 * Ensure that region devices always have their NUMA node set as
	 * early as possible. This way we are able to make certain that
	 * any memory associated with the creation and the creation
	 * itself of the region is associated with the correct node.
	 */
	if (is_nd_region(dev))
		set_dev_node(dev, to_nd_region(dev)->numa_node);

	dev->bus = &nvdimm_bus_type;
	device_set_pm_not_required(dev);
	if (dev->parent) {
		get_device(dev->parent);
		if (dev_to_node(dev) == NUMA_NO_NODE)
			set_dev_node(dev, dev_to_node(dev->parent));
	}
	get_device(dev);

	if (sync)
		nd_async_device_register(dev, 0);
	else
		async_schedule_dev_domain(nd_async_device_register, dev,
				  &nd_async_domain);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

void nd_device_register(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	__nd_device_register(dev, false);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL(nd_device_register);

void nd_device_register_sync(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	__nd_device_register(dev, true);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

void nd_device_unregister(struct device *dev, enum nd_async_mode mode)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, mode=%d pid=%d\n", __func__, dev, mode, current->pid);
	bool killed;

	switch (mode) {
	case ND_ASYNC:
		if (!kill_device(dev)) {
			printf(KERN_INFO "%s: EXIT: not killed (ND_ASYNC) pid=%d\n", __func__, current->pid);
			return;
		}
		get_device(dev);
		async_schedule_domain(nd_async_device_unregister, dev,
				&nd_async_domain);
		break;
	case ND_SYNC:
		device_lock(dev);
		killed = kill_device(dev);
		device_unlock(dev);

		if (!killed) {
			printf(KERN_INFO "%s: EXIT: not killed (ND_SYNC) pid=%d\n", __func__, current->pid);
			return;
		}

		nd_synchronize();
		device_unregister(dev);
		break;
	}
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL(nd_device_unregister);

/**
 * __nd_driver_register() - register a region or a namespace driver
 * @nd_drv: driver to register
 * @owner: automatically set by nd_driver_register() macro
 * @mod_name: automatically set by nd_driver_register() macro
 */
int __nd_driver_register(struct nd_device_driver *nd_drv, struct module *owner,
		const char *mod_name)
{
	printf(KERN_INFO "%s: ENTRY: nd_drv=%p, owner=%p, mod_name=%s pid=%d\n", __func__, nd_drv, owner, mod_name, current->pid);
	struct device_driver *drv = &nd_drv->drv;

	if (!nd_drv->type) {
		pr_debug("driver type bitmask not set (%ps)\n",
				__builtin_return_address(0));
		printf(KERN_INFO "%s: EXIT: -EINVAL (no type) pid=%d\n", __func__, current->pid);
		return -EINVAL;
	}

	if (!nd_drv->probe) {
		pr_debug("%s ->probe() must be specified\n", mod_name);
		printf(KERN_INFO "%s: EXIT: -EINVAL (no probe) pid=%d\n", __func__, current->pid);
		return -EINVAL;
	}

	drv->bus = &nvdimm_bus_type;
	drv->owner = owner;
	drv->mod_name = mod_name;

	int ret = driver_register(drv);
	printf(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}
EXPORT_SYMBOL(__nd_driver_register);

void nvdimm_check_and_set_ro(struct gendisk *disk)
{
	printf(KERN_INFO "%s: ENTRY: disk=%p pid=%d\n", __func__, disk, current->pid);
	struct device *dev = disk_to_dev(disk)->parent;
	struct nd_region *nd_region = to_nd_region(dev->parent);
	int disk_ro = get_disk_ro(disk);

	if (disk_ro == nd_region->ro) {
		printf(KERN_INFO "%s: EXIT: already correct pid=%d\n", __func__, current->pid);
		return;
	}

	dev_info(dev, "%s read-%s, marking %s read-%s\n",
		 dev_name(&nd_region->dev), nd_region->ro ? "only" : "write",
		 disk->disk_name, nd_region->ro ? "only" : "write");
	set_disk_ro(disk, nd_region->ro);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
EXPORT_SYMBOL(nvdimm_check_and_set_ro);

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	ssize_t ret = sprintf(buf, ND_DEVICE_MODALIAS_FMT "\n",
			to_nd_device_type(dev));
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(modalias);

static ssize_t devtype_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	ssize_t ret = sprintf(buf, "%s\n", dev->type->name);
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(devtype);

static struct attribute *nd_device_attributes[] = {
	&dev_attr_modalias.attr,
	&dev_attr_devtype.attr,
	NULL,
};
/*
 * nd_device_attribute_group - generic attributes for all devices on an nd bus
 */
const struct attribute_group nd_device_attribute_group = {
	.attrs = nd_device_attributes,
};


static ssize_t numa_node_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	ssize_t ret = sprintf(buf, "%d\n", dev_to_node(dev));
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(numa_node);

static int nvdimm_dev_to_target_node(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct device *parent = dev->parent;
	struct nd_region *nd_region = NULL;

	if (is_nd_region(dev))
		nd_region = to_nd_region(dev);
	else if (parent && is_nd_region(parent))
		nd_region = to_nd_region(parent);

	if (!nd_region) {
		printf(KERN_INFO "%s: EXIT: NUMA_NO_NODE pid=%d\n", __func__, current->pid);
		return NUMA_NO_NODE;
	}
	printf(KERN_INFO "%s: EXIT: target_node=%d pid=%d\n", __func__, nd_region->target_node, current->pid);
	return nd_region->target_node;
}

static ssize_t target_node_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	ssize_t ret = sprintf(buf, "%d\n", nvdimm_dev_to_target_node(dev));
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(target_node);

static struct attribute *nd_numa_attributes[] = {
	&dev_attr_numa_node.attr,
	&dev_attr_target_node.attr,
	NULL,
};

static umode_t nd_numa_attr_visible(struct kobject *kobj, struct attribute *a,
		int n)
{
	printf(KERN_INFO "%s: ENTRY: kobj=%p, a=%p, n=%d pid=%d\n", __func__, kobj, a, n, current->pid);
	struct device *dev = container_of(kobj, typeof(*dev), kobj);

	if (!IS_ENABLED(CONFIG_NUMA)) {
		printf(KERN_INFO "%s: EXIT: 0 (NUMA not enabled) pid=%d\n", __func__, current->pid);
		return 0;
	}

	if (a == &dev_attr_target_node.attr &&
			nvdimm_dev_to_target_node(dev) == NUMA_NO_NODE) {
		printf(KERN_INFO "%s: EXIT: 0 (no target node) pid=%d\n", __func__, current->pid);
		return 0;
	}

	printf(KERN_INFO "%s: EXIT: mode=%o pid=%d\n", __func__, a->mode, current->pid);
	return a->mode;
}

/*
 * nd_numa_attribute_group - NUMA attributes for all devices on an nd bus
 */
const struct attribute_group nd_numa_attribute_group = {
	.attrs = nd_numa_attributes,
	.is_visible = nd_numa_attr_visible,
};

static void ndctl_release(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	kfree(dev);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static struct lock_class_key nvdimm_ndctl_key;

int nvdimm_bus_create_ndctl(struct nvdimm_bus *nvdimm_bus)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p pid=%d\n", __func__, nvdimm_bus, current->pid);
	dev_t devt = MKDEV(nvdimm_bus_major, nvdimm_bus->id);
	struct device *dev;
	int rc;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		printf(KERN_INFO "%s: EXIT: -ENOMEM pid=%d\n", __func__, current->pid);
		return -ENOMEM;
	}
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &nvdimm_ndctl_key);
	device_set_pm_not_required(dev);
	dev->class = &nd_class;
	dev->parent = &nvdimm_bus->dev;
	dev->devt = devt;
	dev->release = ndctl_release;
	rc = dev_set_name(dev, "ndctl%d", nvdimm_bus->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc) {
		dev_dbg(&nvdimm_bus->dev, "failed to register ndctl%d: %d\n",
				nvdimm_bus->id, rc);
		printf(KERN_INFO "%s: EXIT: dev_set_name failed rc=%d pid=%d\n", __func__, rc, current->pid);
		goto err;
	}
	printf(KERN_INFO "%s: EXIT: success pid=%d\n", __func__, current->pid);
	return 0;

err:
	put_device(dev);
	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

void nvdimm_bus_destroy_ndctl(struct nvdimm_bus *nvdimm_bus)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p pid=%d\n", __func__, nvdimm_bus, current->pid);
	device_destroy(&nd_class, MKDEV(nvdimm_bus_major, nvdimm_bus->id));
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static const struct nd_cmd_desc __nd_cmd_dimm_descs[] = {
	[ND_CMD_IMPLEMENTED] = { },
	[ND_CMD_SMART] = {
		.out_num = 2,
		.out_sizes = { 4, 128, },
	},
	[ND_CMD_SMART_THRESHOLD] = {
		.out_num = 2,
		.out_sizes = { 4, 8, },
	},
	[ND_CMD_DIMM_FLAGS] = {
		.out_num = 2,
		.out_sizes = { 4, 4 },
	},
	[ND_CMD_GET_CONFIG_SIZE] = {
		.out_num = 3,
		.out_sizes = { 4, 4, 4, },
	},
	[ND_CMD_GET_CONFIG_DATA] = {
		.in_num = 2,
		.in_sizes = { 4, 4, },
		.out_num = 2,
		.out_sizes = { 4, UINT_MAX, },
	},
	[ND_CMD_SET_CONFIG_DATA] = {
		.in_num = 3,
		.in_sizes = { 4, 4, UINT_MAX, },
		.out_num = 1,
		.out_sizes = { 4, },
	},
	[ND_CMD_VENDOR] = {
		.in_num = 3,
		.in_sizes = { 4, 4, UINT_MAX, },
		.out_num = 3,
		.out_sizes = { 4, 4, UINT_MAX, },
	},
	[ND_CMD_CALL] = {
		.in_num = 2,
		.in_sizes = { sizeof(struct nd_cmd_pkg), UINT_MAX, },
		.out_num = 1,
		.out_sizes = { UINT_MAX, },
	},
};

const struct nd_cmd_desc *nd_cmd_dimm_desc(int cmd)
{
	printf(KERN_INFO "%s: ENTRY: cmd=%d pid=%d\n", __func__, cmd, current->pid);
	if (cmd < ARRAY_SIZE(__nd_cmd_dimm_descs)) {
		printf(KERN_INFO "%s: EXIT: desc=%p pid=%d\n", __func__, &__nd_cmd_dimm_descs[cmd], current->pid);
		return &__nd_cmd_dimm_descs[cmd];
	}
	printf(KERN_INFO "%s: EXIT: NULL pid=%d\n", __func__, current->pid);
	return NULL;
}
EXPORT_SYMBOL_GPL(nd_cmd_dimm_desc);

static const struct nd_cmd_desc __nd_cmd_bus_descs[] = {
	[ND_CMD_IMPLEMENTED] = { },
	[ND_CMD_ARS_CAP] = {
		.in_num = 2,
		.in_sizes = { 8, 8, },
		.out_num = 4,
		.out_sizes = { 4, 4, 4, 4, },
	},
	[ND_CMD_ARS_START] = {
		.in_num = 5,
		.in_sizes = { 8, 8, 2, 1, 5, },
		.out_num = 2,
		.out_sizes = { 4, 4, },
	},
	[ND_CMD_ARS_STATUS] = {
		.out_num = 3,
		.out_sizes = { 4, 4, UINT_MAX, },
	},
	[ND_CMD_CLEAR_ERROR] = {
		.in_num = 2,
		.in_sizes = { 8, 8, },
		.out_num = 3,
		.out_sizes = { 4, 4, 8, },
	},
	[ND_CMD_CALL] = {
		.in_num = 2,
		.in_sizes = { sizeof(struct nd_cmd_pkg), UINT_MAX, },
		.out_num = 1,
		.out_sizes = { UINT_MAX, },
	},
};

const struct nd_cmd_desc *nd_cmd_bus_desc(int cmd)
{
	printf(KERN_INFO "%s: ENTRY: cmd=%d pid=%d\n", __func__, cmd, current->pid);
	if (cmd < ARRAY_SIZE(__nd_cmd_bus_descs)) {
		printf(KERN_INFO "%s: EXIT: desc=%p pid=%d\n", __func__, &__nd_cmd_bus_descs[cmd], current->pid);
		return &__nd_cmd_bus_descs[cmd];
	}
	printf(KERN_INFO "%s: EXIT: NULL pid=%d\n", __func__, current->pid);
	return NULL;
}
EXPORT_SYMBOL_GPL(nd_cmd_bus_desc);

u32 nd_cmd_in_size(struct nvdimm *nvdimm, int cmd,
		const struct nd_cmd_desc *desc, int idx, void *buf)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p, cmd=%d, desc=%p, idx=%d, buf=%p pid=%d\n", __func__, nvdimm, cmd, desc, idx, buf, current->pid);
	if (idx >= desc->in_num) {
		printf(KERN_INFO "%s: EXIT: UINT_MAX (idx >= in_num) pid=%d\n", __func__, current->pid);
		return UINT_MAX;
	}

	if (desc->in_sizes[idx] < UINT_MAX) {
		printf(KERN_INFO "%s: EXIT: size=%u pid=%d\n", __func__, desc->in_sizes[idx], current->pid);
		return desc->in_sizes[idx];
	}

	if (nvdimm && cmd == ND_CMD_SET_CONFIG_DATA && idx == 2) {
		struct nd_cmd_set_config_hdr *hdr = buf;
		printf(KERN_INFO "%s: EXIT: in_length=%u pid=%d\n", __func__, hdr->in_length, current->pid);
		return hdr->in_length;
	} else if (nvdimm && cmd == ND_CMD_VENDOR && idx == 2) {
		struct nd_cmd_vendor_hdr *hdr = buf;
		printf(KERN_INFO "%s: EXIT: in_length=%u pid=%d\n", __func__, hdr->in_length, current->pid);
		return hdr->in_length;
	} else if (cmd == ND_CMD_CALL) {
		struct nd_cmd_pkg *pkg = buf;
		printf(KERN_INFO "%s: EXIT: nd_size_in=%u pid=%d\n", __func__, pkg->nd_size_in, current->pid);
		return pkg->nd_size_in;
	}

	printf(KERN_INFO "%s: EXIT: UINT_MAX (default) pid=%d\n", __func__, current->pid);
	return UINT_MAX;
}
EXPORT_SYMBOL_GPL(nd_cmd_in_size);

u32 nd_cmd_out_size(struct nvdimm *nvdimm, int cmd,
		const struct nd_cmd_desc *desc, int idx, const u32 *in_field,
		const u32 *out_field, unsigned long remainder)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p, cmd=%d, desc=%p, idx=%d, in_field=%p, out_field=%p, remainder=%lu pid=%d\n", __func__, nvdimm, cmd, desc, idx, in_field, out_field, remainder, current->pid);
	if (idx >= desc->out_num) {
		printf(KERN_INFO "%s: EXIT: UINT_MAX (idx >= out_num) pid=%d\n", __func__, current->pid);
		return UINT_MAX;
	}

	if (desc->out_sizes[idx] < UINT_MAX) {
		printf(KERN_INFO "%s: EXIT: size=%u pid=%d\n", __func__, desc->out_sizes[idx], current->pid);
		return desc->out_sizes[idx];
	}

	if (nvdimm && cmd == ND_CMD_GET_CONFIG_DATA && idx == 1) {
		printf(KERN_INFO "%s: EXIT: in_field[1]=%u pid=%d\n", __func__, in_field[1], current->pid);
		return in_field[1];
	} else if (nvdimm && cmd == ND_CMD_VENDOR && idx == 2) {
		printf(KERN_INFO "%s: EXIT: out_field[1]=%u pid=%d\n", __func__, out_field[1], current->pid);
		return out_field[1];
	} else if (!nvdimm && cmd == ND_CMD_ARS_STATUS && idx == 2) {
		if (out_field[1] < 4) {
			printf(KERN_INFO "%s: EXIT: 0 (out_field[1] < 4) pid=%d\n", __func__, current->pid);
			return 0;
		}
		if (out_field[1] - 4 == remainder) {
			printf(KERN_INFO "%s: EXIT: remainder=%lu pid=%d\n", __func__, remainder, current->pid);
			return remainder;
		}
		printf(KERN_INFO "%s: EXIT: out_field[1]-8=%u pid=%d\n", __func__, out_field[1] - 8, current->pid);
		return out_field[1] - 8;
	} else if (cmd == ND_CMD_CALL) {
		struct nd_cmd_pkg *pkg = (struct nd_cmd_pkg *) in_field;
		printf(KERN_INFO "%s: EXIT: nd_size_out=%u pid=%d\n", __func__, pkg->nd_size_out, current->pid);
		return pkg->nd_size_out;
	}

	printf(KERN_INFO "%s: EXIT: UINT_MAX (default) pid=%d\n", __func__, current->pid);
	return UINT_MAX;
}
EXPORT_SYMBOL_GPL(nd_cmd_out_size);

void wait_nvdimm_bus_probe_idle(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);

	do {
		if (nvdimm_bus->probe_active == 0)
			break;
		nvdimm_bus_unlock(dev);
		device_unlock(dev);
		wait_event(nvdimm_bus->wait,
				nvdimm_bus->probe_active == 0);
		device_lock(dev);
		nvdimm_bus_lock(dev);
	} while (true);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static int nd_pmem_forget_poison_check(struct device *dev, void *data)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, data=%p pid=%d\n", __func__, dev, data, current->pid);
	struct nd_cmd_clear_error *clear_err =
		(struct nd_cmd_clear_error *)data;
	struct nd_btt *nd_btt = is_nd_btt(dev) ? to_nd_btt(dev) : NULL;
	struct nd_pfn *nd_pfn = is_nd_pfn(dev) ? to_nd_pfn(dev) : NULL;
	struct nd_dax *nd_dax = is_nd_dax(dev) ? to_nd_dax(dev) : NULL;
	struct nd_namespace_common *ndns = NULL;
	struct nd_namespace_io *nsio;
	resource_size_t offset = 0, end_trunc = 0, start, end, pstart, pend;

	if (nd_dax || !dev->driver) {
		printf(KERN_INFO "%s: EXIT: 0 (nd_dax or !dev->driver) pid=%d\n", __func__, current->pid);
		return 0;
	}

	start = clear_err->address;
	end = clear_err->address + clear_err->cleared - 1;

	if (nd_btt || nd_pfn || nd_dax) {
		if (nd_btt)
			ndns = nd_btt->ndns;
		else if (nd_pfn)
			ndns = nd_pfn->ndns;
		else if (nd_dax)
			ndns = nd_dax->nd_pfn.ndns;

		if (!ndns) {
			printf(KERN_INFO "%s: EXIT: 0 (!ndns) pid=%d\n", __func__, current->pid);
			return 0;
		}
	} else
		ndns = to_ndns(dev);

	nsio = to_nd_namespace_io(&ndns->dev);
	pstart = nsio->res.start + offset;
	pend = nsio->res.end - end_trunc;

	if ((pstart >= start) && (pend <= end)) {
		printf(KERN_INFO "%s: EXIT: -EBUSY (overlap) pid=%d\n", __func__, current->pid);
		return -EBUSY;
	}

	printf(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;
}

static int nd_ns_forget_poison_check(struct device *dev, void *data)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, data=%p pid=%d\n", __func__, dev, data, current->pid);
	int ret = device_for_each_child(dev, data, nd_pmem_forget_poison_check);
	printf(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

/* set_config requires an idle interleave set */
static int nd_cmd_clear_to_send(struct nvdimm_bus *nvdimm_bus,
		struct nvdimm *nvdimm, unsigned int cmd, void *data)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p, nvdimm=%p, cmd=%u, data=%p pid=%d\n", __func__, nvdimm_bus, nvdimm, cmd, data, current->pid);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;

	if (nd_desc->clear_to_send) {
		int rc = nd_desc->clear_to_send(nd_desc, nvdimm, cmd, data);

		if (rc) {
			printf(KERN_INFO "%s: EXIT: rc=%d (clear_to_send) pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
	}

	if (!nvdimm && cmd == ND_CMD_CLEAR_ERROR) {
		int ret = device_for_each_child(&nvdimm_bus->dev, data,
				nd_ns_forget_poison_check);
		printf(KERN_INFO "%s: EXIT: ret=%d (CLEAR_ERROR) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	if (!nvdimm || cmd != ND_CMD_SET_CONFIG_DATA) {
		printf(KERN_INFO "%s: EXIT: 0 (not SET_CONFIG_DATA) pid=%d\n", __func__, current->pid);
		return 0;
	}

	wait_nvdimm_bus_probe_idle(&nvdimm_bus->dev);
	if (atomic_read(&nvdimm->busy)) {
		printf(KERN_INFO "%s: EXIT: -EBUSY (busy) pid=%d\n", __func__, current->pid);
		return -EBUSY;
	}
	printf(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;
}

enum nd_ioctl_mode {
	BUS_IOCTL,
	DIMM_IOCTL,
};

static int match_dimm(struct device *dev, const void *data)
{
	long id = (long) data;

	if (is_nvdimm(dev)) {
		struct nvdimm *nvdimm = to_nvdimm(dev);

		return nvdimm->id == id;
	}

	return 0;
}

static long nd_ioctl(struct file *file, unsigned int cmd, unsigned long arg,
		enum nd_ioctl_mode mode)
{
	printf(KERN_INFO "%s: ENTRY: file=%p, cmd=0x%x, arg=%lx, mode=%d pid=%d\n", __func__, file, cmd, arg, mode, current->pid);
	struct nvdimm_bus *nvdimm_bus, *found = NULL;
	long id = (long) file->private_data;
	struct nvdimm *nvdimm = NULL;
	int rc, ro;

	ro = ((file->f_flags & O_ACCMODE) == O_RDONLY);
	mutex_lock(&nvdimm_bus_list_mutex);
	list_for_each_entry(nvdimm_bus, &nvdimm_bus_list, list) {
		if (mode == DIMM_IOCTL) {
			struct device *dev;

			dev = device_find_child(&nvdimm_bus->dev,
					file->private_data, match_dimm);
			if (!dev)
				continue;
			nvdimm = to_nvdimm(dev);
			found = nvdimm_bus;
		} else if (nvdimm_bus->id == id) {
			found = nvdimm_bus;
		}

		if (found) {
			atomic_inc(&nvdimm_bus->ioctl_active);
			break;
		}
	}
	mutex_unlock(&nvdimm_bus_list_mutex);

	if (!found)
		return -ENXIO;

	nvdimm_bus = found;
	rc = __nd_ioctl(nvdimm_bus, nvdimm, ro, cmd, arg);

	if (nvdimm)
		put_device(&nvdimm->dev);
	if (atomic_dec_and_test(&nvdimm_bus->ioctl_active))
		wake_up(&nvdimm_bus->wait);

	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static long bus_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return nd_ioctl(file, cmd, arg, BUS_IOCTL);
}

static long dimm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return nd_ioctl(file, cmd, arg, DIMM_IOCTL);
}

static int nd_open(struct inode *inode, struct file *file)
{
	long minor = iminor(inode);

	file->private_data = (void *) minor;
	return 0;
}

static const struct file_operations nvdimm_bus_fops = {
	.owner = THIS_MODULE,
	.open = nd_open,
	.unlocked_ioctl = bus_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static const struct file_operations nvdimm_fops = {
	.owner = THIS_MODULE,
	.open = nd_open,
	.unlocked_ioctl = dimm_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

int __init nvdimm_bus_init(void)
{
	printf(KERN_INFO "%s: ENTRY pid=%d\n", __func__, current->pid);
	int rc;

	rc = bus_register(&nvdimm_bus_type);
	if (rc)
		return rc;

	rc = register_chrdev(0, "ndctl", &nvdimm_bus_fops);
	if (rc < 0)
		goto err_bus_chrdev;
	nvdimm_bus_major = rc;

	rc = register_chrdev(0, "dimmctl", &nvdimm_fops);
	if (rc < 0)
		goto err_dimm_chrdev;
	nvdimm_major = rc;

	rc = class_register(&nd_class);
	if (rc)
		goto err_class;

	rc = driver_register(&nd_bus_driver.drv);
	if (rc)
		goto err_nd_bus;

	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return 0;

 err_nd_bus:
	class_unregister(&nd_class);
 err_class:
	unregister_chrdev(nvdimm_major, "dimmctl");
 err_dimm_chrdev:
	unregister_chrdev(nvdimm_bus_major, "ndctl");
 err_bus_chrdev:
	bus_unregister(&nvdimm_bus_type);

	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

void nvdimm_bus_exit(void)
{
	printf(KERN_INFO "%s: ENTRY pid=%d\n", __func__, current->pid);
	driver_unregister(&nd_bus_driver.drv);
	class_unregister(&nd_class);
	unregister_chrdev(nvdimm_bus_major, "ndctl");
	unregister_chrdev(nvdimm_major, "dimmctl");
	bus_unregister(&nvdimm_bus_type);
	ida_destroy(&nd_ida);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
