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

#include "enumerate.h"
#include "ioctl.h"

// In Linux 5.0, dma_alloc_coherent always zeroes memory and dma_zalloc_coherent
// was removed.
#if LINUX_VERSION_CODE < 0x50000
#define dma_alloc_coherent dma_zalloc_coherent
#endif

#define MAX_DMA_BUF_SIZE_LOG2 22
#define MAX_DMA_BUF_SIZE (1u << MAX_DMA_BUF_SIZE_LOG2)

// These are the mmap offsets for various resources. In the user-kernel
// interface they are dynamic (TENSTORRENT_IOCTL_QUERY_MAPPINGS and
// TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF), but they are actually hard-coded.
#define MMAP_OFFSET_BAR0_UC U64_C(0)
#define MMAP_OFFSET_BAR0_WC (U64_C(1) << 32)
#define MMAP_OFFSET_DMA_BUF (U64_C(2) << 32)
// 2,3,4,5,6,7,8,9 are all DMA buffers.

#define MMAP_SIZE_DMA_BUF (U64_C(1) << 32)

struct dmabuf {
	void *ptr;	// kernel address for dma buffer
	dma_addr_t phys;
	u64 size;	// always a multiple of PAGE_SIZE
};

// This is our device-private data assocated with each open character device fd.
// Accessed through struct file::private_data.
struct chardev_private {
	struct grayskull_device *device;
	struct mutex mutex;
	struct dmabuf dmabufs[TENSTORRENT_MAX_DMA_BUFS];
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

static dev_t devt_for_device(struct grayskull_device *gs_dev)
{
	return MKDEV(MAJOR(tt_device_id), MINOR(tt_device_id) + gs_dev->ordinal);
}

int tenstorrent_register_device(struct grayskull_device *gs_dev)
{
	dev_t devt = devt_for_device(gs_dev);

	device_initialize(&gs_dev->dev);
	gs_dev->dev.devt = devt;
	gs_dev->dev.class = tt_dev_class;
	gs_dev->dev.parent = &gs_dev->pdev->dev;
	gs_dev->dev.groups = NULL;
	gs_dev->dev.release = NULL;

	gs_dev->dev.id = gs_dev->ordinal;
	dev_set_name(&gs_dev->dev, TENSTORRENT "/%d", gs_dev->ordinal);

	cdev_init(&gs_dev->chardev, &chardev_fops);
	return cdev_device_add(&gs_dev->chardev, &gs_dev->dev);
}

void tenstorrent_unregister_device(struct grayskull_device *gs_dev)
{
	cdev_device_del(&gs_dev->chardev, &gs_dev->dev);
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
	struct tenstorrent_mapping mappings[2];

	u32 valid_mappings_to_copy;
	u32 unused_mappings_to_copy;

	struct tenstorrent_query_mappings_in in;
	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	valid_mappings_to_copy = min(in.output_mapping_count, (u32)ARRAY_SIZE(mappings));
	unused_mappings_to_copy = (in.output_mapping_count > ARRAY_SIZE(mappings))
		? in.output_mapping_count - ARRAY_SIZE(mappings) : 0;

	if (U32_MAX / sizeof(struct tenstorrent_mapping) < unused_mappings_to_copy)
		return -EFAULT;

	memset(mappings, 0, sizeof(mappings));

	mappings[0].mapping_id = TENSTORRENT_MAPPING_BAR0_UC;
	mappings[0].mapping_base = MMAP_OFFSET_BAR0_UC;
	mappings[0].mapping_size = pci_resource_len(priv->device->pdev, 0);

	mappings[1].mapping_id = TENSTORRENT_MAPPING_BAR0_WC;
	mappings[1].mapping_base = MMAP_OFFSET_BAR0_WC;
	mappings[1].mapping_size = pci_resource_len(priv->device->pdev, 0);

	if (copy_to_user(&arg->out.mappings, &mappings,
			 valid_mappings_to_copy * sizeof(struct tenstorrent_mapping)))
		return -EFAULT;

	if (clear_user(&arg->out.mappings[valid_mappings_to_copy],
		       unused_mappings_to_copy * sizeof(struct tenstorrent_mapping)))
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
	// struct file, and that's safe because the mapping refcounts the file.
	return -EINVAL;
}

static long ioctl_get_driver_info(struct chardev_private *priv,
				  struct tenstorrent_get_driver_info __user *arg)
{
	const struct pci_dev *pdev = priv->device->pdev;
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

static int tt_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct chardev_private *priv = file->private_data;
	struct pci_dev *pdev = priv->device->pdev;

	u64 bar0_start = pci_resource_start(pdev, 0);
	u64 bar0_size = pci_resource_len(pdev, 0);

	// We multiplex various mappable entities into a single character
	// device using the mapping offset to determine which entity you get.
	// Each mapping must be contained within a single entity.
	// - PCI BAR 0 uncacheable mapping
	// - PCI BAR 0 write-combining mapping
	// - DMA buffer mapping

	if (vma_target_range(vma, MMAP_OFFSET_BAR0_UC, bar0_size)) {

		vma->vm_page_prot = pgprot_device(vma->vm_page_prot);

		return vm_iomap_memory(vma, bar0_start, bar0_size);

	} else if (vma_target_range(vma, MMAP_OFFSET_BAR0_WC, bar0_size)) {

		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

		return vm_iomap_memory(vma, bar0_start, bar0_size);

	} else {
		struct dmabuf *dmabuf = vma_dmabuf_target(priv, vma);
		if (dmabuf != NULL)
			return dma_mmap_coherent(&pdev->dev, vma, dmabuf->ptr,
					 	 dmabuf->phys, dmabuf->size);
		else
			return -EINVAL;
	}
}

static struct grayskull_device *inode_to_gs_dev(struct inode *inode)
{
	return container_of(inode->i_cdev, struct grayskull_device, chardev);
}

static int tt_cdev_open(struct inode *inode, struct file *file)
{
	struct grayskull_device *gs_dev = inode_to_gs_dev(inode);
	struct chardev_private *private_data;

	private_data = kzalloc(sizeof(*private_data), GFP_KERNEL);
	if (private_data == NULL)
		return -ENOMEM;

	mutex_init(&private_data->mutex);

	private_data->device = gs_dev;
	file->private_data = private_data;

	return 0;
}

static int tt_cdev_release(struct inode *inode, struct file *file)
{
	struct chardev_private *priv = file->private_data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(priv->dmabufs); i++) {
		struct dmabuf *dmabuf = &priv->dmabufs[i];

		if (dmabuf->ptr != NULL)
			dma_free_coherent(&priv->device->pdev->dev,
					  dmabuf->size, dmabuf->ptr, dmabuf->phys);
	}

	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}
