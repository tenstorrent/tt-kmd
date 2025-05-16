// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include <linux/iommu.h>
#include <linux/file.h>
#include <linux/vmalloc.h>

#include "chardev_private.h"
#include "device.h"
#include "memory.h"
#include "ioctl.h"
#include "sg_helpers.h"
#include "tlb.h"

#define BAR0_SIZE (1UL << 29)

static int get_sorted_iatu_region_indices(const struct tenstorrent_outbound_iatu_region *regions, int *sorted_indices)
{
	int i;
	int in_use_count = 0;

	// First, collect indices of in-use regions.
	for (i = 0; i < TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS; i++) {
		if (regions[i].priv) {
			sorted_indices[in_use_count++] = i;
		}
	}

	// Insertion sort the collected indices by the corresponding region's base.
	for (i = 1; i < in_use_count; i++) {
		int index = sorted_indices[i];
		u64 base = regions[index].base;
		int j = i - 1;

		while (j >= 0 && regions[sorted_indices[j]].base > base) {
			sorted_indices[j + 1] = sorted_indices[j];
			j--;
		}
		sorted_indices[j + 1] = index;
	}

	return in_use_count;
}

static u64 find_iatu_region_top_down(const struct tenstorrent_outbound_iatu_region *regions, u64 max_addr, u64 size)
{
	int sorted_indices[TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS];
	u64 current_pos = max_addr;
	int in_use_count;
	int i;

	in_use_count = get_sorted_iatu_region_indices(regions, sorted_indices);

	if (in_use_count == 0) {
		// Allocate at top if there's enough space.
		if (size <= (max_addr + 1)) {
			return max_addr - size + 1;
		}
		return U64_MAX; // Size too large for address space.
	}

	// Check each region from top to bottom.
	for (i = in_use_count - 1; i >= 0; i--) {
		const struct tenstorrent_outbound_iatu_region *region = &regions[sorted_indices[i]];

		if ((current_pos - region->limit) >= size)
			return current_pos - size + 1;

		current_pos = region->base - 1;
	}

	// Check gap at the bottom (from 0 to the lowest region).
	if ((current_pos + 1) >= size)
		return current_pos - size + 1;

	return U64_MAX; // No suitable gap found.
}

static u64 find_iatu_region_bottom_up(const struct tenstorrent_outbound_iatu_region *regions, u64 max_addr, u64 size)
{
	int sorted_indices[TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS];
	u64 current_pos = 0;
	int in_use_count;
	int i;

	in_use_count = get_sorted_iatu_region_indices(regions, sorted_indices);

	if (in_use_count == 0) {
		// Allocate at bottom if there's enough space.
		if (size <= max_addr + 1) {
			return 0;
		}
		return U64_MAX;
	}

	// Check each region from bottom to top.
	for (i = 0; i < in_use_count; i++) {
		const struct tenstorrent_outbound_iatu_region *region = &regions[sorted_indices[i]];

		if ((region->base - current_pos) >= size)
			return current_pos;

		current_pos = region->limit + 1;
	}

	// Check gap at the top (from highest region to max_addr).
	if ((max_addr - current_pos + 1) >= size)
		return current_pos;

	return U64_MAX; // No suitable gap found.
}

// returns the region number or a negative error code.
static int configure_outbound_iatu(struct chardev_private *priv, u64 base, u64 limit, u64 target)
{
	struct tenstorrent_device *tt_dev = priv->device;
	int region = -1;
	int i;
	int ret;

	if (base > limit)
		return -EINVAL;

	// Find a free region.
	for (i = 0; i < TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS; ++i) {
		if (tt_dev->outbound_iatus[i].priv == NULL) {
			region = i;
			break;
		}
	}

	if (region < 0)
		return -ENOSPC;

	// Program the hardware.
	ret = tt_dev->dev_class->configure_outbound_atu(tt_dev, region, base, limit, target);
	if (ret)
		return ret;

	// Mark region as in use.
	tt_dev->outbound_iatus[region].priv = priv;
	tt_dev->outbound_iatus[region].base = base;
	tt_dev->outbound_iatus[region].limit = limit;
	tt_dev->outbound_iatus[region].target = target;

	return region;
}

// Return the iATU region number or a negative error code.
static int setup_noc_dma(struct chardev_private *priv, bool top_down, size_t size, u64 target, u64 *noc_address)
{
	struct tenstorrent_device *tt_dev = priv->device;
	u64 max_addr = tt_dev->dev_class->noc_dma_limit;
	u64 base;
	u64 limit;
	int iatu_region;

	if (size == 0)
		return -EINVAL;

	mutex_lock(&tt_dev->iatu_mutex);
	if (top_down)
		base = find_iatu_region_top_down(tt_dev->outbound_iatus, max_addr, size);
	else
		base = find_iatu_region_bottom_up(tt_dev->outbound_iatus, max_addr, size);

	if (base == U64_MAX) {
		mutex_unlock(&tt_dev->iatu_mutex);
		return -ENOMEM;
	}

	limit = base + size - 1;
	iatu_region = configure_outbound_iatu(priv, base, limit, target);
	*noc_address = tt_dev->dev_class->noc_pcie_offset + base;

	mutex_unlock(&tt_dev->iatu_mutex);
	return iatu_region;
}

