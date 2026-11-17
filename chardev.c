// SPDX-License-Identifier: GPL-2.0-only
/*
 * Character device /dev/tenstorrent/<N>: open/release with O_EXCL admission,
 * ioctl dispatch, mmap dispatch, the lazily faulted MMIO mapping helper and
 * the core ioctls (driver/device info, mappings, resource locks, reset, peer
 * BAR mapping).
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>

#include "device.h"
#include "chardev.h"
#include "blackhole.h"

/* ---- ioctl helpers ---- */

int tt_check_output_size(u32 output_size_bytes)
{
	return output_size_bytes > TT_IOCTL_MAX_OUTPUT_SIZE ? -EINVAL : 0;
}

int tt_copy_out_sized(void __user *uout, const void *out, size_t out_size,
		      u32 output_size_bytes)
{
	size_t n = min_t(size_t, out_size, output_size_bytes);

	if (tt_check_output_size(output_size_bytes))
		return -EINVAL;
	if (n && copy_to_user(uout, out, n))
		return -EFAULT;
	if (output_size_bytes > n &&
	    clear_user((u8 __user *)uout + n, output_size_bytes - n))
		return -EFAULT;
	return 0;
}

/* ---- core ioctls ---- */

static long tt_ioctl_get_driver_info(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_get_driver_info_in in;
	struct tenstorrent_get_driver_info_out out = {};

	if (tt_copy_in(&in, uarg, sizeof(in)))
		return -EFAULT;

	out.output_size_bytes = sizeof(out);
	out.driver_version = TENSTORRENT_DRIVER_VERSION;
	out.driver_version_major = TT_DRIVER_VERSION_MAJOR;
	out.driver_version_minor = TT_DRIVER_VERSION_MINOR;
	out.driver_version_patch = TT_DRIVER_VERSION_PATCH;

	return tt_copy_out_sized(TT_UOUT(uarg, struct tenstorrent_get_driver_info),
				 &out, sizeof(out), in.output_size_bytes);
}

static long tt_ioctl_get_device_info(struct chardev_private *priv, void __user *uarg)
{
	struct pci_dev *pdev = priv->dev->pdev;
	struct tenstorrent_get_device_info_in in;
	struct tenstorrent_get_device_info_out out = {};

	if (tt_copy_in(&in, uarg, sizeof(in)))
		return -EFAULT;

	out.output_size_bytes = sizeof(out);
	out.vendor_id = pdev->vendor;
	out.device_id = pdev->device;
	out.subsystem_vendor_id = pdev->subsystem_vendor;
	out.subsystem_id = pdev->subsystem_device;
	out.bus_dev_fn = PCI_DEVID(pdev->bus->number, pdev->devfn);
	out.max_dma_buf_size_log2 = TT_MAX_DMA_BUF_SIZE_LOG2;
	out.pci_domain = pci_domain_nr(pdev->bus);

	return tt_copy_out_sized(TT_UOUT(uarg, struct tenstorrent_get_device_info),
				 &out, sizeof(out), in.output_size_bytes);
}

static long tt_ioctl_query_mappings(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_query_mappings_in in;
	struct tenstorrent_mapping __user *umap;
	size_t n;

	if (tt_copy_in(&in, uarg, sizeof(in)))
		return -EFAULT;

	if ((u64)in.output_mapping_count * sizeof(*umap) > TT_IOCTL_MAX_OUTPUT_SIZE)
		return -EINVAL;

	umap = (struct tenstorrent_mapping __user *)
		((u8 __user *)uarg + offsetof(struct tenstorrent_query_mappings, out.mappings));

	n = min_t(size_t, in.output_mapping_count, dev->num_mappings);
	if (n && copy_to_user(umap, dev->mappings, n * sizeof(*umap)))
		return -EFAULT;

	if (in.output_mapping_count > n &&
	    clear_user(umap + n, ((size_t)in.output_mapping_count - n) * sizeof(*umap)))
		return -EFAULT;

	return 0;
}

static bool tt_lock_try_acquire(struct tenstorrent_device *dev, unsigned int index,
				struct chardev_private *priv)
{
	bool acquired = false;

	spin_lock(&dev->resource_lock);
	if (!dev->lock_owner[index]) {
		dev->lock_owner[index] = priv;
		acquired = true;
	}
	spin_unlock(&dev->resource_lock);
	return acquired;
}

