// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "chardev.h"

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>
#include <linux/hashtable.h>

#include "device.h"
#include "enumerate.h"
#include "ioctl.h"
#include "pcie.h"

#define ONE_GIG (1UL << 30)
#define ONE_MEG (1UL << 20)


static int tt_dev_setup_iatu_hugepages(struct tenstorrent_device *tt_dev) {
	u64 limit = ONE_GIG - 1;
	u64 src = 0;
	int i;

	for (i = 0; i < tt_dev->num_hugepages; ++i) {
		dma_addr_t dma_addr = tt_dev->hugepages_dma[i];

		if (!tt_dev->dev_class->setup_outbound_iatu) {
			dev_err(&tt_dev->pdev->dev, "Couldn't set up outbound address translation.\n");
			return -1;
		}

		tt_dev->dev_class->setup_outbound_iatu(tt_dev, i, src, dma_addr, limit);
		limit += ONE_GIG;
		src += ONE_GIG;
	}
	return 0;
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


#define MAX_DMA_BUF_SIZE_LOG2 28
#define MAX_DMA_BUF_SIZE (1u << MAX_DMA_BUF_SIZE_LOG2)

// These are the mmap offsets for various resources. In the user-kernel
// interface they are dynamic (TENSTORRENT_IOCTL_QUERY_MAPPINGS and
// TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF), but they are actually hard-coded.
#define MMAP_OFFSET_RESOURCE0_UC	(U64_C(0) << 32)
#define MMAP_OFFSET_RESOURCE0_WC	(U64_C(1) << 32)
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(2) << 32)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(3) << 32)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(4) << 32)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(5) << 32)
#define MMAP_OFFSET_RESOURCE_TENSIX_DMA	(U64_C(6) << 32)

// tenstorrent_allocate_dma_buf_in.buf_index is u8 so that sets a limit of
// U8_MAX DMA buffers per fd. 32-bit mmap offsets are divided by PAGE_SIZE,
// so PAGE_SIZE << 32 is the largest possible offset.
#define MMAP_OFFSET_DMA_BUF		((u64)(PAGE_SIZE-U8_MAX) << 32)

#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)

#define DMABUF_HASHTABLE_BITS 4
struct dmabuf {
	struct hlist_node hash_chain;

	void *ptr;	// kernel address for dma buffer
	dma_addr_t phys;
	u64 size;	// always a multiple of PAGE_SIZE
	u8 index;
};

struct pinned_page_range {
	struct list_head list;

	unsigned long page_count;
	struct page **pages;	// vmalloc/vfree
};

// This is our device-private data assocated with each open character device fd.
// Accessed through struct file::private_data.
struct chardev_private {
	struct tenstorrent_device *device;
	struct mutex mutex;
	DECLARE_HASHTABLE(dmabufs, DMABUF_HASHTABLE_BITS);	// keyed on by dmabuf.index, chained on struct dmabuf.hash_chain
	struct list_head pinnings;	// struct pinned_page_range.list

	DECLARE_BITMAP(resource_lock, TENSTORRENT_RESOURCE_LOCK_COUNT);
};

static dev_t tt_device_id;
static struct class *tt_dev_class;
static unsigned int tt_max_devices;

static long tt_cdev_ioctl(struct file *, unsigned int, unsigned long);
static int tt_cdev_mmap(struct file *, struct vm_area_struct *);
static int tt_cdev_open(struct inode *, struct file *);
static int tt_cdev_release(struct inode *, struct file *);

static struct file_operations chardev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = tt_cdev_ioctl,
	.mmap = tt_cdev_mmap,
	.open = tt_cdev_open,
	.release = tt_cdev_release,
};

int init_char_driver(unsigned int max_devices)
{
	int res;

	tt_max_devices = max_devices;

	// Allocate a device major/minor (one minor) for this driver.
	res = alloc_chrdev_region(&tt_device_id, 0, max_devices, TENSTORRENT);
	if (res < 0)
		goto alloc_chrdev_region_failed;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	tt_dev_class = class_create(TENSTORRENT);
#else
	tt_dev_class = class_create(THIS_MODULE, TENSTORRENT);
#endif
	if (IS_ERR(tt_dev_class)) {
		tt_dev_class = NULL;
		goto class_create_failed;
	}

	return 0;

class_create_failed:
	unregister_chrdev_region(tt_device_id, max_devices);
alloc_chrdev_region_failed:
	return res;
}

void cleanup_char_driver(void)
{
	class_destroy(tt_dev_class);
	tt_dev_class = NULL;

	unregister_chrdev_region(tt_device_id, tt_max_devices);
}

