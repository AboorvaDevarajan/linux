// SPDX-License-Identifier: GPL-2.0-only
/*
 * Persistent Memory Driver
 *
 * Copyright (c) 2014-2015, Intel Corporation.
 * Copyright (c) 2015, Christoph Hellwig <hch@lst.de>.
 * Copyright (c) 2015, Boaz Harrosh <boaz@plexistor.com>.
 */

#include <linux/blkdev.h>
#include <linux/pagemap.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/set_memory.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/badblocks.h>
#include <linux/memremap.h>
#include <linux/kstrtox.h>
#include <linux/vmalloc.h>
#include <linux/blk-mq.h>
#include <linux/pfn_t.h>
#include <linux/slab.h>
#include <linux/uio.h>
#include <linux/dax.h>
#include <linux/nd.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>
#include "pmem.h"
#include "btt.h"
#include "pfn.h"
#include "nd.h"

static struct device *to_dev(struct pmem_device *pmem)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p pid=%d\n", __func__, pmem, current->pid);
	struct device *ret = pmem->bb.dev;
	printk(KERN_INFO "%s: EXIT: ret=%p pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static struct nd_region *to_region(struct pmem_device *pmem)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p pid=%d\n", __func__, pmem, current->pid);
	struct nd_region *ret = to_nd_region(to_dev(pmem)->parent);
	printk(KERN_INFO "%s: EXIT: ret=%p pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static phys_addr_t pmem_to_phys(struct pmem_device *pmem, phys_addr_t offset)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, offset=%pa pid=%d\n", __func__, pmem, &offset, current->pid);
	phys_addr_t ret = pmem->phys_addr + offset;
	printk(KERN_INFO "%s: EXIT: ret=%pa pid=%d\n", __func__, &ret, current->pid);
	return ret;
}

static sector_t to_sect(struct pmem_device *pmem, phys_addr_t offset)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, offset=%pa pid=%d\n", __func__, pmem, &offset, current->pid);
	sector_t ret = (offset - pmem->data_offset) >> SECTOR_SHIFT;
	printk(KERN_INFO "%s: EXIT: ret=%llu pid=%d\n", __func__, (unsigned long long)ret, current->pid);
	return ret;
}

static phys_addr_t to_offset(struct pmem_device *pmem, sector_t sector)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, sector=%llu pid=%d\n", __func__, pmem, (unsigned long long)sector, current->pid);
	phys_addr_t ret = (sector << SECTOR_SHIFT) + pmem->data_offset;
	printk(KERN_INFO "%s: EXIT: ret=%pa pid=%d\n", __func__, &ret, current->pid);
	return ret;
}