// In Linux 5.0, dma_alloc_coherent always zeroes memory and dma_zalloc_coherent
// was removed.
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#define dma_alloc_coherent dma_zalloc_coherent
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// vma array allocation removed in 52650c8b466bac399aec213c61d74bfe6f7af1a4.
	return pin_user_pages_fast(start, nr_pages, gup_flags | FOLL_LONGTERM, pages);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// Can't use pin_user_pages_fast(FOLL_LONGTERM) because it calls __gup_longterm_locked with vmas = NULL
	// which allocates a contiguous vmas array and that fails often.

	int ret;

	struct vm_area_struct **vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *), GFP_KERNEL);
	if (vmas == NULL)
		return -ENOMEM;

	ret = pin_user_pages(start, nr_pages, gup_flags | FOLL_LONGTERM, pages, vmas);

	kvfree(vmas);
	return ret;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// Can't use get_user_pages_fast(FOLL_LONGTERM) because it calls __gup_longterm_locked with vmas = NULL
	// which allocates a contiguous vmas array and that fails often.

	int ret;

	struct vm_area_struct **vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *), GFP_KERNEL);
	if (vmas == NULL)
		return -ENOMEM;

	ret = get_user_pages(start, nr_pages, gup_flags | FOLL_LONGTERM, pages, vmas);

	kvfree(vmas);
	return ret;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 4)
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	int ret;

	// If we don't pass in vmas, get_user_pages_longterm will allocate it in contiguous memory and that fails often.
	struct vm_area_struct **vmas = kvmalloc_array(nr_pages, sizeof(struct vm_area_struct *), GFP_KERNEL);
	if (vmas == NULL)
		return -ENOMEM;

	down_read(&current->mm->mmap_sem);
	ret = get_user_pages_longterm(start, nr_pages, gup_flags, pages, vmas);
	up_read(&current->mm->mmap_sem);

	kvfree(vmas);
	return ret;
}
#else
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	// Kernels this old don't know about long-term pinning, so they don't allocate the vmas array.
	return get_user_pages_fast(start, nr_pages, gup_flags, pages);
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
// unpin_user_pages_dirty_lock is provided by the kernel.
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	put_user_pages_dirty_lock(pages, npages, make_dirty);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	if (make_dirty)
		put_user_pages_dirty_lock(pages, npages);
	else
		put_user_pages(pages, npages);
}
#else
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	struct page **end = pages + npages;
	for (; pages != end; pages++) {
		if (make_dirty)
			set_page_dirty_lock(*pages);
		put_page(*pages);
	}
}
#endif

#define MAX_DMA_BUF_SIZE (1u << MAX_DMA_BUF_SIZE_LOG2)

// These are the mmap offsets for various resources. In the user-kernel
// interface they are dynamic (TENSTORRENT_IOCTL_QUERY_MAPPINGS and
// TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF), but they are actually hard-coded.
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 36)
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 36)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 36)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 36)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 36)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 36)
#define MMAP_OFFSET_TLB_UC		(U64_C(6) << 36)
#define MMAP_OFFSET_TLB_WC		(U64_C(7) << 36)

#define MMAP_RESOURCE_SIZE (U64_C(1) << 36)

// tenstorrent_allocate_dma_buf_in.buf_index is u8 so that sets a limit of
// U8_MAX DMA buffers per fd. 32-bit mmap offsets are divided by PAGE_SIZE,
// so PAGE_SIZE << 32 is the largest possible offset.
#define MMAP_OFFSET_DMA_BUF		((u64)(PAGE_SIZE-U8_MAX-1) << 32)

#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)

struct pinned_page_range {
	struct list_head list;

	unsigned long page_count;
	struct page **pages;	// vmalloc/vfree

	struct sg_table dma_mapping;	// alloc_chained_sgt_for_pages / free_chained_sgt
	u64 virtual_address;

	int outbound_iatu_region;
};

