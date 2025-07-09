// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pseries Memory Hotplug infrastructure.
 *
 * Copyright (C) 2008 Badari Pulavarty, IBM Corporation
 */

#define pr_fmt(fmt)	"pseries-hotplug-mem: " fmt

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/memblock.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/slab.h>

#include <asm/firmware.h>
#include <asm/machdep.h>
#include <asm/sparsemem.h>
#include <asm/fadump.h>
#include <asm/drmem.h>
#include "pseries.h"

static void dlpar_free_property(struct property *prop)
{
	pr_info("[HOTPLUG TRACE] ENTRY: dlpar_free_property(prop=%p)\n", prop);
	kfree(prop->name);
	kfree(prop->value);
	kfree(prop);
	pr_info("[HOTPLUG TRACE] EXIT: dlpar_free_property returns void\n");
}

static struct property *dlpar_clone_property(struct property *prop,
					     u32 prop_size)
{
	pr_info("[HOTPLUG TRACE] ENTRY: dlpar_clone_property(prop=%p, prop_size=%u)\n", prop, prop_size);
	struct property *new_prop;

	new_prop = kzalloc(sizeof(*new_prop), GFP_KERNEL);
	if (!new_prop)
		return NULL;

	new_prop->name = kstrdup(prop->name, GFP_KERNEL);
	new_prop->value = kzalloc(prop_size, GFP_KERNEL);
	if (!new_prop->name || !new_prop->value) {
		dlpar_free_property(new_prop);
		return NULL;
	}

	memcpy(new_prop->value, prop->value, prop->length);
	new_prop->length = prop_size;

	of_property_set_flag(new_prop, OF_DYNAMIC);
	pr_info("[HOTPLUG TRACE] EXIT: dlpar_clone_property returns prop=%p\n", new_prop);
	return new_prop;
}

static bool find_aa_index(struct device_node *dr_node,
			 struct property *ala_prop,
			 const u32 *lmb_assoc, u32 *aa_index)
{
	pr_info("[HOTPLUG TRACE:ASSOC] ENTRY: find_aa_index(dr_node=%p, ala_prop=%p, lmb_assoc=%p, aa_index=%p)\n", dr_node, ala_prop, lmb_assoc, aa_index);
	__be32 *assoc_arrays;
	u32 new_prop_size;
	struct property *new_prop;
	int aa_arrays, aa_array_entries, aa_array_sz;
	int i, index;

	assoc_arrays = ala_prop->value;
	
	aa_arrays = be32_to_cpu(assoc_arrays[0]);
	aa_array_entries = be32_to_cpu(assoc_arrays[1]);
	aa_array_sz = aa_array_entries * sizeof(u32);
	pr_info("[HOTPLUG TRACE:ASSOC] aa_arrays=%d, aa_array_entries=%d, aa_array_sz=%d\n", aa_arrays, aa_array_entries, aa_array_sz);

	for (i = 0; i < aa_arrays; i++) {
		index = (i * aa_array_entries) + 2;
		pr_info("[HOTPLUG TRACE:ASSOC] Iteration %d: comparing with associativity array at index %d\n", i, index);
		pr_info("[HOTPLUG TRACE:ASSOC] Comparing: ");
		for (int j = 0; j < aa_array_entries; j++) {
			pr_cont("%08x ", be32_to_cpu(assoc_arrays[index + j]));
		}
		pr_cont("vs ");
		for (int j = 0; j < aa_array_entries; j++) {
			pr_cont("%08x ", lmb_assoc[1 + j]);
		}
		pr_cont("\n");
		if (memcmp(&assoc_arrays[index], &lmb_assoc[1], aa_array_sz) == 0) {
			*aa_index = i;
			pr_info("[HOTPLUG TRACE:ASSOC] Match found at index %d, aa_index set to %u\n", i, *aa_index);
			pr_info("[HOTPLUG TRACE:ASSOC] EXIT: find_aa_index returns true\n");
			return true;
		} else {
			pr_info("[HOTPLUG TRACE:ASSOC] No match at index %d\n", i);
		}
	}

	pr_info("[HOTPLUG TRACE:ASSOC] No match found, adding new associativity array\n");
	new_prop_size = ala_prop->length + aa_array_sz;
	new_prop = dlpar_clone_property(ala_prop, new_prop_size);
	if (!new_prop) {
		pr_info("[HOTPLUG TRACE:ASSOC] Failed to clone property, EXIT: find_aa_index returns false\n");
		return false;
	}

	assoc_arrays = new_prop->value;
	assoc_arrays[0] = cpu_to_be32(aa_arrays + 1);
	index = aa_arrays * aa_array_entries + 2;
	memcpy(&assoc_arrays[index], &lmb_assoc[1], aa_array_sz);
	pr_info("[HOTPLUG TRACE:ASSOC] Added new associativity array at index %d: ", aa_arrays);
	for (int j = 0; j < aa_array_entries; j++) {
		pr_cont("%08x ", lmb_assoc[1 + j]);
	}
	pr_cont("\n");

	of_update_property(dr_node, new_prop);
	pr_info("[HOTPLUG TRACE:ASSOC] Updated property in device tree\n");

	*aa_index = be32_to_cpu(assoc_arrays[0]) - 1;
	pr_info("[HOTPLUG TRACE:ASSOC] Set aa_index to %u\n", *aa_index);
	pr_info("[HOTPLUG TRACE:ASSOC] EXIT: find_aa_index returns true\n");
	return true;
}

