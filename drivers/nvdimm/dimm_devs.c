// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/ndctl.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "nd-core.h"
#include "label.h"
#include "pmem.h"
#include "nd.h"

static DEFINE_IDA(dimm_ida);

/*
 * Retrieve bus and dimm handle and return if this bus supports
 * get_config_data commands
 */
int nvdimm_check_config_data(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	if (!nvdimm->cmd_mask ||
	    !test_bit(ND_CMD_GET_CONFIG_DATA, &nvdimm->cmd_mask)) {
		if (test_bit(NDD_LABELING, &nvdimm->flags)) {
			printf(KERN_INFO "%s: EXIT: -ENXIO (labeling) pid=%d\n", __func__, current->pid);
			return -ENXIO;
		} else {
			printf(KERN_INFO "%s: EXIT: -ENOTTY (no cmd_mask) pid=%d\n", __func__, current->pid);
			return -ENOTTY;
		}
	}
	printf(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;
}

static int validate_dimm(struct nvdimm_drvdata *ndd)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	int rc;

	if (!ndd) {
		printf(KERN_INFO "%s: EXIT: -EINVAL (ndd is NULL) pid=%d\n", __func__, current->pid);
		return -EINVAL;
	}

	rc = nvdimm_check_config_data(ndd->dev);
	if (rc)
		dev_dbg(ndd->dev, "%ps: %s error: %d\n",
				__builtin_return_address(0), __func__, rc);
	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

/**
 * nvdimm_init_nsarea - determine the geometry of a dimm's namespace area
 * @ndd: dimm to initialize
 *
 * Returns: %0 if the area is already valid, -errno on error
 */
int nvdimm_init_nsarea(struct nvdimm_drvdata *ndd)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	struct nd_cmd_get_config_size *cmd = &ndd->nsarea;
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	struct nvdimm_bus_descriptor *nd_desc;
	int rc = validate_dimm(ndd);
	int cmd_rc = 0;

	if (rc) {
		printf(KERN_INFO "%s: EXIT: rc=%d (validate_dimm failed) pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	if (cmd->config_size) {
		printf(KERN_INFO "%s: EXIT: 0 (already valid) pid=%d\n", __func__, current->pid);
		return 0; /* already valid */
	}

	memset(cmd, 0, sizeof(*cmd));
	nd_desc = nvdimm_bus->nd_desc;
	rc = nd_desc->ndctl(nd_desc, to_nvdimm(ndd->dev), ND_CMD_GET_CONFIG_SIZE, cmd, sizeof(*cmd), &cmd_rc);
	if (rc < 0) {
		printf(KERN_INFO "%s: EXIT: rc=%d (ndctl failed) pid=%d\n", __func__, rc, current->pid);
		return rc;
	}
	printf(KERN_INFO "%s: EXIT: cmd_rc=%d pid=%d\n", __func__, cmd_rc, current->pid);
	return cmd_rc;
}

int nvdimm_get_config_data(struct nvdimm_drvdata *ndd, void *buf,
			   size_t offset, size_t len)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p, buf=%p, offset=%zu, len=%zu pid=%d\n", __func__, ndd, buf, offset, len, current->pid);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	int rc = validate_dimm(ndd), cmd_rc = 0;
	struct nd_cmd_get_config_data_hdr *cmd;
	size_t max_cmd_size, buf_offset;

	if (rc) {
		printf(KERN_INFO "%s: EXIT: rc=%d (validate_dimm failed) pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	if (offset + len > ndd->nsarea.config_size) {
		printf(KERN_INFO "%s: EXIT: -ENXIO (out of bounds) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	max_cmd_size = min_t(u32, len, ndd->nsarea.max_xfer);
	cmd = kvzalloc(max_cmd_size + sizeof(*cmd), GFP_KERNEL);
	if (!cmd) {
		printf(KERN_INFO "%s: EXIT: -ENOMEM (kvzalloc failed) pid=%d\n", __func__, current->pid);
		return -ENOMEM;
	}

	for (buf_offset = 0; len; len -= cmd->in_length, buf_offset += cmd->in_length) {
		size_t cmd_size;

		cmd->in_offset = offset + buf_offset;
		cmd->in_length = min(max_cmd_size, len);

		cmd_size = sizeof(*cmd) + cmd->in_length;

		rc = nd_desc->ndctl(nd_desc, to_nvdimm(ndd->dev), ND_CMD_GET_CONFIG_DATA, cmd, cmd_size, &cmd_rc);
		if (rc < 0) {
			printf(KERN_INFO "%s: EXIT: rc=%d (ndctl failed) pid=%d\n", __func__, rc, current->pid);
			break;
		}
		if (cmd_rc < 0) {
			rc = cmd_rc;
			printf(KERN_INFO "%s: EXIT: cmd_rc=%d (cmd_rc failed) pid=%d\n", __func__, cmd_rc, current->pid);
			break;
		}

		memcpy(buf + buf_offset, cmd->out_buf, cmd->in_length);
	}
	kvfree(cmd);

	return rc;
}

int nvdimm_set_config_data(struct nvdimm_drvdata *ndd, size_t offset,
		void *buf, size_t len)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p, offset=%zu, buf=%p, len=%zu pid=%d\n", __func__, ndd, offset, buf, len, current->pid);
	size_t max_cmd_size, buf_offset;
	struct nd_cmd_set_config_hdr *cmd;
	int rc = validate_dimm(ndd), cmd_rc = 0;
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;

	if (rc) {
		printf(KERN_INFO "%s: EXIT: rc=%d (validate_dimm failed) pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	if (offset + len > ndd->nsarea.config_size) {
		printf(KERN_INFO "%s: EXIT: -ENXIO (out of bounds) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	max_cmd_size = min_t(u32, len, ndd->nsarea.max_xfer);
	cmd = kvzalloc(max_cmd_size + sizeof(*cmd) + sizeof(u32), GFP_KERNEL);
	if (!cmd) {
		printf(KERN_INFO "%s: EXIT: -ENOMEM (kvzalloc failed) pid=%d\n", __func__, current->pid);
		return -ENOMEM;
	}

	for (buf_offset = 0; len; len -= cmd->in_length, buf_offset += cmd->in_length) {
		size_t cmd_size;

		cmd->in_offset = offset + buf_offset;
		cmd->in_length = min(max_cmd_size, len);
		memcpy(cmd->in_buf, buf + buf_offset, cmd->in_length);

		cmd_size = sizeof(*cmd) + cmd->in_length + sizeof(u32);

		rc = nd_desc->ndctl(nd_desc, to_nvdimm(ndd->dev), ND_CMD_SET_CONFIG_DATA, cmd, cmd_size, &cmd_rc);
		if (rc < 0) {
			printf(KERN_INFO "%s: EXIT: rc=%d (ndctl failed) pid=%d\n", __func__, rc, current->pid);
			break;
		}
		if (cmd_rc < 0) {
			rc = cmd_rc;
			printf(KERN_INFO "%s: EXIT: cmd_rc=%d (cmd_rc failed) pid=%d\n", __func__, cmd_rc, current->pid);
			break;
		}
	}
	kvfree(cmd);

	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

void nvdimm_set_labeling(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	set_bit(NDD_LABELING, &nvdimm->flags);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

void nvdimm_set_locked(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	set_bit(NDD_LOCKED, &nvdimm->flags);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

void nvdimm_clear_locked(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	clear_bit(NDD_LOCKED, &nvdimm->flags);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nvdimm_release(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	ida_free(&dimm_ida, nvdimm->id);
	kfree(nvdimm);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

struct nvdimm *to_nvdimm(struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nvdimm *nvdimm = container_of(dev, struct nvdimm, dev);

	WARN_ON(!is_nvdimm(dev));
	printf(KERN_INFO "%s: EXIT: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	return nvdimm;
}
EXPORT_SYMBOL_GPL(to_nvdimm);

struct nvdimm_drvdata *to_ndd(struct nd_mapping *nd_mapping)
{
	printf(KERN_INFO "%s: ENTRY: nd_mapping=%p pid=%d\n", __func__, nd_mapping, current->pid);
	struct nvdimm *nvdimm = nd_mapping->nvdimm;

	WARN_ON_ONCE(!is_nvdimm_bus_locked(&nvdimm->dev));

	struct nvdimm_drvdata *ret = dev_get_drvdata(&nvdimm->dev);
	printf(KERN_INFO "%s: EXIT: ret=%p pid=%d\n", __func__, ret, current->pid);
	return ret;
}
EXPORT_SYMBOL(to_ndd);

void nvdimm_drvdata_release(struct kref *kref)
{
	printf(KERN_INFO "%s: ENTRY: kref=%p pid=%d\n", __func__, kref, current->pid);
	struct nvdimm_drvdata *ndd = container_of(kref, typeof(*ndd), kref);
	struct device *dev = ndd->dev;
	struct resource *res, *_r;

	dev_dbg(dev, "trace\n");
	nvdimm_bus_lock(dev);
	for_each_dpa_resource_safe(ndd, res, _r)
		nvdimm_free_dpa(ndd, res);
	nvdimm_bus_unlock(dev);

	kvfree(ndd->data);
	kfree(ndd);
	put_device(dev);
	printf(KERN_INFO "%s: EXIT: dev=%p, ndd=%p pid=%d\n", __func__, dev, ndd, current->pid);
}

void get_ndd(struct nvdimm_drvdata *ndd)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	kref_get(&ndd->kref);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

void put_ndd(struct nvdimm_drvdata *ndd)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	if (ndd)
		kref_put(&ndd->kref, nvdimm_drvdata_release);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

const char *nvdimm_name(struct nvdimm *nvdimm)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	const char *ret = dev_name(&nvdimm->dev);
	printf(KERN_INFO "%s: EXIT: ret=%s pid=%d\n", __func__, ret, current->pid);
	return ret;
}
EXPORT_SYMBOL_GPL(nvdimm_name);

struct kobject *nvdimm_kobj(struct nvdimm *nvdimm)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	struct kobject *ret = &nvdimm->dev.kobj;
	printf(KERN_INFO "%s: EXIT: ret=%p pid=%d\n", __func__, ret, current->pid);
	return ret;
}
EXPORT_SYMBOL_GPL(nvdimm_kobj);

unsigned long nvdimm_cmd_mask(struct nvdimm *nvdimm)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	unsigned long ret = nvdimm->cmd_mask;
	printf(KERN_INFO "%s: EXIT: ret=%lu pid=%d\n", __func__, ret, current->pid);
	return ret;
}
EXPORT_SYMBOL_GPL(nvdimm_cmd_mask);

void *nvdimm_provider_data(struct nvdimm *nvdimm)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	if (nvdimm) {
		printf(KERN_INFO "%s: EXIT: provider_data=%p pid=%d\n", __func__, nvdimm->provider_data, current->pid);
		return nvdimm->provider_data;
	}
	printf(KERN_INFO "%s: EXIT: NULL (nvdimm is NULL) pid=%d\n", __func__, current->pid);
	return NULL;
}
EXPORT_SYMBOL_GPL(nvdimm_provider_data);

static ssize_t commands_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	int cmd, len = 0;

	if (!nvdimm->cmd_mask) {
		ssize_t ret = sprintf(buf, "\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (no cmd_mask) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	for_each_set_bit(cmd, &nvdimm->cmd_mask, BITS_PER_LONG)
		len += sprintf(buf + len, "%s ", nvdimm_cmd_name(cmd));
	len += sprintf(buf + len, "\n");
	printf(KERN_INFO "%s: EXIT: len=%d pid=%d\n", __func__, len, current->pid);
	return len;
}
static DEVICE_ATTR_RO(commands);

static ssize_t flags_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	ssize_t ret = sprintf(buf, "%s%s\n",
			test_bit(NDD_LABELING, &nvdimm->flags) ? "label " : "",
			test_bit(NDD_LOCKED, &nvdimm->flags) ? "lock " : "");
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(flags);

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	nvdimm_bus_lock(dev);
	nvdimm_bus_unlock(dev);
	ssize_t ret = sprintf(buf, "%s\n", atomic_read(&nvdimm->busy) ? "active" : "idle");
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(state);

static ssize_t __available_slots_show(struct nvdimm_drvdata *ndd, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p, buf=%p pid=%d\n", __func__, ndd, buf, current->pid);
	struct device *dev;
	ssize_t rc;
	u32 nfree;

	if (!ndd) {
		printf(KERN_INFO "%s: EXIT: -ENXIO (ndd is NULL) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	dev = ndd->dev;
	nvdimm_bus_lock(dev);
	nfree = nd_label_nfree(ndd);
	if (nfree - 1 > nfree) {
		dev_WARN_ONCE(dev, 1, "we ate our last label?\n");
		nfree = 0;
	} else
		nfree--;
	rc = sprintf(buf, "%d\n", nfree);
	nvdimm_bus_unlock(dev);
	printf(KERN_INFO "%s: EXIT: rc=%zd pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static ssize_t available_slots_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	ssize_t rc;
	device_lock(dev);
	rc = __available_slots_show(dev_get_drvdata(dev), buf);
	device_unlock(dev);
	printf(KERN_INFO "%s: EXIT: rc=%zd pid=%d\n", __func__, rc, current->pid);
	return rc;
}
static DEVICE_ATTR_RO(available_slots);

static ssize_t security_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	if (IS_ENABLED(CONFIG_NVDIMM_SECURITY_TEST))
		nvdimm->sec.flags = nvdimm_security_flags(nvdimm, NVDIMM_USER);

	if (test_bit(NVDIMM_SECURITY_OVERWRITE, &nvdimm->sec.flags)) {
		ssize_t ret = sprintf(buf, "overwrite\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (overwrite) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	if (test_bit(NVDIMM_SECURITY_DISABLED, &nvdimm->sec.flags)) {
		ssize_t ret = sprintf(buf, "disabled\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (disabled) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	if (test_bit(NVDIMM_SECURITY_UNLOCKED, &nvdimm->sec.flags)) {
		ssize_t ret = sprintf(buf, "unlocked\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (unlocked) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	if (test_bit(NVDIMM_SECURITY_LOCKED, &nvdimm->sec.flags)) {
		ssize_t ret = sprintf(buf, "locked\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (locked) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	printf(KERN_INFO "%s: EXIT: -ENOTTY (no state) pid=%d\n", __func__, current->pid);
	return -ENOTTY;
}

static ssize_t frozen_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	ssize_t ret = sprintf(buf, "%d\n", test_bit(NVDIMM_SECURITY_FROZEN, &nvdimm->sec.flags));
	printf(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}
static DEVICE_ATTR_RO(frozen);

static ssize_t security_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p, len=%zu pid=%d\n", __func__, dev, attr, buf, len, current->pid);
	ssize_t rc;
	device_lock(dev);
	nvdimm_bus_lock(dev);
	wait_nvdimm_bus_probe_idle(dev);
	rc = nvdimm_security_store(dev, buf, len);
	nvdimm_bus_unlock(dev);
	device_unlock(dev);
	printf(KERN_INFO "%s: EXIT: rc=%zd pid=%d\n", __func__, rc, current->pid);
	return rc;
}
static DEVICE_ATTR_RW(security);

static struct attribute *nvdimm_attributes[] = {
	&dev_attr_state.attr,
	&dev_attr_flags.attr,
	&dev_attr_commands.attr,
	&dev_attr_available_slots.attr,
	&dev_attr_security.attr,
	&dev_attr_frozen.attr,
	NULL,
};

static umode_t nvdimm_visible(struct kobject *kobj, struct attribute *a, int n)
{
	printf(KERN_INFO "%s: ENTRY: kobj=%p, a=%p, n=%d pid=%d\n", __func__, kobj, a, n, current->pid);
	struct device *dev = container_of(kobj, typeof(*dev), kobj);
	struct nvdimm *nvdimm = to_nvdimm(dev);

	if (a != &dev_attr_security.attr && a != &dev_attr_frozen.attr) {
		printf(KERN_INFO "%s: EXIT: mode=%o pid=%d\n", __func__, a->mode, current->pid);
		return a->mode;
	}
	if (!nvdimm->sec.flags) {
		printf(KERN_INFO "%s: EXIT: 0 (no sec.flags) pid=%d\n", __func__, current->pid);
		return 0;
	}

	if (a == &dev_attr_security.attr) {
		if (nvdimm->sec.ops->freeze || nvdimm->sec.ops->disable || nvdimm->sec.ops->change_key || nvdimm->sec.ops->erase || nvdimm->sec.ops->overwrite) {
			printf(KERN_INFO "%s: EXIT: mode=%o (security ops) pid=%d\n", __func__, a->mode, current->pid);
			return a->mode;
		}
		printf(KERN_INFO "%s: EXIT: 0444 (security read-only) pid=%d\n", __func__, current->pid);
		return 0444;
	}

	if (nvdimm->sec.ops->freeze) {
		printf(KERN_INFO "%s: EXIT: mode=%o (freeze) pid=%d\n", __func__, a->mode, current->pid);
		return a->mode;
	}
	printf(KERN_INFO "%s: EXIT: 0 (no freeze) pid=%d\n", __func__, current->pid);
	return 0;
}

static const struct attribute_group nvdimm_attribute_group = {
	.attrs = nvdimm_attributes,
	.is_visible = nvdimm_visible,
};

static ssize_t result_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	enum nvdimm_fwa_result result;

	if (!nvdimm->fw_ops) {
		printf(KERN_INFO "%s: EXIT: -EOPNOTSUPP (no fw_ops) pid=%d\n", __func__, current->pid);
		return -EOPNOTSUPP;
	}

	nvdimm_bus_lock(dev);
	result = nvdimm->fw_ops->activate_result(nvdimm);
	nvdimm_bus_unlock(dev);

	switch (result) {
	case NVDIMM_FWA_RESULT_NONE: {
		ssize_t ret = sprintf(buf, "none\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (none) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	case NVDIMM_FWA_RESULT_SUCCESS: {
		ssize_t ret = sprintf(buf, "success\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (success) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	case NVDIMM_FWA_RESULT_FAIL: {
		ssize_t ret = sprintf(buf, "fail\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (fail) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	case NVDIMM_FWA_RESULT_NOTSTAGED: {
		ssize_t ret = sprintf(buf, "not_staged\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (not_staged) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	case NVDIMM_FWA_RESULT_NEEDRESET: {
		ssize_t ret = sprintf(buf, "need_reset\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (need_reset) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	default:
		printf(KERN_INFO "%s: EXIT: -ENXIO (default) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}
}
static DEVICE_ATTR_ADMIN_RO(result);

static ssize_t activate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	enum nvdimm_fwa_state state;

	if (!nvdimm->fw_ops) {
		printf(KERN_INFO "%s: EXIT: -EOPNOTSUPP (no fw_ops) pid=%d\n", __func__, current->pid);
		return -EOPNOTSUPP;
	}

	nvdimm_bus_lock(dev);
	state = nvdimm->fw_ops->activate_state(nvdimm);
	nvdimm_bus_unlock(dev);

	switch (state) {
	case NVDIMM_FWA_IDLE: {
		ssize_t ret = sprintf(buf, "idle\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (idle) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	case NVDIMM_FWA_BUSY: {
		ssize_t ret = sprintf(buf, "busy\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (busy) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	case NVDIMM_FWA_ARMED: {
		ssize_t ret = sprintf(buf, "armed\n");
		printf(KERN_INFO "%s: EXIT: ret=%zd (armed) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	default:
		printf(KERN_INFO "%s: EXIT: -ENXIO (default) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}
}

static ssize_t activate_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t len)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p, len=%zu pid=%d\n", __func__, dev, attr, buf, len, current->pid);
	struct nvdimm *nvdimm = to_nvdimm(dev);
	enum nvdimm_fwa_trigger arg;
	int rc;

	if (!nvdimm->fw_ops) {
		printf(KERN_INFO "%s: EXIT: -EOPNOTSUPP (no fw_ops) pid=%d\n", __func__, current->pid);
		return -EOPNOTSUPP;
	}

	if (sysfs_streq(buf, "arm"))
		arg = NVDIMM_FWA_ARM;
	else if (sysfs_streq(buf, "disarm"))
		arg = NVDIMM_FWA_DISARM;
	else {
		printf(KERN_INFO "%s: EXIT: -EINVAL (invalid arg) pid=%d\n", __func__, current->pid);
		return -EINVAL;
	}

	nvdimm_bus_lock(dev);
	rc = nvdimm->fw_ops->arm(nvdimm, arg);
	nvdimm_bus_unlock(dev);

	if (rc < 0) {
		printf(KERN_INFO "%s: EXIT: rc=%d (fw_ops->arm failed) pid=%d\n", __func__, rc, current->pid);
		return rc;
	}
	printf(KERN_INFO "%s: EXIT: len=%zu pid=%d\n", __func__, len, current->pid);
	return len;
}
static DEVICE_ATTR_ADMIN_RW(activate);

static struct attribute *nvdimm_firmware_attributes[] = {
	&dev_attr_activate.attr,
	&dev_attr_result.attr,
	NULL,
};

static umode_t nvdimm_firmware_visible(struct kobject *kobj, struct attribute *a, int n)
{
	printf(KERN_INFO "%s: ENTRY: kobj=%p, a=%p, n=%d pid=%d\n", __func__, kobj, a, n, current->pid);
	struct device *dev = container_of(kobj, typeof(*dev), kobj);
	struct nvdimm_bus *nvdimm_bus = walk_to_nvdimm_bus(dev);
	struct nvdimm_bus_descriptor *nd_desc = nvdimm_bus->nd_desc;
	struct nvdimm *nvdimm = to_nvdimm(dev);
	enum nvdimm_fwa_capability cap;

	if (!nd_desc->fw_ops) {
		printf(KERN_INFO "%s: EXIT: 0 (no fw_ops) pid=%d\n", __func__, current->pid);
		return 0;
	}
	if (!nvdimm->fw_ops) {
		printf(KERN_INFO "%s: EXIT: 0 (no nvdimm fw_ops) pid=%d\n", __func__, current->pid);
		return 0;
	}

	nvdimm_bus_lock(dev);
	cap = nd_desc->fw_ops->capability(nd_desc);
	nvdimm_bus_unlock(dev);

	if (cap < NVDIMM_FWA_CAP_QUIESCE) {
		printf(KERN_INFO "%s: EXIT: 0 (cap < QUIESCE) pid=%d\n", __func__, current->pid);
		return 0;
	}

	printf(KERN_INFO "%s: EXIT: mode=%o pid=%d\n", __func__, a->mode, current->pid);
	return a->mode;
}

static const struct attribute_group nvdimm_firmware_attribute_group = {
	.name = "firmware",
	.attrs = nvdimm_firmware_attributes,
	.is_visible = nvdimm_firmware_visible,
};

static const struct attribute_group *nvdimm_attribute_groups[] = {
	&nd_device_attribute_group,
	&nvdimm_attribute_group,
	&nvdimm_firmware_attribute_group,
	NULL,
};

static const struct device_type nvdimm_device_type = {
	.name = "nvdimm",
	.release = nvdimm_release,
	.groups = nvdimm_attribute_groups,
};

bool is_nvdimm(const struct device *dev)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	bool ret = dev->type == &nvdimm_device_type;
	printf(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static struct lock_class_key nvdimm_key;

struct nvdimm *__nvdimm_create(struct nvdimm_bus *nvdimm_bus,
		void *provider_data, const struct attribute_group **groups,
		unsigned long flags, unsigned long cmd_mask, int num_flush,
		struct resource *flush_wpq, const char *dimm_id,
		const struct nvdimm_security_ops *sec_ops,
		const struct nvdimm_fw_ops *fw_ops)
{
	struct nvdimm *nvdimm = kzalloc(sizeof(*nvdimm), GFP_KERNEL);
	struct device *dev;
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p, provider_data=%p, groups=%p, flags=%lx, cmd_mask=%lx, num_flush=%d, flush_wpq=%p, dimm_id=%s, sec_ops=%p, fw_ops=%p pid=%d\n", __func__, nvdimm_bus, provider_data, groups, flags, cmd_mask, num_flush, flush_wpq, dimm_id, sec_ops, fw_ops, current->pid);
	if (!nvdimm) {
		printf(KERN_INFO "%s: EXIT: NULL (nvdimm allocation failed) pid=%d\n", __func__, current->pid);
		return NULL;
	}

	nvdimm->id = ida_alloc(&dimm_ida, GFP_KERNEL);
	if (nvdimm->id < 0) {
		kfree(nvdimm);
		printf(KERN_INFO "%s: EXIT: NULL (id allocation failed) pid=%d\n", __func__, current->pid);
		return NULL;
	}

	nvdimm->dimm_id = dimm_id;
	nvdimm->provider_data = provider_data;
	nvdimm->flags = flags;
	nvdimm->cmd_mask = cmd_mask;
	nvdimm->num_flush = num_flush;
	nvdimm->flush_wpq = flush_wpq;
	atomic_set(&nvdimm->busy, 0);
	dev = &nvdimm->dev;
	dev_set_name(dev, "nmem%d", nvdimm->id);
	dev->parent = &nvdimm_bus->dev;
	dev->type = &nvdimm_device_type;
	dev->devt = MKDEV(nvdimm_major, nvdimm->id);
	dev->groups = groups;
	nvdimm->sec.ops = sec_ops;
	nvdimm->fw_ops = fw_ops;
	nvdimm->sec.overwrite_tmo = 0;
	INIT_DELAYED_WORK(&nvdimm->dwork, nvdimm_security_overwrite_query);
	/*
	 * Security state must be initialized before device_add() for
	 * attribute visibility.
	 */
	/* get security state and extended (master) state */
	nvdimm->sec.flags = nvdimm_security_flags(nvdimm, NVDIMM_USER);
	nvdimm->sec.ext_flags = nvdimm_security_flags(nvdimm, NVDIMM_MASTER);
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &nvdimm_key);
	if (test_bit(NDD_REGISTER_SYNC, &flags))
		nd_device_register_sync(dev);
	else
		nd_device_register(dev);

	printf(KERN_INFO "%s: EXIT: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	return nvdimm;
}
EXPORT_SYMBOL_GPL(__nvdimm_create);

void nvdimm_delete(struct nvdimm *nvdimm)
{
	struct device *dev = &nvdimm->dev;
	bool dev_put = false;
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	/* We are shutting down. Make state frozen artificially. */
	nvdimm_bus_lock(dev);
	set_bit(NVDIMM_SECURITY_FROZEN, &nvdimm->sec.flags);
	if (test_and_clear_bit(NDD_WORK_PENDING, &nvdimm->flags))
		dev_put = true;
	nvdimm_bus_unlock(dev);
	cancel_delayed_work_sync(&nvdimm->dwork);
	if (dev_put)
		put_device(dev);
	nd_device_unregister(dev, ND_SYNC);
	printf(KERN_INFO "%s: EXIT: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
}
EXPORT_SYMBOL_GPL(nvdimm_delete);

static void shutdown_security_notify(void *data)
{
	struct nvdimm *nvdimm = data;

	sysfs_put(nvdimm->sec.overwrite_state);
}

int nvdimm_security_setup_events(struct device *dev)
{
	struct nvdimm *nvdimm = to_nvdimm(dev);

	if (!nvdimm->sec.flags || !nvdimm->sec.ops
			|| !nvdimm->sec.ops->overwrite)
		return 0;
	nvdimm->sec.overwrite_state = sysfs_get_dirent(dev->kobj.sd, "security");
	if (!nvdimm->sec.overwrite_state)
		return -ENOMEM;

	return devm_add_action_or_reset(dev, shutdown_security_notify, nvdimm);
}
EXPORT_SYMBOL_GPL(nvdimm_security_setup_events);

int nvdimm_in_overwrite(struct nvdimm *nvdimm)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	int ret = test_bit(NDD_SECURITY_OVERWRITE, &nvdimm->flags);
	printf(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}
EXPORT_SYMBOL_GPL(nvdimm_in_overwrite);

int nvdimm_security_freeze(struct nvdimm *nvdimm)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm=%p pid=%d\n", __func__, nvdimm, current->pid);
	int rc;

	WARN_ON_ONCE(!is_nvdimm_bus_locked(&nvdimm->dev));

	if (!nvdimm->sec.ops || !nvdimm->sec.ops->freeze) {
		printf(KERN_INFO "%s: EXIT: -EOPNOTSUPP (no freeze op) pid=%d\n", __func__, current->pid);
		return -EOPNOTSUPP;
	}

	if (!nvdimm->sec.flags) {
		printf(KERN_INFO "%s: EXIT: -EIO (no sec.flags) pid=%d\n", __func__, current->pid);
		return -EIO;
	}

	if (test_bit(NDD_SECURITY_OVERWRITE, &nvdimm->flags)) {
		dev_warn(&nvdimm->dev, "Overwrite operation in progress.\n");
		printf(KERN_INFO "%s: EXIT: -EBUSY (overwrite in progress) pid=%d\n", __func__, current->pid);
		return -EBUSY;
	}

	rc = nvdimm->sec.ops->freeze(nvdimm);
	nvdimm->sec.flags = nvdimm_security_flags(nvdimm, NVDIMM_USER);
	printf(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static unsigned long dpa_align(struct nd_region *nd_region)
{
	struct device *dev = &nd_region->dev;

	if (dev_WARN_ONCE(dev, !is_nvdimm_bus_locked(dev),
				"bus lock required for capacity provision\n"))
		return 0;
	if (dev_WARN_ONCE(dev, !nd_region->ndr_mappings || nd_region->align
				% nd_region->ndr_mappings,
				"invalid region align %#lx mappings: %d\n",
				nd_region->align, nd_region->ndr_mappings))
		return 0;
	return nd_region->align / nd_region->ndr_mappings;
}

/**
 * nd_pmem_max_contiguous_dpa - For the given dimm+region, return the max
 *			   contiguous unallocated dpa range.
 * @nd_region: constrain available space check to this reference region
 * @nd_mapping: container of dpa-resource-root + labels
 *
 * Returns: %0 if there is an alignment error, otherwise the max
 *		unallocated dpa range
 */
resource_size_t nd_pmem_max_contiguous_dpa(struct nd_region *nd_region,
					   struct nd_mapping *nd_mapping)
{
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nvdimm_bus *nvdimm_bus;
	resource_size_t max = 0;
	struct resource *res;
	unsigned long align;

	/* if a dimm is disabled the available capacity is zero */
	if (!ndd)
		return 0;

	align = dpa_align(nd_region);
	if (!align)
		return 0;

	nvdimm_bus = walk_to_nvdimm_bus(ndd->dev);
	if (__reserve_free_pmem(&nd_region->dev, nd_mapping->nvdimm))
		return 0;
	for_each_dpa_resource(ndd, res) {
		resource_size_t start, end;

		if (strcmp(res->name, "pmem-reserve") != 0)
			continue;
		/* trim free space relative to current alignment setting */
		start = ALIGN(res->start, align);
		end = ALIGN_DOWN(res->end + 1, align) - 1;
		if (end < start)
			continue;
		if (end - start + 1 > max)
			max = end - start + 1;
	}
	release_free_pmem(nvdimm_bus, nd_mapping);
	return max;
}

/**
 * nd_pmem_available_dpa - for the given dimm+region account unallocated dpa
 * @nd_mapping: container of dpa-resource-root + labels
 * @nd_region: constrain available space check to this reference region
 *
 * Validate that a PMEM label, if present, aligns with the start of an
 * interleave set.
 *
 * Returns: %0 if there is an alignment error, otherwise the unallocated dpa
 */
resource_size_t nd_pmem_available_dpa(struct nd_region *nd_region,
				      struct nd_mapping *nd_mapping)
{
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	resource_size_t map_start, map_end, busy = 0;
	struct resource *res;
	unsigned long align;

	if (!ndd)
		return 0;

	align = dpa_align(nd_region);
	if (!align)
		return 0;

	map_start = nd_mapping->start;
	map_end = map_start + nd_mapping->size - 1;
	for_each_dpa_resource(ndd, res) {
		resource_size_t start, end;

		start = ALIGN_DOWN(res->start, align);
		end = ALIGN(res->end + 1, align) - 1;
		if (start >= map_start && start < map_end) {
			if (end > map_end) {
				nd_dbg_dpa(nd_region, ndd, res,
					   "misaligned to iset\n");
				return 0;
			}
			busy += end - start + 1;
		} else if (end >= map_start && end <= map_end) {
			busy += end - start + 1;
		} else if (map_start > start && map_start < end) {
			/* total eclipse of the mapping */
			busy += nd_mapping->size;
		}
	}

	if (busy < nd_mapping->size)
		return ALIGN_DOWN(nd_mapping->size - busy, align);
	return 0;
}

void nvdimm_free_dpa(struct nvdimm_drvdata *ndd, struct resource *res)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p, res=%p pid=%d\n", __func__, ndd, res, current->pid);
	WARN_ON_ONCE(!is_nvdimm_bus_locked(ndd->dev));
	kfree(res->name);
	__release_region(&ndd->dpa, res->start, resource_size(res));
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

struct resource *nvdimm_allocate_dpa(struct nvdimm_drvdata *ndd,
		struct nd_label_id *label_id, resource_size_t start,
		resource_size_t n)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p, label_id=%p, start=%llu, n=%llu pid=%d\n", __func__, ndd, label_id, start, n, current->pid);
	char *name = kmemdup(label_id, sizeof(*label_id), GFP_KERNEL);
	struct resource *res;

	if (!name) {
		printf(KERN_INFO "%s: EXIT: NULL (kmemdup failed) pid=%d\n", __func__, current->pid);
		return NULL;
	}

	WARN_ON_ONCE(!is_nvdimm_bus_locked(ndd->dev));
	res = __request_region(&ndd->dpa, start, n, name, 0);
	if (!res) {
		kfree(name);
		printf(KERN_INFO "%s: EXIT: NULL (__request_region failed) pid=%d\n", __func__, current->pid);
		return NULL;
	}
	printf(KERN_INFO "%s: EXIT: res=%p pid=%d\n", __func__, res, current->pid);
	return res;
}

/**
 * nvdimm_allocated_dpa - sum up the dpa currently allocated to this label_id
 * @ndd: container of dpa-resource-root + labels
 * @label_id: dpa resource name of the form pmem-<human readable uuid>
 *
 * Returns: sum of the dpa allocated to the label_id
 */
resource_size_t nvdimm_allocated_dpa(struct nvdimm_drvdata *ndd,
		struct nd_label_id *label_id)
{
	printf(KERN_INFO "%s: ENTRY: ndd=%p, label_id=%p pid=%d\n", __func__, ndd, label_id, current->pid);
	resource_size_t allocated = 0;
	struct resource *res;

	for_each_dpa_resource(ndd, res)
		if (strcmp(res->name, label_id->id) == 0)
			allocated += resource_size(res);

	printf(KERN_INFO "%s: EXIT: allocated=%llu pid=%d\n", __func__, allocated, current->pid);
	return allocated;
}

static int count_dimms(struct device *dev, void *c)
{
	printf(KERN_INFO "%s: ENTRY: dev=%p, c=%p pid=%d\n", __func__, dev, c, current->pid);
	int *count = c;

	if (is_nvdimm(dev))
		(*count)++;
	printf(KERN_INFO "%s: EXIT: *count=%d pid=%d\n", __func__, *count, current->pid);
	return 0;
}

int nvdimm_bus_check_dimm_count(struct nvdimm_bus *nvdimm_bus, int dimm_count)
{
	printf(KERN_INFO "%s: ENTRY: nvdimm_bus=%p, dimm_count=%d pid=%d\n", __func__, nvdimm_bus, dimm_count, current->pid);
	int count = 0;
	nd_synchronize();
	device_for_each_child(&nvdimm_bus->dev, &count, count_dimms);
	dev_dbg(&nvdimm_bus->dev, "count: %d\n", count);
	if (count != dimm_count) {
		printf(KERN_INFO "%s: EXIT: -ENXIO (count mismatch) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}
	printf(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;
}
EXPORT_SYMBOL_GPL(nvdimm_bus_check_dimm_count);

void __exit nvdimm_devs_exit(void)
{
	printf(KERN_INFO "%s: ENTRY pid=%d\n", __func__, current->pid);
	ida_destroy(&dimm_ida);
	printf(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}