static void unpin_pinned_page_range(struct chardev_private *priv,
	struct pinned_page_range *pinning)
{
	if (pinning->outbound_iatu_region >= 0) {
		struct tenstorrent_device *tt_dev = priv->device;
		struct tenstorrent_outbound_iatu_region *region;
		mutex_lock(&tt_dev->iatu_mutex);

		region = &priv->device->outbound_iatus[pinning->outbound_iatu_region];
		tt_dev->dev_class->configure_outbound_atu(tt_dev, pinning->outbound_iatu_region, 0, 0, 0);

		region->priv = NULL;
		region->base = 0;
		region->limit = 0;
		region->target = 0;

		mutex_unlock(&tt_dev->iatu_mutex);
	}

	dma_unmap_sgtable(&priv->device->pdev->dev, &pinning->dma_mapping, DMA_BIDIRECTIONAL, 0);
	free_chained_sgt(&pinning->dma_mapping);

	unpin_user_pages_dirty_lock(pinning->pages, pinning->page_count, true);
	vfree(pinning->pages);

	list_del(&pinning->list);
	kfree(pinning);
}

struct peer_resource_mapping {
	struct list_head list;

	dma_addr_t mapped_address;
	size_t size;
};

// This replaces tenstorrent_query_mappings from ioctl.h with a version
// that uses a flexible array member rather than a zero-length array.
// This keeps UBSAN from triggering when we write the output mappings.
struct tenstorrent_query_mappings_flex {
	struct tenstorrent_query_mappings_in in;
	struct tenstorrent_mapping out_mappings[];
};

long ioctl_query_mappings(struct chardev_private *priv,
			  struct tenstorrent_query_mappings __user *arg_)
{
	struct tenstorrent_query_mappings_flex __user *arg = (struct tenstorrent_query_mappings_flex __user *)arg_;

	struct tenstorrent_mapping mappings[6];
	struct tenstorrent_mapping *next_mapping;

	u32 valid_mappings_to_copy;
	u32 extra_mappings_to_clear;
	u32 valid_mappings;

	resource_size_t resource_len;

	struct tenstorrent_query_mappings_in in;
	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	memset(mappings, 0, sizeof(mappings));
	next_mapping = mappings;

	resource_len = pci_resource_len(priv->device->pdev, 0);
	if (resource_len > 0) {
		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE0_UC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE0_UC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;

		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE0_WC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE0_WC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;
	}

	resource_len = pci_resource_len(priv->device->pdev, 2);
	if (resource_len > 0) {
		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE1_UC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE1_UC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;

		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE1_WC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE1_WC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;
	}

	resource_len = pci_resource_len(priv->device->pdev, 4);
	if (resource_len > 0) {
		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE2_UC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE2_UC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;

		next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE2_WC;
		next_mapping->mapping_base = MMAP_OFFSET_RESOURCE2_WC;
		next_mapping->mapping_size = resource_len;
		next_mapping++;
	}

	valid_mappings = next_mapping - mappings;

	valid_mappings_to_copy = min(in.output_mapping_count, valid_mappings);
	extra_mappings_to_clear = (in.output_mapping_count > valid_mappings)
		? in.output_mapping_count - valid_mappings : 0;

	if (U32_MAX / sizeof(struct tenstorrent_mapping) < extra_mappings_to_clear)
		return -EFAULT;

