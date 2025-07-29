// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
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
#include <linux/version.h>

#include "chardev_private.h"
#include "device.h"
#include "enumerate.h"
#include "ioctl.h"
#include "pcie.h"
#include "memory.h"
#include "module.h"
#include "tlb.h"

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

	INIT_LIST_HEAD(&tt_dev->open_fds_list);

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

	struct tenstorrent_get_device_info_in in;
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

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;
}

static long ioctl_get_driver_info(struct chardev_private *priv,
				  struct tenstorrent_get_driver_info __user *arg)
{
	u32 bytes_to_copy;

	struct tenstorrent_get_driver_info_in in;
	struct tenstorrent_get_driver_info_out out;
	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	out.output_size_bytes = sizeof(out);
	out.driver_version = TENSTORRENT_DRIVER_VERSION;
	out.driver_version_major = TENSTORRENT_DRIVER_VERSION_MAJOR;
	out.driver_version_minor = TENSTORRENT_DRIVER_VERSION_MINOR;
	out.driver_version_patch = TENSTORRENT_DRIVER_VERSION_PATCH;

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;
}

static long ioctl_reset_device(struct chardev_private *priv,
			       struct tenstorrent_reset_device __user *arg)
{
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
		if (safe_pci_restore_state(pdev)) {
			priv->device->dev_class->restore_reset_state(priv->device);
			ok = priv->device->dev_class->init_hardware(priv->device);
		} else {
			ok = false;
		}
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK) {
		ok = pcie_hot_reset_and_restore_state(pdev);
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_CONFIG_WRITE) {
		ok = pcie_timer_interrupt(pdev);
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_SET_RESET_MARKER) {
		ok = set_reset_marker(pdev);
	} else {
		return -EINVAL;
	}

	out.output_size_bytes = sizeof(out);
	out.result = !ok;

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;
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

static long ioctl_set_noc_cleanup(struct chardev_private *priv,
			   struct tenstorrent_set_noc_cleanup __user *arg)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_set_noc_cleanup data = {0};

	// First, ensure the underlying device class supports this operation.
	if (!tt_dev->dev_class->noc_write32)
		return -EOPNOTSUPP;

	if (copy_from_user(&data, arg, sizeof(data)) != 0)
		return -EFAULT;

	// The `argsz` field allows the kernel to validate that the userspace caller
	// has the same understanding of the structure size. For this specific
	// version of the API, we require an exact match.
	if (data.argsz != sizeof(data))
		return -EINVAL;

	// Validate reserved fields to ensure future compatibility.
	if (data.flags != 0)
		return -EINVAL;

	// The `enabled` field acts as a boolean; reject other values.
	if (data.enabled > 1)
		return -EINVAL;

	// Address must be 4-byte aligned.
	if (data.addr & 0x3)
		return -EINVAL;

	// NOC must be either 0 or 1.
	if (data.noc > 1)
		return -EINVAL;

	// TODO: Implement a more robust coordinate validation scheme.
	if (data.x > 64 || data.y > 64)
		return -EINVAL;

	mutex_lock(&priv->mutex);
	priv->noc_cleanup = data;
	mutex_unlock(&priv->mutex);

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

		case TENSTORRENT_IOCTL_RESET_DEVICE:
			ret = ioctl_reset_device(priv, (struct tenstorrent_reset_device __user *)arg);
			break;

		case TENSTORRENT_IOCTL_PIN_PAGES:
			ret = ioctl_pin_pages(priv, (struct tenstorrent_pin_pages __user *)arg);
			break;

		case TENSTORRENT_IOCTL_LOCK_CTL:
			ret = ioctl_lock_ctl(priv, (struct tenstorrent_lock_ctl __user *)arg);
			break;

		case TENSTORRENT_IOCTL_MAP_PEER_BAR:
			ret = ioctl_map_peer_bar(priv, (struct tenstorrent_map_peer_bar __user *)arg);
			break;

		case TENSTORRENT_IOCTL_UNPIN_PAGES:
			ret = ioctl_unpin_pages(priv, (struct tenstorrent_unpin_pages __user *)arg);
			break;

		case TENSTORRENT_IOCTL_ALLOCATE_TLB:
			ret = ioctl_allocate_tlb(priv, (struct tenstorrent_allocate_tlb __user *)arg);
			break;

		case TENSTORRENT_IOCTL_FREE_TLB:
			ret = ioctl_free_tlb(priv, (struct tenstorrent_free_tlb __user *)arg);
			break;

		case TENSTORRENT_IOCTL_CONFIGURE_TLB:
			ret = ioctl_configure_tlb(priv, (struct tenstorrent_configure_tlb __user *)arg);
			break;

		case TENSTORRENT_IOCTL_SET_NOC_CLEANUP:
			ret = ioctl_set_noc_cleanup(priv, (struct tenstorrent_set_noc_cleanup __user *)arg);
			break;

		default:
			ret = -EINVAL;
			break;
	}

	return ret;
}

static int tt_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct chardev_private *priv = file->private_data;

	return tenstorrent_mmap(priv, vma);
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
	INIT_LIST_HEAD(&private_data->peer_mappings);

	kref_get(&tt_dev->kref);
	private_data->device = tt_dev;
	file->private_data = private_data;

	mutex_lock(&tt_dev->chardev_mutex);
	list_add(&private_data->open_fd, &tt_dev->open_fds_list);
	mutex_unlock(&tt_dev->chardev_mutex);

	increment_cdev_open_count(tt_dev);

	return 0;
}

static int tt_cdev_release(struct inode *inode, struct file *file)
{
	struct chardev_private *priv = file->private_data;
	struct tenstorrent_device *tt_dev = priv->device;
	unsigned int bitpos;

	if (priv->noc_cleanup.enabled) {
		tt_dev->dev_class->noc_write32(
			tt_dev,
			priv->noc_cleanup.x,
			priv->noc_cleanup.y,
			priv->noc_cleanup.addr,
			priv->noc_cleanup.data & 0xFFFFFFFF,
			priv->noc_cleanup.noc);
	}

	decrement_cdev_open_count(tt_dev);

	tenstorrent_memory_cleanup(priv);

	// Release all locally held resources.
	for (bitpos = 0; bitpos < TENSTORRENT_RESOURCE_LOCK_COUNT; ++bitpos) {
		// Same as in the ioctl handler, first clear the local data because it is set last during the lock.
		if (test_and_clear_bit(bitpos, priv->resource_lock))
			clear_bit(bitpos, priv->device->resource_lock);
	}

	// Release all TLBs held by this file descriptor.
	for_each_set_bit(bitpos, priv->tlbs, TENSTORRENT_MAX_INBOUND_TLBS)
		tenstorrent_device_free_tlb(tt_dev, bitpos);

	tenstorrent_device_put(tt_dev);

	mutex_lock(&tt_dev->chardev_mutex);
	list_del(&priv->open_fd);
	mutex_unlock(&tt_dev->chardev_mutex);

	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

struct chardev_private *get_tenstorrent_priv(struct file *f)
{
	if (f->f_op != &chardev_fops)
		return NULL;

	return f->private_data;
}
