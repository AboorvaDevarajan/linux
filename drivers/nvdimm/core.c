// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 */
#include <linux/libnvdimm.h>
#include <linux/suspend.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/ctype.h>
#include <linux/ndctl.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include "nd-core.h"
#include "nd.h"

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

LIST_HEAD(nvdimm_bus_list);
DEFINE_MUTEX(nvdimm_bus_list_mutex);

void nvdimm_bus_lock(struct device *dev)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);

	if (!nvdimm_bus)
		DBG_EXIT("no nvdimm_bus");
		return;
	mutex_lock(&nvdimm_bus->reconfig_mutex);
	DBG_EXIT("");
}
EXPORT_SYMBOL(nvdimm_bus_lock);

void nvdimm_bus_unlock(struct device *dev)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);

	if (!nvdimm_bus)
		DBG_EXIT("no nvdimm_bus");
		return;
	mutex_unlock(&nvdimm_bus->reconfig_mutex);
	DBG_EXIT("");
}
EXPORT_SYMBOL(nvdimm_bus_unlock);

bool is_nvdimm_bus_locked(struct device *dev)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);

	if (!nvdimm_bus)
		DBG_EXIT("no nvdimm_bus, returning false");
		return false;
	DBG_EXIT("locked=%d", mutex_is_locked(&nvdimm_bus->reconfig_mutex));
	return mutex_is_locked(&nvdimm_bus->reconfig_mutex);
}
EXPORT_SYMBOL(is_nvdimm_bus_locked);

struct nvdimm_map {
	struct nvdimm_bus *nvdimm_bus;
	struct list_head list;
	resource_size_t offset;
	unsigned long flags;
	size_t size;
	union {
		void *mem;
		void __iomem *iomem;
	};
	struct kref kref;
};

static struct nvdimm_map *find_nvdimm_map(struct device *dev,
		resource_size_t offset)
{
	DBG_ENTRY("offset=%pa", &offset);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
	struct nvdimm_map *nvdimm_map;

	list_for_each_entry(nvdimm_map, &nvdimm_bus->mapping_list, list)
		if (nvdimm_map->offset == offset) {
			DBG_EXIT("found nvdimm_map=%p", nvdimm_map);
			return nvdimm_map;
		}
	DBG_EXIT("not found");
	return NULL;
}

static struct nvdimm_map *alloc_nvdimm_map(struct device *dev,
		resource_size_t offset, size_t size, unsigned long flags)
{
	DBG_ENTRY("offset=%pa, size=%zu, flags=%lx", &offset, size, flags);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
	struct nvdimm_map *nvdimm_map;

	nvdimm_map = kzalloc(sizeof(*nvdimm_map), GFP_KERNEL);
	if (!nvdimm_map)
		DBG_EXIT("kzalloc failed");
		return NULL;

	INIT_LIST_HEAD(&nvdimm_map->list);
	nvdimm_map->nvdimm_bus = nvdimm_bus;
	nvdimm_map->offset = offset;
	nvdimm_map->flags = flags;
	nvdimm_map->size = size;
	kref_init(&nvdimm_map->kref);

	if (!request_mem_region(offset, size, dev_name(&nvdimm_bus->dev))) {
		dev_err(&nvdimm_bus->dev, "failed to request %pa + %zd for %s\n",
				&offset, size, dev_name(dev));
		DBG_EXIT("request_mem_region failed");
		goto err_request_region;
	}

	if (flags)
		nvdimm_map->mem = memremap(offset, size, flags);
	else
		nvdimm_map->iomem = ioremap(offset, size);

	if (!nvdimm_map->mem)
		DBG_EXIT("memremap/ioremap failed");
		goto err_map;

	dev_WARN_ONCE(dev, !is_nvdimm_bus_locked(dev), "%s: bus unlocked!",
			__func__);
	list_add(&nvdimm_map->list, &nvdimm_bus->mapping_list);
	DBG_EXIT("nvdimm_map=%p", nvdimm_map);
	return nvdimm_map;

 err_map:
	release_mem_region(offset, size);
 err_request_region:
	kfree(nvdimm_map);
	DBG_EXIT("error path");
	return NULL;
}