static bool tt_lock_release(struct tenstorrent_device *dev, unsigned int index,
			    struct chardev_private *priv)
{
	bool released = false;

	spin_lock(&dev->resource_lock);
	if (dev->lock_owner[index] == priv) {
		dev->lock_owner[index] = NULL;
		released = true;
	}
	spin_unlock(&dev->resource_lock);

	if (released)
		wake_up_all(&dev->lock_wq);
	return released;
}

static long tt_ioctl_lock_ctl(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_lock_ctl_in in;
	struct tenstorrent_lock_ctl_out out = {};
	bool acquired = false;
	unsigned int idx;
	int ret;

	if (tt_copy_in(&in, uarg, sizeof(in)))
		return -EFAULT;

	if (READ_ONCE(dev->detached))
		return -ENODEV;

	if (in.index >= TENSTORRENT_RESOURCE_LOCK_COUNT)
		return -EINVAL;
	if (tt_check_output_size(in.output_size_bytes))
		return -EINVAL;
	idx = in.index;

	switch (in.flags) {
	case TENSTORRENT_LOCK_CTL_ACQUIRE:
		acquired = tt_lock_try_acquire(dev, idx, priv);
		out.value = acquired;
		break;

	case TENSTORRENT_LOCK_CTL_RELEASE:
		out.value = tt_lock_release(dev, idx, priv);
		break;

	case TENSTORRENT_LOCK_CTL_TEST:
		spin_lock(&dev->resource_lock);
		if (dev->lock_owner[idx] == priv)
			out.value |= 1;
		if (dev->lock_owner[idx])
			out.value |= 2;
		spin_unlock(&dev->resource_lock);
		break;

	case TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING:
		/* -ERESTARTSYS on a signal: restarted with SA_RESTART, else EINTR.
		 * The condition is true (and ret 0) as soon as we hold the lock. */
		ret = wait_event_interruptible(dev->lock_wq,
					       (acquired = tt_lock_try_acquire(dev, idx, priv)) ||
					       READ_ONCE(dev->detached));
		if (ret)
			return ret;
		if (!acquired)		/* woken by removal */
			return -ENODEV;
		out.value = 1;
		break;

	default:
		return -EINVAL;
	}

	ret = tt_copy_out_sized(TT_UOUT(uarg, struct tenstorrent_lock_ctl),
				&out, sizeof(out), in.output_size_bytes);
	/* The caller cannot learn that it got the lock: don't keep it. */
	if (ret && acquired)
		tt_lock_release(dev, idx, priv);
	return ret;
}

static void tt_locks_release_all(struct chardev_private *priv)
{
	struct tenstorrent_device *dev = priv->dev;
	bool released = false;
	unsigned int i;

	spin_lock(&dev->resource_lock);
	for (i = 0; i < TENSTORRENT_RESOURCE_LOCK_COUNT; i++) {
		if (dev->lock_owner[i] == priv) {
			dev->lock_owner[i] = NULL;
			released = true;
		}
	}
	spin_unlock(&dev->resource_lock);

	if (released)
		wake_up_all(&dev->lock_wq);
}

static long tt_ioctl_reset_device(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_reset_device_in in;
	struct tenstorrent_reset_device_out out = {};
	long ret;

	if (tt_copy_in(&in, uarg, sizeof(in)))
		return -EFAULT;
	if (tt_check_output_size(in.output_size_bytes))
		return -EINVAL;

	switch (in.flags) {
	case TENSTORRENT_RESET_DEVICE_RESTORE_STATE:
		/* No reset is implemented, so there is no state to restore. */
		out.result = 0;
		break;

	case TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK:
	case TENSTORRENT_RESET_DEVICE_CONFIG_WRITE:
	case TENSTORRENT_RESET_DEVICE_USER_RESET:
	case TENSTORRENT_RESET_DEVICE_ASIC_RESET:
	case TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET:
	case TENSTORRENT_RESET_DEVICE_POST_RESET:
		/* Refuse while P2P importers may be using a TLB export. */
		ret = tt_tlb_reset_check(dev);
		if (ret)
			return ret;
		return -EOPNOTSUPP;

	default:
		return -EINVAL;
	}

	out.output_size_bytes = sizeof(out);
	return tt_copy_out_sized(TT_UOUT(uarg, struct tenstorrent_reset_device),
				 &out, sizeof(out), in.output_size_bytes);
}

