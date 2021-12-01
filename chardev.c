#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
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

#include "device.h"
#include "enumerate.h"
#include "ioctl.h"

// In Linux 5.0, dma_alloc_coherent always zeroes memory and dma_zalloc_coherent
// was removed.
#if LINUX_VERSION_CODE < 0x50000
#define dma_alloc_coherent dma_zalloc_coherent
#endif

#if LINUX_VERSION_CODE >= 0x050600
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	return pin_user_pages_fast(start, nr_pages, gup_flags | FOLL_LONGTERM, pages);
}
#elif LINUX_VERSION_CODE >= 0x050200
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	return get_user_pages_fast(start, nr_pages, gup_flags | FOLL_LONGTERM, pages);
}
#elif LINUX_VERSION_CODE >= 0x041404
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	int ret;

	down_read(&current->mm->mmap_sem);
	ret = get_user_pages_longterm(start, nr_pages, gup_flags, pages, NULL);
	up_read(&current->mm->mmap_sem);
	return ret;
}
#else
static int pin_user_pages_fast_longterm(unsigned long start, int nr_pages, unsigned int gup_flags, struct page **pages)
{
	return get_user_pages_fast(start, nr_pages, gup_flags, pages);
}
#endif

#if LINUX_VERSION_CODE >= 0x050600
// unpin_user_pages_dirty_lock is provided by the kernel.
#elif LINUX_VERSION_CODE >= 0x050400
static void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages, bool make_dirty)
{
	put_user_pages_dirty_lock(pages, npages, make_dirty);
}
#elif LINUX_VERSION_CODE >= 0x050200
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
#define MMAP_OFFSET_DMA_BUF		(U64_C(2) << 32)
// 2,3,4,5,6,7,8,9 are all DMA buffers.
#define MMAP_OFFSET_RESOURCE1_UC	(U64_C(10) << 32)
#define MMAP_OFFSET_RESOURCE1_WC	(U64_C(11) << 32)
#define MMAP_OFFSET_RESOURCE2_UC	(U64_C(12) << 32)
#define MMAP_OFFSET_RESOURCE2_WC	(U64_C(13) << 32)

#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)

struct dmabuf {
	void *ptr;	// kernel address for dma buffer
	dma_addr_t phys;
	u64 size;	// always a multiple of PAGE_SIZE
};

// This is our device-private data assocated with each open character device fd.
// Accessed through struct file::private_data.
struct chardev_private {
	struct tenstorrent_device *device;
	struct mutex mutex;
	struct dmabuf dmabufs[TENSTORRENT_MAX_DMA_BUFS];

	unsigned long pinned_page_count;
	struct page **pinned_pages;
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

	tt_dev_class = class_create(THIS_MODULE, TENSTORRENT);
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

static long ioctl_get_device_info(struct chardev_private *priv,
				  struct tenstorrent_get_device_info __user *arg)
{
	const struct pci_dev *pdev = priv->device->pdev;
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

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0)
		return -EFAULT;

	return 0;
}

static long ioctl_query_mappings(struct chardev_private *priv,
				 struct tenstorrent_query_mappings __user *arg)
{
	struct tenstorrent_mapping mappings[6];
	struct tenstorrent_mapping *next_mapping;

	u32 valid_mappings_to_copy;
	u32 extra_mappings_to_clear;
	u32 valid_mappings;

	int resource_len;

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