	if (copy_to_user(&arg->out_mappings, &mappings,
			 valid_mappings_to_copy * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	if (clear_user(&arg->out_mappings[valid_mappings_to_copy],
		       extra_mappings_to_clear * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	return 0;
}

static struct dmabuf *lookup_dmabuf_by_index(struct chardev_private *priv, u8 buf_index) {
	struct dmabuf *dmabuf;

	hash_for_each_possible(priv->dmabufs, dmabuf, hash_chain, buf_index)
		if (dmabuf->index == buf_index)
			return dmabuf;

	return NULL;
}

static u64 dmabuf_mapping_start(u8 buf_index) {
	return MMAP_OFFSET_DMA_BUF + buf_index * MMAP_SIZE_DMA_BUF;
}

long ioctl_allocate_dma_buf(struct chardev_private *priv,
			    struct tenstorrent_allocate_dma_buf __user *arg)
{
	dma_addr_t dma_handle = 0;
	void *dma_buf_kernel_ptr;
	struct dmabuf *dmabuf;
	long ret = 0;

	struct tenstorrent_allocate_dma_buf_in in;
	struct tenstorrent_allocate_dma_buf_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (!priv->device->dma_capable)
		return -EINVAL;

	if (in.buf_index >= TENSTORRENT_MAX_DMA_BUFS)
		return -EINVAL;

	if (in.requested_size % PAGE_SIZE != 0
	    || in.requested_size == 0
	    || in.requested_size > MAX_DMA_BUF_SIZE)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	if (lookup_dmabuf_by_index(priv, in.buf_index)) {
		ret = -EINVAL;
		goto out;
	}

	dmabuf = kzalloc(sizeof(*dmabuf), GFP_KERNEL);
	if (!dmabuf) {
		ret = -ENOMEM;
		goto out;
	}

	dma_buf_kernel_ptr = dma_alloc_coherent(&priv->device->pdev->dev,
						in.requested_size,
						&dma_handle, GFP_KERNEL);

	if (dma_buf_kernel_ptr == NULL) {
		kfree(dmabuf);
		ret = -ENOMEM;
		goto out;
	}

	dmabuf->index = in.buf_index;
	dmabuf->ptr = dma_buf_kernel_ptr;
	dmabuf->phys = dma_handle;
	dmabuf->size = in.requested_size;

	out.physical_address = (u64)dmabuf->phys;
	out.mapping_offset = dmabuf_mapping_start(in.buf_index);
	out.size = in.requested_size;

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0) {
		dma_free_coherent(&priv->device->pdev->dev, dmabuf->size,
				  dmabuf->ptr, dmabuf->phys);

		kfree(dmabuf);
		ret = -EFAULT;
		goto out;
	}

	hash_add(priv->dmabufs, &dmabuf->hash_chain, dmabuf->index);

out:
	mutex_unlock(&priv->mutex);
	return ret;
}

long ioctl_free_dma_buf(struct chardev_private *priv,
			struct tenstorrent_free_dma_buf __user *arg)
{
	// This is unsupported until I figure out how to block freeing as long
	// as a mapping exists. Otherwise the dma buffer is freed when the
	// struct file is destroyed, and that's safe because the mapping
	// refcounts the file.
	return -EINVAL;
}


static bool is_iommu_translated(struct device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	return domain && domain->type != IOMMU_DOMAIN_IDENTITY;
}

static bool is_pin_pages_size_safe(u64 size)
{
	// With IOMMU enabled on 5.4, 2GB pinnings may succeed, but then soft lockup on process exit.
	// (tt_cdev_release -> unmap_sg -> __unmap_single -> iommu_unmap_page)
	// This doesn't happen in 5.15, but I don't know exactly when it was fixed.
#if LINUX_VERSION_CODE <= KERNEL_VERSION(5, 4, 0)
	return size <= 1 << 30;
#else
	return true;
#endif
}

long ioctl_pin_pages(struct chardev_private *priv,
		     struct tenstorrent_pin_pages __user *arg)
{
	const u32 valid_flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS | TENSTORRENT_PIN_PAGES_NOC_DMA |
				TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN;
	unsigned long nr_pages;
	struct page **pages;
	int pages_pinned;
	struct pinned_page_range *pinning;
	struct sg_table dma_mapping = {0};
	long ret;
	u32 bytes_to_copy;
	u64 noc_address = 0;
	int iatu_region = -1;
	bool top_down = false;

	struct tenstorrent_pin_pages_in in;
	struct tenstorrent_pin_pages_out_extended out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (in.flags & ~valid_flags)
		return -EINVAL;

	if (!PAGE_ALIGNED(in.virtual_address) || !PAGE_ALIGNED(in.size) || in.size == 0)
		return -EINVAL;

	if (!is_pin_pages_size_safe(in.size))
		return -EINVAL;

	top_down = in.flags & TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN;

	mutex_lock(&priv->mutex);

	// Block duplicate (VA/size) pinnings. Prevents ambiguity in UNPIN_PAGES
	// regarding iATU teardown if the same range were pinned multiple times with
	// different NOC_DMA flags.
	list_for_each_entry(pinning, &priv->pinnings, list) {
		if (pinning->virtual_address == in.virtual_address &&
		    pinning->page_count == PAGE_ALIGN(in.size) >> PAGE_SHIFT) {
			mutex_unlock(&priv->mutex);
			return -EEXIST;
		}
	}

	pinning = kmalloc(sizeof(*pinning), GFP_KERNEL);
	if (!pinning) {
		mutex_unlock(&priv->mutex);
		return -ENOMEM;
	}

	nr_pages = PAGE_ALIGN(in.size) >> PAGE_SHIFT;
	pages = vzalloc(nr_pages * sizeof(struct page *));
	if (!pages) {
		pr_err("vzalloc failed for %lu page pointers\n", nr_pages);
		ret = -ENOMEM;
		goto err_free_pinning;
	}

	pages_pinned = pin_user_pages_fast_longterm(in.virtual_address, nr_pages, FOLL_WRITE, pages);
	if (pages_pinned < 0) {
		pr_warn("pin_user_pages_longterm failed: %d\n", pages_pinned);
		ret = pages_pinned;
		goto err_vfree_pages;
	}

	if (pages_pinned != nr_pages) {
		pr_err("could only pin %d of %lu pages\n", pages_pinned, nr_pages);
		ret = -EINVAL;
		goto err_unpin_pages;
	}

	if (is_iommu_translated(&priv->device->pdev->dev)) {
		struct scatterlist *sg;
		unsigned int i;
		dma_addr_t expected_next_address;
		unsigned long total_dma_len = 0;

		if (!alloc_chained_sgt_for_pages(&dma_mapping, pages, nr_pages)) {
			pr_warn("alloc_chained_sgt_for_pages failed for %lu pages, probably out of memory.\n", nr_pages);
			ret = -ENOMEM;
			goto err_unpin_pages;
		}

		ret = dma_map_sgtable(&priv->device->pdev->dev, &dma_mapping, DMA_BIDIRECTIONAL, 0);

		if (ret != 0) {
			pr_err("dma_map_sg failed.\n");
			goto err_unlock_priv;
		}

		// This can only happen due to a misconfiguration or a bug.
		for_each_sgtable_dma_sg((&dma_mapping), sg, i) {
			if (i > 0 && sg_dma_address(sg) != expected_next_address) {
				pr_err("discontiguous mapping\n");
				ret = -EINVAL;
			}

			expected_next_address = sg_dma_address(sg) + sg_dma_len(sg);
			total_dma_len += sg_dma_len(sg);
		}

		if (total_dma_len != nr_pages * PAGE_SIZE) {
			pr_err("dma-mapped (%lX) != original length (%lX).\n", total_dma_len, nr_pages * PAGE_SIZE);
			ret = -EINVAL;
		}

		if (ret != 0) {
			debug_print_sgtable(&dma_mapping);
			goto err_dma_unmap;
		}

		out.physical_address = sg_dma_address(dma_mapping.sgl);

		if (in.flags & TENSTORRENT_PIN_PAGES_NOC_DMA) {
			ret = setup_noc_dma(priv, top_down, in.size, out.physical_address, &noc_address);

			if (ret < 0)
				goto err_dma_unmap;
			iatu_region = ret;
		}
	} else {
		int i;

		for (i = 1; i < pages_pinned; i++) {
			if (page_to_pfn(pages[i]) != page_to_pfn(pages[i-1]) + 1) {
				pr_err("pages discontiguous at %d\n", i);
				ret = -EINVAL;
				goto err_unpin_pages;
			}
		}

		out.physical_address = page_to_phys(pages[0]);

		if (in.flags & TENSTORRENT_PIN_PAGES_NOC_DMA) {
			ret = setup_noc_dma(priv, top_down, in.size, out.physical_address, &noc_address);

			if (ret < 0)
				goto err_unpin_pages;
			iatu_region = ret;
		}
	}

	pinning->page_count = nr_pages;
	pinning->pages = pages;
	pinning->dma_mapping = dma_mapping;
	pinning->virtual_address = in.virtual_address;
	pinning->outbound_iatu_region = iatu_region;

	list_add(&pinning->list, &priv->pinnings);
	mutex_unlock(&priv->mutex);

	out.noc_address = noc_address;
	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;

err_dma_unmap:
	dma_unmap_sgtable(&priv->device->pdev->dev, &dma_mapping, DMA_BIDIRECTIONAL, 0);
err_unlock_priv:
	free_chained_sgt(&dma_mapping);
err_unpin_pages:
	unpin_user_pages_dirty_lock(pages, pages_pinned, false);
err_vfree_pages:
	vfree(pages);
err_free_pinning:
	kfree(pinning);
	mutex_unlock(&priv->mutex);
	return ret;
}

long ioctl_unpin_pages(struct chardev_private *priv,
		       struct tenstorrent_unpin_pages __user *arg)
{
	struct tenstorrent_unpin_pages_in in = {0};
	struct pinned_page_range *pinning, *tmp_pinning;
	unsigned long nr_pages;
	long ret = -EINVAL;

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	nr_pages = in.size >> PAGE_SHIFT;

	if (in.reserved != 0 || in.size == 0 || nr_pages == 0)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	list_for_each_entry_safe(pinning, tmp_pinning, &priv->pinnings, list) {
		if (pinning->virtual_address != in.virtual_address)
			continue;

		if (pinning->page_count != nr_pages) {
			ret = -EINVAL;
			break;
		}

		unpin_pinned_page_range(priv, pinning);
		ret = 0;
		break;
	}

	mutex_unlock(&priv->mutex);
	return ret;
}

long ioctl_map_peer_bar(struct chardev_private *priv,
			struct tenstorrent_map_peer_bar __user *arg) {

	struct file *peer_file;
	struct chardev_private *peer_priv;
	struct peer_resource_mapping *peer_mapping;
	resource_size_t resource_len;
	phys_addr_t phys_addr;
	dma_addr_t mapping;
	int ret;

	struct tenstorrent_map_peer_bar_in in;
	struct tenstorrent_map_peer_bar_out out;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (in.flags != 0)
		return -EINVAL;

	if (in.peer_bar_index >= PCI_NUM_RESOURCES)
		return -EINVAL;

	if (in.peer_bar_length == 0)
		return -EINVAL;

	peer_file = fget(in.peer_fd);
	if (!peer_file)
		return -EBADF;

	peer_priv = get_tenstorrent_priv(peer_file);
	if (!peer_priv) {
		ret = -EINVAL;
		goto err_fput;
	}

	if (peer_priv->device == priv->device) {
		ret = -EINVAL;
		goto err_fput;
	}

	if (peer_priv->device->dev_class != priv->device->dev_class) {
		ret = -EINVAL;
		goto err_fput;
	}

	peer_mapping = kmalloc(sizeof(*peer_mapping), GFP_KERNEL);
	if (!peer_mapping) {
		ret = -ENOMEM;
		goto err_fput;
	}

	// Avoid deadlocks on concurrent calls to IOCTL_MAP_PEER_BAR
	// by locking in a globally-consistent order.
	if (priv->device < peer_priv->device) {
		mutex_lock(&priv->mutex);
		mutex_lock(&peer_priv->mutex);
	} else {
		mutex_lock(&peer_priv->mutex);
		mutex_lock(&priv->mutex);
	}

	resource_len = pci_resource_len(peer_priv->device->pdev, in.peer_bar_index);
	if (in.peer_bar_offset >= resource_len || in.peer_bar_length > resource_len - in.peer_bar_offset) {
		ret = -EINVAL;
		goto err_unlock;
	}

	phys_addr = pci_resource_start(peer_priv->device->pdev, in.peer_bar_index) + in.peer_bar_offset;

	mapping = dma_map_resource(&priv->device->pdev->dev, phys_addr, in.peer_bar_length, DMA_BIDIRECTIONAL, 0);
	ret = dma_mapping_error(&priv->device->pdev->dev, mapping);
	if (ret != 0)
		goto err_unlock;

	peer_mapping->mapped_address = mapping;
	peer_mapping->size = in.peer_bar_length;

	list_add(&peer_mapping->list, &priv->peer_mappings);

	mutex_unlock(&priv->mutex);
	mutex_unlock(&peer_priv->mutex);

	fput(peer_file);

	out.dma_address = mapping;

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0)
		return -EFAULT;

	return 0;

err_unlock:
	mutex_unlock(&priv->mutex);
	mutex_unlock(&peer_priv->mutex);

	kfree(peer_mapping);

err_fput:
	fput(peer_file);

	return ret;
}

long ioctl_allocate_tlb(struct chardev_private *priv,
			struct tenstorrent_allocate_tlb __user *arg) {
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_allocate_tlb_in in = {0};
	struct tenstorrent_allocate_tlb_out out = {0};
	struct tlb_descriptor tlb_desc = { 0 };
	size_t size;
	int id;
	u64 encoded_id;

	if (!tt_dev->dev_class->describe_tlb)
		return -EINVAL;

	if (copy_from_user(&in, &arg->in, sizeof(in)))
		return -EFAULT;

	size = in.size;
	id = tenstorrent_device_allocate_tlb(tt_dev, &size);

	if (id < 0)
		return id;

	if (tt_dev->dev_class->describe_tlb(tt_dev, id, &tlb_desc)) {
		tenstorrent_device_free_tlb(tt_dev, id);
		return -EINVAL;
	}

	// TLB windows only exist in BAR0 (GS/WH/BH) and BAR4 (BH).
	if (tlb_desc.bar != 0 && tlb_desc.bar != 4) {
		tenstorrent_device_free_tlb(tt_dev, id);
		return -EINVAL;
	}

	out.id = id;

	// mmap offsets match the offsets of the TLB windows in BAR0, with one
	// exception: the mmap offsets for the 4G windows in Blackhole BAR4 begin
	// at 512M, i.e. the size of BAR0.
	encoded_id = tlb_desc.bar_offset;
	if (tlb_desc.bar == 4)
		encoded_id += BAR0_SIZE;

	out.mmap_offset_uc = MMAP_OFFSET_TLB_UC + encoded_id;
	out.mmap_offset_wc = MMAP_OFFSET_TLB_WC + encoded_id;

	if (copy_to_user(&arg->out, &out, sizeof(out))) {
		tenstorrent_device_free_tlb(tt_dev, id);
		return -EFAULT;
	}

	set_bit(id, priv->tlbs);

	return 0;
}

long ioctl_free_tlb(struct chardev_private *priv, struct tenstorrent_free_tlb __user *arg) {
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_free_tlb_in in = {0};
	int ret;

	if (copy_from_user(&in, &arg->in, sizeof(in)))
		return -EFAULT;

	if (in.id < 0 || in.id >= TENSTORRENT_MAX_INBOUND_TLBS)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	if (!test_bit(in.id, priv->tlbs)) {
		ret = -EPERM;
		goto unlock;
	}

	if (atomic_read(&tt_dev->tlb_refs[in.id]) > 0) {
		ret = -EBUSY;
		goto unlock;
	}

	clear_bit(in.id, priv->tlbs);
	ret = tenstorrent_device_free_tlb(tt_dev, in.id);

unlock:
	mutex_unlock(&priv->mutex);
	return ret;
}

long ioctl_configure_tlb(struct chardev_private *priv,
			 struct tenstorrent_configure_tlb __user *arg) {
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_configure_tlb_in in = {0};

	if (copy_from_user(&in, &arg->in, sizeof(in)))
		return -EFAULT;

	if (in.id < 0 || in.id >= TENSTORRENT_MAX_INBOUND_TLBS)
		return -EINVAL;

	if (!test_bit(in.id, priv->tlbs))
		return -EPERM;

	return tenstorrent_device_configure_tlb(tt_dev, in.id, &in.config);
}

// Is the mapping target range contained entirely with start - start+len?
// start and len must be page-aligned.
static bool vma_target_range(struct vm_area_struct *vma, u64 start, resource_size_t len)
{
	unsigned long mapping_len_pg = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	unsigned long mapping_end_pg = vma->vm_pgoff + mapping_len_pg;

	if (vma->vm_pgoff >= start >> PAGE_SHIFT
	    && mapping_end_pg <= (start + len) >> PAGE_SHIFT) {
		vma->vm_pgoff -= start >> PAGE_SHIFT;
		return true;
	} else {
		return false;
	}
}

static struct dmabuf *vma_dmabuf_target(struct chardev_private *priv,
					struct vm_area_struct *vma) {
	unsigned long dmabuf_index;
	struct dmabuf *dmabuf;

	if (vma->vm_pgoff < MMAP_OFFSET_DMA_BUF >> PAGE_SHIFT)
		// Not in DMA buffer offset range (too low).
		return NULL;

	dmabuf_index = (vma->vm_pgoff - (MMAP_OFFSET_DMA_BUF >> PAGE_SHIFT)) / (MMAP_SIZE_DMA_BUF >> PAGE_SHIFT);
	if (dmabuf_index >= TENSTORRENT_MAX_DMA_BUFS)
		// Not in DMA buffer offset range (too high).
		return NULL;

	dmabuf = lookup_dmabuf_by_index(priv, dmabuf_index);
	if (!dmabuf)
		// No allocated DMA buffer for that index.
		return NULL;

	if (vma_target_range(vma, dmabuf_mapping_start(dmabuf_index), dmabuf->size))
		return dmabuf;
	else
		// Allocated DMA buffer does not cover requested size.
		return NULL;
}

static int map_pci_bar(struct pci_dev *pdev, struct vm_area_struct *vma, unsigned int bar)
{
	resource_size_t bar_start = pci_resource_start(pdev, bar);
	resource_size_t bar_len = pci_resource_len(pdev, bar);

	return vm_iomap_memory(vma, bar_start, bar_len);
}

static void tlb_vma_open(struct vm_area_struct *vma)
{
	struct chardev_private *priv;
	struct tenstorrent_device *tt_dev;
	unsigned int id;

	if (!vma->vm_file)
		return;

	priv = vma->vm_file->private_data;
	if (!priv)
		return;

	tt_dev = priv->device;
	id = (int)((uintptr_t)vma->vm_private_data);

	if (id >= TENSTORRENT_MAX_INBOUND_TLBS)
		return;

	atomic_inc(&tt_dev->tlb_refs[id]);
}

static void tlb_vma_close(struct vm_area_struct *vma)
{
	struct chardev_private *priv;
	struct tenstorrent_device *tt_dev;
	unsigned int id;

	if (!vma->vm_file)
		return;

	priv = vma->vm_file->private_data;
	if (!priv)
		return;

	tt_dev = priv->device;
	id = (int)((uintptr_t)vma->vm_private_data);

	if (id >= TENSTORRENT_MAX_INBOUND_TLBS)
		return;

	if (atomic_dec_if_positive(&tt_dev->tlb_refs[id]) < 0)
		pr_err("vma_close: negative refcount\n");	// Should never happen
}

static int tlb_vma_may_split(struct vm_area_struct *vma, unsigned long address)
{
	// Forbid splitting TLB windows.
	return -EINVAL;
}

static const struct vm_operations_struct tlb_vm_ops = {
	.open = tlb_vma_open,
	.close = tlb_vma_close,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
	.may_split = tlb_vma_may_split,
#else
	.split = tlb_vma_may_split,
#endif
};

static int map_tlb_window(struct chardev_private *priv, struct vm_area_struct *vma)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct tlb_descriptor tlb_desc = {0};
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long pfn;
	bool bar4 = offset >= BAR0_SIZE;
	phys_addr_t bar_start;
	int i;
	int id = -1;
	int ret = 0;
	u64 total_tlbs = 0;

	if (!tt_dev->dev_class->describe_tlb)
		return -EINVAL;

	if (tt_dev->dev_class->tlb_kinds == 0)
		return -EINVAL;

	for (i = 0; i < tt_dev->dev_class->tlb_kinds; ++i)
		total_tlbs += tt_dev->dev_class->tlb_counts[i];

	if (bar4)
		offset -= BAR0_SIZE;

	// Find the window matching the requested offset.
	for (i = 0; i < total_tlbs; ++i) {
		if (tt_dev->dev_class->describe_tlb(tt_dev, i, &tlb_desc))
			return -EINVAL;

		if (tlb_desc.bar_offset == offset && (tlb_desc.bar == 4) == bar4) {
			id = i;
			break;
		}
	}

	if (id < 0)
		return -EINVAL;

	if (size > tlb_desc.size)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	if (!test_bit(id, priv->tlbs)) {
		ret = -EPERM;
		goto unlock;
	}

	bar_start = pci_resource_start(tt_dev->pdev, tlb_desc.bar);
	pfn = (bar_start + tlb_desc.bar_offset) >> PAGE_SHIFT;

	vma->vm_ops = &tlb_vm_ops;
	vma->vm_private_data = (void*)(uintptr_t)id;

	if (io_remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot)) {
		ret = -EAGAIN;
		goto unlock;
	}