static int update_lmb_associativity_index(struct drmem_lmb *lmb)
{
	pr_info("[HOTPLUG TRACE:ASSOC] ENTRY: update_lmb_associativity_index(lmb=%p)\n", lmb);
	struct device_node *parent, *lmb_node, *dr_node;
	struct property *ala_prop;
	const u32 *lmb_assoc;
	u32 aa_index;
	bool found;

	pr_info("[HOTPLUG TRACE:ASSOC] Looking for device tree root node\n");
	parent = of_find_node_by_path("/");
	if (!parent) {
		pr_info("[HOTPLUG TRACE:ASSOC] Failed to find root node, returning -ENODEV\n");
		return -ENODEV;
	}

	pr_info("[HOTPLUG TRACE:ASSOC] Configuring connector for LMB drc_index=0x%x\n", lmb->drc_index);
	lmb_node = dlpar_configure_connector(cpu_to_be32(lmb->drc_index), parent);
	of_node_put(parent);
	if (!lmb_node) {
		pr_info("[HOTPLUG TRACE:ASSOC] Failed to configure connector, returning -EINVAL\n");
		return -EINVAL;
	}

	// Trace the entire device tree node structure for lmb_node
	#define TRACE_DT_MAX_DEPTH 5
	void trace_dt_node(const struct device_node *node, int depth) {
		if (!node || depth > TRACE_DT_MAX_DEPTH) return;
		struct property *prop;
		struct device_node *child;
		pr_info("[HOTPLUG TRACE:DT] %*sNode: %s (full path: %s)\n", depth*2, "", node->name, node->full_name);
		for_each_property_of_node(node, prop) {
			pr_info("[HOTPLUG TRACE:DT] %*s  Property: %s, len=%d\n", depth*2, "", prop->name, prop->length);
			if (prop->length > 0 && prop->length <= 32) {
				char buf[65] = {0};
				int i;
				for (i = 0; i < prop->length && i < 32; ++i)
					snprintf(buf + i*2, 3, "%02x", ((u8*)prop->value)[i]);
				pr_info("[HOTPLUG TRACE:DT] %*s    Value(hex): %s\n", depth*2, "", buf);
			}
		}
		for_each_child_of_node(node, child) {
			trace_dt_node(child, depth+1);
		}
	}
	trace_dt_node(lmb_node, 0);

	pr_info("[HOTPLUG TRACE:ASSOC] Getting ibm,associativity property\n");
	lmb_assoc = of_get_property(lmb_node, "ibm,associativity", NULL);
	if (!lmb_assoc) {
		dlpar_free_cc_nodes(lmb_node);
		pr_info("[HOTPLUG TRACE:ASSOC] No ibm,associativity property, returning -ENODEV\n");
		return -ENODEV;
	}

	pr_info("[HOTPLUG TRACE:ASSOC] Calling update_numa_distance\n");
	update_numa_distance(lmb_node);

	pr_info("[HOTPLUG TRACE:ASSOC] Looking for /ibm,dynamic-reconfiguration-memory node\n");
	dr_node = of_find_node_by_path("/ibm,dynamic-reconfiguration-memory");
	if (!dr_node) {
		dlpar_free_cc_nodes(lmb_node);
		pr_info("[HOTPLUG TRACE:ASSOC] Failed to find DR node, returning -ENODEV\n");
		return -ENODEV;
	}

	pr_info("[HOTPLUG TRACE:ASSOC] Looking for ibm,associativity-lookup-arrays property\n");
	ala_prop = of_find_property(dr_node, "ibm,associativity-lookup-arrays", NULL);
	if (!ala_prop) {
		of_node_put(dr_node);
		dlpar_free_cc_nodes(lmb_node);
		pr_info("[HOTPLUG TRACE:ASSOC] No lookup arrays property, returning -ENODEV\n");
		return -ENODEV;
	}

	pr_info("[HOTPLUG TRACE:ASSOC] Calling find_aa_index\n");
	found = find_aa_index(dr_node, ala_prop, lmb_assoc, &aa_index);

	of_node_put(dr_node);
	dlpar_free_cc_nodes(lmb_node);

	if (!found) {
		pr_err("[HOTPLUG TRACE:ASSOC] Could not find LMB associativity\n");
		return -1;
	}

	lmb->aa_index = aa_index;
	pr_info("[HOTPLUG TRACE:ASSOC] Set lmb->aa_index = %u\n", aa_index);
	pr_info("[HOTPLUG TRACE:ASSOC] EXIT: update_lmb_associativity_index returns 0\n");
	return 0;
}