	if (copy_to_user(&arg->out.mappings, &mappings,
			 valid_mappings_to_copy * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	if (clear_user(&arg->out.mappings[valid_mappings_to_copy],
		       extra_mappings_to_clear * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	return 0;
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

	dmabuf = &priv->dmabufs[in.buf_index];

	if (in.requested_size % PAGE_SIZE != 0
	    || in.requested_size == 0
	    || in.requested_size > MAX_DMA_BUF_SIZE)
		return -EINVAL;

	if (dmabuf->ptr != NULL)
		return -EINVAL;

	mutex_lock(&priv->mutex);
	dma_buf_kernel_ptr = dma_alloc_coherent(&priv->device->pdev->dev,
						in.requested_size,
						&dma_handle, GFP_KERNEL);

	if (dma_buf_kernel_ptr == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	dmabuf->ptr = dma_buf_kernel_ptr;
	dmabuf->phys = dma_handle;
	dmabuf->size = in.requested_size;

	out.physical_address = (u64)dmabuf->phys;
	out.mapping_offset = MMAP_OFFSET_DMA_BUF + MMAP_SIZE_DMA_BUF * in.buf_index;
	out.size = in.requested_size;

	if (copy_to_user(&arg->out, &out, sizeof(out)) != 0) {
		dma_free_coherent(&priv->device->pdev->dev, dmabuf->size,
				  dmabuf->ptr, dmabuf->phys);
		dmabuf->ptr = NULL;
		ret = -EFAULT;
	}

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
	bool ok;
	u32 bytes_to_copy;

	struct tenstorrent_reset_device_in in;
	struct tenstorrent_reset_device_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (in.flags != 0)
		return -EINVAL;

	pci_restore_state(priv->device->pdev);
	ok = priv->device->dev_class->init_hardware(priv->device);

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
	long ret;
	int i;
	u32 bytes_to_copy;

	struct tenstorrent_pin_pages_in in;
	struct tenstorrent_pin_pages_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	if (!PAGE_ALIGNED(in.virtual_address) || !PAGE_ALIGNED(in.size))
		return -EINVAL;

	if (!(in.flags & TENSTORRENT_PIN_PAGES_CONTIGUOUS))
		return -EINVAL;

	if (priv->pinned_page_count != 0)
		return -EINVAL;

	nr_pages = PAGE_ALIGN(in.size) >> PAGE_SHIFT;
	pages = vzalloc(nr_pages * sizeof(struct page *));
	if (!pages) {
		pr_err("vzalloc failed for %lu page pointers\n", nr_pages);
		return -ENOMEM;
	}

	pages_pinned = pin_user_pages_fast_longterm(in.virtual_address, nr_pages, FOLL_WRITE, pages);
	if (pages_pinned < 0) {
		pr_warn("pin_user_pages_longterm failed: %d\n", pages_pinned);
		ret = pages_pinned;
		goto err_vfree;
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

	mutex_lock(&priv->mutex);

	if (priv->pinned_page_count != 0) {
		mutex_unlock(&priv->mutex);
		ret = -EINVAL;
		goto err_unpin_pages;
	}

	priv->pinned_page_count = pages_pinned;
	priv->pinned_pages = pages;

	out.physical_address = page_to_phys(pages[0]);

	mutex_unlock(&priv->mutex);

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;

err_unpin_pages:
	unpin_user_pages_dirty_lock(pages, pages_pinned, false);
err_vfree:
	vfree(pages);
	return ret;
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
			ret = ioctl_query_mappings(priv, (struct tenstorrent_query_mappings __user *)arg);
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
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(priv->dmabufs); i++) {
		u64 start = MMAP_OFFSET_DMA_BUF + i * MMAP_SIZE_DMA_BUF;
		struct dmabuf *dmabuf = &priv->dmabufs[i];

		if (dmabuf->ptr != NULL
		    && vma_target_range(vma, start, dmabuf->size))
			return dmabuf;
	}

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

	private_data->device = tt_dev;
	file->private_data = private_data;

	increment_cdev_open_count(tt_dev);

	return 0;
}

static int tt_cdev_release(struct inode *inode, struct file *file)
{
	struct tenstorrent_device *tt_dev = inode_to_tt_dev(inode);
	struct chardev_private *priv = file->private_data;
	unsigned int i;

	decrement_cdev_open_count(tt_dev);

	for (i = 0; i < ARRAY_SIZE(priv->dmabufs); i++) {
		struct dmabuf *dmabuf = &priv->dmabufs[i];

		if (dmabuf->ptr != NULL)
			dma_free_coherent(&priv->device->pdev->dev,
					  dmabuf->size, dmabuf->ptr, dmabuf->phys);
	}

	if (priv->pinned_page_count != 0) {
		unpin_user_pages_dirty_lock(priv->pinned_pages, priv->pinned_page_count, true);
		vfree(priv->pinned_pages);
	}

	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}