static void nvdimm_map_release(struct kref *kref)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus;
	struct nvdimm_map *nvdimm_map;

	nvdimm_map = container_of(kref, struct nvdimm_map, kref);
	nvdimm_bus = nvdimm_map->nvdimm_bus;

	dev_dbg(&nvdimm_bus->dev, "%pa\n", &nvdimm_map->offset);
	list_del(&nvdimm_map->list);
	if (nvdimm_map->flags)
		memunmap(nvdimm_map->mem);
	else
		iounmap(nvdimm_map->iomem);
	release_mem_region(nvdimm_map->offset, nvdimm_map->size);
	kfree(nvdimm_map);
	DBG_EXIT("");
}

static void nvdimm_map_put(void *data)
{
	DBG_ENTRY("");
	struct nvdimm_map *nvdimm_map = data;
	struct nvdimm_bus *nvdimm_bus = nvdimm_map->nvdimm_bus;

	nvdimm_bus_lock(&nvdimm_bus->dev);
	kref_put(&nvdimm_map->kref, nvdimm_map_release);
	nvdimm_bus_unlock(&nvdimm_bus->dev);
	DBG_EXIT("");
}

/**
 * devm_nvdimm_memremap - map a resource that is shared across regions
 * @dev: device that will own a reference to the shared mapping
 * @offset: physical base address of the mapping
 * @size: mapping size
 * @flags: memremap flags, or, if zero, perform an ioremap instead
 */
void *devm_nvdimm_memremap(struct device *dev, resource_size_t offset,
		size_t size, unsigned long flags)
{
	DBG_ENTRY("offset=%pa, size=%zu, flags=%lx", &offset, size, flags);
	struct nvdimm_map *nvdimm_map;

	nvdimm_bus_lock(dev);
	nvdimm_map = find_nvdimm_map(dev, offset);
	if (!nvdimm_map)
		nvdimm_map = alloc_nvdimm_map(dev, offset, size, flags);
	else
		kref_get(&nvdimm_map->kref);
	nvdimm_bus_unlock(dev);

	if (!nvdimm_map)
		DBG_EXIT("nvdimm_map allocation failed");
		return NULL;

	if (devm_add_action_or_reset(dev, nvdimm_map_put, nvdimm_map))
		DBG_EXIT("devm_add_action_or_reset failed");
		return NULL;

	DBG_EXIT("mem=%p", nvdimm_map->mem);
	return nvdimm_map->mem;
}
EXPORT_SYMBOL_GPL(devm_nvdimm_memremap);

u64 nd_fletcher64(void *addr, size_t len, bool le)
{
	DBG_ENTRY("len=%zu, le=%d", len, le);
	u32 *buf = addr;
	u32 lo32 = 0;
	u64 hi32 = 0;
	int i;

	for (i = 0; i < len / sizeof(u32); i++) {
		lo32 += le ? le32_to_cpu((__le32) buf[i]) : buf[i];
		hi32 += lo32;
	}

	DBG_EXIT("fletcher64=0x%llx", (hi32 << 32 | lo32));
	return hi32 << 32 | lo32;
}
EXPORT_SYMBOL_GPL(nd_fletcher64);

struct nvdimm_bus_descriptor *to_nd_desc(struct nvdimm_bus *nvdimm_bus)
{
	DBG_ENTRY("");
	/* struct nvdimm_bus definition is private to libnvdimm */
	DBG_EXIT("nd_desc=%p", nvdimm_bus->nd_desc);
	return nvdimm_bus->nd_desc;
}
EXPORT_SYMBOL_GPL(to_nd_desc);