static dev_t devt_for_device(struct tenstorrent_device *tt_dev)
{
	return MKDEV(MAJOR(tt_device_id), MINOR(tt_device_id) + tt_dev->ordinal);
}

int tenstorrent_register_device(struct tenstorrent_device *tt_dev)
{
	dev_t devt = devt_for_device(tt_dev);

	device_initialize(&tt_dev->dev);
	tt_dev->dev.devt = devt;
	tt_dev->dev.class = tt_dev_class;
	tt_dev->dev.parent = &tt_dev->pdev->dev;
	tt_dev->dev.groups = NULL;
	tt_dev->dev.release = NULL;

	tt_dev->dev.id = tt_dev->ordinal;
	dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", tt_dev->ordinal);

	cdev_init(&tt_dev->chardev, &chardev_fops);
	return cdev_device_add(&tt_dev->chardev, &tt_dev->dev);
}

void tenstorrent_unregister_device(struct tenstorrent_device *tt_dev)
{
	cdev_device_del(&tt_dev->chardev, &tt_dev->dev);
}

void tenstorrent_cleanup_hugepages(struct tenstorrent_device *tt_dev)
{
	int i;
	for (i = 0; i < tt_dev->num_hugepages; ++i) {
		dma_unmap_page(&tt_dev->pdev->dev, tt_dev->hugepages_dma[i], ONE_GIG, DMA_BIDIRECTIONAL);
		put_page(tt_dev->hugepages[i]);
	}
	tt_dev->num_hugepages = 0;
}

static long ioctl_get_device_info(struct chardev_private *priv,
				  struct tenstorrent_get_device_info __user *arg)
{
	struct pci_dev *pdev = priv->device->pdev;
	u32 bytes_to_copy;

	struct tenstorrent_get_device_info_out in;
	struct tenstorrent_get_device_info_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	out.output_size_bytes = sizeof(out);
	out.vendor_id = pdev->vendor;
	out.device_id = pdev->device;
	out.subsystem_vendor_id = pdev->subsystem_vendor;
	out.subsystem_id = pdev->subsystem_device;
	out.bus_dev_fn = PCI_DEVID(pdev->bus->number, pdev->devfn);
	out.max_dma_buf_size_log2 = MAX_DMA_BUF_SIZE_LOG2;
	out.pci_domain = pci_domain_nr(pdev->bus);
	out.numa_node = dev_to_node(&pdev->dev);

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0)
		return -EFAULT;

	return 0;
}

// This replaces tenstorrent_query_mappings from ioctl.h with a version
// that uses a flexible array member rather than a zero-length array.
// This keeps UBSAN from triggering when we write the output mappings.
struct tenstorrent_query_mappings_flex {
	struct tenstorrent_query_mappings_in in;
	struct tenstorrent_mapping out_mappings[];
};

static long ioctl_query_mappings(struct chardev_private *priv,
				 struct tenstorrent_query_mappings_flex __user *arg)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_mapping mappings[7];
	struct tenstorrent_mapping *next_mapping;

	u32 valid_mappings_to_copy;
	u32 extra_mappings_to_clear;
	u32 valid_mappings;

	ssize_t resource_len;

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

	// TODO: Reserve top 256MiB of 4GiB range for platform use.
	resource_len = tt_dev->num_hugepages * ONE_GIG;
	next_mapping->mapping_id = TENSTORRENT_MAPPING_RESOURCE_DMA;
	next_mapping->mapping_base = MMAP_OFFSET_RESOURCE_TENSIX_DMA;
	next_mapping->mapping_size = resource_len;
	next_mapping++;

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

static long ioctl_allocate_dma_buf(struct chardev_private *priv,
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

static long ioctl_free_dma_buf(struct chardev_private *priv,
			       struct tenstorrent_free_dma_buf __user *arg)
{
	// This is unsupported until I figure out how to block freeing as long
	// as a mapping exists. Otherwise the dma buffer is freed when the
	// struct file is destroyed, and that's safe because the mapping
	// refcounts the file.
	return -EINVAL;
}

static long ioctl_get_driver_info(struct chardev_private *priv,
				  struct tenstorrent_get_driver_info __user *arg)
{
	u32 bytes_to_copy;

	struct tenstorrent_get_driver_info_out in;
	struct tenstorrent_get_driver_info_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	out.output_size_bytes = sizeof(out);
	out.driver_version = TENSTORRENT_DRIVER_VERSION;

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0)
		return -EFAULT;

	return 0;
}