/* ---- MAP_PEER_BAR ---- */

static void tt_peer_mapping_free(struct chardev_private *priv, struct tt_peer_mapping *pm)
{
	if (pm->dma_mapped)
		dma_unmap_resource(&priv->dev->pdev->dev, pm->dma_addr, pm->size,
				   DMA_BIDIRECTIONAL, 0);
	pci_dev_put(pm->peer);
	kfree(pm);
}

/* Remove (fd_detach) and release: whichever comes first unmaps. */
static void tt_peer_mappings_teardown(struct chardev_private *priv)
{
	struct tt_peer_mapping *pm, *tmp;

	mutex_lock(&priv->peer_lock);
	list_for_each_entry_safe(pm, tmp, &priv->peer_mappings, node) {
		list_del(&pm->node);
		tt_peer_mapping_free(priv, pm);
	}
	priv->num_peer_mappings = 0;
	mutex_unlock(&priv->peer_lock);
}

/* Does the IOMMU translate DMA from @pdev (as opposed to no IOMMU/identity)? */
static bool tt_dma_translated(struct pci_dev *pdev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&pdev->dev);

	return domain && (domain->type & __IOMMU_DOMAIN_PAGING);
}

static long tt_ioctl_map_peer_bar(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_map_peer_bar_in in;
	struct tenstorrent_map_peer_bar_out out = {};
	struct chardev_private *peer_priv;
	struct tenstorrent_device *peer;
	struct tt_peer_mapping *pm;
	resource_size_t bar_len;
	struct fd f;
	long ret;
	int idx;

	if (tt_copy_in(&in, uarg, sizeof(in)))
		return -EFAULT;

	if (in.flags || in.reserved)
		return -EINVAL;
	if (in.peer_bar_index > PCI_ROM_RESOURCE)
		return -EINVAL;
	if (!in.peer_bar_length || !PAGE_ALIGNED(in.peer_bar_offset) ||
	    !PAGE_ALIGNED(in.peer_bar_length))
		return -EINVAL;

	f = fdget(in.peer_fd);
	if (!f.file)
		return -EBADF;

	peer_priv = tt_file_priv(f.file);
	if (!peer_priv) {
		ret = -EINVAL;
		goto out_fdput;
	}
	peer = peer_priv->dev;

	/* Same card, or a different kind of chip: not a valid peer. */
	if (peer == dev || peer->pdev == dev->pdev ||
	    peer->pdev->vendor != dev->pdev->vendor ||
	    peer->pdev->device != dev->pdev->device) {
		ret = -EINVAL;
		goto out_fdput;
	}

	ret = tt_dev_enter(peer, &idx);
	if (ret)
		goto out_fdput;

	bar_len = pci_resource_len(peer->pdev, in.peer_bar_index);
	if (!(pci_resource_flags(peer->pdev, in.peer_bar_index) & IORESOURCE_MEM) ||
	    (u64)in.peer_bar_offset + in.peer_bar_length > bar_len) {
		ret = -EINVAL;
		goto out_peer;
	}

	pm = kzalloc(sizeof(*pm), GFP_KERNEL);
	if (!pm) {
		ret = -ENOMEM;
		goto out_peer;
	}
	pm->size = in.peer_bar_length;
	pm->peer = pci_dev_get(peer->pdev);

	if (tt_dma_translated(dev->pdev)) {
		/* The IOMMU must map the peer's BAR into our DMA space. */
		pm->dma_addr = dma_map_resource(&dev->pdev->dev,
						pci_resource_start(peer->pdev, in.peer_bar_index) +
						in.peer_bar_offset,
						pm->size, DMA_BIDIRECTIONAL, 0);
		if (dma_mapping_error(&dev->pdev->dev, pm->dma_addr)) {
			pci_dev_put(pm->peer);
			kfree(pm);
			ret = -EIO;
			goto out_peer;
		}
		pm->dma_mapped = true;
	} else {
		/* Untranslated peer-to-peer: the peer's PCI bus address. */
		pm->dma_addr = pci_bus_address(peer->pdev, in.peer_bar_index) + in.peer_bar_offset;
	}

	out.dma_address = pm->dma_addr;
	if (copy_to_user(TT_UOUT(uarg, struct tenstorrent_map_peer_bar), &out, sizeof(out))) {
		tt_peer_mapping_free(priv, pm);
		ret = -EFAULT;
		goto out_peer;
	}

	mutex_lock(&priv->peer_lock);
	if (priv->num_peer_mappings >= TT_MAX_PEER_MAPPINGS) {
		mutex_unlock(&priv->peer_lock);
		tt_peer_mapping_free(priv, pm);
		ret = -ENOSPC;
		goto out_peer;
	}
	list_add_tail(&pm->node, &priv->peer_mappings);
	priv->num_peer_mappings++;
	mutex_unlock(&priv->peer_lock);
	ret = 0;

out_peer:
	tt_dev_exit(peer, idx);
out_fdput:
	fdput(f);
	return ret;
}