struct device *to_nvdimm_bus_dev(struct nvdimm_bus *nvdimm_bus)
{
	DBG_ENTRY("");
	/* struct nvdimm_bus definition is private to libnvdimm */
	DBG_EXIT("dev=%p", &nvdimm_bus->dev);
	return &nvdimm_bus->dev;
}
EXPORT_SYMBOL_GPL(to_nvdimm_bus_dev);

/**
 * nd_uuid_store: common implementation for writing 'uuid' sysfs attributes
 * @dev: container device for the uuid property
 * @uuid_out: uuid buffer to replace
 * @buf: raw sysfs buffer to parse
 *
 * Enforce that uuids can only be changed while the device is disabled
 * (driver detached)
 * LOCKING: expects device_lock() is held on entry
 */
int nd_uuid_store(struct device *dev, uuid_t **uuid_out, const char *buf,
		size_t len)
{
	DBG_ENTRY("");
	uuid_t uuid;
	int rc;

	if (dev->driver)
		DBG_EXIT("device busy");
		return -EBUSY;

	rc = uuid_parse(buf, &uuid);
	if (rc)
		DBG_EXIT("uuid_parse failed");
		return rc;

	kfree(*uuid_out);
	*uuid_out = kmemdup(&uuid, sizeof(uuid), GFP_KERNEL);
	if (!(*uuid_out))
		DBG_EXIT("kmemdup failed");
		return -ENOMEM;

	DBG_EXIT("success");
	return 0;
}

ssize_t nd_size_select_show(unsigned long current_size,
		const unsigned long *supported, char *buf)
{
	DBG_ENTRY("current_size=%lu", current_size);
	ssize_t len = 0;
	int i;

	for (i = 0; supported[i]; i++)
		if (current_size == supported[i])
			len += sprintf(buf + len, "[%ld] ", supported[i]);
		else
			len += sprintf(buf + len, "%ld ", supported[i]);
	len += sprintf(buf + len, "\n");
	DBG_EXIT("len=%zd", len);
	return len;
}

ssize_t nd_size_select_store(struct device *dev, const char *buf,
		unsigned long *current_size, const unsigned long *supported)
{
	DBG_ENTRY("");
	unsigned long lbasize;
	int rc, i;

	if (dev->driver)
		DBG_EXIT("device busy");
		return -EBUSY;

	rc = kstrtoul(buf, 0, &lbasize);
	if (rc)
		DBG_EXIT("kstrtoul failed");
		return rc;

	for (i = 0; supported[i]; i++)
		if (lbasize == supported[i])
			break;

	if (supported[i]) {
		*current_size = lbasize;
		DBG_EXIT("success");
		return 0;
	} else {
		DBG_EXIT("invalid value");
		return -EINVAL;
	}
}

static ssize_t commands_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	int cmd, len = 0;
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;

	for_each_set_bit(cmd, &nd_desc->cmd_mask, BITS_PER_LONG)
		len += sprintf(buf + len, "%s ", nvdimm_bus_cmd_name(cmd));
	len += sprintf(buf + len, "\n");
	DBG_EXIT("len=%d", len);
	return len;
}
static DEVICE_ATTR_RO(commands);

static const char *nvdimm_bus_provider(struct nvdimm_bus *nvdimm_bus)
{
	DBG_ENTRY("");
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	struct device *parent = nvdimm_bus->dev.parent;

	if (nd_desc->provider_name)
		DBG_EXIT("provider_name=%s", nd_desc->provider_name);
		return nd_desc->provider_name;
	else if (parent)
		DBG_EXIT("parent name=%s", dev_name(parent));
		return dev_name(parent);
	else
		DBG_EXIT("unknown");
		return "unknown";
}

static ssize_t provider_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);

	DBG_EXIT("");
	return sprintf(buf, "%s\n", nvdimm_bus_provider(nvdimm_bus));
}
static DEVICE_ATTR_RO(provider);

