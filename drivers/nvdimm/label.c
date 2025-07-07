// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2013-2015 Intel Corporation. All rights reserved.
 */
#include <linux/device.h>
#include <linux/ndctl.h>
#include <linux/uuid.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/nd.h>
#include "nd-core.h"
#include "label.h"
#include "nd.h"

static guid_t nvdimm_btt_guid;
static guid_t nvdimm_btt2_guid;
static guid_t nvdimm_pfn_guid;
static guid_t nvdimm_dax_guid;

static uuid_t nvdimm_btt_uuid;
static uuid_t nvdimm_btt2_uuid;
static uuid_t nvdimm_pfn_uuid;
static uuid_t nvdimm_dax_uuid;

static uuid_t cxl_region_uuid;
static uuid_t cxl_namespace_uuid;

static const char NSINDEX_SIGNATURE[] = "NAMESPACE_INDEX\0";

static u32 best_seq(u32 a, u32 b)
{
	printk(KERN_INFO "%s: ENTRY: a=%u, b=%u pid=%d\n", __func__, a, b, current->pid);
	a &= NSINDEX_SEQ_MASK;
	b &= NSINDEX_SEQ_MASK;

	if (a == 0 || a == b) {
		printk(KERN_INFO "%s: EXIT: b=%u pid=%d\n", __func__, b, current->pid);
		return b;
	} else if (b == 0) {
		printk(KERN_INFO "%s: EXIT: a=%u pid=%d\n", __func__, a, current->pid);
		return a;
	} else if (nd_inc_seq(a) == b) {
		printk(KERN_INFO "%s: EXIT: b=%u pid=%d\n", __func__, b, current->pid);
		return b;
	} else {
		printk(KERN_INFO "%s: EXIT: a=%u pid=%d\n", __func__, a, current->pid);
		return a;
	}
}

unsigned sizeof_namespace_label(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	printk(KERN_INFO "%s: EXIT: nslabel_size=%u pid=%d\n", __func__, ndd->nslabel_size, current->pid);
	return ndd->nslabel_size;
}