/* ---- open / release ---- */

static bool tt_can_admit(struct tenstorrent_device *dev, bool excl)
{
	return !READ_ONCE(dev->excl_open) && (!excl || READ_ONCE(dev->open_count) == 0);
}

/*
 * O_EXCL admission: an open-time reader/writer lock. Plain opens are readers,
 * O_EXCL is the writer. O_NONBLOCK makes it a trylock (-EAGAIN); otherwise
 * wait interruptibly (-ERESTARTSYS).
 */
static int tt_open_admit(struct tenstorrent_device *dev, struct chardev_private *priv,
			 bool nonblock)
{
	int ret;

	for (;;) {
		mutex_lock(&dev->open_lock);
		if (READ_ONCE(dev->detached)) {
			mutex_unlock(&dev->open_lock);
			return -ENODEV;
		}
		if (tt_can_admit(dev, priv->excl)) {
			list_add_tail(&priv->open_fd_node, &dev->open_fds);
			WRITE_ONCE(dev->open_count, dev->open_count + 1);
			if (priv->excl)
				WRITE_ONCE(dev->excl_open, true);
			mutex_unlock(&dev->open_lock);
			return 0;
		}
		mutex_unlock(&dev->open_lock);

		if (nonblock)
			return -EAGAIN;

		ret = wait_event_interruptible(dev->open_wq,
					       READ_ONCE(dev->detached) ||
					       tt_can_admit(dev, priv->excl));
		if (ret)
			return ret;
	}
}

static void tt_open_leave(struct tenstorrent_device *dev, struct chardev_private *priv)
{
	mutex_lock(&dev->open_lock);
	list_del(&priv->open_fd_node);
	WRITE_ONCE(dev->open_count, dev->open_count - 1);
	if (priv->excl)
		WRITE_ONCE(dev->excl_open, false);
	mutex_unlock(&dev->open_lock);
	wake_up_all(&dev->open_wq);
}

static void tt_set_live(struct chardev_private *priv, bool live)
{
	mutex_lock(&priv->dev->open_lock);
	priv->live = live;
	mutex_unlock(&priv->dev->open_lock);
}

/* Per-fd subsystem hooks, in open order. Release runs in this order too:
 * NOC cleanup write first (power is released last, separately). */
struct tt_fd_hooks {
	void (*init)(struct chardev_private *priv);
	int (*open)(struct chardev_private *priv);
	void (*release)(struct chardev_private *priv);
	void (*detach)(struct chardev_private *priv);
};

static const struct tt_fd_hooks tt_fd_hooks[] = {
	{ tt_noc_fd_init, tt_noc_fd_open, tt_noc_fd_release, tt_noc_fd_detach },
	{ tt_tlb_fd_init, tt_tlb_fd_open, tt_tlb_fd_release, tt_tlb_fd_detach },
	{ tt_memory_fd_init, tt_memory_fd_open, tt_memory_fd_release, tt_memory_fd_detach },
	{ tt_smc_fd_init, tt_smc_fd_open, tt_smc_fd_release, tt_smc_fd_detach },
};

static void tt_fd_hooks_release(struct chardev_private *priv, int count)
{
	int i;

	for (i = 0; i < count; i++)
		tt_fd_hooks[i].release(priv);
}

static void tt_priv_free(struct chardev_private *priv)
{
	put_pid(priv->pid);
	tt_device_put(priv->dev);
	kfree(priv);
}