static int flush_namespaces(struct device *dev, void *data)
{
	DBG_ENTRY("");
	device_lock(dev);
	device_unlock(dev);
	DBG_EXIT("");
	return 0;
}

static int flush_regions_dimms(struct device *dev, void *data)
{
	DBG_ENTRY("");
	device_lock(dev);
	device_unlock(dev);
	device_for_each_child(dev, NULL, flush_namespaces);
	DBG_EXIT("");
	return 0;
}

static ssize_t wait_probe_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	int rc;

	if (nd_desc->flush_probe) {
		rc = nd_desc->flush_probe(nd_desc);
		if (rc)
			DBG_EXIT("flush_probe failed");
			return rc;
	}
	nd_synchronize();
	device_for_each_child(dev, NULL, flush_regions_dimms);
	DBG_EXIT("");
	return sprintf(buf, "1\n");
}
static DEVICE_ATTR_RO(wait_probe);

static struct attribute *nvdimm_bus_attributes[] = {
	&dev_attr_commands.attr,
	&dev_attr_wait_probe.attr,
	&dev_attr_provider.attr,
	NULL,
};

static const struct attribute_group nvdimm_bus_attribute_group = {
	.attrs = nvdimm_bus_attributes,
};

static ssize_t capability_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	enum nvdimm_fwa_capability cap;

	if (!nd_desc->fw_ops)
		DBG_EXIT("no fw_ops");
		return -EOPNOTSUPP;

	cap = nd_desc->fw_ops->capability(nd_desc);

	switch (cap) {
	case NVDIMM_FWA_CAP_QUIESCE:
		DBG_EXIT("quiesce");
		return sprintf(buf, "quiesce\n");
	case NVDIMM_FWA_CAP_LIVE:
		DBG_EXIT("live");
		return sprintf(buf, "live\n");
	default:
		DBG_EXIT("unsupported");
		return -EOPNOTSUPP;
	}
}

static DEVICE_ATTR_RO(capability);

static ssize_t activate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	enum nvdimm_fwa_capability cap;
	enum nvdimm_fwa_state state;

	if (!nd_desc->fw_ops)
		DBG_EXIT("no fw_ops");
		return -EOPNOTSUPP;

	cap = nd_desc->fw_ops->capability(nd_desc);
	state = nd_desc->fw_ops->activate_state(nd_desc);

	if (cap < NVDIMM_FWA_CAP_QUIESCE)
		DBG_EXIT("cap < QUIESCE");
		return -EOPNOTSUPP;

	switch (state) {
	case NVDIMM_FWA_IDLE:
		DBG_EXIT("idle");
		return sprintf(buf, "idle\n");
	case NVDIMM_FWA_BUSY:
		DBG_EXIT("busy");
		return sprintf(buf, "busy\n");
	case NVDIMM_FWA_ARMED:
		DBG_EXIT("armed");
		return sprintf(buf, "armed\n");
	case NVDIMM_FWA_ARM_OVERFLOW:
		DBG_EXIT("overflow");
		return sprintf(buf, "overflow\n");
	default:
		DBG_EXIT("ENXIO");
		return -ENXIO;
	}
}

static int exec_firmware_activate(void *data)
{
	DBG_ENTRY("");
	struct nvdimm_bus_descriptor *nd_desc = data;

	DBG_EXIT("");
	return nd_desc->fw_ops->activate(nd_desc);
}