static struct memory_block *lmb_to_memblock(struct drmem_lmb *lmb)
{
	unsigned long section_nr;
	struct memory_block *mem_block;

	section_nr = pfn_to_section_nr(PFN_DOWN(lmb->base_addr));

	mem_block = find_memory_block(section_nr);
	return mem_block;
}

static int get_lmb_range(u32 drc_index, int n_lmbs,
			 struct drmem_lmb **start_lmb,
			 struct drmem_lmb **end_lmb)
{
	struct drmem_lmb *lmb, *start, *end;
	struct drmem_lmb *limit;

	start = NULL;
	for_each_drmem_lmb(lmb) {
		if (lmb->drc_index == drc_index) {
			start = lmb;
			break;
		}
	}

	if (!start)
		return -EINVAL;

	end = &start[n_lmbs];

	limit = &drmem_info->lmbs[drmem_info->n_lmbs];
	if (end > limit)
		return -EINVAL;

	*start_lmb = start;
	*end_lmb = end;
	return 0;
}

static int dlpar_change_lmb_state(struct drmem_lmb *lmb, bool online)
{
	struct memory_block *mem_block;
	int rc;

	mem_block = lmb_to_memblock(lmb);
	if (!mem_block) {
		pr_err("Failed memory block lookup for LMB 0x%x\n", lmb->drc_index);
		return -EINVAL;
	}

	if (online && mem_block->dev.offline)
		rc = device_online(&mem_block->dev);
	else if (!online && !mem_block->dev.offline)
		rc = device_offline(&mem_block->dev);
	else
		rc = 0;

	put_device(&mem_block->dev);

	return rc;
}

static int dlpar_online_lmb(struct drmem_lmb *lmb)
{
	return dlpar_change_lmb_state(lmb, true);
}

#ifdef CONFIG_MEMORY_HOTREMOVE
static int dlpar_offline_lmb(struct drmem_lmb *lmb)
{
	return dlpar_change_lmb_state(lmb, false);
}

static int pseries_remove_memblock(unsigned long base, unsigned long memblock_size)
{
	unsigned long start_pfn;
	int sections_per_block;
	int i;

	start_pfn = base >> PAGE_SHIFT;

	lock_device_hotplug();

	if (!pfn_valid(start_pfn))
		goto out;

	sections_per_block = memory_block_size / MIN_MEMORY_BLOCK_SIZE;

	for (i = 0; i < sections_per_block; i++) {
		__remove_memory(base, MIN_MEMORY_BLOCK_SIZE);
		base += MIN_MEMORY_BLOCK_SIZE;
	}

out:
	/* Update memory regions for memory remove */
	memblock_remove(base, memblock_size);
	unlock_device_hotplug();
	return 0;
}