static size_t __sizeof_namespace_index(u32 nslot)
{
	printk(KERN_INFO "%s: ENTRY: nslot=%u pid=%d\n", __func__, nslot, current->pid);
	size_t ret = ALIGN(sizeof(struct nd_namespace_index) + DIV_ROUND_UP(nslot, 8), NSINDEX_ALIGN);
	printk(KERN_INFO "%s: EXIT: ret=%zu pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static int __nvdimm_num_label_slots(struct nvdimm_drvdata *ndd, size_t index_size)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, index_size=%zu pid=%d\n", __func__, ndd, index_size, current->pid);
	int ret = (ndd->nsarea.config_size - index_size * 2) / sizeof_namespace_label(ndd);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

int nvdimm_num_label_slots(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	u32 tmp_nslot, n;

	tmp_nslot = ndd->nsarea.config_size / sizeof_namespace_label(ndd);
	n = __sizeof_namespace_index(tmp_nslot) / NSINDEX_ALIGN;
	int ret = __nvdimm_num_label_slots(ndd, NSINDEX_ALIGN * n);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

size_t sizeof_namespace_index(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	u32 nslot, space, size;

	nslot = nvdimm_num_label_slots(ndd);
	space = ndd->nsarea.config_size - nslot * sizeof_namespace_label(ndd);
	size = __sizeof_namespace_index(nslot) * 2;
	if (size <= space && nslot >= 2) {
		size_t ret = size / 2;
		printk(KERN_INFO "%s: EXIT: ret=%zu pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	dev_err(ndd->dev, "label area (%d) too small to host (%d byte) labels\n", ndd->nsarea.config_size, sizeof_namespace_label(ndd));
	printk(KERN_INFO "%s: EXIT: ret=0 pid=%d\n", __func__, current->pid);
	return 0;
}

static int __nd_label_validate(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	/*
	 * On media label format consists of two index blocks followed
	 * by an array of labels.  None of these structures are ever
	 * updated in place.  A sequence number tracks the current
	 * active index and the next one to write, while labels are
	 * written to free slots.
	 *
	 *     +------------+
	 *     |            |
	 *     |  nsindex0  |
	 *     |            |
	 *     +------------+
	 *     |            |
	 *     |  nsindex1  |
	 *     |            |
	 *     +------------+
	 *     |   label0   |
	 *     +------------+
	 *     |   label1   |
	 *     +------------+
	 *     |            |
	 *      ....nslot...
	 *     |            |
	 *     +------------+
	 *     |   labelN   |
	 *     +------------+
	 */
	struct nd_namespace_index *nsindex[] = {
		to_namespace_index(ndd, 0),
		to_namespace_index(ndd, 1),
	};
	const int num_index = ARRAY_SIZE(nsindex);
	struct device *dev = ndd->dev;
	bool valid[2] = { 0 };
	int i, num_valid = 0;
	u32 seq;

	for (i = 0; i < num_index; i++) {
		u32 nslot;
		u8 sig[NSINDEX_SIG_LEN];
		u64 sum_save, sum, size;
		unsigned int version, labelsize;

		memcpy(sig, nsindex[i]->sig, NSINDEX_SIG_LEN);
		if (memcmp(sig, NSINDEX_SIGNATURE, NSINDEX_SIG_LEN) != 0) {
			dev_dbg(dev, "nsindex%d signature invalid\n", i);
			continue;
		}

		/* label sizes larger than 128 arrived with v1.2 */
		version = __le16_to_cpu(nsindex[i]->major) * 100
			+ __le16_to_cpu(nsindex[i]->minor);
		if (version >= 102)
			labelsize = 1 << (7 + nsindex[i]->labelsize);
		else
			labelsize = 128;

		if (labelsize != sizeof_namespace_label(ndd)) {
			dev_dbg(dev, "nsindex%d labelsize %d invalid\n",
					i, nsindex[i]->labelsize);
			continue;
		}

		sum_save = __le64_to_cpu(nsindex[i]->checksum);
		nsindex[i]->checksum = __cpu_to_le64(0);
		sum = nd_fletcher64(nsindex[i], sizeof_namespace_index(ndd), 1);
		nsindex[i]->checksum = __cpu_to_le64(sum_save);
		if (sum != sum_save) {
			dev_dbg(dev, "nsindex%d checksum invalid\n", i);
			continue;
		}

		seq = __le32_to_cpu(nsindex[i]->seq);
		if ((seq & NSINDEX_SEQ_MASK) == 0) {
			dev_dbg(dev, "nsindex%d sequence: %#x invalid\n", i, seq);
			continue;
		}

		/* sanity check the index against expected values */
		if (__le64_to_cpu(nsindex[i]->myoff)
				!= i * sizeof_namespace_index(ndd)) {
			dev_dbg(dev, "nsindex%d myoff: %#llx invalid\n",
					i, (unsigned long long)
					__le64_to_cpu(nsindex[i]->myoff));
			continue;
		}
		if (__le64_to_cpu(nsindex[i]->otheroff)
				!= (!i) * sizeof_namespace_index(ndd)) {
			dev_dbg(dev, "nsindex%d otheroff: %#llx invalid\n",
					i, (unsigned long long)
					__le64_to_cpu(nsindex[i]->otheroff));
			continue;
		}
		if (__le64_to_cpu(nsindex[i]->labeloff)
				!= 2 * sizeof_namespace_index(ndd)) {
			dev_dbg(dev, "nsindex%d labeloff: %#llx invalid\n",
					i, (unsigned long long)
					__le64_to_cpu(nsindex[i]->labeloff));
			continue;
		}

		size = __le64_to_cpu(nsindex[i]->mysize);
		if (size > sizeof_namespace_index(ndd)
				|| size < sizeof(struct nd_namespace_index)) {
			dev_dbg(dev, "nsindex%d mysize: %#llx invalid\n", i, size);
			continue;
		}

		nslot = __le32_to_cpu(nsindex[i]->nslot);
		if (nslot * sizeof_namespace_label(ndd)
				+ 2 * sizeof_namespace_index(ndd)
				> ndd->nsarea.config_size) {
			dev_dbg(dev, "nsindex%d nslot: %u invalid, config_size: %#x\n",
					i, nslot, ndd->nsarea.config_size);
			continue;
		}
		valid[i] = true;
		num_valid++;
	}

	int ret = -1;
	switch (num_valid) {
	case 0:
		break;
	case 1:
		for (i = 0; i < num_index; i++)
			if (valid[i]) {
				printk(KERN_INFO "%s: EXIT: i=%d pid=%d\n", __func__, i, current->pid);
				return i;
			}
		WARN_ON(1);
		break;
	default:
		seq = best_seq(__le32_to_cpu(nsindex[0]->seq), __le32_to_cpu(nsindex[1]->seq));
		if (seq == (__le32_to_cpu(nsindex[1]->seq) & NSINDEX_SEQ_MASK)) {
			printk(KERN_INFO "%s: EXIT: 1 pid=%d\n", __func__, current->pid);
			return 1;
		} else {
			printk(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
			return 0;
		}
	}
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static int nd_label_validate(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	int label_size[] = { 128, 256 };
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(label_size); i++) {
		ndd->nslabel_size = label_size[i];
		rc = __nd_label_validate(ndd);
		if (rc >= 0) {
			printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
	}
	printk(KERN_INFO "%s: EXIT: rc=-1 pid=%d\n", __func__, current->pid);
	return -1;
}

static void nd_label_copy(struct nvdimm_drvdata *ndd, struct nd_namespace_index *dst, struct nd_namespace_index *src)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, dst=%p, src=%p pid=%d\n", __func__, ndd, dst, src, current->pid);
	if (!dst || !src) {
		printk(KERN_INFO "%s: EXIT: dst or src is NULL pid=%d\n", __func__, current->pid);
		return;
	}
	memcpy(dst, src, sizeof_namespace_index(ndd));
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static struct nd_namespace_label *nd_label_base(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	void *base = to_namespace_index(ndd, 0);
	struct nd_namespace_label *ret = base + 2 * sizeof_namespace_index(ndd);
	printk(KERN_INFO "%s: EXIT: ret=%p pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static int to_slot(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p pid=%d\n", __func__, ndd, nd_label, current->pid);
	unsigned long label, base;
	label = (unsigned long) nd_label;
	base = (unsigned long) nd_label_base(ndd);
	int ret = (label - base) / sizeof_namespace_label(ndd);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static struct nd_namespace_label *to_label(struct nvdimm_drvdata *ndd, int slot)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, slot=%d pid=%d\n", __func__, ndd, slot, current->pid);
	unsigned long label, base;
	base = (unsigned long) nd_label_base(ndd);
	label = base + sizeof_namespace_label(ndd) * slot;
	struct nd_namespace_label *ret = (struct nd_namespace_label *) label;
	printk(KERN_INFO "%s: EXIT: ret=%p pid=%d\n", __func__, ret, current->pid);
	return ret;
}

#define for_each_clear_bit_le(bit, addr, size) \
	for ((bit) = find_next_zero_bit_le((addr), (size), 0);  \
	     (bit) < (size);                                    \
	     (bit) = find_next_zero_bit_le((addr), (size), (bit) + 1))

/**
 * preamble_index - common variable initialization for nd_label_* routines
 * @ndd: dimm container for the relevant label set
 * @idx: namespace_index index
 * @nsindex_out: on return set to the currently active namespace index
 * @free: on return set to the free label bitmap in the index
 * @nslot: on return set to the number of slots in the label space
 */
static bool preamble_index(struct nvdimm_drvdata *ndd, int idx,
		struct nd_namespace_index **nsindex_out,
		unsigned long **free, u32 *nslot)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, idx=%d pid=%d\n", __func__, ndd, idx, current->pid);
	struct nd_namespace_index *nsindex;

	nsindex = to_namespace_index(ndd, idx);
	if (nsindex == NULL) {
		printk(KERN_INFO "%s: EXIT: nsindex is NULL pid=%d\n", __func__, current->pid);
		return false;
	}

	*free = (unsigned long *) nsindex->free;
	*nslot = __le32_to_cpu(nsindex->nslot);
	*nsindex_out = nsindex;

	printk(KERN_INFO "%s: EXIT: true pid=%d\n", __func__, current->pid);
	return true;
}

char *nd_label_gen_id(struct nd_label_id *label_id, const uuid_t *uuid, u32 flags)
{
	printk(KERN_INFO "%s: ENTRY: label_id=%p, uuid=%p, flags=%u pid=%d\n", __func__, label_id, uuid, flags, current->pid);
	if (!label_id || !uuid) {
		printk(KERN_INFO "%s: EXIT: NULL (label_id or uuid is NULL) pid=%d\n", __func__, current->pid);
		return NULL;
	}
	snprintf(label_id->id, ND_LABEL_ID_SIZE, "pmem-%pUb", uuid);
	printk(KERN_INFO "%s: EXIT: id=%s pid=%d\n", __func__, label_id->id, current->pid);
	return label_id->id;
}

static bool preamble_current(struct nvdimm_drvdata *ndd, struct nd_namespace_index **nsindex, unsigned long **free, u32 *nslot)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	bool ret = preamble_index(ndd, ndd->ns_current, nsindex, free, nslot);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static bool preamble_next(struct nvdimm_drvdata *ndd, struct nd_namespace_index **nsindex, unsigned long **free, u32 *nslot)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	bool ret = preamble_index(ndd, ndd->ns_next, nsindex, free, nslot);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static bool nsl_validate_checksum(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p pid=%d\n", __func__, ndd, nd_label, current->pid);
	u64 sum, sum_save;

	if (!ndd->cxl && !efi_namespace_label_has(ndd, checksum)) {
		printk(KERN_INFO "%s: EXIT: true (no checksum) pid=%d\n", __func__, current->pid);
		return true;
	}

	sum_save = nsl_get_checksum(ndd, nd_label);
	nsl_set_checksum(ndd, nd_label, 0);
	sum = nd_fletcher64(nd_label, sizeof_namespace_label(ndd), 1);
	nsl_set_checksum(ndd, nd_label, sum_save);
	bool ret = (sum == sum_save);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static void nsl_calculate_checksum(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p pid=%d\n", __func__, ndd, nd_label, current->pid);
	u64 sum;

	if (!ndd->cxl && !efi_namespace_label_has(ndd, checksum)) {
		printk(KERN_INFO "%s: EXIT: no checksum pid=%d\n", __func__, current->pid);
		return;
	}
	nsl_set_checksum(ndd, nd_label, 0);
	sum = nd_fletcher64(nd_label, sizeof_namespace_label(ndd), 1);
	nsl_set_checksum(ndd, nd_label, sum);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static bool slot_valid(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label, u32 slot)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p, slot=%u pid=%d\n", __func__, ndd, nd_label, slot, current->pid);
	bool valid;

	if (slot != nsl_get_slot(ndd, nd_label)) {
		printk(KERN_INFO "%s: EXIT: slot mismatch pid=%d\n", __func__, current->pid);
		return false;
	}
	valid = nsl_validate_checksum(ndd, nd_label);
	if (!valid)
		dev_dbg(ndd->dev, "fail checksum. slot: %d\n");
	printk(KERN_INFO "%s: EXIT: valid=%d pid=%d\n", __func__, valid, current->pid);
	return valid;
}

int nd_label_reserve_dpa(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;

	if (!preamble_current(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: no label, nothing to reserve pid=%d\n", __func__, current->pid);
		return 0;
	}

	for_each_clear_bit_le(slot, free, nslot) {
		struct nd_namespace_label *nd_label;
		struct nd_region *nd_region = NULL;
		struct nd_label_id label_id;
		struct resource *res;
		uuid_t label_uuid;
		u32 flags;

		nd_label = to_label(ndd, slot);

		if (!slot_valid(ndd, nd_label, slot))
			continue;

		nsl_get_uuid(ndd, nd_label, &label_uuid);
		flags = nsl_get_flags(ndd, nd_label);
		nd_label_gen_id(&label_id, &label_uuid, flags);
		res = nvdimm_allocate_dpa(ndd, &label_id, nsl_get_dpa(ndd, nd_label), nsl_get_rawsize(ndd, nd_label));
		nd_dbg_dpa(nd_region, ndd, res, "reserve\n");
		if (!res) {
			printk(KERN_INFO "%s: EXIT: -EBUSY pid=%d\n", __func__, current->pid);
			return -EBUSY;
		}
	}

	printk(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;
}

int nd_label_data_init(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	size_t config_size, read_size, max_xfer, offset;
	struct nd_namespace_index *nsindex;
	unsigned int i;
	int rc = 0;
	u32 nslot;

	if (ndd->data) {
		printk(KERN_INFO "%s: EXIT: already initialized pid=%d\n", __func__, current->pid);
		return 0;
	}

	if (ndd->nsarea.status || ndd->nsarea.max_xfer == 0 || ndd->nsarea.config_size == 0) {
		printk(KERN_INFO "%s: EXIT: invalid nsarea pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	ndd->nslabel_size = 128;
	read_size = sizeof_namespace_index(ndd) * 2;
	if (!read_size) {
		printk(KERN_INFO "%s: EXIT: read_size is zero pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	config_size = ndd->nsarea.config_size;
	ndd->data = kvzalloc(config_size, GFP_KERNEL);
	if (!ndd->data) {
		printk(KERN_INFO "%s: EXIT: kvzalloc failed pid=%d\n", __func__, current->pid);
		return -ENOMEM;
	}

	max_xfer = min_t(size_t, ndd->nsarea.max_xfer, config_size);
	if (read_size < max_xfer) {
		max_xfer -= ((max_xfer - 1) - (config_size - 1) % max_xfer) / DIV_ROUND_UP(config_size, max_xfer);
		if (max_xfer < read_size)
			max_xfer = read_size;
	}
	read_size = min(DIV_ROUND_UP(read_size, max_xfer) * max_xfer, config_size);
	rc = nvdimm_get_config_data(ndd, ndd->data, 0, read_size);
	if (rc)
		goto out_err;
	ndd->ns_current = nd_label_validate(ndd);
	if (ndd->ns_current < 0) {
		printk(KERN_INFO "%s: EXIT: ns_current < 0 pid=%d\n", __func__, current->pid);
		return 0;
	}
	ndd->ns_next = nd_label_next_nsindex(ndd->ns_current);
	nsindex = to_current_namespace_index(ndd);
	nd_label_copy(ndd, to_next_namespace_index(ndd), nsindex);
	offset = __le64_to_cpu(nsindex->labeloff);
	nslot = __le32_to_cpu(nsindex->nslot);
	for (i = 0; i < nslot; i++, offset += ndd->nslabel_size) {
		size_t label_read_size;
		if (test_bit_le(i, nsindex->free)) {
			memset(ndd->data + offset, 0, ndd->nslabel_size);
			continue;
		}
		if (offset + ndd->nslabel_size <= read_size)
			continue;
		if (read_size < offset)
			read_size = offset;
		label_read_size = offset + ndd->nslabel_size - read_size;
		label_read_size = DIV_ROUND_UP(label_read_size, max_xfer) * max_xfer;
		if (read_size + label_read_size > config_size)
			label_read_size = config_size - read_size;
		rc = nvdimm_get_config_data(ndd, ndd->data + read_size, read_size, label_read_size);
		if (rc)
			goto out_err;
		read_size += label_read_size;
	}
	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
out_err:
	printk(KERN_INFO "%s: EXIT: error rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

int nd_label_active_count(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;
	int count = 0;

	if (!preamble_current(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: preamble_current failed pid=%d\n", __func__, current->pid);
		return 0;
	}

	for_each_clear_bit_le(slot, free, nslot) {
		struct nd_namespace_label *nd_label;
		nd_label = to_label(ndd, slot);
		if (!slot_valid(ndd, nd_label, slot)) {
			u32 label_slot = nsl_get_slot(ndd, nd_label);
			u64 size = nsl_get_rawsize(ndd, nd_label);
			u64 dpa = nsl_get_dpa(ndd, nd_label);
			continue;
		}
		count++;
	}
	printk(KERN_INFO "%s: EXIT: count=%d pid=%d\n", __func__, count, current->pid);
	return count;
}

struct nd_namespace_label *nd_label_active(struct nvdimm_drvdata *ndd, int n)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, n=%d pid=%d\n", __func__, ndd, n, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;

	if (!preamble_current(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: preamble_current failed pid=%d\n", __func__, current->pid);
		return NULL;
	}

	for_each_clear_bit_le(slot, free, nslot) {
		struct nd_namespace_label *nd_label;
		nd_label = to_label(ndd, slot);
		if (!slot_valid(ndd, nd_label, slot))
			continue;
		if (n-- == 0) {
			printk(KERN_INFO "%s: EXIT: found label=%p pid=%d\n", __func__, nd_label, current->pid);
			return to_label(ndd, slot);
		}
	}
	printk(KERN_INFO "%s: EXIT: not found pid=%d\n", __func__, current->pid);
	return NULL;
}

u32 nd_label_alloc_slot(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot, slot;

	if (!preamble_next(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: preamble_next failed pid=%d\n", __func__, current->pid);
		return UINT_MAX;
	}

	slot = find_next_bit_le(free, nslot, 0);
	if (slot == nslot) {
		printk(KERN_INFO "%s: EXIT: no free slot pid=%d\n", __func__, current->pid);
		return UINT_MAX;
	}

	clear_bit_le(slot, free);
	printk(KERN_INFO "%s: EXIT: slot=%u pid=%d\n", __func__, slot, current->pid);
	return slot;
}

bool nd_label_free_slot(struct nvdimm_drvdata *ndd, u32 slot)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, slot=%u pid=%d\n", __func__, ndd, slot, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot;

	if (!preamble_next(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: preamble_next failed pid=%d\n", __func__, current->pid);
		return false;
	}

	if (slot < nslot) {
		bool ret = !test_and_set_bit_le(slot, free);
		printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	printk(KERN_INFO "%s: EXIT: slot out of range pid=%d\n", __func__, current->pid);
	return false;
}

u32 nd_label_nfree(struct nvdimm_drvdata *ndd)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p pid=%d\n", __func__, ndd, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	u32 nslot;

	if (!preamble_next(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: preamble_next failed pid=%d\n", __func__, current->pid);
		return nvdimm_num_label_slots(ndd);
	}

	u32 ret = bitmap_weight(free, nslot);
	printk(KERN_INFO "%s: EXIT: nfree=%u pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static int nd_label_write_index(struct nvdimm_drvdata *ndd, int index, u32 seq, unsigned long flags)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, index=%d, seq=%u, flags=0x%lx pid=%d\n", __func__, ndd, index, seq, flags, current->pid);
	struct nd_namespace_index *nsindex;
	unsigned long offset;
	u64 checksum;
	u32 nslot;
	int rc;

	nsindex = to_namespace_index(ndd, index);
	if (flags & ND_NSINDEX_INIT)
		nslot = nvdimm_num_label_slots(ndd);
	else
		nslot = __le32_to_cpu(nsindex->nslot);

	memcpy(nsindex->sig, NSINDEX_SIGNATURE, NSINDEX_SIG_LEN);
	memset(&nsindex->flags, 0, 3);
	nsindex->labelsize = sizeof_namespace_label(ndd) >> 8;
	nsindex->seq = __cpu_to_le32(seq);
	offset = (unsigned long) nsindex - (unsigned long) to_namespace_index(ndd, 0);
	nsindex->myoff = __cpu_to_le64(offset);
	nsindex->mysize = __cpu_to_le64(sizeof_namespace_index(ndd));
	offset = (unsigned long) to_namespace_index(ndd, nd_label_next_nsindex(index)) - (unsigned long) to_namespace_index(ndd, 0);
	nsindex->otheroff = __cpu_to_le64(offset);
	offset = (unsigned long) nd_label_base(ndd) - (unsigned long) to_namespace_index(ndd, 0);
	nsindex->labeloff = __cpu_to_le64(offset);
	nsindex->nslot = __cpu_to_le32(nslot);
	nsindex->major = __cpu_to_le16(1);
	if (sizeof_namespace_label(ndd) < 256)
		nsindex->minor = __cpu_to_le16(1);
	else
		nsindex->minor = __cpu_to_le16(2);
	nsindex->checksum = __cpu_to_le64(0);
	if (flags & ND_NSINDEX_INIT) {
		unsigned long *free = (unsigned long *) nsindex->free;
		u32 nfree = ALIGN(nslot, BITS_PER_LONG);
		int last_bits, i;

		memset(nsindex->free, 0xff, nfree / 8);
		for (i = 0, last_bits = nfree - nslot; i < last_bits; i++)
			clear_bit_le(nslot + i, free);
	}
	checksum = nd_fletcher64(nsindex, sizeof_namespace_index(ndd), 1);
	nsindex->checksum = __cpu_to_le64(checksum);
	rc = nvdimm_set_config_data(ndd, __le64_to_cpu(nsindex->myoff), nsindex, sizeof_namespace_index(ndd));
	if (rc < 0) {
		printk(KERN_INFO "%s: EXIT: nvdimm_set_config_data failed rc=%d pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	if (flags & ND_NSINDEX_INIT) {
		printk(KERN_INFO "%s: EXIT: 0 (init) pid=%d\n", __func__, current->pid);
		return 0;
	}

	WARN_ON(index != ndd->ns_next);
	nd_label_copy(ndd, to_current_namespace_index(ndd), nsindex);
	ndd->ns_current = nd_label_next_nsindex(ndd->ns_current);
	ndd->ns_next = nd_label_next_nsindex(ndd->ns_next);
	WARN_ON(ndd->ns_current == ndd->ns_next);

	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static unsigned long nd_label_offset(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p pid=%d\n", __func__, ndd, nd_label, current->pid);
	unsigned long ret = (unsigned long) nd_label - (unsigned long) to_namespace_index(ndd, 0);
	printk(KERN_INFO "%s: EXIT: ret=%lu pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static enum nvdimm_claim_class guid_to_nvdimm_cclass(guid_t *guid)
{
	printk(KERN_INFO "%s: ENTRY: guid=%p pid=%d\n", __func__, guid, current->pid);
	if (guid_equal(guid, &nvdimm_btt_guid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_BTT pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_BTT;
	} else if (guid_equal(guid, &nvdimm_btt2_guid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_BTT2 pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_BTT2;
	} else if (guid_equal(guid, &nvdimm_pfn_guid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_PFN pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_PFN;
	} else if (guid_equal(guid, &nvdimm_dax_guid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_DAX pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_DAX;
	} else if (guid_equal(guid, &guid_null)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_NONE pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_NONE;
	}
	printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_UNKNOWN pid=%d\n", __func__, current->pid);
	return NVDIMM_CCLASS_UNKNOWN;
}

/* CXL labels store UUIDs instead of GUIDs for the same data */
static enum nvdimm_claim_class uuid_to_nvdimm_cclass(uuid_t *uuid)
{
	printk(KERN_INFO "%s: ENTRY: uuid=%p pid=%d\n", __func__, uuid, current->pid);
	if (uuid_equal(uuid, &nvdimm_btt_uuid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_BTT pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_BTT;
	} else if (uuid_equal(uuid, &nvdimm_btt2_uuid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_BTT2 pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_BTT2;
	} else if (uuid_equal(uuid, &nvdimm_pfn_uuid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_PFN pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_PFN;
	} else if (uuid_equal(uuid, &nvdimm_dax_uuid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_DAX pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_DAX;
	} else if (uuid_equal(uuid, &uuid_null)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_NONE pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_NONE;
	}
	printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_UNKNOWN pid=%d\n", __func__, current->pid);
	return NVDIMM_CCLASS_UNKNOWN;
}

static const guid_t *to_abstraction_guid(enum nvdimm_claim_class claim_class, guid_t *target)
{
	printk(KERN_INFO "%s: ENTRY: claim_class=%d, target=%p pid=%d\n", __func__, claim_class, target, current->pid);
	if (claim_class == NVDIMM_CCLASS_BTT) {
		printk(KERN_INFO "%s: EXIT: nvdimm_btt_guid pid=%d\n", __func__, current->pid);
		return &nvdimm_btt_guid;
	} else if (claim_class == NVDIMM_CCLASS_BTT2) {
		printk(KERN_INFO "%s: EXIT: nvdimm_btt2_guid pid=%d\n", __func__, current->pid);
		return &nvdimm_btt2_guid;
	} else if (claim_class == NVDIMM_CCLASS_PFN) {
		printk(KERN_INFO "%s: EXIT: nvdimm_pfn_guid pid=%d\n", __func__, current->pid);
		return &nvdimm_pfn_guid;
	} else if (claim_class == NVDIMM_CCLASS_DAX) {
		printk(KERN_INFO "%s: EXIT: nvdimm_dax_guid pid=%d\n", __func__, current->pid);
		return &nvdimm_dax_guid;
	} else if (claim_class == NVDIMM_CCLASS_UNKNOWN) {
		printk(KERN_INFO "%s: EXIT: target (unknown) pid=%d\n", __func__, current->pid);
		return target;
	} else {
		printk(KERN_INFO "%s: EXIT: guid_null pid=%d\n", __func__, current->pid);
		return &guid_null;
	}
}

/* CXL labels store UUIDs instead of GUIDs for the same data */
static const uuid_t *to_abstraction_uuid(enum nvdimm_claim_class claim_class, uuid_t *target)
{
	printk(KERN_INFO "%s: ENTRY: claim_class=%d, target=%p pid=%d\n", __func__, claim_class, target, current->pid);
	if (claim_class == NVDIMM_CCLASS_BTT) {
		printk(KERN_INFO "%s: EXIT: nvdimm_btt_uuid pid=%d\n", __func__, current->pid);
		return &nvdimm_btt_uuid;
	} else if (claim_class == NVDIMM_CCLASS_BTT2) {
		printk(KERN_INFO "%s: EXIT: nvdimm_btt2_uuid pid=%d\n", __func__, current->pid);
		return &nvdimm_btt2_uuid;
	} else if (claim_class == NVDIMM_CCLASS_PFN) {
		printk(KERN_INFO "%s: EXIT: nvdimm_pfn_uuid pid=%d\n", __func__, current->pid);
		return &nvdimm_pfn_uuid;
	} else if (claim_class == NVDIMM_CCLASS_DAX) {
		printk(KERN_INFO "%s: EXIT: nvdimm_dax_uuid pid=%d\n", __func__, current->pid);
		return &nvdimm_dax_uuid;
	} else if (claim_class == NVDIMM_CCLASS_UNKNOWN) {
		printk(KERN_INFO "%s: EXIT: target (unknown) pid=%d\n", __func__, current->pid);
		return target;
	} else {
		printk(KERN_INFO "%s: EXIT: uuid_null pid=%d\n", __func__, current->pid);
		return &uuid_null;
	}
}

static void reap_victim(struct nd_mapping *nd_mapping, struct nd_label_ent *victim)
{
	printk(KERN_INFO "%s: ENTRY: nd_mapping=%p, victim=%p pid=%d\n", __func__, nd_mapping, victim, current->pid);
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	u32 slot = to_slot(ndd, victim->label);

	dev_dbg(ndd->dev, "free: %d\n");
	nd_label_free_slot(ndd, slot);
	victim->label = NULL;
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nsl_set_type_guid(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label, guid_t *guid)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p, guid=%p pid=%d\n", __func__, ndd, nd_label, guid, current->pid);
	if (efi_namespace_label_has(ndd, type_guid))
		guid_copy(&nd_label->efi.type_guid, guid);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

bool nsl_validate_type_guid(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label, guid_t *guid)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p, guid=%p pid=%d\n", __func__, ndd, nd_label, guid, current->pid);
	if (ndd->cxl || !efi_namespace_label_has(ndd, type_guid)) {
		printk(KERN_INFO "%s: EXIT: true (cxl or no type_guid) pid=%d\n", __func__, current->pid);
		return true;
	}
	if (!guid_equal(&nd_label->efi.type_guid, guid)) {
		dev_dbg(ndd->dev, "expect type_guid %pUb got %pUb\n", guid, &nd_label->efi.type_guid);
		printk(KERN_INFO "%s: EXIT: false (type_guid mismatch) pid=%d\n", __func__, current->pid);
		return false;
	}
	printk(KERN_INFO "%s: EXIT: true pid=%d\n", __func__, current->pid);
	return true;
}

static void nsl_set_claim_class(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label, enum nvdimm_claim_class claim_class)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p, claim_class=%d pid=%d\n", __func__, ndd, nd_label, claim_class, current->pid);
	if (ndd->cxl) {
		uuid_t uuid;
		import_uuid(&uuid, nd_label->cxl.abstraction_uuid);
		export_uuid(nd_label->cxl.abstraction_uuid, to_abstraction_uuid(claim_class, &uuid));
		printk(KERN_INFO "%s: EXIT (cxl) pid=%d\n", __func__, current->pid);
		return;
	}
	if (!efi_namespace_label_has(ndd, abstraction_guid)) {
		printk(KERN_INFO "%s: EXIT (no abstraction_guid) pid=%d\n", __func__, current->pid);
		return;
	}
	guid_copy(&nd_label->efi.abstraction_guid, to_abstraction_guid(claim_class, &nd_label->efi.abstraction_guid));
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

enum nvdimm_claim_class nsl_get_claim_class(struct nvdimm_drvdata *ndd, struct nd_namespace_label *nd_label)
{
	printk(KERN_INFO "%s: ENTRY: ndd=%p, nd_label=%p pid=%d\n", __func__, ndd, nd_label, current->pid);
	if (ndd->cxl) {
		uuid_t uuid;
		import_uuid(&uuid, nd_label->cxl.abstraction_uuid);
		enum nvdimm_claim_class ret = uuid_to_nvdimm_cclass(&uuid);
		printk(KERN_INFO "%s: EXIT: ret=%d (cxl) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}
	if (!efi_namespace_label_has(ndd, abstraction_guid)) {
		printk(KERN_INFO "%s: EXIT: NVDIMM_CCLASS_NONE (no abstraction_guid) pid=%d\n", __func__, current->pid);
		return NVDIMM_CCLASS_NONE;
	}
	enum nvdimm_claim_class ret = guid_to_nvdimm_cclass(&nd_label->efi.abstraction_guid);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static int __pmem_label_update(struct nd_region *nd_region, struct nd_mapping *nd_mapping, struct nd_namespace_pmem *nspm, int pos, unsigned long flags)
{
	printk(KERN_INFO "%s: ENTRY: nd_region=%p, nd_mapping=%p, nspm=%p, pos=%d, flags=%lx pid=%d\n", __func__, nd_region, nd_mapping, nspm, pos, flags, current->pid);
	struct nd_namespace_common *ndns = &nspm->nsio.common;
	struct nd_interleave_set *nd_set = nd_region->nd_set;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nd_namespace_label *nd_label;
	struct nd_namespace_index *nsindex;
	struct nd_label_ent *label_ent;
	struct nd_label_id label_id;
	struct resource *res;
	unsigned long *free;
	u32 nslot, slot;
	size_t offset;
	u64 cookie;
	int rc;

	if (!preamble_next(ndd, &nsindex, &free, &nslot))
		return -ENXIO;

	cookie = nd_region_interleave_set_cookie(nd_region, nsindex);
	nd_label_gen_id(&label_id, nspm->uuid, 0);
	for_each_dpa_resource(ndd, res)
		if (strcmp(res->name, label_id.id) == 0)
			break;

	if (!res) {
		WARN_ON_ONCE(1);
		return -ENXIO;
	}

	/* allocate and write the label to the staging (next) index */
	slot = nd_label_alloc_slot(ndd);
	if (slot == UINT_MAX)
		return -ENXIO;
	dev_dbg(ndd->dev, "allocated: %d\n");

	nd_label = to_label(ndd, slot);
	memset(nd_label, 0, sizeof_namespace_label(ndd));
	nsl_set_uuid(ndd, nd_label, nspm->uuid);
	nsl_set_name(ndd, nd_label, nspm->alt_name);
	nsl_set_flags(ndd, nd_label, flags);
	nsl_set_nlabel(ndd, nd_label, nd_region->ndr_mappings);
	nsl_set_nrange(ndd, nd_label, 1);
	nsl_set_position(ndd, nd_label, pos);
	nsl_set_isetcookie(ndd, nd_label, cookie);
	nsl_set_rawsize(ndd, nd_label, resource_size(res));
	nsl_set_lbasize(ndd, nd_label, nspm->lbasize);
	nsl_set_dpa(ndd, nd_label, res->start);
	nsl_set_slot(ndd, nd_label, slot);
	nsl_set_type_guid(ndd, nd_label, &nd_set->type_guid);
	nsl_set_claim_class(ndd, nd_label, ndns->claim_class);
	nsl_calculate_checksum(ndd, nd_label);
	nd_dbg_dpa(nd_region, ndd, res, "\n");

	/* update label */
	offset = nd_label_offset(ndd, nd_label);
	rc = nvdimm_set_config_data(ndd, offset, nd_label,
			sizeof_namespace_label(ndd));
	if (rc < 0) {
		printk(KERN_INFO "%s: EXIT: nvdimm_set_config_data failed rc=%d pid=%d\n", __func__, rc, current->pid);
		return rc;
	}

	/* Garbage collect the previous label */
	mutex_lock(&nd_mapping->lock);
	list_for_each_entry(label_ent, &nd_mapping->labels, list) {
		if (!label_ent->label)
			continue;
		if (test_and_clear_bit(ND_LABEL_REAP, &label_ent->flags) ||
		    nsl_uuid_equal(ndd, label_ent->label, nspm->uuid))
			reap_victim(nd_mapping, label_ent);
	}

	/* update index */
	rc = nd_label_write_index(ndd, ndd->ns_next,
			nd_inc_seq(__le32_to_cpu(nsindex->seq)), 0);
	if (rc == 0) {
		list_for_each_entry(label_ent, &nd_mapping->labels, list)
			if (!label_ent->label) {
				label_ent->label = nd_label;
				nd_label = NULL;
				break;
			}
		dev_WARN_ONCE(&nspm->nsio.common.dev, nd_label,
				"failed to track label: %d\n",
				to_slot(ndd, nd_label));
		if (nd_label)
			rc = -ENXIO;
	}
	mutex_unlock(&nd_mapping->lock);

	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static int init_labels(struct nd_mapping *nd_mapping, int num_labels)
{
	int i, old_num_labels = 0;
	struct nd_label_ent *label_ent;
	struct nd_namespace_index *nsindex;
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);

	printk(KERN_INFO "%s: ENTRY: ndd=%p, num_labels=%d pid=%d\n", __func__, ndd, num_labels, current->pid);
	mutex_lock(&nd_mapping->lock);
	list_for_each_entry(label_ent, &nd_mapping->labels, list)
		old_num_labels++;
	mutex_unlock(&nd_mapping->lock);

	/*
	 * We need to preserve all the old labels for the mapping so
	 * they can be garbage collected after writing the new labels.
	 */
	for (i = old_num_labels; i < num_labels; i++) {
		label_ent = kzalloc(sizeof(*label_ent), GFP_KERNEL);
		if (!label_ent) {
			printk(KERN_INFO "%s: EXIT: -ENOMEM pid=%d\n", __func__, current->pid);
			return -ENOMEM;
		}
		mutex_lock(&nd_mapping->lock);
		list_add_tail(&label_ent->list, &nd_mapping->labels);
		mutex_unlock(&nd_mapping->lock);
	}

	if (ndd->ns_current == -1 || ndd->ns_next == -1) {
		printk(KERN_INFO "%s: EXIT: max(num_labels, old_num_labels)=%d pid=%d\n", __func__, max(num_labels, old_num_labels), current->pid);
		return max(num_labels, old_num_labels);
	}

	nsindex = to_namespace_index(ndd, 0);
	memset(nsindex, 0, ndd->nsarea.config_size);
	for (i = 0; i < 2; i++) {
		int rc = nd_label_write_index(ndd, i, 3 - i, ND_NSINDEX_INIT);

		if (rc) {
			printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
	}
	ndd->ns_next = 1;
	ndd->ns_current = 0;
	printk(KERN_INFO "%s: EXIT: max(num_labels, old_num_labels)=%d pid=%d\n", __func__, max(num_labels, old_num_labels), current->pid);
	return max(num_labels, old_num_labels);
}

static int del_labels(struct nd_mapping *nd_mapping, uuid_t *uuid)
{
	struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
	struct nd_label_ent *label_ent, *e;
	struct nd_namespace_index *nsindex;
	unsigned long *free;
	LIST_HEAD(list);
	u32 nslot, slot;
	int active = 0;

	printk(KERN_INFO "%s: ENTRY: ndd=%p, uuid=%p pid=%d\n", __func__, ndd, uuid, current->pid);
	if (!uuid) {
		printk(KERN_INFO "%s: EXIT: 0 (uuid is NULL) pid=%d\n", __func__, current->pid);
		return 0;
	}


	/* no index || no labels == nothing to delete */
	if (!preamble_next(ndd, &nsindex, &free, &nslot)) {
		printk(KERN_INFO "%s: EXIT: 0 (preamble_next failed) pid=%d\n", __func__, current->pid);
		return 0;
	}

	mutex_lock(&nd_mapping->lock);
	list_for_each_entry_safe(label_ent, e, &nd_mapping->labels, list) {
		struct nd_namespace_label *nd_label = label_ent->label;

		if (!nd_label)
			continue;
		active++;
		if (!nsl_uuid_equal(ndd, nd_label, uuid))
			continue;
		active--;
		slot = to_slot(ndd, nd_label);
		nd_label_free_slot(ndd, slot);
		dev_dbg(ndd->dev, "free: %d\n");
		list_move_tail(&label_ent->list, &list);
		label_ent->label = NULL;
	}
	list_splice_tail_init(&list, &nd_mapping->labels);

	if (active == 0) {
		nd_mapping_free_labels(nd_mapping);
		dev_dbg(ndd->dev, "no more active labels\n");
	}
	mutex_unlock(&nd_mapping->lock);

	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);

	return nd_label_write_index(ndd, ndd->ns_next,
			nd_inc_seq(__le32_to_cpu(nsindex->seq)), 0);
}

int nd_pmem_namespace_label_update(struct nd_region *nd_region, struct nd_namespace_pmem *nspm, resource_size_t size)
{
	printk(KERN_INFO "%s: ENTRY: nd_region=%p, nspm=%p, size=%pa pid=%d\n", __func__, nd_region, nspm, &size, current->pid);
	int i, rc = 0;
	for (i = 0; i < nd_region->ndr_mappings; i++) {
		struct nd_mapping *nd_mapping = &nd_region->mapping[i];
		struct nvdimm_drvdata *ndd = to_ndd(nd_mapping);
		struct resource *res;
		int count = 0;
		if (size == 0) {
			rc = del_labels(nd_mapping, nspm->uuid);
			if (rc) {
				printk(KERN_INFO "%s: EXIT: rc=%d (del_labels) pid=%d\n", __func__, rc, current->pid);
				return rc;
			}
			continue;
		}
		for_each_dpa_resource(ndd, res)
			if (strncmp(res->name, "pmem", 4) == 0)
				count++;
		WARN_ON_ONCE(!count);
		rc = init_labels(nd_mapping, count);
		if (rc < 0) {
			printk(KERN_INFO "%s: EXIT: rc=%d (init_labels) pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
		rc = __pmem_label_update(nd_region, nd_mapping, nspm, i, NSLABEL_FLAG_UPDATING);
		if (rc) {
			printk(KERN_INFO "%s: EXIT: rc=%d (__pmem_label_update) pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
	}
	if (size == 0) {
		printk(KERN_INFO "%s: EXIT: 0 (size==0) pid=%d\n", __func__, current->pid);
		return 0;
	}
	for (i = 0; i < nd_region->ndr_mappings; i++) {
		struct nd_mapping *nd_mapping = &nd_region->mapping[i];
		rc = __pmem_label_update(nd_region, nd_mapping, nspm, i, 0);
		if (rc) {
			printk(KERN_INFO "%s: EXIT: rc=%d (clear UPDATING) pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
	}
	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

int __init nd_label_init(void)
{
	printk(KERN_INFO "%s: ENTRY pid=%d\n", __func__, current->pid);
	WARN_ON(guid_parse(NVDIMM_BTT_GUID, &nvdimm_btt_guid));
	WARN_ON(guid_parse(NVDIMM_BTT2_GUID, &nvdimm_btt2_guid));
	WARN_ON(guid_parse(NVDIMM_PFN_GUID, &nvdimm_pfn_guid));
	WARN_ON(guid_parse(NVDIMM_DAX_GUID, &nvdimm_dax_guid));
	WARN_ON(uuid_parse(NVDIMM_BTT_GUID, &nvdimm_btt_uuid));
	WARN_ON(uuid_parse(NVDIMM_BTT2_GUID, &nvdimm_btt2_uuid));
	WARN_ON(uuid_parse(NVDIMM_PFN_GUID, &nvdimm_pfn_uuid));
	WARN_ON(uuid_parse(NVDIMM_DAX_GUID, &nvdimm_dax_uuid));
	WARN_ON(uuid_parse(CXL_REGION_UUID, &cxl_region_uuid));
	WARN_ON(uuid_parse(CXL_NAMESPACE_UUID, &cxl_namespace_uuid));
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
	return 0;
}