static long ioctl_reset_device(struct chardev_private *priv,
			       struct tenstorrent_reset_device __user *arg)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct pci_dev *pdev = priv->device->pdev;
	bool ok;
	u32 bytes_to_copy;

	struct tenstorrent_reset_device_in in;
	struct tenstorrent_reset_device_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (in.flags == TENSTORRENT_RESET_DEVICE_RESTORE_STATE) {
		if (safe_pci_restore_state(pdev))
			ok = priv->device->dev_class->init_hardware(priv->device);
		else
			ok = false;

	} else if (in.flags == TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK) {
		ok = pcie_hot_reset_and_restore_state(pdev);
	} else {
		return -EINVAL;
	}

	tt_dev_setup_iatu_hugepages(tt_dev);

	out.output_size_bytes = sizeof(out);
	out.result = !ok;

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0)
		return -EFAULT;

	return 0;
}

static long ioctl_pin_pages(struct chardev_private *priv,
			    struct tenstorrent_pin_pages __user *arg)
{
	unsigned long nr_pages;
	struct page **pages;
	int pages_pinned;
	struct pinned_page_range *pinning;
	long ret;
	int i;
	u32 bytes_to_copy;

	struct tenstorrent_pin_pages_in in;
	struct tenstorrent_pin_pages_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (!PAGE_ALIGNED(in.virtual_address) || !PAGE_ALIGNED(in.size) || in.size == 0)
		return -EINVAL;

	if (!(in.flags & TENSTORRENT_PIN_PAGES_CONTIGUOUS))
		return -EINVAL;

	pinning = kmalloc(sizeof(*pinning), GFP_KERNEL);
	if (!pinning)
		return -ENOMEM;

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

	for (i = 1; i < pages_pinned; i++) {
		if (page_to_pfn(pages[i]) != page_to_pfn(pages[i-1]) + 1) {
			pr_err("pages discontiguous at %d\n", i);
			ret = -EINVAL;
			goto err_unpin_pages;
		}
	}

	pinning->page_count = nr_pages;
	pinning->pages = pages;

	out.physical_address = page_to_phys(pages[0]);

	mutex_lock(&priv->mutex);
	list_add(&pinning->list, &priv->pinnings);
	mutex_unlock(&priv->mutex);

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;

err_unpin_pages:
	unpin_user_pages_dirty_lock(pages, pages_pinned, false);
err_vfree_pages:
	vfree(pages);
err_free_pinning:
	kfree(pinning);
	return ret;
}

static long ioctl_lock_ctl(struct chardev_private *priv,
			    struct tenstorrent_lock_ctl __user *arg) {
	u32 bytes_to_copy;

	struct tenstorrent_lock_ctl_in in;
	struct tenstorrent_lock_ctl_out out;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (in.index >= TENSTORRENT_RESOURCE_LOCK_COUNT)
		return -EINVAL;

	switch (in.flags) {
		case TENSTORRENT_LOCK_CTL_ACQUIRE:
			// If the global lock was unset then the local one must also be unset.
			// In a race this won't desync because the global lock is set first in the lock
			// and unset last in the unlock.
			if (!test_and_set_bit(in.index, priv->device->resource_lock)) {
				set_bit(in.index, priv->resource_lock);

				// 1 means that the lock was aquired.
				out.value = 1;
			} else {
				// 0 means that the lock failed to be aquired.
				out.value = 0;
			}
			break;
		case TENSTORRENT_LOCK_CTL_RELEASE:
			// First check the local lock, it is set last when locking so must be unset first when unlocking.
			if (test_and_clear_bit(in.index, priv->resource_lock)) {
				clear_bit(in.index, priv->device->resource_lock);

				// 1 means that the lock was released.
				out.value = 1;
			} else {
				// 0 means that the lock failed to be released.
				out.value = 0;
			}
			break;
		case TENSTORRENT_LOCK_CTL_TEST:
			// The local view goes in the first bit and the global goes in the second.
			// This should ensure that you can still convert to a bool in order to check if the lock
			// has been set at all, but also allows means that the caller doesn't always needs to do their own bookkeeping.
			out.value = (test_bit(in.index, priv->device->resource_lock) << 1) |
				     test_bit(in.index, priv->resource_lock);
			break;
		default:
			return -EINVAL;
	}

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;
}