static int pseries_remove_mem_node(struct device_node *np)
{
	int ret;
	struct resource res;

	/*
	 * Check to see if we are actually removing memory
	 */
	if (!of_node_is_type(np, "memory"))
		return 0;

	/*
	 * Find the base address and size of the memblock
	 */
	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		return ret;

	pseries_remove_memblock(res.start, resource_size(&res));
	return 0;
}

static bool lmb_is_removable(struct drmem_lmb *lmb)
{
	if ((lmb->flags & DRCONF_MEM_RESERVED) ||
		!(lmb->flags & DRCONF_MEM_ASSIGNED))
		return false;

#ifdef CONFIG_FA_DUMP
	/*
	 * Don't hot-remove memory that falls in fadump boot memory area
	 * and memory that is reserved for capturing old kernel memory.
	 */
	if (is_fadump_memory_area(lmb->base_addr, memory_block_size_bytes()))
		return false;
#endif
	/* device_offline() will determine if we can actually remove this lmb */
	return true;
}

static int dlpar_add_lmb(struct drmem_lmb *);

static int dlpar_remove_lmb(struct drmem_lmb *lmb)
{
	struct memory_block *mem_block;
	int rc;

	if (!lmb_is_removable(lmb))
		return -EINVAL;

	mem_block = lmb_to_memblock(lmb);
	if (mem_block == NULL)
		return -EINVAL;

	rc = dlpar_offline_lmb(lmb);
	if (rc) {
		put_device(&mem_block->dev);
		return rc;
	}

	__remove_memory(lmb->base_addr, memory_block_size);
	put_device(&mem_block->dev);

	/* Update memory regions for memory remove */
	memblock_remove(lmb->base_addr, memory_block_size);

	invalidate_lmb_associativity_index(lmb);
	lmb->flags &= ~DRCONF_MEM_ASSIGNED;

	return 0;
}

static int dlpar_memory_remove_by_count(u32 lmbs_to_remove)
{
	struct drmem_lmb *lmb;
	int lmbs_reserved = 0;
	int lmbs_available = 0;
	int rc;

	pr_info("Attempting to hot-remove %d LMB(s)\n", lmbs_to_remove);

	if (lmbs_to_remove == 0)
		return -EINVAL;

	/* Validate that there are enough LMBs to satisfy the request */
	for_each_drmem_lmb(lmb) {
		if (lmb_is_removable(lmb))
			lmbs_available++;

		if (lmbs_available == lmbs_to_remove)
			break;
	}

	if (lmbs_available < lmbs_to_remove) {
		pr_info("Not enough LMBs available (%d of %d) to satisfy request\n",
			lmbs_available, lmbs_to_remove);
		return -EINVAL;
	}

	for_each_drmem_lmb(lmb) {
		rc = dlpar_remove_lmb(lmb);
		if (rc)
			continue;

		/* Mark this lmb so we can add it later if all of the
		 * requested LMBs cannot be removed.
		 */
		drmem_mark_lmb_reserved(lmb);

		lmbs_reserved++;
		if (lmbs_reserved == lmbs_to_remove)
			break;
	}

	if (lmbs_reserved != lmbs_to_remove) {
		pr_err("Memory hot-remove failed, adding LMB's back\n");

		for_each_drmem_lmb(lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			rc = dlpar_add_lmb(lmb);
			if (rc)
				pr_err("Failed to add LMB back, drc index %x\n",
				       lmb->drc_index);

			drmem_remove_lmb_reservation(lmb);

			lmbs_reserved--;
			if (lmbs_reserved == 0)
				break;
		}

		rc = -EINVAL;
	} else {
		for_each_drmem_lmb(lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			dlpar_release_drc(lmb->drc_index);
			pr_info("Memory at %llx was hot-removed\n",
				lmb->base_addr);

			drmem_remove_lmb_reservation(lmb);

			lmbs_reserved--;
			if (lmbs_reserved == 0)
				break;
		}
		rc = 0;
	}

	return rc;
}