static int tt_cdev_open(struct inode *inode, struct file *filp)
{
	struct tenstorrent_device *dev;
	struct chardev_private *priv;
	int i, ret, idx;

	dev = tt_device_lookup(iminor(inode));
	if (!dev)
		return -ENODEV;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		tt_device_put(dev);
		return -ENOMEM;
	}

	priv->dev = dev;
	priv->filp = filp;
	priv->pid = get_pid(task_tgid(current));
	get_task_comm(priv->comm, current);
	/* O_EXCL is still in f_flags here; the VFS strips it after ->open. */
	priv->excl = !!(filp->f_flags & O_EXCL);
	priv->o_append = !!(filp->f_flags & O_APPEND);
	INIT_LIST_HEAD(&priv->open_fd_node);
	spin_lock_init(&priv->bar_lock);
	INIT_LIST_HEAD(&priv->bar_mappings);
	mutex_init(&priv->peer_lock);
	INIT_LIST_HEAD(&priv->peer_mappings);
	for (i = 0; i < ARRAY_SIZE(tt_fd_hooks); i++)
		tt_fd_hooks[i].init(priv);
	tt_power_fd_init(priv);

	ret = tt_open_admit(dev, priv, filp->f_flags & O_NONBLOCK);
	if (ret)
		goto err_free;

	ret = tt_dev_enter(dev, &idx);
	if (ret)
		goto err_leave;

	for (i = 0; i < ARRAY_SIZE(tt_fd_hooks); i++) {
		ret = tt_fd_hooks[i].open(priv);
		if (ret)
			goto err_hooks;
	}

	ret = tt_power_fd_open(priv, priv->o_append);
	if (ret)
		goto err_hooks;

	tt_dev_exit(dev, idx);

	filp->private_data = priv;
	tt_set_live(priv, true);
	return nonseekable_open(inode, filp);

err_hooks:
	while (i--)
		tt_fd_hooks[i].release(priv);
	tt_dev_exit(dev, idx);
err_leave:
	tt_open_leave(dev, priv);
err_free:
	tt_priv_free(priv);
	return ret;
}

static int tt_cdev_release(struct inode *inode, struct file *filp)
{
	struct chardev_private *priv = filp->private_data;
	struct tenstorrent_device *dev = priv->dev;
	int idx;

	tt_set_live(priv, false);

	/* Hooks run even after removal (dev->detached set): they must free
	 * their software state and skip the hardware then. */
	idx = srcu_read_lock(&dev->detach_srcu);
	tt_fd_hooks_release(priv, ARRAY_SIZE(tt_fd_hooks));
	tt_power_fd_release(priv);
	srcu_read_unlock(&dev->detach_srcu, idx);

	tt_peer_mappings_teardown(priv);
	tt_locks_release_all(priv);

	/* Last: until here remove's fd_detach walk can still see this fd. */
	tt_open_leave(dev, priv);

	/* No BAR records remain: every VMA held a file reference. */
	tt_priv_free(priv);
	return 0;
}

void tt_chardev_detach_fds(struct tenstorrent_device *dev)
{
	struct chardev_private *priv;
	int i;

	mutex_lock(&dev->open_lock);
	list_for_each_entry(priv, &dev->open_fds, open_fd_node) {
		for (i = 0; i < ARRAY_SIZE(tt_fd_hooks); i++)
			tt_fd_hooks[i].detach(priv);
		tt_power_fd_detach(priv);
		tt_peer_mappings_teardown(priv);
	}
	mutex_unlock(&dev->open_lock);
}

struct chardev_private *tt_file_priv(struct file *file)
{
	if (file->f_op != &tt_chardev_fops)
		return NULL;
	return file->private_data;
}

/* ---- ioctl ---- */