static long ioctl_hugepage_setup(struct chardev_private *priv, struct tenstorrent_hugepage_setup __user *arg) {
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_hugepage_setup data;
	int i;
	int dev_node;
	int ret;

	memset(&data, 0, sizeof(data));

	if (copy_from_user(&data, arg, sizeof(data)))
		return -EFAULT;

	if (data.num_hugepages > TENSTORRENT_MAX_HUGEPAGES_PER_CARD)
		return -EINVAL;

	mutex_lock(&priv->mutex);

	tenstorrent_cleanup_hugepages(tt_dev);

	dev_node = dev_to_node(&tt_dev->pdev->dev);

	for (i = 0; i < data.num_hugepages; ++i) {
		dma_addr_t dma_addr;
		struct page *page;
		int node;

		if (get_user_pages(data.virt_addrs[i], 1, FOLL_WRITE, &page, NULL) < 0)
			goto err;

		if (!PageHuge(page))
			goto err;	// Not huge

		if (compound_order(page) != 18)
			goto err;	// Not 1G huge

		node = page_to_nid(page);
		if (dev_node != -1 && node != dev_node) {
			put_page(page);
			dev_err(&tt_dev->pdev->dev, "Mismatch between device (%d) and hugepage (%d) NUMA nodes", node, dev_node);
			goto err;	// Not on the same node
		}

		// On 5.4.0 (Ubuntu 20.04) this is bugged with IOMMU on and dma_mask set
		// to 32 bits: it'll return 0 for dma_addr rather than failing.  With
		// IOMMU off it'll try to use swiotlb - not what we want!
		dma_addr = dma_map_page(&priv->device->pdev->dev, page, 0, ONE_GIG, DMA_BIDIRECTIONAL);
		if (dma_addr == 0 || dma_mapping_error(&priv->device->pdev->dev, dma_addr)) {
			put_page(page);
			goto err;
		}

		tt_dev->num_hugepages++;
		tt_dev->hugepages[i] = page;
		tt_dev->hugepages_dma[i] = dma_addr;
	}

	ret = tt_dev_setup_iatu_hugepages(tt_dev);
	mutex_unlock(&priv->mutex);
	return ret;

err:
	tenstorrent_cleanup_hugepages(tt_dev);
	mutex_unlock(&priv->mutex);
	return -EINVAL;
}

static long tt_cdev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	long ret = -EINVAL;
	struct chardev_private *priv = f->private_data;

	switch (cmd) {
		case TENSTORRENT_IOCTL_GET_DEVICE_INFO:
			ret = ioctl_get_device_info(priv, (struct tenstorrent_get_device_info __user *)arg);
			break;

		case TENSTORRENT_IOCTL_GET_HARVESTING:
			break;

		case TENSTORRENT_IOCTL_QUERY_MAPPINGS:
			ret = ioctl_query_mappings(priv, (struct tenstorrent_query_mappings_flex __user *)arg);
			break;

		case TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF:
			ret = ioctl_allocate_dma_buf(priv, (struct tenstorrent_allocate_dma_buf __user *)arg);
			break;

		case TENSTORRENT_IOCTL_FREE_DMA_BUF:
			ret = ioctl_free_dma_buf(priv, (struct tenstorrent_free_dma_buf __user *)arg);
			break;

		case TENSTORRENT_IOCTL_GET_DRIVER_INFO:
			ret = ioctl_get_driver_info(priv, (struct tenstorrent_get_driver_info __user *)arg);
			break;

		case TENSTORRENT_IOCTL_RESET_DEVICE:
			ret = ioctl_reset_device(priv, (struct tenstorrent_reset_device __user *)arg);
			break;

		case TENSTORRENT_IOCTL_PIN_PAGES:
			ret = ioctl_pin_pages(priv, (struct tenstorrent_pin_pages __user *)arg);
			break;

		case TENSTORRENT_IOCTL_LOCK_CTL:
			ret = ioctl_lock_ctl(priv, (struct tenstorrent_lock_ctl __user *)arg);
			break;

		case TENSTORRENT_IOCTL_HUGEPAGE_SETUP:
			ret = ioctl_hugepage_setup(priv, (struct tenstorrent_hugepage_setup __user *)arg);
			break;

		default:
			ret = -EINVAL;
			break;
	}

	return ret;
}

// Is the mapping target range contained entirely with start - start+len?
// start and len must be page-aligned.
static bool vma_target_range(struct vm_area_struct *vma, u64 start, u64 len)
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
	u64 bar_start = pci_resource_start(pdev, bar);
	u64 bar_len = pci_resource_len(pdev, bar);

	return vm_iomap_memory(vma, bar_start, bar_len);
}