static int dlpar_memory_remove_by_index(u32 drc_index)
{
	struct drmem_lmb *lmb;
	int lmb_found;
	int rc;

	pr_debug("Attempting to hot-remove LMB, drc index %x\n", drc_index);

	lmb_found = 0;
	for_each_drmem_lmb(lmb) {
		if (lmb->drc_index == drc_index) {
			lmb_found = 1;
			rc = dlpar_remove_lmb(lmb);
			if (!rc)
				dlpar_release_drc(lmb->drc_index);

			break;
		}
	}

	if (!lmb_found) {
		pr_debug("Failed to look up LMB for drc index %x\n", drc_index);
		rc = -EINVAL;
	} else if (rc) {
		pr_debug("Failed to hot-remove memory at %llx\n",
			 lmb->base_addr);
	} else {
		pr_debug("Memory at %llx was hot-removed\n", lmb->base_addr);
	}

	return rc;
}

static int dlpar_memory_remove_by_ic(u32 lmbs_to_remove, u32 drc_index)
{
	struct drmem_lmb *lmb, *start_lmb, *end_lmb;
	int rc;

	pr_info("Attempting to hot-remove %u LMB(s) at %x\n",
		lmbs_to_remove, drc_index);

	if (lmbs_to_remove == 0)
		return -EINVAL;

	rc = get_lmb_range(drc_index, lmbs_to_remove, &start_lmb, &end_lmb);
	if (rc)
		return -EINVAL;

	/*
	 * Validate that all LMBs in range are not reserved. Note that it
	 * is ok if they are !ASSIGNED since our goal here is to remove the
	 * LMB range, regardless of whether some LMBs were already removed
	 * by any other reason.
	 *
	 * This is a contrast to what is done in remove_by_count() where we
	 * check for both RESERVED and !ASSIGNED (via lmb_is_removable()),
	 * because we want to remove a fixed amount of LMBs in that function.
	 */
	for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
		if (lmb->flags & DRCONF_MEM_RESERVED) {
			pr_err("Memory at %llx (drc index %x) is reserved\n",
				lmb->base_addr, lmb->drc_index);
			return -EINVAL;
		}
	}

	for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
		/*
		 * dlpar_remove_lmb() will error out if the LMB is already
		 * !ASSIGNED, but this case is a no-op for us.
		 */
		if (!(lmb->flags & DRCONF_MEM_ASSIGNED))
			continue;

		rc = dlpar_remove_lmb(lmb);
		if (rc)
			break;

		drmem_mark_lmb_reserved(lmb);
	}

	if (rc) {
		pr_err("Memory indexed-count-remove failed, adding any removed LMBs\n");


		for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			/*
			 * Setting the isolation state of an UNISOLATED/CONFIGURED
			 * device to UNISOLATE is a no-op, but the hypervisor can
			 * use it as a hint that the LMB removal failed.
			 */
			dlpar_unisolate_drc(lmb->drc_index);

			rc = dlpar_add_lmb(lmb);
			if (rc)
				pr_err("Failed to add LMB, drc index %x\n",
				       lmb->drc_index);

			drmem_remove_lmb_reservation(lmb);
		}
		rc = -EINVAL;
	} else {
		for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			dlpar_release_drc(lmb->drc_index);
			pr_info("Memory at %llx (drc index %x) was hot-removed\n",
				lmb->base_addr, lmb->drc_index);

			drmem_remove_lmb_reservation(lmb);
		}
	}

	return rc;
}

#else
static inline int pseries_remove_memblock(unsigned long base,
					  unsigned long memblock_size)
{
	return -EOPNOTSUPP;
}
static inline int pseries_remove_mem_node(struct device_node *np)
{
	return 0;
}
static int dlpar_remove_lmb(struct drmem_lmb *lmb)
{
	return -EOPNOTSUPP;
}
static int dlpar_memory_remove_by_count(u32 lmbs_to_remove)
{
	return -EOPNOTSUPP;
}
static int dlpar_memory_remove_by_index(u32 drc_index)
{
	return -EOPNOTSUPP;
}

static int dlpar_memory_remove_by_ic(u32 lmbs_to_remove, u32 drc_index)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_MEMORY_HOTREMOVE */