static void pmem_mkpage_present(struct pmem_device *pmem, phys_addr_t offset,
		unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, offset=%pa, len=%u pid=%d\n", __func__, pmem, &offset, len, current->pid);
	phys_addr_t phys = pmem_to_phys(pmem, offset);
	unsigned long pfn_start, pfn_end, pfn;

	/* only pmem in the linear map supports HWPoison */
	if (is_vmalloc_addr(pmem->virt_addr))
		return;

	pfn_start = PHYS_PFN(phys);
	pfn_end = pfn_start + PHYS_PFN(len);
	for (pfn = pfn_start; pfn < pfn_end; pfn++) {
		struct page *page = pfn_to_page(pfn);

		/*
		 * Note, no need to hold a get_dev_pagemap() reference
		 * here since we're in the driver I/O path and
		 * outstanding I/O requests pin the dev_pagemap.
		 */
		if (test_and_clear_pmem_poison(page))
			clear_mce_nospec(pfn);
	}
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void pmem_clear_bb(struct pmem_device *pmem, sector_t sector, long blks)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, sector=%llu, blks=%ld pid=%d\n", __func__, pmem, (unsigned long long)sector, blks, current->pid);
	if (blks == 0)
		return;
	badblocks_clear(&pmem->bb, sector, blks);
	if (pmem->bb_state)
		sysfs_notify_dirent(pmem->bb_state);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static long __pmem_clear_poison(struct pmem_device *pmem,
		phys_addr_t offset, unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, offset=%pa, len=%u pid=%d\n", __func__, pmem, &offset, len, current->pid);
	phys_addr_t phys = pmem_to_phys(pmem, offset);
	long cleared = nvdimm_clear_poison(to_dev(pmem), phys, len);

	if (cleared > 0) {
		pmem_mkpage_present(pmem, offset, cleared);
		arch_invalidate_pmem(pmem->virt_addr + offset, len);
	}
	printk(KERN_INFO "%s: EXIT: cleared=%ld pid=%d\n", __func__, cleared, current->pid);
	return cleared;
}

static blk_status_t pmem_clear_poison(struct pmem_device *pmem,
		phys_addr_t offset, unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, offset=%pa, len=%u pid=%d\n", __func__, pmem, &offset, len, current->pid);
	long cleared = __pmem_clear_poison(pmem, offset, len);

	if (cleared < 0) {
		printk(KERN_INFO "%s: EXIT: BLK_STS_IOERR pid=%d\n", __func__, current->pid);
		return BLK_STS_IOERR;
	}

	pmem_clear_bb(pmem, to_sect(pmem, offset), cleared >> SECTOR_SHIFT);
	if (cleared < len) {
		printk(KERN_INFO "%s: EXIT: BLK_STS_IOERR pid=%d\n", __func__, current->pid);
		return BLK_STS_IOERR;
	}
	printk(KERN_INFO "%s: EXIT: BLK_STS_OK pid=%d\n", __func__, current->pid);
	return BLK_STS_OK;
}

static void write_pmem(void *pmem_addr, struct page *page,
		unsigned int off, unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: pmem_addr=%p, page=%p, off=%u, len=%u pid=%d\n", __func__, pmem_addr, page, off, len, current->pid);
	unsigned int chunk;
	void *mem;

	while (len) {
		mem = kmap_atomic(page);
		chunk = min_t(unsigned int, len, PAGE_SIZE - off);
		memcpy_flushcache(pmem_addr, mem + off, chunk);
		kunmap_atomic(mem);
		len -= chunk;
		off = 0;
		page++;
		pmem_addr += chunk;
	}
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static blk_status_t read_pmem(struct page *page, unsigned int off,
		void *pmem_addr, unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: page=%p, off=%u, pmem_addr=%p, len=%u pid=%d\n", __func__, page, off, pmem_addr, len, current->pid);
	unsigned int chunk;
	unsigned long rem;
	void *mem;
	blk_status_t rc = BLK_STS_OK;

	while (len) {
		mem = kmap_atomic(page);
		chunk = min_t(unsigned int, len, PAGE_SIZE - off);
		rem = copy_mc_to_kernel(mem + off, pmem_addr, chunk);
		kunmap_atomic(mem);
		if (rem) {
			rc = BLK_STS_IOERR;
			break;
		}
		len -= chunk;
		off = 0;
		page++;
		pmem_addr += chunk;
	}
	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static blk_status_t pmem_do_read(struct pmem_device *pmem,
			struct page *page, unsigned int page_off,
			sector_t sector, unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, page=%p, page_off=%u, sector=%llu, len=%u pid=%d\n", __func__, pmem, page, page_off, (unsigned long long)sector, len, current->pid);
	blk_status_t rc;
	phys_addr_t pmem_off = to_offset(pmem, sector);
	void *pmem_addr = pmem->virt_addr + pmem_off;

	if (unlikely(is_bad_pmem(&pmem->bb, sector, len)))
		return BLK_STS_IOERR;

	rc = read_pmem(page, page_off, pmem_addr, len);
	flush_dcache_page(page);
	printk(KERN_INFO "%s: EXIT: rc=%d pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static blk_status_t pmem_do_write(struct pmem_device *pmem,
			struct page *page, unsigned int page_off,
			sector_t sector, unsigned int len)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, page=%p, page_off=%u, sector=%llu, len=%u pid=%d\n", __func__, pmem, page, page_off, (unsigned long long)sector, len, current->pid);
	phys_addr_t pmem_off = to_offset(pmem, sector);
	void *pmem_addr = pmem->virt_addr + pmem_off;

	if (unlikely(is_bad_pmem(&pmem->bb, sector, len))) {
		blk_status_t rc = pmem_clear_poison(pmem, pmem_off, len);

		if (rc != BLK_STS_OK) {
			printk(KERN_INFO "%s: EXIT: rc=%d (clear poison failed) pid=%d\n", __func__, rc, current->pid);
			return rc;
		}
	}

	flush_dcache_page(page);
	write_pmem(pmem_addr, page, page_off, len);

	printk(KERN_INFO "%s: EXIT: BLK_STS_OK pid=%d\n", __func__, current->pid);
	return BLK_STS_OK;
}

static void pmem_submit_bio(struct bio *bio)
{
	printk(KERN_INFO "%s: ENTRY: bio=%p pid=%d\n", __func__, bio, current->pid);
	int ret = 0;
	blk_status_t rc = 0;
	bool do_acct;
	unsigned long start;
	struct bio_vec bvec;
	struct bvec_iter iter;
	struct pmem_device *pmem = bio->bi_bdev->bd_disk->private_data;
	struct nd_region *nd_region = to_region(pmem);

	if (bio->bi_opf & REQ_PREFLUSH)
		ret = nvdimm_flush(nd_region, bio);

	do_acct = blk_queue_io_stat(bio->bi_bdev->bd_disk->queue);
	if (do_acct)
		start = bio_start_io_acct(bio);
	bio_for_each_segment(bvec, bio, iter) {
		if (op_is_write(bio_op(bio)))
			rc = pmem_do_write(pmem, bvec.bv_page, bvec.bv_offset,
				iter.bi_sector, bvec.bv_len);
		else
			rc = pmem_do_read(pmem, bvec.bv_page, bvec.bv_offset,
				iter.bi_sector, bvec.bv_len);
		if (rc) {
			bio->bi_status = rc;
			break;
		}
	}
	if (do_acct)
		bio_end_io_acct(bio, start);

	if (bio->bi_opf & REQ_FUA)
		ret = nvdimm_flush(nd_region, bio);

	if (ret)
		bio->bi_status = errno_to_blk_status(ret);

	bio_endio(bio);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

/* see "strong" declaration in tools/testing/nvdimm/pmem-dax.c */
__weak long __pmem_direct_access(struct pmem_device *pmem, pgoff_t pgoff,
		long nr_pages, enum dax_access_mode mode, void **kaddr,
		pfn_t *pfn)
{
	printk(KERN_INFO "%s: ENTRY: pmem=%p, pgoff=%lu, nr_pages=%ld, mode=%d, kaddr=%p, pfn=%p pid=%d\n", __func__, pmem, pgoff, nr_pages, mode, kaddr, pfn, current->pid);
	resource_size_t offset = PFN_PHYS(pgoff) + pmem->data_offset;
	sector_t sector = PFN_PHYS(pgoff) >> SECTOR_SHIFT;
	unsigned int num = PFN_PHYS(nr_pages) >> SECTOR_SHIFT;
	struct badblocks *bb = &pmem->bb;
	sector_t first_bad;
	sector_t num_bad;

	if (kaddr)
		*kaddr = pmem->virt_addr + offset;
	if (pfn)
		*pfn = phys_to_pfn_t(pmem->phys_addr + offset, pmem->pfn_flags);

	if (bb->count &&
	    badblocks_check(bb, sector, num, &first_bad, &num_bad)) {
		long actual_nr;

		if (mode != DAX_RECOVERY_WRITE)
			return -EHWPOISON;

		/*
		 * Set the recovery stride is set to kernel page size because
		 * the underlying driver and firmware clear poison functions
		 * don't appear to handle large chunk(such as 2MiB) reliably.
		 */
		actual_nr = PHYS_PFN(
			PAGE_ALIGN((first_bad - sector) << SECTOR_SHIFT));
		dev_dbg(pmem->bb.dev, "start sector(%llu), nr_pages(%ld), first_bad(%llu), actual_nr(%ld)\n",
				sector, nr_pages, first_bad, actual_nr);
		if (actual_nr)
			return actual_nr;
		return 1;
	}

	/*
	 * If badblocks are present but not in the range, limit known good range
	 * to the requested range.
	 */
	if (bb->count)
		return nr_pages;
	return PHYS_PFN(pmem->size - pmem->pfn_pad - offset);
}

static const struct block_device_operations pmem_fops = {
	.owner =		THIS_MODULE,
	.submit_bio =		pmem_submit_bio,
};

static int pmem_dax_zero_page_range(struct dax_device *dax_dev, pgoff_t pgoff,
				    size_t nr_pages)
{
	printk(KERN_INFO "%s: ENTRY: dax_dev=%p, pgoff=%lu, nr_pages=%zu pid=%d\n", __func__, dax_dev, pgoff, nr_pages, current->pid);
	struct pmem_device *pmem = dax_get_private(dax_dev);
	int ret = blk_status_to_errno(pmem_do_write(pmem, ZERO_PAGE(0), 0,
				   PFN_PHYS(pgoff) >> SECTOR_SHIFT,
				   PAGE_SIZE));
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static long pmem_dax_direct_access(struct dax_device *dax_dev,
		pgoff_t pgoff, long nr_pages, enum dax_access_mode mode,
		void **kaddr, pfn_t *pfn)
{
	printk(KERN_INFO "%s: ENTRY: dax_dev=%p, pgoff=%lu, nr_pages=%ld, mode=%d, kaddr=%p, pfn=%p pid=%d\n", __func__, dax_dev, pgoff, nr_pages, mode, kaddr, pfn, current->pid);
	struct pmem_device *pmem = dax_get_private(dax_dev);
	long ret = __pmem_direct_access(pmem, pgoff, nr_pages, mode, kaddr, pfn);
	printk(KERN_INFO "%s: EXIT: ret=%ld pid=%d\n", __func__, ret, current->pid);
	return ret;
}

/*
 * The recovery write thread started out as a normal pwrite thread and
 * when the filesystem was told about potential media error in the
 * range, filesystem turns the normal pwrite to a dax_recovery_write.
 *
 * The recovery write consists of clearing media poison, clearing page
 * HWPoison bit, re-enable page-wide read-write permission, flush the
 * caches and finally write.  A competing pread thread will be held
 * off during the recovery process since data read back might not be
 * valid, and this is achieved by clearing the badblock records after
 * the recovery write is complete. Competing recovery write threads
 * are already serialized by writer lock held by dax_iomap_rw().
 */
static size_t pmem_recovery_write(struct dax_device *dax_dev, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i)
{
	printk(KERN_INFO "%s: ENTRY: dax_dev=%p, pgoff=%lu, addr=%p, bytes=%zu, i=%p pid=%d\n", __func__, dax_dev, pgoff, addr, bytes, i, current->pid);
	struct pmem_device *pmem = dax_get_private(dax_dev);
	size_t olen, len, off;
	phys_addr_t pmem_off;
	struct device *dev = pmem->bb.dev;
	long cleared;

	off = offset_in_page(addr);
	len = PFN_PHYS(PFN_UP(off + bytes));
	if (!is_bad_pmem(&pmem->bb, PFN_PHYS(pgoff) >> SECTOR_SHIFT, len))
		return _copy_from_iter_flushcache(addr, bytes, i);

	/*
	 * Not page-aligned range cannot be recovered. This should not
	 * happen unless something else went wrong.
	 */
	if (off || !PAGE_ALIGNED(bytes)) {
		dev_dbg(dev, "Found poison, but addr(%p) or bytes(%#zx) not page aligned\n",
			addr, bytes);
		return 0;
	}

	pmem_off = PFN_PHYS(pgoff) + pmem->data_offset;
	cleared = __pmem_clear_poison(pmem, pmem_off, len);
	if (cleared > 0 && cleared < len) {
		dev_dbg(dev, "poison cleared only %ld out of %zu bytes\n",
			cleared, len);
		return 0;
	}
	if (cleared < 0) {
		dev_dbg(dev, "poison clear failed: %ld\n", cleared);
		return 0;
	}

	olen = _copy_from_iter_flushcache(addr, bytes, i);
	pmem_clear_bb(pmem, to_sect(pmem, pmem_off), cleared >> SECTOR_SHIFT);

	printk(KERN_INFO "%s: EXIT: olen=%zu pid=%d\n", __func__, olen, current->pid);
	return olen;
}

static const struct dax_operations pmem_dax_ops = {
	.direct_access = pmem_dax_direct_access,
	.zero_page_range = pmem_dax_zero_page_range,
	.recovery_write = pmem_recovery_write,
};

static ssize_t write_cache_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p pid=%d\n", __func__, dev, attr, buf, current->pid);
	struct pmem_device *pmem = dev_to_disk(dev)->private_data;
	ssize_t ret = sprintf(buf, "%d\n", !!dax_write_cache_enabled(pmem->dax_dev));
	printk(KERN_INFO "%s: EXIT: ret=%zd pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static ssize_t write_cache_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p, attr=%p, buf=%p, len=%zu pid=%d\n", __func__, dev, attr, buf, len, current->pid);
	struct pmem_device *pmem = dev_to_disk(dev)->private_data;
	bool write_cache;
	int rc = kstrtobool(buf, &write_cache);
	if (rc) {
		printk(KERN_INFO "%s: EXIT: rc=%d (kstrtobool failed) pid=%d\n", __func__, rc, current->pid);
		return rc;
	}
	dax_write_cache(pmem->dax_dev, write_cache);
	printk(KERN_INFO "%s: EXIT: len=%zu pid=%d\n", __func__, len, current->pid);
	return len;
}
static DEVICE_ATTR_RW(write_cache);

static umode_t dax_visible(struct kobject *kobj, struct attribute *a, int n)
{
	printk(KERN_INFO "%s: ENTRY: kobj=%p, a=%p, n=%d pid=%d\n", __func__, kobj, a, n, current->pid);
#ifndef CONFIG_ARCH_HAS_PMEM_API
	if (a == &dev_attr_write_cache.attr)
		return 0;
#endif
	printk(KERN_INFO "%s: EXIT: mode=%o pid=%d\n", __func__, a->mode, current->pid);
	return a->mode;
}

static struct attribute *dax_attributes[] = {
	&dev_attr_write_cache.attr,
	NULL,
};

static const struct attribute_group dax_attribute_group = {
	.name		= "dax",
	.attrs		= dax_attributes,
	.is_visible	= dax_visible,
};

static const struct attribute_group *pmem_attribute_groups[] = {
	&dax_attribute_group,
	NULL,
};

static void pmem_release_disk(void *__pmem)
{
	printk(KERN_INFO "%s: ENTRY: __pmem=%p pid=%d\n", __func__, __pmem, current->pid);
	struct pmem_device *pmem = __pmem;
	dax_remove_host(pmem->disk);
	kill_dax(pmem->dax_dev);
	put_dax(pmem->dax_dev);
	del_gendisk(pmem->disk);
	put_disk(pmem->disk);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static int pmem_pagemap_memory_failure(struct dev_pagemap *pgmap,
		unsigned long pfn, unsigned long nr_pages, int mf_flags)
{
	printk(KERN_INFO "%s: ENTRY: pgmap=%p, pfn=%lu, nr_pages=%lu, mf_flags=%d pid=%d\n", __func__, pgmap, pfn, nr_pages, mf_flags, current->pid);
	struct pmem_device *pmem = container_of(pgmap, struct pmem_device, pgmap);
	u64 offset = PFN_PHYS(pfn) - pmem->phys_addr - pmem->data_offset;
	u64 len = nr_pages << PAGE_SHIFT;
	int ret = dax_holder_notify_failure(pmem->dax_dev, offset, len, mf_flags);
	printk(KERN_INFO "%s: EXIT: ret=%d pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static const struct dev_pagemap_ops fsdax_pagemap_ops = {
	.memory_failure		= pmem_pagemap_memory_failure,
};

static int pmem_attach_disk(struct device *dev,
		struct nd_namespace_common *ndns)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p, ndns=%p pid=%d\n", __func__, dev, ndns, current->pid);
	struct nd_namespace_io *nsio = to_nd_namespace_io(&ndns->dev);
	struct nd_region *nd_region = to_nd_region(dev->parent);
	struct queue_limits lim = {
		.logical_block_size	= pmem_sector_size(ndns),
		.physical_block_size	= PAGE_SIZE,
		.max_hw_sectors		= UINT_MAX,
		.features		= BLK_FEAT_WRITE_CACHE |
					  BLK_FEAT_SYNCHRONOUS,
	};
	int nid = dev_to_node(dev), fua;
	struct resource *res = &nsio->res;
	struct range bb_range;
	struct nd_pfn *nd_pfn = NULL;
	struct dax_device *dax_dev;
	struct nd_pfn_sb *pfn_sb;
	struct pmem_device *pmem;
	struct gendisk *disk;
	void *addr;
	int rc;

	pmem = devm_kzalloc(dev, sizeof(*pmem), GFP_KERNEL);
	if (!pmem) {
		printk(KERN_INFO "%s: EXIT: -ENOMEM pid=%d\n", __func__, current->pid);
		return -ENOMEM;
	}

	rc = devm_namespace_enable(dev, ndns, nd_info_block_reserve());
	if (rc)
		return rc;

	/* while nsio_rw_bytes is active, parse a pfn info block if present */
	if (is_nd_pfn(dev)) {
		nd_pfn = to_nd_pfn(dev);
		rc = nvdimm_setup_pfn(nd_pfn, &pmem->pgmap);
		if (rc)
			return rc;
	}

	/* we're attaching a block device, disable raw namespace access */
	devm_namespace_disable(dev, ndns);

	dev_set_drvdata(dev, pmem);
	pmem->phys_addr = res->start;
	pmem->size = resource_size(res);
	fua = nvdimm_has_flush(nd_region);
	if (!IS_ENABLED(CONFIG_ARCH_HAS_UACCESS_FLUSHCACHE) || fua < 0) {
		dev_warn(dev, "unable to guarantee persistence of writes\n");
		fua = 0;
	}
	if (fua)
		lim.features |= BLK_FEAT_FUA;
	if (is_nd_pfn(dev) || pmem_should_map_pages(dev))
		lim.features |= BLK_FEAT_DAX;

	if (!devm_request_mem_region(dev, res->start, resource_size(res),
				dev_name(&ndns->dev))) {
		dev_warn(dev, "could not reserve region %pR\n", res);
		return -EBUSY;
	}

	disk = blk_alloc_disk(&lim, nid);
	if (IS_ERR(disk))
		return PTR_ERR(disk);

	pmem->disk = disk;
	pmem->pgmap.owner = pmem;
	pmem->pfn_flags = 0;
	if (is_nd_pfn(dev)) {
		pmem->pgmap.type = MEMORY_DEVICE_FS_DAX;
		pmem->pgmap.ops = &fsdax_pagemap_ops;
		addr = devm_memremap_pages(dev, &pmem->pgmap);
		pfn_sb = nd_pfn->pfn_sb;
		pmem->data_offset = le64_to_cpu(pfn_sb->dataoff);
		pmem->pfn_pad = resource_size(res) -
			range_len(&pmem->pgmap.range);
		bb_range = pmem->pgmap.range;
		bb_range.start += pmem->data_offset;
	} else if (pmem_should_map_pages(dev)) {
		pmem->pgmap.range.start = res->start;
		pmem->pgmap.range.end = res->end;
		pmem->pgmap.nr_range = 1;
		pmem->pgmap.type = MEMORY_DEVICE_FS_DAX;
		pmem->pgmap.ops = &fsdax_pagemap_ops;
		addr = devm_memremap_pages(dev, &pmem->pgmap);
		bb_range = pmem->pgmap.range;
	} else {
		addr = devm_memremap(dev, pmem->phys_addr,
				pmem->size, ARCH_MEMREMAP_PMEM);
		bb_range.start =  res->start;
		bb_range.end = res->end;
	}

	if (IS_ERR(addr)) {
		rc = PTR_ERR(addr);
		printk(KERN_INFO "%s: EXIT: PTR_ERR(addr)=%d pid=%d\n", __func__, rc, current->pid);
		goto out;
	}
	pmem->virt_addr = addr;

	disk->fops		= &pmem_fops;
	disk->private_data	= pmem;
	nvdimm_namespace_disk_name(ndns, disk->disk_name);
	set_capacity(disk, (pmem->size - pmem->pfn_pad - pmem->data_offset)
			/ 512);
	if (devm_init_badblocks(dev, &pmem->bb))
		return -ENOMEM;
	nvdimm_badblocks_populate(nd_region, &pmem->bb, &bb_range);
	disk->bb = &pmem->bb;

	dax_dev = alloc_dax(pmem, &pmem_dax_ops);
	if (IS_ERR(dax_dev)) {
		rc = PTR_ERR(dax_dev);
		if (rc != -EOPNOTSUPP) {
			printk(KERN_INFO "%s: EXIT: PTR_ERR(dax_dev)=%d pid=%d\n", __func__, rc, current->pid);
			goto out;
		}
	} else {
		set_dax_nocache(dax_dev);
		set_dax_nomc(dax_dev);
		if (is_nvdimm_sync(nd_region))
			set_dax_synchronous(dax_dev);
		pmem->dax_dev = dax_dev;
		rc = dax_add_host(dax_dev, disk);
		if (rc)
			goto out_cleanup_dax;
		dax_write_cache(dax_dev, nvdimm_has_cache(nd_region));
	}
	rc = device_add_disk(dev, disk, pmem_attribute_groups);
	if (rc)
		goto out_remove_host;
	if (devm_add_action_or_reset(dev, pmem_release_disk, pmem))
		return -ENOMEM;

	nvdimm_check_and_set_ro(disk);

	pmem->bb_state = sysfs_get_dirent(disk_to_dev(disk)->kobj.sd,
					  "badblocks");
	if (!pmem->bb_state)
		dev_warn(dev, "'badblocks' notification disabled\n");
	printk(KERN_INFO "%s: EXIT: 0 pid=%d\n", __func__, current->pid);
	return 0;

out_remove_host:
	dax_remove_host(pmem->disk);
out_cleanup_dax:
	kill_dax(pmem->dax_dev);
	put_dax(pmem->dax_dev);
out:
	put_disk(pmem->disk);
	printk(KERN_INFO "%s: EXIT: rc=%d (out label) pid=%d\n", __func__, rc, current->pid);
	return rc;
}

static int nd_pmem_probe(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	int ret;
	struct nd_namespace_common *ndns;

	ndns = nvdimm_namespace_common_probe(dev);
	if (IS_ERR(ndns)) {
		printk(KERN_INFO "%s: EXIT: PTR_ERR(ndns)=%ld pid=%d\n", __func__, PTR_ERR(ndns), current->pid);
		return PTR_ERR(ndns);
	}

	if (is_nd_btt(dev)) {
		ret = nvdimm_namespace_attach_btt(ndns);
		printk(KERN_INFO "%s: EXIT: ret=%d (is_nd_btt) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	if (is_nd_pfn(dev)) {
		ret = pmem_attach_disk(dev, ndns);
		printk(KERN_INFO "%s: EXIT: ret=%d (is_nd_pfn) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	ret = devm_namespace_enable(dev, ndns, nd_info_block_reserve());
	if (ret) {
		printk(KERN_INFO "%s: EXIT: ret=%d (devm_namespace_enable) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	ret = nd_btt_probe(dev, ndns);
	if (ret == 0) {
		printk(KERN_INFO "%s: EXIT: -ENXIO (nd_btt_probe==0) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	}

	/*
	 * We have two failure conditions here, there is no
	 * info reserver block or we found a valid info reserve block
	 * but failed to initialize the pfn superblock.
	 *
	 * For the first case consider namespace as a raw pmem namespace
	 * and attach a disk.
	 *
	 * For the latter, consider this a success and advance the namespace
	 * seed.
	 */
	ret = nd_pfn_probe(dev, ndns);
	if (ret == 0) {
		printk(KERN_INFO "%s: EXIT: -ENXIO (nd_pfn_probe==0) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	} else if (ret == -EOPNOTSUPP) {
		printk(KERN_INFO "%s: EXIT: ret=%d (nd_pfn_probe) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	ret = nd_dax_probe(dev, ndns);
	if (ret == 0) {
		printk(KERN_INFO "%s: EXIT: -ENXIO (nd_dax_probe==0) pid=%d\n", __func__, current->pid);
		return -ENXIO;
	} else if (ret == -EOPNOTSUPP) {
		printk(KERN_INFO "%s: EXIT: ret=%d (nd_dax_probe) pid=%d\n", __func__, ret, current->pid);
		return ret;
	}

	/* probe complete, attach handles namespace enabling */
	devm_namespace_disable(dev, ndns);

	ret = pmem_attach_disk(dev, ndns);
	printk(KERN_INFO "%s: EXIT: ret=%d (final attach) pid=%d\n", __func__, ret, current->pid);
	return ret;
}

static void nd_pmem_remove(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct pmem_device *pmem = dev_get_drvdata(dev);

	if (is_nd_btt(dev))
		nvdimm_namespace_detach_btt(to_nd_btt(dev));
	else {
		/*
		 * Note, this assumes device_lock() context to not
		 * race nd_pmem_notify()
		 */
		sysfs_put(pmem->bb_state);
		pmem->bb_state = NULL;
	}
	nvdimm_flush(to_nd_region(dev->parent), NULL);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nd_pmem_shutdown(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	nvdimm_flush(to_nd_region(dev->parent), NULL);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void pmem_revalidate_poison(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct nd_region *nd_region;
	resource_size_t offset = 0, end_trunc = 0;
	struct nd_namespace_common *ndns;
	struct nd_namespace_io *nsio;
	struct badblocks *bb;
	struct range range;
	struct kernfs_node *bb_state;

	if (is_nd_btt(dev)) {
		struct nd_btt *nd_btt = to_nd_btt(dev);

		ndns = nd_btt->ndns;
		nd_region = to_nd_region(ndns->dev.parent);
		nsio = to_nd_namespace_io(&ndns->dev);
		bb = &nsio->bb;
		bb_state = NULL;
	} else {
		struct pmem_device *pmem = dev_get_drvdata(dev);

		nd_region = to_region(pmem);
		bb = &pmem->bb;
		bb_state = pmem->bb_state;

		if (is_nd_pfn(dev)) {
			struct nd_pfn *nd_pfn = to_nd_pfn(dev);
			struct nd_pfn_sb *pfn_sb = nd_pfn->pfn_sb;

			ndns = nd_pfn->ndns;
			offset = pmem->data_offset +
					__le32_to_cpu(pfn_sb->start_pad);
			end_trunc = __le32_to_cpu(pfn_sb->end_trunc);
		} else {
			ndns = to_ndns(dev);
		}

		nsio = to_nd_namespace_io(&ndns->dev);
	}

	range.start = nsio->res.start + offset;
	range.end = nsio->res.end - end_trunc;
	nvdimm_badblocks_populate(nd_region, bb, &range);
	if (bb_state)
		sysfs_notify_dirent(bb_state);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void pmem_revalidate_region(struct device *dev)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p pid=%d\n", __func__, dev, current->pid);
	struct pmem_device *pmem;

	if (is_nd_btt(dev)) {
		struct nd_btt *nd_btt = to_nd_btt(dev);
		struct btt *btt = nd_btt->btt;

		nvdimm_check_and_set_ro(btt->btt_disk);
		return;
	}

	pmem = dev_get_drvdata(dev);
	nvdimm_check_and_set_ro(pmem->disk);
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

static void nd_pmem_notify(struct device *dev, enum nvdimm_event event)
{
	printk(KERN_INFO "%s: ENTRY: dev=%p, event=%d pid=%d\n", __func__, dev, event, current->pid);
	switch (event) {
	case NVDIMM_REVALIDATE_POISON:
		pmem_revalidate_poison(dev);
		break;
	case NVDIMM_REVALIDATE_REGION:
		pmem_revalidate_region(dev);
		break;
	default:
		dev_WARN_ONCE(dev, 1, "notify: unknown event: %d\n", event);
		break;
	}
	printk(KERN_INFO "%s: EXIT pid=%d\n", __func__, current->pid);
}

MODULE_ALIAS("pmem");
MODULE_ALIAS_ND_DEVICE(ND_DEVICE_NAMESPACE_IO);
MODULE_ALIAS_ND_DEVICE(ND_DEVICE_NAMESPACE_PMEM);
static struct nd_device_driver nd_pmem_driver = {
	.probe = nd_pmem_probe,
	.remove = nd_pmem_remove,
	.notify = nd_pmem_notify,
	.shutdown = nd_pmem_shutdown,
	.drv = {
		.name = "nd_pmem",
	},
	.type = ND_DRIVER_NAMESPACE_IO | ND_DRIVER_NAMESPACE_PMEM,
};

module_nd_driver(nd_pmem_driver);

MODULE_AUTHOR("Ross Zwisler <ross.zwisler@linux.intel.com>");
MODULE_DESCRIPTION("NVDIMM Persistent Memory Driver");
MODULE_LICENSE("GPL v2");