static int tt_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct chardev_private *priv = file->private_data;
	struct pci_dev *pdev = priv->device->pdev;
	struct tenstorrent_device *tt_dev = priv->device;

	// We multiplex various mappable entities into a single character
	// device using the mapping offset to determine which entity you get.
	// Each mapping must be contained within a single entity.
	// - PCI BAR 0/2/4 uncacheable mapping
	// - PCI BAR 0/2/4 write-combining mapping
	// - DMA buffer mapping
	// - Tensix DMA buffer mapping

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

	} else if (vma_target_range(vma, MMAP_OFFSET_RESOURCE_TENSIX_DMA, tt_dev->num_hugepages * ONE_GIG)) {
		unsigned long vsize = vma->vm_end - vma->vm_start;
		unsigned long psize = tt_dev->num_hugepages * ONE_GIG;
		size_t i;

		if (vsize > psize)
			return -EINVAL;

		for (i = 0; i < tt_dev->num_hugepages; ++i) {
			unsigned long offset = i * ONE_GIG;
			struct page *page = tt_dev->hugepages[i];

			if (offset >= vsize)
				break;
			if (remap_pfn_range(vma, vma->vm_start + offset, page_to_pfn(page), min(vsize - offset, ONE_GIG), vma->vm_page_prot)) {
				return -EINVAL;
			}
		}

		return 0;
	} else {
		struct dmabuf *dmabuf = vma_dmabuf_target(priv, vma);
		if (dmabuf != NULL)
			return dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr,
						 dmabuf->phys, dmabuf->size);
		else
			return -EINVAL;
	}
}

static struct tenstorrent_device *inode_to_tt_dev(struct inode *inode)
{
	return container_of(inode->i_cdev, struct tenstorrent_device, chardev);
}

static void increment_cdev_open_count(struct tenstorrent_device *tt_dev) {
	mutex_lock(&tt_dev->chardev_mutex);
	if (!tt_dev->chardev_open_count && tt_dev->dev_class->first_open_cb)
		tt_dev->dev_class->first_open_cb(tt_dev);
	tt_dev->chardev_open_count++;
	mutex_unlock(&tt_dev->chardev_mutex);
}

static void decrement_cdev_open_count(struct tenstorrent_device *tt_dev) {
	mutex_lock(&tt_dev->chardev_mutex);
	tt_dev->chardev_open_count--;
	if (!tt_dev->chardev_open_count && tt_dev->dev_class->last_release_cb)
		tt_dev->dev_class->last_release_cb(tt_dev);
	mutex_unlock(&tt_dev->chardev_mutex);
}

static int tt_cdev_open(struct inode *inode, struct file *file)
{
	struct tenstorrent_device *tt_dev = inode_to_tt_dev(inode);
	struct chardev_private *private_data;

	private_data = kzalloc(sizeof(*private_data), GFP_KERNEL);
	if (private_data == NULL)
		return -ENOMEM;

	mutex_init(&private_data->mutex);

	hash_init(private_data->dmabufs);
	INIT_LIST_HEAD(&private_data->pinnings);

	kref_get(&tt_dev->kref);
	private_data->device = tt_dev;
	file->private_data = private_data;

	increment_cdev_open_count(tt_dev);

	return 0;
}

static int tt_cdev_release(struct inode *inode, struct file *file)
{
	struct chardev_private *priv = file->private_data;
	struct tenstorrent_device *tt_dev = priv->device;
	struct pinned_page_range *pinning, *tmp_pinning;
	struct hlist_node *tmp_dmabuf;
	struct dmabuf *dmabuf;
	unsigned int i;
	unsigned int bitpos;

	decrement_cdev_open_count(tt_dev);

	hash_for_each_safe(priv->dmabufs, i, tmp_dmabuf, dmabuf, hash_chain) {
		dma_free_coherent(&tt_dev->pdev->dev, dmabuf->size, dmabuf->ptr, dmabuf->phys);

		hash_del(&dmabuf->hash_chain);
		kfree(dmabuf);
	}

	list_for_each_entry_safe(pinning, tmp_pinning, &priv->pinnings, list) {
		unpin_user_pages_dirty_lock(pinning->pages, pinning->page_count, true);
		vfree(pinning->pages);

		list_del(&pinning->list);
		kfree(pinning);
	}

	// Release all locally held resources.
	for (bitpos = 0; bitpos < TENSTORRENT_RESOURCE_LOCK_COUNT; ++bitpos) {
		// Same as in the ioctl handler, first clear the local data because it is set last during the lock.
		if (test_and_clear_bit(bitpos, priv->resource_lock))
			clear_bit(bitpos, priv->device->resource_lock);
	}

	tenstorrent_device_put(tt_dev);

	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}