static int dlpar_add_lmb(struct drmem_lmb *lmb)
{
	pr_info("[HOTPLUG TRACE] ENTRY: dlpar_add_lmb(lmb=%p)\n", lmb);
	unsigned long block_sz;
	int nid, rc;

	if (lmb->flags & DRCONF_MEM_ASSIGNED)
		return -EINVAL;

	rc = update_lmb_associativity_index(lmb);
	if (rc) {
		dlpar_release_drc(lmb->drc_index);
		pr_err("Failed to configure LMB 0x%x\n", lmb->drc_index);
		return rc;
	}

	block_sz = memory_block_size_bytes();

	/* Find the node id for this LMB.  Fake one if necessary. */
	nid = of_drconf_to_nid_single(lmb);
	if (nid < 0 || !node_possible(nid))
		nid = first_online_node;

	/* Add the memory */
	rc = __add_memory(nid, lmb->base_addr, block_sz, MHP_MEMMAP_ON_MEMORY);
	if (rc) {
		pr_err("Failed to add LMB 0x%x to node %u", lmb->drc_index, nid);
		invalidate_lmb_associativity_index(lmb);
		return rc;
	}

	rc = dlpar_online_lmb(lmb);
	if (rc) {
		pr_err("Failed to online LMB 0x%x on node %u\n", lmb->drc_index, nid);
		__remove_memory(lmb->base_addr, block_sz);
		invalidate_lmb_associativity_index(lmb);
	} else {
		lmb->flags |= DRCONF_MEM_ASSIGNED;
	}

	pr_info("[HOTPLUG TRACE] EXIT: dlpar_add_lmb returns %d\n", rc);
	return rc;
}

static int dlpar_memory_add_by_count(u32 lmbs_to_add)
{
	struct drmem_lmb *lmb;
	int lmbs_available = 0;
	int lmbs_reserved = 0;
	int rc;

	pr_info("Attempting to hot-add %d LMB(s)\n", lmbs_to_add);

	if (lmbs_to_add == 0)
		return -EINVAL;

	/* Validate that there are enough LMBs to satisfy the request */
	for_each_drmem_lmb(lmb) {
		if (lmb->flags & DRCONF_MEM_RESERVED)
			continue;

		if (!(lmb->flags & DRCONF_MEM_ASSIGNED))
			lmbs_available++;

		if (lmbs_available == lmbs_to_add)
			break;
	}

	if (lmbs_available < lmbs_to_add)
		return -EINVAL;

	for_each_drmem_lmb(lmb) {
		if (lmb->flags & DRCONF_MEM_ASSIGNED)
			continue;

		rc = dlpar_acquire_drc(lmb->drc_index);
		if (rc)
			continue;

		rc = dlpar_add_lmb(lmb);
		if (rc) {
			dlpar_release_drc(lmb->drc_index);
			continue;
		}

		/* Mark this lmb so we can remove it later if all of the
		 * requested LMBs cannot be added.
		 */
		drmem_mark_lmb_reserved(lmb);
		lmbs_reserved++;
		if (lmbs_reserved == lmbs_to_add)
			break;
	}

	if (lmbs_reserved != lmbs_to_add) {
		pr_err("Memory hot-add failed, removing any added LMBs\n");

		for_each_drmem_lmb(lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			rc = dlpar_remove_lmb(lmb);
			if (rc)
				pr_err("Failed to remove LMB, drc index %x\n",
				       lmb->drc_index);
			else
				dlpar_release_drc(lmb->drc_index);

			drmem_remove_lmb_reservation(lmb);
			lmbs_reserved--;

			if (lmbs_reserved == 0)
				break;
		}
		rc = -EINVAL;
	} else {
		for_each_drmem_lmb(lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			pr_debug("Memory at %llx (drc index %x) was hot-added\n",
				 lmb->base_addr, lmb->drc_index);
			drmem_remove_lmb_reservation(lmb);
			lmbs_reserved--;

			if (lmbs_reserved == 0)
				break;
		}
		rc = 0;
	}

	return rc;
}