static long tt_cdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct chardev_private *priv = filp->private_data;
	struct tenstorrent_device *dev = priv->dev;
	void __user *uarg = (void __user *)arg;
	long ret;
	int idx;

	/* Pure software and may block indefinitely: not in a detach section. */
	if (cmd == TENSTORRENT_IOCTL_LOCK_CTL)
		return tt_ioctl_lock_ctl(priv, uarg);

	ret = tt_dev_enter(dev, &idx);
	if (ret)
		return ret;

	switch (cmd) {
	case TENSTORRENT_IOCTL_GET_DEVICE_INFO:
		ret = tt_ioctl_get_device_info(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_QUERY_MAPPINGS:
		ret = tt_ioctl_query_mappings(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF:
		ret = tt_memory_ioctl_allocate_dma_buf(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_FREE_DMA_BUF:
		ret = tt_memory_ioctl_free_dma_buf(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_GET_DRIVER_INFO:
		ret = tt_ioctl_get_driver_info(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_RESET_DEVICE:
		ret = tt_ioctl_reset_device(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_PIN_PAGES:
		ret = tt_memory_ioctl_pin_pages(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_MAP_PEER_BAR:
		ret = tt_ioctl_map_peer_bar(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_UNPIN_PAGES:
		ret = tt_memory_ioctl_unpin_pages(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_ALLOCATE_TLB:
		ret = tt_tlb_ioctl_allocate(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_FREE_TLB:
		ret = tt_tlb_ioctl_free(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_CONFIGURE_TLB:
		ret = tt_tlb_ioctl_configure(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_SET_NOC_CLEANUP:
		ret = tt_noc_ioctl_set_cleanup(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_SET_POWER_STATE:
		ret = tt_power_ioctl_set_state(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF:
		ret = tt_tlb_ioctl_export_dmabuf(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_SMC_MSG:
		ret = tt_smc_ioctl_msg(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_NOC_READ:
		ret = tt_noc_ioctl_read(priv, uarg);
		break;
	case TENSTORRENT_IOCTL_NOC_WRITE:
		ret = tt_noc_ioctl_write(priv, uarg);
		break;
	default:	/* includes the obsolete GET_HARVESTING */
		ret = -ENOTTY;
		break;
	}

	tt_dev_exit(dev, idx);
	return ret;
}

/* ---- lazily faulted MMIO mappings ---- */

#define TT_MMIO_FAULT_CHUNK_PAGES	(SZ_2M >> PAGE_SHIFT)

static bool tt_mmio_page_ok(struct tt_mmio_mapping *m, unsigned long pg)
{
	return !m->ops->page_allowed || m->ops->page_allowed(m, (u64)pg << PAGE_SHIFT);
}

static vm_fault_t tt_mmio_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct tt_mmio_mapping *m = vma->vm_private_data;
	struct tenstorrent_device *dev = m->priv->dev;
	unsigned long npages = m->size >> PAGE_SHIFT;
	unsigned long vma_first, vma_end, first, last, pg, fault_pg;
	vm_fault_t ret;
	int idx;

	/* Page indices relative to the start of the original mapping. */
	if (vmf->pgoff < m->pgoff || vmf->pgoff - m->pgoff >= npages)
		return VM_FAULT_SIGBUS;
	fault_pg = vmf->pgoff - m->pgoff;

	/* No PTE may appear once remove has started (it zaps afterwards). */
	if (tt_dev_enter(dev, &idx))
		return VM_FAULT_SIGBUS;

	if (!tt_mmio_page_ok(m, fault_pg)) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	ret = vmf_insert_pfn_prot(vma, vmf->address, PHYS_PFN(m->phys) + fault_pg,
				  vma->vm_page_prot);
	if (ret & VM_FAULT_ERROR)
		goto out;

	/* Prefault the rest of the 2 MiB chunk that lies inside this VMA. */
	vma_first = vma->vm_pgoff - m->pgoff;
	vma_end = min(vma_first + vma_pages(vma), npages);
	first = max(fault_pg & ~(TT_MMIO_FAULT_CHUNK_PAGES - 1), vma_first);
	last = min((fault_pg | (TT_MMIO_FAULT_CHUNK_PAGES - 1)) + 1, vma_end);
	for (pg = first; pg < last; pg++) {
		unsigned long addr = vma->vm_start + ((pg - vma_first) << PAGE_SHIFT);

		if (pg == fault_pg || !tt_mmio_page_ok(m, pg))
			continue;
		if (vmf_insert_pfn_prot(vma, addr, PHYS_PFN(m->phys) + pg,
					vma->vm_page_prot) & VM_FAULT_ERROR)
			break;
	}
	ret = VM_FAULT_NOPAGE;
out:
	tt_dev_exit(dev, idx);
	return ret;
}

static void tt_mmio_kref_release(struct kref *kref)
{
	struct tt_mmio_mapping *m = container_of(kref, struct tt_mmio_mapping, kref);

	if (m->wc_reserved)
		arch_io_free_memtype_wc(m->phys, m->size);
	m->ops->release(m);
}

static void tt_mmio_vm_open(struct vm_area_struct *vma)
{
	struct tt_mmio_mapping *m = vma->vm_private_data;

	kref_get(&m->kref);
}

static void tt_mmio_vm_close(struct vm_area_struct *vma)
{
	struct tt_mmio_mapping *m = vma->vm_private_data;

	kref_put(&m->kref, tt_mmio_kref_release);
}

static int tt_mmio_may_split(struct vm_area_struct *vma, unsigned long addr)
{
	struct tt_mmio_mapping *m = vma->vm_private_data;

	return m->no_split ? -EINVAL : 0;
}

static const struct vm_operations_struct tt_mmio_vm_ops = {
	.open = tt_mmio_vm_open,
	.close = tt_mmio_vm_close,
	.may_split = tt_mmio_may_split,
	.fault = tt_mmio_fault,
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

int tt_mmio_mmap(struct vm_area_struct *vma, struct tt_mmio_mapping *m)
{
	int ret;

	/* MAP_SHARED only: a COW mapping of MMIO makes no sense. */
	if (!(vma->vm_flags & VM_MAYSHARE))
		return -EINVAL;

	m->size = vma->vm_end - vma->vm_start;
	m->pgoff = vma->vm_pgoff;
	m->va = vma->vm_start;
	m->wc_reserved = false;

	if (m->wc) {
		/* With PAT, inserted PFNs get the memtype reserved for their
		 * physical range; without a reservation they would be UC-. */
		ret = arch_io_reserve_memtype_wc(m->phys, m->size);
		if (ret)
			return ret;
		m->wc_reserved = true;
	}

	kref_init(&m->kref);
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = m->wc ? pgprot_writecombine(vma->vm_page_prot)
				  : pgprot_noncached(vma->vm_page_prot);
	vma->vm_private_data = m;
	vma->vm_ops = &tt_mmio_vm_ops;
	return 0;
}

/* ---- BAR mmap ---- */

/* Pages of BAR0 user mappings must not reach: the kernel's window and the
 * TLB config page (which would let userspace retarget the kernel window). */
static bool tt_bar0_page_allowed(struct tt_mmio_mapping *m, u64 off)
{
	struct tt_bar_mapping *bm = container_of(m, struct tt_bar_mapping, mmio);
	u64 bar_off = bm->bar_offset + off;
	u64 kwin = bh_tlb_window_bar_offset(BH_KERNEL_TLB_INDEX);

	if (bar_off >= kwin && bar_off < kwin + BH_TLB_2M_SIZE)
		return false;
	if (bar_off >= BH_TLB_CONFIG_BAR0_OFFSET && bar_off < BH_TLB_CONFIG_BAR0_OFFSET + PAGE_SIZE)
		return false;
	return true;
}

static void tt_bar_mapping_release(struct tt_mmio_mapping *m)
{
	struct tt_bar_mapping *bm = container_of(m, struct tt_bar_mapping, mmio);
	struct chardev_private *priv = m->priv;

	spin_lock(&priv->bar_lock);
	list_del(&bm->node);
	spin_unlock(&priv->bar_lock);
	kfree(bm);
}

static const struct tt_mmio_ops tt_bar0_mmio_ops = {
	.page_allowed = tt_bar0_page_allowed,
	.release = tt_bar_mapping_release,
};

static const struct tt_mmio_ops tt_bar_mmio_ops = {
	.release = tt_bar_mapping_release,
};

static int tt_bar_mmap(struct chardev_private *priv, struct vm_area_struct *vma, u64 off)
{
	struct tenstorrent_device *dev = priv->dev;
	u64 size = vma->vm_end - vma->vm_start;
	u64 rel = off & (TT_MMAP_BAR_REGION_SIZE - 1);
	const struct tt_mmio_ops *ops;
	resource_size_t start, len;
	struct tt_bar_mapping *bm;
	unsigned int bar;
	int ret;

	switch (off >> TT_MMAP_BAR_REGION_SHIFT) {
	case TT_MMAP_BAR0_UC >> TT_MMAP_BAR_REGION_SHIFT:
		bar = 0, start = dev->bar0_start, len = dev->bar0_len, ops = &tt_bar0_mmio_ops;
		break;
	case TT_MMAP_BAR2_UC >> TT_MMAP_BAR_REGION_SHIFT:
		bar = 2, start = dev->bar2_start, len = dev->bar2_len, ops = &tt_bar_mmio_ops;
		break;
	default:
		return -EINVAL;
	}

	if (rel > len || size > len - rel)
		return -EINVAL;

	bm = kzalloc(sizeof(*bm), GFP_KERNEL);
	if (!bm)
		return -ENOMEM;
	bm->bar = bar;
	bm->bar_offset = rel;
	bm->mmio.priv = priv;
	bm->mmio.ops = ops;
	bm->mmio.phys = start + rel;
	bm->mmio.wc = false;

	ret = tt_mmio_mmap(vma, &bm->mmio);
	if (ret) {
		kfree(bm);
		return ret;
	}

	spin_lock(&priv->bar_lock);
	list_add_tail(&bm->node, &priv->bar_mappings);
	spin_unlock(&priv->bar_lock);
	return 0;
}

static int tt_cdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct chardev_private *priv = filp->private_data;
	struct tenstorrent_device *dev = priv->dev;
	u64 off, size, end;
	int ret, idx;

	if (vma->vm_pgoff >= (TT_MMAP_LIMIT >> PAGE_SHIFT))
		return -EINVAL;
	off = (u64)vma->vm_pgoff << PAGE_SHIFT;
	size = vma->vm_end - vma->vm_start;
	end = off + size;	/* cannot overflow: off < 2^44, size < 2^57 */

	ret = tt_dev_enter(dev, &idx);
	if (ret)
		return ret;

	if (end <= TT_MMAP_BAR_END)
		ret = tt_bar_mmap(priv, vma, off);
	else if (off >= TT_MMAP_DMA_BUF_BASE && end <= TT_MMAP_DMA_BUF_END)
		ret = tt_memory_mmap(priv, vma);
	else if (off >= TT_MMAP_TLB_BASE && end <= TT_MMAP_TLB_END)
		ret = tt_tlb_mmap(priv, vma);
	else
		ret = -EINVAL;

	tt_dev_exit(dev, idx);
	return ret;
}

void tt_chardev_zap_mappings(struct tenstorrent_device *dev)
{
	struct chardev_private *priv;

	mutex_lock(&dev->open_lock);
	list_for_each_entry(priv, &dev->open_fds, open_fd_node)
		unmap_mapping_range(priv->filp->f_mapping, 0, 0, 1);
	mutex_unlock(&dev->open_lock);
}

const struct file_operations tt_chardev_fops = {
	.owner = THIS_MODULE,
	.open = tt_cdev_open,
	.release = tt_cdev_release,
	.unlocked_ioctl = tt_cdev_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.mmap = tt_cdev_mmap,
	.llseek = no_llseek,
};

/* ---- device node ---- */

int tt_chardev_add(struct tenstorrent_device *dev)
{
	struct device *cd;
	int ret;

	dev->cdev = cdev_alloc();
	if (!dev->cdev)
		return -ENOMEM;
	dev->cdev->owner = THIS_MODULE;
	dev->cdev->ops = &tt_chardev_fops;

	ret = cdev_add(dev->cdev, dev->devt, 1);
	if (ret) {
		kobject_put(&dev->cdev->kobj);
		dev->cdev = NULL;
		return ret;
	}

	/* "tenstorrent!N" -> /dev/tenstorrent/N, and
	 * /sys/bus/pci/devices/<bdf>/tenstorrent/tenstorrent!N/dev */
	cd = device_create(tt_class, &dev->pdev->dev, dev->devt, dev,
			   TT_DRIVER_NAME "!%d", dev->ordinal);
	if (IS_ERR(cd)) {
		cdev_del(dev->cdev);
		dev->cdev = NULL;
		return PTR_ERR(cd);
	}
	dev->chardev = cd;
	return 0;
}

void tt_chardev_remove(struct tenstorrent_device *dev)
{
	if (dev->chardev)
		device_destroy(tt_class, dev->devt);
	dev->chardev = NULL;
	if (dev->cdev)
		cdev_del(dev->cdev);
	dev->cdev = NULL;
}
