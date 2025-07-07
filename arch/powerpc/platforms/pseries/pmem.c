// SPDX-License-Identifier: GPL-2.0

/*
 * Handles hot and cold plug of persistent memory regions on pseries.
 */

#define pr_fmt(fmt)     "pseries-pmem: " fmt

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/sched.h>	/* for idle_task_exit */
#include <linux/sched/hotplug.h>
#include <linux/cpu.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <asm/rtas.h>
#include <asm/firmware.h>
#include <asm/machdep.h>
#include <asm/vdso_datapage.h>
#include <asm/plpar_wrappers.h>
#include <asm/topology.h>

#include "pseries.h"

static struct device_node *pmem_node;

static ssize_t pmem_drc_add_node(u32 drc_index)
{
	printk(KERN_INFO "%s: ENTRY: drc_index=0x%x pid=%d\n", __func__, drc_index, current->pid);
	struct device_node *dn;
	int rc;

	rc = dlpar_acquire_drc(drc_index);
	if (rc) {
		printk(KERN_INFO "%s: EXIT: failed to acquire DRC rc=%d pid=%d\n", __func__, rc, current->pid);
		return -EINVAL;
	}

	dn = dlpar_configure_connector(cpu_to_be32(drc_index), pmem_node);
	if (!dn) {
		printk(KERN_INFO "%s: EXIT: configure-connector failed pid=%d\n", __func__, current->pid);
		dlpar_release_drc(drc_index);
		return -EINVAL;
	}

	/* NB: The of reconfig notifier creates platform device from the node */
	rc = dlpar_attach_node(dn, pmem_node);
	if (rc) {
		printk(KERN_INFO "%s: EXIT: failed to attach node rc=%d pid=%d\n", __func__, rc, current->pid);
		if (dlpar_release_drc(drc_index))
			dlpar_free_cc_nodes(dn);
		return rc;
	}

	printk(KERN_INFO "%s: EXIT: success dn=%p drc_index=0x%x pid=%d\n", __func__, dn, drc_index, current->pid);
	return 0;
}

static ssize_t pmem_drc_remove_node(u32 drc_index)
{
	printk(KERN_INFO "%s: ENTRY: drc_index=0x%x pid=%d\n", __func__, drc_index, current->pid);
	struct device_node *dn;
	uint32_t index;
	int rc;

	for_each_child_of_node(pmem_node, dn) {
		if (of_property_read_u32(dn, "ibm,my-drc-index", &index))
			continue;
		if (index == drc_index)
			break;
	}

	if (!dn) {
		printk(KERN_INFO "%s: EXIT: node not found pid=%d\n", __func__, current->pid);
		return -ENODEV;
	}

	pr_debug("Attempting to remove %pOF, drc index: %x\n", dn, drc_index);

	/* * NB: tears down the ibm,pmemory device as a side-effect */
	rc = dlpar_detach_node(dn);
	if (rc) {
		printk(KERN_INFO "%s: EXIT: detach_node failed rc=%d pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	rc = dlpar_release_drc(drc_index);
	if (rc) {
		printk(KERN_INFO "%s: EXIT: release_drc failed rc=%d pid=%d\n", __func__, rc, current->pid);
		dlpar_attach_node(dn, pmem_node);
		return rc;
	}

	printk(KERN_INFO "%s: EXIT: success drc_index=0x%x pid=%d\n", __func__, drc_index, current->pid);
	return 0;
}

int dlpar_hp_pmem(struct pseries_hp_errorlog *hp_elog)
{
	printk(KERN_INFO "%s: ENTRY: hp_elog=%p pid=%d\n", __func__, hp_elog, current->pid);
	u32 drc_index;
	int rc;

	/* slim chance, but we might get a hotplug event while booting */
	if (!pmem_node)
		pmem_node = of_find_node_by_type(NULL, "ibm,persistent-memory");
	if (!pmem_node) {
		printk(KERN_INFO "%s: EXIT: pmem_node not found pid=%d\n", __func__, current->pid);
		return -ENODEV;
	}

	if (hp_elog->id_type != PSERIES_HP_ELOG_ID_DRC_INDEX) {
		printk(KERN_INFO "%s: EXIT: unsupported id_type=%d pid=%d\n", __func__, hp_elog->id_type, current->pid);
		return -EINVAL;
	}

	drc_index = be32_to_cpu(hp_elog->_drc_u.drc_index);

	lock_device_hotplug();

	if (hp_elog->action == PSERIES_HP_ELOG_ACTION_ADD) {
		rc = pmem_drc_add_node(drc_index);
	} else if (hp_elog->action == PSERIES_HP_ELOG_ACTION_REMOVE) {
		rc = pmem_drc_remove_node(drc_index);
	} else {
		printk(KERN_INFO "%s: EXIT: unsupported action=%d pid=%d\n", __func__, hp_elog->action, current->pid);
		rc = -EINVAL;
	}

	unlock_device_hotplug();
	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static const struct of_device_id drc_pmem_match[] = {
	{ .type = "ibm,persistent-memory", },
	{}
};

static int pseries_pmem_init(void)
{
	printk(KERN_INFO "%s: ENTRY pid=%d\n", __func__, current->pid);
	/*
	 * Only supported on POWER8 and above.
	 */
	if (!cpu_has_feature(CPU_FTR_ARCH_207S)) {
		printk(KERN_INFO "%s: EXIT: unsupported CPU pid=%d\n", __func__, current->pid);
		return 0;
	}

	pmem_node = of_find_node_by_type(NULL, "ibm,persistent-memory");
	if (!pmem_node) {
		printk(KERN_INFO "%s: EXIT: pmem_node not found pid=%d\n", __func__, current->pid);
		return 0;
	}

	/*
	 * The generic OF bus probe/populate handles creating platform devices
	 * from the child (ibm,pmemory) nodes. The generic code registers an of
	 * reconfig notifier to handle the hot-add/remove cases too.
	 */
	of_platform_bus_probe(pmem_node, drc_pmem_match, NULL);
	printk(KERN_INFO "%s: EXIT: success pid=%d\n", __func__, current->pid);
	return 0;
}
machine_arch_initcall(pseries, pseries_pmem_init);