static int dlpar_memory_add_by_index(u32 drc_index)
{
	struct drmem_lmb *lmb;
	int rc, lmb_found;

	pr_info("Attempting to hot-add LMB, drc index %x\n", drc_index);

	lmb_found = 0;
	for_each_drmem_lmb(lmb) {
		if (lmb->drc_index == drc_index) {
			lmb_found = 1;
			rc = dlpar_acquire_drc(lmb->drc_index);
			if (!rc) {
				rc = dlpar_add_lmb(lmb);
				if (rc)
					dlpar_release_drc(lmb->drc_index);
			}

			break;
		}
	}

	if (!lmb_found)
		rc = -EINVAL;

	if (rc)
		pr_info("Failed to hot-add memory, drc index %x\n", drc_index);
	else
		pr_info("Memory at %llx (drc index %x) was hot-added\n",
			lmb->base_addr, drc_index);

	return rc;
}

static int dlpar_memory_add_by_ic(u32 lmbs_to_add, u32 drc_index)
{
	struct drmem_lmb *lmb, *start_lmb, *end_lmb;
	int rc;

	pr_info("Attempting to hot-add %u LMB(s) at index %x\n",
		lmbs_to_add, drc_index);

	if (lmbs_to_add == 0)
		return -EINVAL;

	rc = get_lmb_range(drc_index, lmbs_to_add, &start_lmb, &end_lmb);
	if (rc)
		return -EINVAL;

	/* Validate that the LMBs in this range are not reserved */
	for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
		/* Fail immediately if the whole range can't be hot-added */
		if (lmb->flags & DRCONF_MEM_RESERVED) {
			pr_err("Memory at %llx (drc index %x) is reserved\n",
					lmb->base_addr, lmb->drc_index);
			return -EINVAL;
		}
	}

	for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
		if (lmb->flags & DRCONF_MEM_ASSIGNED)
			continue;

		rc = dlpar_acquire_drc(lmb->drc_index);
		if (rc)
			break;

		rc = dlpar_add_lmb(lmb);
		if (rc) {
			dlpar_release_drc(lmb->drc_index);
			break;
		}

		drmem_mark_lmb_reserved(lmb);
	}

	if (rc) {
		pr_err("Memory indexed-count-add failed, removing any added LMBs\n");

		for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			rc = dlpar_remove_lmb(lmb);
			if (rc)
				pr_err("Failed to remove LMB, drc index %x\n",
				       lmb->drc_index);
			else
				dlpar_release_drc(lmb->drc_index);

			drmem_remove_lmb_reservation(lmb);
		}
		rc = -EINVAL;
	} else {
		for_each_drmem_lmb_in_range(lmb, start_lmb, end_lmb) {
			if (!drmem_lmb_reserved(lmb))
				continue;

			pr_info("Memory at %llx (drc index %x) was hot-added\n",
				lmb->base_addr, lmb->drc_index);
			drmem_remove_lmb_reservation(lmb);
		}
	}

	return rc;
}

