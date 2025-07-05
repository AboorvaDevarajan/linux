// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2013-2016 Intel Corporation. All rights reserved.
 */
#include <linux/device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include "nd-core.h"
#include "pfn.h"
#include "nd.h"

static void nd_dax_release(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p\n", __func__, dev);
	struct nd_region *nd_region = to_nd_region(dev->parent);
	struct nd_dax *nd_dax = to_nd_dax(dev);
	struct nd_pfn *nd_pfn = &nd_dax->nd_pfn;

	dev_dbg(dev, "trace\n");
	nd_detach_ndns(dev, &nd_pfn->ndns);
	ida_free(&nd_region->dax_ida, nd_pfn->id);
	kfree(nd_pfn->uuid);
	kfree(nd_dax);
	printk(KERN_INFO "%s: EXIT\n", __func__);
}

struct nd_dax *to_nd_dax(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p\n", __func__, dev);
	struct nd_dax *nd_dax = container_of(dev, struct nd_dax, nd_pfn.dev);
	WARN_ON(!is_nd_dax(dev));
	printk(KERN_INFO "%s: EXIT: nd_dax=%p\n", __func__, nd_dax);
	return nd_dax;
}
EXPORT_SYMBOL(to_nd_dax);

static const struct device_type nd_dax_device_type = {
	.name = "nd_dax",
	.release = nd_dax_release,
	.groups = nd_pfn_attribute_groups,
};

bool is_nd_dax(const struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p\n", __func__, dev);
	bool ret = dev ? dev->type == &nd_dax_device_type : false;
	printk(KERN_INFO "%s: EXIT: ret=%d\n", __func__, ret);
	return ret;
}
EXPORT_SYMBOL(is_nd_dax);

static struct nd_dax *nd_dax_alloc(struct nd_region *nd_region)
{
	printk(KERN_INFO "%s: ENTRY: nd_region=%p\n", __func__, nd_region);
	struct nd_pfn *nd_pfn;
	struct nd_dax *nd_dax;
	struct device *dev;

	nd_dax = kzalloc(sizeof(*nd_dax), GFP_KERNEL);
	if (!nd_dax) {
		printk(KERN_INFO "%s: EXIT: nd_dax allocation failed\n", __func__);
		return NULL;
	}

	nd_pfn = &nd_dax->nd_pfn;
	nd_pfn->id = ida_alloc(&nd_region->dax_ida, GFP_KERNEL);
	if (nd_pfn->id < 0) {
		kfree(nd_dax);
		printk(KERN_INFO "%s: EXIT: ida_alloc failed\n", __func__);
		return NULL;
	}

	dev = &nd_pfn->dev;
	dev_set_name(dev, "dax%d.%d", nd_region->id, nd_pfn->id);
	dev->type = &nd_dax_device_type;
	dev->parent = &nd_region->dev;

	printk(KERN_INFO "%s: EXIT: nd_dax=%p\n", __func__, nd_dax);
	return nd_dax;
}

struct device *nd_dax_create(struct nd_region *nd_region)
{
	printk(KERN_INFO "%s: ENTRY: nd_region=%p\n", __func__, nd_region);
	struct device *dev = NULL;
	struct nd_dax *nd_dax;

	if (!is_memory(&nd_region->dev)) {
		printk(KERN_INFO "%s: EXIT: not memory region\n", __func__);
		return NULL;
	}

	nd_dax = nd_dax_alloc(nd_region);
	if (nd_dax)
		dev = nd_pfn_devinit(&nd_dax->nd_pfn, NULL);
	nd_device_register(dev);
	printk(KERN_INFO "%s: EXIT: dev=%p\n", __func__, dev);
	return dev;
}

int nd_dax_probe(struct device *dev, struct nd_namespace_common *ndns)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p, ndns=%p\n", __func__, dev, ndns);
	int rc;
	struct nd_dax *nd_dax;
	struct device *dax_dev;
	struct nd_pfn *nd_pfn;
	struct nd_pfn_sb *pfn_sb;
	struct nd_region *nd_region = to_nd_region(ndns->dev.parent);

	if (ndns->force_raw) {
		printk(KERN_INFO "%s: EXIT: force_raw set\n", __func__);
		return -ENODEV;
	}

	switch (ndns->claim_class) {
	case NVDIMM_CCLASS_NONE:
	case NVDIMM_CCLASS_DAX:
		break;
	default:
		printk(KERN_INFO "%s: EXIT: claim_class not supported\n", __func__);
		return -ENODEV;
	}

	nvdimm_bus_lock(&ndns->dev);
	nd_dax = nd_dax_alloc(nd_region);
	dax_dev = nd_dax_devinit(nd_dax, ndns);
	nvdimm_bus_unlock(&ndns->dev);
	if (!dax_dev) {
		printk(KERN_INFO "%s: EXIT: dax_dev allocation failed\n", __func__);
		return -ENOMEM;
	}
	pfn_sb = devm_kmalloc(dev, sizeof(*pfn_sb), GFP_KERNEL);
	nd_pfn = &nd_dax->nd_pfn;
	nd_pfn->pfn_sb = pfn_sb;
	rc = nd_pfn_validate(nd_pfn, DAX_SIG);
	dev_dbg(dev, "dax: %s\n", rc == 0 ? dev_name(dax_dev) : "<none>");
	if (rc < 0) {
		nd_detach_ndns(dax_dev, &nd_pfn->ndns);
		put_device(dax_dev);
	} else
		nd_device_register(dax_dev);

	printk(KERN_INFO "%s: EXIT: rc=%d\n", __func__, rc);
	return rc;
}
EXPORT_SYMBOL(nd_dax_probe);