static ssize_t activate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	DBG_ENTRY("");
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	enum nvdimm_fwa_state state;
	bool quiesce;
	ssize_t rc;

	if (!nd_desc->fw_ops)
		DBG_EXIT("no fw_ops");
		return -EOPNOTSUPP;

	if (sysfs_streq(buf, "live"))
		quiesce = false;
	else if (sysfs_streq(buf, "quiesce"))
		quiesce = true;
	else
		DBG_EXIT("EINVAL");
		return -EINVAL;

	state = nd_desc->fw_ops->activate_state(nd_desc);

	switch (state) {
	case NVDIMM_FWA_BUSY:
		DBG_EXIT("EBUSY");
		rc = -EBUSY;
		break;
	case NVDIMM_FWA_ARMED:
	case NVDIMM_FWA_ARM_OVERFLOW:
		if (quiesce)
			rc = hibernate_quiet_exec(exec_firmware_activate, nd_desc);
		else
			rc = nd_desc->fw_ops->activate(nd_desc);
		DBG_MID("activate rc=%zd", rc);
		break;
	case NVDIMM_FWA_IDLE:
	default:
		DBG_EXIT("ENXIO");
		rc = -ENXIO;
	}

	if (rc == 0)
		rc = len;
	DBG_EXIT("rc=%zd", rc);
	return rc;
}

static DEVICE_ATTR_ADMIN_RW(activate);

static umode_t nvdimm_bus_firmware_visible(struct kobject *kobj, struct attribute *a, int n)
{
	DBG_ENTRY("");
	struct device *dev = container_of(kobj, typeof(*dev), kobj);
	struct nvdimm_bus *nvdimm_bus = to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	enum nvdimm_fwa_capability cap;

	if (!nd_desc->fw_ops)
		DBG_EXIT("no fw_ops");
		return 0;

	cap = nd_desc->fw_ops->capability(nd_desc);
	if (cap < NVDIMM_FWA_CAP_QUIESCE)
		DBG_EXIT("cap < QUIESCE");
		return 0;

	DBG_EXIT("mode=%o", a->mode);
	return a->mode;
}
static struct attribute *nvdimm_bus_firmware_attributes[] = {
	&dev_attr_activate.attr,
	&dev_attr_capability.attr,
	NULL,
};

static const struct attribute_group nvdimm_bus_firmware_attribute_group = {
	.name = "firmware",
	.attrs = nvdimm_bus_firmware_attributes,
	.is_visible = nvdimm_bus_firmware_visible,
};

const struct attribute_group *nvdimm_bus_attribute_groups[] = {
	&nvdimm_bus_attribute_group,
	&nvdimm_bus_firmware_attribute_group,
	NULL,
};

int nvdimm_bus_add_badrange(struct nvdimm_bus *nvdimm_bus, u64 addr, u64 length)
{
	DBG_ENTRY("addr=0x%llx, length=0x%llx", addr, length);
	int rc = badrange_add(&nvdimm_bus->badrange, addr, length);
	DBG_EXIT("rc=%d", rc);
	return rc;
}
EXPORT_SYMBOL_GPL(nvdimm_bus_add_badrange);

static __init int libnvdimm_init(void)
{
	DBG_ENTRY("");
	int rc;

	rc = nvdimm_bus_init();
	if (rc)
		DBG_EXIT("nvdimm_bus_init failed");
		return rc;
	rc = nvdimm_init();
	if (rc)
		DBG_EXIT("nvdimm_init failed");
		goto err_dimm;
	rc = nd_region_init();
	if (rc)
		DBG_EXIT("nd_region_init failed");
		goto err_region;

	nd_label_init();

	DBG_EXIT("success");
	return 0;
 err_region:
	nvdimm_exit();
 err_dimm:
	nvdimm_bus_exit();
	DBG_EXIT("error path rc=%d", rc);
	return rc;
}

static __exit void libnvdimm_exit(void)
{
	DBG_ENTRY("");
	WARN_ON(!list_empty(&nvdimm_bus_list));
	nd_region_exit();
	nvdimm_exit();
	nvdimm_bus_exit();
	nvdimm_devs_exit();
	DBG_EXIT("");
}

MODULE_DESCRIPTION("NVDIMM (Non-Volatile Memory Device) core");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");
subsys_initcall(libnvdimm_init);
module_exit(libnvdimm_exit);