int dlpar_memory(struct pseries_hp_errorlog *hp_elog)
{
	u32 count, drc_index;
	int rc;

	pr_info("[DLPAR TRACE] ENTRY: dlpar_memory hp_elog=%p action=%d id_type=%d\n", hp_elog, hp_elog ? hp_elog->action : -1, hp_elog ? hp_elog->id_type : -1);

	lock_device_hotplug();

	switch (hp_elog->action) {
	case PSERIES_HP_ELOG_ACTION_ADD:
		pr_info("[DLPAR TRACE] Action: ADD\n");
		switch (hp_elog->id_type) {
		case PSERIES_HP_ELOG_ID_DRC_COUNT:
			count = be32_to_cpu(hp_elog->_drc_u.drc_count);
			pr_info("[DLPAR TRACE] ID_TYPE: DRC_COUNT, count=%u\n", count);
			rc = dlpar_memory_add_by_count(count);
			break;
		case PSERIES_HP_ELOG_ID_DRC_INDEX:
			drc_index = be32_to_cpu(hp_elog->_drc_u.drc_index);
			pr_info("[DLPAR TRACE] ID_TYPE: DRC_INDEX, drc_index=0x%x\n", drc_index);
			rc = dlpar_memory_add_by_index(drc_index);
			break;
		case PSERIES_HP_ELOG_ID_DRC_IC:
			count = be32_to_cpu(hp_elog->_drc_u.ic.count);
			drc_index = be32_to_cpu(hp_elog->_drc_u.ic.index);
			pr_info("[DLPAR TRACE] ID_TYPE: DRC_IC, count=%u, drc_index=0x%x\n", count, drc_index);
			rc = dlpar_memory_add_by_ic(count, drc_index);
			break;
		default:
			pr_info("[DLPAR TRACE] ID_TYPE: UNKNOWN (%d)\n", hp_elog->id_type);
			rc = -EINVAL;
			break;
		}
		break;
	case PSERIES_HP_ELOG_ACTION_REMOVE:
		pr_info("[DLPAR TRACE] Action: REMOVE\n");
		switch (hp_elog->id_type) {
		case PSERIES_HP_ELOG_ID_DRC_COUNT:
			count = be32_to_cpu(hp_elog->_drc_u.drc_count);
			pr_info("[DLPAR TRACE] ID_TYPE: DRC_COUNT, count=%u\n", count);
			rc = dlpar_memory_remove_by_count(count);
			break;
		case PSERIES_HP_ELOG_ID_DRC_INDEX:
			drc_index = be32_to_cpu(hp_elog->_drc_u.drc_index);
			pr_info("[DLPAR TRACE] ID_TYPE: DRC_INDEX, drc_index=0x%x\n", drc_index);
			rc = dlpar_memory_remove_by_index(drc_index);
			break;
		case PSERIES_HP_ELOG_ID_DRC_IC:
			count = be32_to_cpu(hp_elog->_drc_u.ic.count);
			drc_index = be32_to_cpu(hp_elog->_drc_u.ic.index);
			pr_info("[DLPAR TRACE] ID_TYPE: DRC_IC, count=%u, drc_index=0x%x\n", count, drc_index);
			rc = dlpar_memory_remove_by_ic(count, drc_index);
			break;
		default:
			pr_info("[DLPAR TRACE] ID_TYPE: UNKNOWN (%d)\n", hp_elog->id_type);
			rc = -EINVAL;
			break;
		}
		break;
	default:
		pr_err("[DLPAR TRACE] Invalid action (%d) specified\n", hp_elog->action);
		rc = -EINVAL;
		break;
	}

	if (!rc) {
		pr_info("[DLPAR TRACE] Calling drmem_update_dt\n");
		rc = drmem_update_dt();
	}

	unlock_device_hotplug();
	pr_info("[DLPAR TRACE] EXIT: dlpar_memory rc=%d\n", rc);
	return rc;
}

static int pseries_add_mem_node(struct device_node *np)
{
	int ret;
	struct resource res;

	/*
	 * Check to see if we are actually adding memory
	 */
	if (!of_node_is_type(np, "memory"))
		return 0;

	/*
	 * Find the base and size of the memblock
	 */
	ret = of_address_to_resource(np, 0, &res);
	if (ret)
		return ret;

	/*
	 * Update memory region to represent the memory add
	 */
	ret = memblock_add(res.start, resource_size(&res));
	return (ret < 0) ? -EINVAL : 0;
}

static int pseries_memory_notifier(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	struct of_reconfig_data *rd = data;
	int err = 0;

	switch (action) {
	case OF_RECONFIG_ATTACH_NODE:
		err = pseries_add_mem_node(rd->dn);
		break;
	case OF_RECONFIG_DETACH_NODE:
		err = pseries_remove_mem_node(rd->dn);
		break;
	case OF_RECONFIG_UPDATE_PROPERTY:
		if (!strcmp(rd->dn->name,
			    "ibm,dynamic-reconfiguration-memory"))
			drmem_update_lmbs(rd->prop);
	}
	return notifier_from_errno(err);
}

static struct notifier_block pseries_mem_nb = {
	.notifier_call = pseries_memory_notifier,
};

static int __init pseries_memory_hotplug_init(void)
{
	if (firmware_has_feature(FW_FEATURE_LPAR))
		of_reconfig_notifier_register(&pseries_mem_nb);

	return 0;
}
machine_device_initcall(pseries, pseries_memory_hotplug_init);