	tlb_vma_open(vma);

unlock:
	mutex_unlock(&priv->mutex);
	return ret;
}

int tenstorrent_mmap(struct chardev_private *priv, struct vm_area_struct *vma)
{
	struct pci_dev *pdev = priv->device->pdev;

	// We multiplex various mappable entities into a single character
	// device using the mapping offset to determine which entity you get.
	// Each mapping must be contained within a single entity.
	// - PCI BAR 0/2/4 uncacheable mapping
	// - PCI BAR 0/2/4 write-combining mapping
	// - DMA buffer mapping

	if (vma_target_range(vma, MMAP_OFFSET_RESOURCE0_UC, pci_resource_len(pdev, 0))) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 0);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE0_WC, pci_resource_len(pdev, 0))) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 0);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE1_UC, pci_resource_len(pdev, 2))) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 2);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE1_WC, pci_resource_len(pdev, 2))) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 2);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE2_UC, pci_resource_len(pdev, 4))) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 4);

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE2_WC, pci_resource_len(pdev, 4))) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_pci_bar(pdev, vma, 4);

	} else if (vma_target_range(vma, MMAP_OFFSET_TLB_UC, MMAP_RESOURCE_SIZE)) {
		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);
		return map_tlb_window(priv, vma);

	} else if (vma_target_range(vma, MMAP_OFFSET_TLB_WC, MMAP_RESOURCE_SIZE)) {
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
		return map_tlb_window(priv, vma);

	} else {
		struct dmabuf *dmabuf = vma_dmabuf_target(priv, vma);
		if (dmabuf != NULL)
			return dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr,
						 dmabuf->phys, dmabuf->size);
		else
			return -EINVAL;
	}
}

void tenstorrent_memory_cleanup(struct chardev_private *priv)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct pinned_page_range *pinning, *tmp_pinning;
	struct hlist_node *tmp_dmabuf;
	struct dmabuf *dmabuf;
	unsigned int i;
	struct peer_resource_mapping *peer_mapping, *tmp_peer_mapping;

	mutex_lock(&priv->mutex);

	hash_for_each_safe(priv->dmabufs, i, tmp_dmabuf, dmabuf, hash_chain) {
		dma_free_coherent(&tt_dev->pdev->dev, dmabuf->size, dmabuf->ptr, dmabuf->phys);

		hash_del(&dmabuf->hash_chain);
		kfree(dmabuf);
	}

	list_for_each_entry_safe(pinning, tmp_pinning, &priv->pinnings, list) {
		unpin_pinned_page_range(priv, pinning);
	}

	list_for_each_entry_safe(peer_mapping, tmp_peer_mapping, &priv->peer_mappings, list) {
		dma_unmap_resource(&priv->device->pdev->dev, peer_mapping->mapped_address, peer_mapping->size, DMA_BIDIRECTIONAL, 0);

		list_del(&peer_mapping->list);
		kfree(peer_mapping);
	}

	mutex_unlock(&priv->mutex);
}
