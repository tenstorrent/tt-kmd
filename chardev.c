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
#include <linux/debugfs.h>
#include <linux/proc_fs.h>

#include "chardev_private.h"
#include "device.h"
#include "enumerate.h"
#include "ioctl.h"
#include "pcie.h"
#include "memory.h"
#include "module.h"
#include "tlb.h"

#define TT_POWER_FLAG_ALL 0x7FFF

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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0) || TT_RHEL_RELEASE_GE(9, 4)
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
	char name[16];

	init_waitqueue_head(&tt_dev->resource_lock_waitqueue);
	init_waitqueue_head(&tt_dev->chardev_excl_waitqueue);

	device_initialize(&tt_dev->dev);
	tt_dev->dev.devt = devt;
	tt_dev->dev.class = tt_dev_class;
	tt_dev->dev.parent = &tt_dev->pdev->dev;
	tt_dev->dev.groups = NULL;
	tt_dev->dev.release = NULL;

	tt_dev->dev.id = tt_dev->ordinal;
	dev_set_name(&tt_dev->dev, TENSTORRENT "/%d", tt_dev->ordinal);

	snprintf(name, sizeof(name), "%d", tt_dev->ordinal);
	if (tt_debugfs_root)
		tt_dev->debugfs_root = debugfs_create_dir(name, tt_debugfs_root);
	tt_dev->procfs_root = proc_mkdir(name, tt_procfs_root);
	if (tt_dev->procfs_root)
		proc_create_single_data("pids", 0444, tt_dev->procfs_root, pids_proc_show, tt_dev);

	INIT_LIST_HEAD(&tt_dev->open_fds_list);

	cdev_init(&tt_dev->chardev, &chardev_fops);
	return cdev_device_add(&tt_dev->chardev, &tt_dev->dev);
}

void tenstorrent_unregister_device(struct tenstorrent_device *tt_dev)
{
	debugfs_remove_recursive(tt_dev->debugfs_root);
	proc_remove(tt_dev->procfs_root);
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

// Bump the device's reset generation and bring this fd's open_reset_gen along
// with it. Other fds become permanently invalid, but the resetter keeps a live
// fd so it can complete the reset sequence without a close/reopen window.
static void bump_reset_gen(struct chardev_private *priv)
{
	priv->open_reset_gen = atomic_long_inc_return(&priv->device->reset_gen);
}

// Reclaim TLB windows from fds this reset invalidated.
// An invalidated fd can only free its windows at close(), which a wedged
// process may never reach, leaking windows until the pool is exhausted.
// Caller holds reset_rwsem exclusive and has already called bump_reset_gen()
// and tenstorrent_vma_zap().
static void tenstorrent_reset_reclaim_tlbs(struct tenstorrent_device *tt_dev)
{
	struct chardev_private *priv;
	long gen = atomic_long_read(&tt_dev->reset_gen);
	unsigned int bitpos;

	mutex_lock(&tt_dev->chardev_mutex);
	list_for_each_entry(priv, &tt_dev->open_fds_list, open_fd) {
		// Skip the resetting fd, since it survives the reset.
		if (priv->open_reset_gen == gen)
			continue;

		mutex_lock(&priv->tlb_mutex);
		for_each_set_bit(bitpos, priv->tlbs, TENSTORRENT_MAX_INBOUND_TLBS) {
			tenstorrent_device_free_tlb(tt_dev, bitpos);
			clear_bit(bitpos, priv->tlbs);
		}
		mutex_unlock(&priv->tlb_mutex);
	}
	mutex_unlock(&tt_dev->chardev_mutex);
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

	// Refuse a destructive in-place reset while any TLB window is exported as
	// a dma-buf: an importer may be doing peer-to-peer DMA into it, and a
	// pin-only importer cannot be revoked to make the reset safe. See the
	// EXPORT_TLB_DMABUF rationale in ioctl.h. RESTORE_STATE/POST_RESET are the
	// re-init halves of a reset sequence, not destructive, so they are left
	// alone.
	switch (in.flags) {
	case TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK:
	case TENSTORRENT_RESET_DEVICE_CONFIG_WRITE:
	case TENSTORRENT_RESET_DEVICE_USER_RESET:
	case TENSTORRENT_RESET_DEVICE_ASIC_RESET:
	case TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET:
		if (tenstorrent_has_tlb_dmabuf_exports(tt_dev))
			return -EBUSY;
		break;
	default:
		break;
	}

	// Drain any deferred idle powerdown before disturbing the device.
	// open()/release() take reset_rwsem shared and we hold it exclusive
	// here, so no new arm can land between the cancel and the reset.
	cancel_delayed_work_sync(&tt_dev->power_down_work);

	if (in.flags == TENSTORRENT_RESET_DEVICE_RESTORE_STATE) {
		if (safe_pci_restore_state(pdev)) {
			priv->device->dev_class->restore_reset_state(priv->device);
			ok = priv->device->dev_class->init_hardware(priv->device);
		} else {
			ok = false;
		}
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK) {
		tenstorrent_vma_zap(tt_dev);
		ok = pcie_hot_reset_and_restore_state(pdev);
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_CONFIG_WRITE) {
		bump_reset_gen(priv);
		tenstorrent_vma_zap(tt_dev);
		tenstorrent_reset_reclaim_tlbs(tt_dev);
		tenstorrent_reset_reclaim_iatus(tt_dev);
		ok = pcie_timer_interrupt(pdev);
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_USER_RESET) {
		bump_reset_gen(priv);
		tenstorrent_vma_zap(tt_dev);
		tenstorrent_reset_reclaim_tlbs(tt_dev);
		tenstorrent_reset_reclaim_iatus(tt_dev);
		ok = set_reset_marker(pdev);
		priv->device->needs_hw_init = true;
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_ASIC_RESET) {
		bump_reset_gen(priv);
		tenstorrent_vma_zap(tt_dev);
		tenstorrent_reset_reclaim_tlbs(tt_dev);
		tenstorrent_reset_reclaim_iatus(tt_dev);
		ok = priv->device->dev_class->reset(priv->device, in.flags);
		priv->device->needs_hw_init = true;
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET) {
		bump_reset_gen(priv);
		tenstorrent_vma_zap(tt_dev);
		tenstorrent_reset_reclaim_tlbs(tt_dev);
		tenstorrent_reset_reclaim_iatus(tt_dev);
		ok = priv->device->dev_class->reset(priv->device, in.flags);
		priv->device->needs_hw_init = true;
	} else if (in.flags == TENSTORRENT_RESET_DEVICE_POST_RESET) {
		ok = is_reset_marker_zero(pdev);

		// In the hotplug case, needs_hw_init is false and there is nothing to
		// do here. Otherwise this was an in-place reset, so re-initialize now.
		if (priv->device->needs_hw_init) {
			priv->device->needs_hw_init = false;
			if (ok && safe_pci_restore_state(pdev)) {
				priv->device->dev_class->restore_reset_state(priv->device);
				ok = priv->device->dev_class->init_hardware(priv->device);

				// Re-probe telemetry tag addresses in case
				// firmware was updated before this reset.
				if (ok && priv->device->dev_class->probe_telemetry)
					priv->device->dev_class->probe_telemetry(priv->device);
			} else {
				ok = false;
			}
		}
	} else {
		return -EINVAL;
	}

	out.output_size_bytes = sizeof(out);
	out.result = !ok;

	// Wake any LOCK_CTL ACQUIRE_BLOCKING waiters. If reset_gen was bumped
	// or the device is otherwise unusable for them, they will observe it
	// and return -ENODEV instead of waiting forever for a lock that no
	// pre-reset fd can ever release via ioctl.
	wake_up_interruptible(&tt_dev->resource_lock_waitqueue);

	if (clear_user(&arg->out, in.output_size_bytes) != 0)
		return -EFAULT;

	bytes_to_copy = min(in.output_size_bytes, (u32)sizeof(out));

	if (copy_to_user(&arg->out, &out, bytes_to_copy) != 0)
		return -EFAULT;

	return 0;
}

// Resource locks survive reset.  A reset invalidates every pre-reset fd
// (open_reset_gen no longer matches reset_gen), so the holder cannot release
// via ioctl; only close(fd) clears the bits.  ACQUIRE_BLOCKING from a
// pre-reset fd therefore returns -ENODEV after a reset because the fd is no
// longer allowed to acquire anything.
//
// Holding reset_rwsem across wait_event_interruptible deadlocks RESET_DEVICE
// (which needs the rwsem for write) and starves all other ioctls because the
// rwsem is writer-fair.  Drop it for the duration of the wait and reacquire
// before returning so the caller's matching up_read in tt_cdev_ioctl stays
// balanced.
static long acquire_resource_lock_blocking(struct chardev_private *priv, u8 idx)
{
	struct tenstorrent_device *tt_dev = priv->device;
	bool got_bit = false;
	long w = 0;

	up_read(&tt_dev->reset_rwsem);

	while (!got_bit) {
		w = wait_event_interruptible(tt_dev->resource_lock_waitqueue,
			!test_bit(idx, tt_dev->resource_lock) ||
			tt_dev->detached ||
			atomic_long_read(&tt_dev->reset_gen) != priv->open_reset_gen);
		if (w)
			break;
		if (tt_dev->detached ||
		    atomic_long_read(&tt_dev->reset_gen) != priv->open_reset_gen)
			break;
		got_bit = !test_and_set_bit(idx, tt_dev->resource_lock);
	}

	down_read(&tt_dev->reset_rwsem);

	if (w)
		return -ERESTARTSYS;

	// If a reset/remove raced in after we set the device bit, give the
	// bit back: the reset is treated as having won against the unlock
	// that would have made the bit takeable.  The caller never observes
	// a successful acquire on a fd that is now invalid.
	if (tt_dev->detached ||
	    atomic_long_read(&tt_dev->reset_gen) != priv->open_reset_gen) {
		if (got_bit) {
			clear_bit(idx, tt_dev->resource_lock);
			wake_up_interruptible(&tt_dev->resource_lock_waitqueue);
		}
		return -ENODEV;
	}

	if (!got_bit)
		return -ENODEV;

	set_bit(idx, priv->resource_lock);
	return 0;
}

// Ordering invariant: acquire sets global then local; release clears local then
// global. This ensures the global bit is always set while the local bit is set.
static long ioctl_lock_ctl(struct chardev_private *priv, struct tenstorrent_lock_ctl __user *arg)
{
	u32 bytes_to_copy;
	long blocking_ret;

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
		if (!test_and_set_bit(in.index, priv->device->resource_lock)) {
			set_bit(in.index, priv->resource_lock);
			out.value = 1;	// Acquired
		} else {
			out.value = 0;	// Failed to acquire
		}
		break;
	case TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING:
		blocking_ret = acquire_resource_lock_blocking(priv, in.index);
		if (blocking_ret)
			return blocking_ret;
		out.value = 1;
		break;
	case TENSTORRENT_LOCK_CTL_RELEASE:
		if (test_and_clear_bit(in.index, priv->resource_lock)) {
			clear_bit(in.index, priv->device->resource_lock);
			wake_up_interruptible(&priv->device->resource_lock_waitqueue);
			out.value = 1;	// Released
		} else {
			out.value = 0;	// Failed to release
		}
		break;
	case TENSTORRENT_LOCK_CTL_TEST:
		// Bit 0: this fd holds the lock. Bit 1: any fd holds the lock.
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

static int tenstorrent_set_aggregated_power_state_locked(struct tenstorrent_device *tt_dev)
{
	struct tenstorrent_power_state power_state = { 0 };
	struct chardev_private *priv;
	u8 max_settings_count = 0;

	lockdep_assert_held(&tt_dev->chardev_mutex);

	list_for_each_entry(priv, &tt_dev->open_fds_list, open_fd) {
		u8 flags_count;
		u8 settings_count;
		u16 unspecified_flags_mask;
		u16 effective_flags;
		int i;

		// Skip fds that have a different reset generation than the device.
		if (atomic_long_read(&tt_dev->reset_gen) != priv->open_reset_gen)
			continue;

		mutex_lock(&priv->mutex);

		// Extract validity counts from the packed validity field.
		// Bits 0-3: number of valid flags (0-15)
		// Bits 4-7: number of valid settings (0-14)
		flags_count = (priv->power_state.validity >> 0) & 0xF;
		settings_count = (priv->power_state.validity >> 4) & 0xF;
		max_settings_count = max(max_settings_count, settings_count);

		// Create a mask of flags the FD didn't specify (bits past flags_count).
		// These unspecified flags default to ON for backward compatibility.
		// Bit 15 is reserved per firmware spec, so mask to bits 0-14 (0x7FFF).
		unspecified_flags_mask = ~((1U << flags_count) - 1) & TT_POWER_FLAG_ALL;

		// Apply the aggregation formula: OR this FD's explicit power_flags
		// with the unspecified flags (defaulted to ON). This ensures older
		// clients that don't know about newer power flags won't turn them off.
		effective_flags = priv->power_state.power_flags | unspecified_flags_mask;

		// OR this FD's effective flags into the aggregate.
		power_state.power_flags |= effective_flags;

		// Aggregate power_settings: for each setting index, take the maximum
		// value across all FDs that have marked that setting as valid.
		settings_count = min_t(u8, settings_count, ARRAY_SIZE(power_state.power_settings));
		for (i = 0; i < settings_count; i++)
			power_state.power_settings[i] = max(power_state.power_settings[i], priv->power_state.power_settings[i]);

		mutex_unlock(&priv->mutex);
	}

	// Always send maximum validity (15 flags, max_settings_count settings) to
	// firmware. This ensures FW applies all bits of the aggregated power_flags,
	// regardless of what validity individual FDs specified.
	power_state.validity = TT_POWER_VALIDITY(15, max_settings_count);

	return tt_dev->dev_class->set_power_state(tt_dev, &power_state);
}

int tenstorrent_set_aggregated_power_state(struct tenstorrent_device *tt_dev)
{
	int ret;

	mutex_lock(&tt_dev->chardev_mutex);
	ret = tenstorrent_set_aggregated_power_state_locked(tt_dev);
	mutex_unlock(&tt_dev->chardev_mutex);

	return ret;
}

// Delayed work: send the aggregated idle power-down message after the
// grace period elapses with no open fds.  Safe to run unguarded:
// tt_cdev_release only arms this under chardev_mutex when !detached,
// and tenstorrent_pci_remove writes detached=true under the same mutex
// before cancel_delayed_work_sync, so the handler cannot fire after
// teardown begins.
void tenstorrent_power_down_work_func(struct work_struct *work)
{
	struct tenstorrent_device *tt_dev = container_of(to_delayed_work(work),
							 struct tenstorrent_device,
							 power_down_work);

	tenstorrent_set_aggregated_power_state(tt_dev);
}

static long ioctl_set_power_state(struct chardev_private *priv, struct tenstorrent_power_state __user *arg)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct tenstorrent_power_state data = {0};

	if (copy_from_user(&data, arg, sizeof(data)) != 0)
		return -EFAULT;

	if (data.argsz != sizeof(data))
		return -EINVAL;

	if (data.flags != 0 || data.reserved0 != 0)
		return -EINVAL;

	// validity encodes power_flags count in bits 0-3 (max 0xF=15) and
	// power_settings count in bits 4-7 (max 0xE=14), see ioctl.h for detail.
	if (data.validity > TT_POWER_VALIDITY(15, 14))
		return -EINVAL;

	dev_dbg(&tt_dev->pdev->dev, "Power state request: validity=0x%x flags=0x%x\n",
		data.validity, data.power_flags);

	mutex_lock(&priv->mutex);
	priv->power_state = data;
	mutex_unlock(&priv->mutex);

	return tenstorrent_set_aggregated_power_state(tt_dev);
}

static long tt_cdev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct chardev_private *priv = f->private_data;
	struct tenstorrent_device *tt_dev = priv->device;
	long ret = -EINVAL;
	bool is_reset_ioctl = (cmd == TENSTORRENT_IOCTL_RESET_DEVICE);

	if (is_reset_ioctl)
		down_write(&tt_dev->reset_rwsem);	// Exclusive
	else
		down_read(&tt_dev->reset_rwsem);	// Shared

	// File descriptor from a removed/hotplugged device is permanently invalid.
	if (priv->device->detached) {
		ret = -ENODEV;
		goto out;
	}

	// File descriptor opened before the reset is permanently invalid.
	if (atomic_long_read(&priv->device->reset_gen) != priv->open_reset_gen) {
		ret = -ENODEV;
		goto out;
	}

	// During reset window, only allow info queries and reset operations.
	if (priv->device->needs_hw_init) {
		bool allowed = (cmd == TENSTORRENT_IOCTL_GET_DEVICE_INFO ||
				cmd == TENSTORRENT_IOCTL_GET_DRIVER_INFO ||
				cmd == TENSTORRENT_IOCTL_RESET_DEVICE);
		if (!allowed) {
			ret = -ENODEV;
			goto out;
		}
	}

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

		case TENSTORRENT_IOCTL_SET_POWER_STATE:
			ret = ioctl_set_power_state(priv, (struct tenstorrent_power_state __user *)arg);
			break;

		case TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF:
			ret = ioctl_export_tlb_dmabuf(priv, (struct tenstorrent_export_tlb_dmabuf __user *)arg);
			break;

		default:
			ret = -EINVAL;
			break;
	}

out:
	if (is_reset_ioctl)
		up_write(&tt_dev->reset_rwsem);
	else
		up_read(&tt_dev->reset_rwsem);

	return ret;
}

static int tt_cdev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct chardev_private *priv = file->private_data;
	struct tenstorrent_device *tt_dev = priv->device;
	int ret;

	// Use trylock to avoid ABBA deadlock: mmap_lock is held by the kernel, and
	// tenstorrent_vma_zap() needs mmap_lock while holding reset_rwsem. If reset
	// is in progress, this fd will be stale after reset anyway.
	if (!down_read_trylock(&tt_dev->reset_rwsem))
		return -ENODEV;

	if (tt_dev->detached) {
		ret = -ENODEV;
		goto out;
	}

	// File descriptor opened before reset is permanently invalid.
	if (atomic_long_read(&tt_dev->reset_gen) != priv->open_reset_gen) {
		ret = -ENODEV;
		goto out;
	}

	ret = tenstorrent_mmap(priv, vma);

out:
	up_read(&tt_dev->reset_rwsem);
	return ret;
}

static struct tenstorrent_device *inode_to_tt_dev(struct inode *inode)
{
	return container_of(inode->i_cdev, struct tenstorrent_device, chardev);
}

// Arbitrate open vs existing fds.  This is an open()-time reader/writer lock:
// O_EXCL is the writer and plain opens are readers.  A writer waits for the
// device to be idle; a reader waits for any O_EXCL holder to go away.  In both
// cases O_NONBLOCK turns the wait into -EAGAIN.  While an O_EXCL fd is held it
// is the only fd on the list, so the holder's release is exactly the
// list_empty transition that wakes both kinds of waiter.
static int admit_chardev_open(struct tenstorrent_device *tt_dev, struct chardev_private *priv, unsigned int f_flags)
{
	bool want_excl = f_flags & O_EXCL;
	bool nonblock = f_flags & O_NONBLOCK;
	int ret;

	mutex_lock(&tt_dev->chardev_mutex);

	if (want_excl) {
		// Writer: wait until no other fd is open, then take exclusivity.
		while (!list_empty(&tt_dev->open_fds_list)) {
			mutex_unlock(&tt_dev->chardev_mutex);
			if (nonblock)
				return -EAGAIN;
			ret = wait_event_interruptible_exclusive(tt_dev->chardev_excl_waitqueue,
								 list_empty(&tt_dev->open_fds_list));
			if (ret)
				return ret;
			mutex_lock(&tt_dev->chardev_mutex);
		}
		WRITE_ONCE(tt_dev->chardev_excl_held, true);
	} else {
		// Reader: wait until no O_EXCL fd is held.  Registered non-exclusively
		// so all readers wake together when the holder releases.
		while (tt_dev->chardev_excl_held) {
			mutex_unlock(&tt_dev->chardev_mutex);
			if (nonblock)
				return -EAGAIN;
			ret = wait_event_interruptible(tt_dev->chardev_excl_waitqueue,
						       !READ_ONCE(tt_dev->chardev_excl_held));
			if (ret)
				return ret;
			mutex_lock(&tt_dev->chardev_mutex);
		}
	}

	list_add(&priv->open_fd, &tt_dev->open_fds_list);

	mutex_unlock(&tt_dev->chardev_mutex);
	return 0;
}

static int tt_cdev_open(struct inode *inode, struct file *file)
{
	struct tenstorrent_device *tt_dev = inode_to_tt_dev(inode);
	struct chardev_private *private_data;
	bool power_aware = file->f_flags & O_APPEND;
	int ret;

	private_data = kzalloc(sizeof(*private_data), GFP_KERNEL);
	if (private_data == NULL)
		return -ENOMEM;

	mutex_init(&private_data->mutex);

	hash_init(private_data->dmabufs);
	INIT_LIST_HEAD(&private_data->pinnings);
	INIT_LIST_HEAD(&private_data->peer_mappings);
	INIT_LIST_HEAD(&private_data->vma_list);
	mutex_init(&private_data->vma_lock);
	mutex_init(&private_data->tlb_mutex);

	kref_get(&tt_dev->kref);
	private_data->device = tt_dev;
	private_data->open_reset_gen = atomic_long_read(&tt_dev->reset_gen);

	private_data->pid = get_pid(task_pid(current->group_leader));
	get_task_comm(private_data->comm, current);

	// Legacy client: default to AICLK=Low, everything else enabled.
	// Else, client is expected to explicitly request power states via ioctl.
	// Initialize before admit_chardev_open() so this fd is fully initialized
	// before it becomes visible on open_fds_list.
	private_data->power_state.validity = TT_POWER_VALIDITY(15, 0);
	if (!power_aware)
		private_data->power_state.power_flags = TT_POWER_FLAG_ALL & ~TT_POWER_FLAG_MAX_AI_CLK;

	ret = admit_chardev_open(tt_dev, private_data, file->f_flags);
	if (ret) {
		put_pid(private_data->pid);
		kfree(private_data);
		tenstorrent_device_put(tt_dev);
		return ret;
	}

	// Hold reset_rwsem (shared) across the body so the reset ioctl (which holds
	// it exclusive) cannot interleave with the deferred powerdown cancel or the
	// initial aggregation.  Without this, a reset that drained the work could
	// find a fresh arm landed by this open after its cancel on the
	// power_down_work.
	down_read(&tt_dev->reset_rwsem);

	// Eagerly drain any deferred powerdown armed by a previous last-close so an
	// open() arriving during the grace window doesn't ship a spurious
	// powerdown->powerup pair.  Correctness doesn't depend on this: the handler
	// re-aggregates current state and would ship the up-state since this fd is
	// already on the list.  If the work is merely pending, the cancel is silent
	// (no FW traffic, chip is still up); if mid-fire, we wait for the powerdown
	// to complete and the aggregation below issues the balancing powerup.  Safe
	// no-op when nothing is armed.
	if (tt_dev->dev_class->defer_idle_powerdown
	    && cancel_delayed_work_sync(&tt_dev->power_down_work))
		dev_dbg(&tt_dev->pdev->dev, "cancelled pending idle powerdown\n");

	if (!power_aware && !tt_dev->detached && !tt_dev->needs_hw_init) {
		ret = tenstorrent_set_aggregated_power_state(tt_dev);
		if (ret < 0)
			dev_warn(&tt_dev->pdev->dev, "Failed to set initial power state: %d\n", ret);
	}

	up_read(&tt_dev->reset_rwsem);

	file->private_data = private_data;

	return 0;
}

static void tt_cdev_release_noc_cleanup(struct chardev_private *priv)
{
	struct tenstorrent_device *tt_dev = priv->device;

	if (tt_dev->detached || !priv->noc_cleanup.enabled)
		return;

	tt_dev->dev_class->noc_write32(tt_dev, priv->noc_cleanup.x, priv->noc_cleanup.y,
				       priv->noc_cleanup.addr, priv->noc_cleanup.data & 0xFFFFFFFF,
				       priv->noc_cleanup.noc);
}

static void tt_cdev_release_resource_locks(struct chardev_private *priv)
{
	unsigned int bitpos;
	for (bitpos = 0; bitpos < TENSTORRENT_RESOURCE_LOCK_COUNT; ++bitpos) {
		if (test_and_clear_bit(bitpos, priv->resource_lock))
			clear_bit(bitpos, priv->device->resource_lock);
	}
	wake_up_interruptible(&priv->device->resource_lock_waitqueue);
}

static void tt_cdev_release_tlbs(struct chardev_private *priv)
{
	unsigned int bitpos;

	mutex_lock(&priv->tlb_mutex);
	for_each_set_bit(bitpos, priv->tlbs, TENSTORRENT_MAX_INBOUND_TLBS) {
		tenstorrent_device_free_tlb(priv->device, bitpos);
		clear_bit(bitpos, priv->tlbs);
	}
	mutex_unlock(&priv->tlb_mutex);
}

static void tt_cdev_release_power(struct chardev_private *priv)
{
	struct tenstorrent_device *tt_dev = priv->device;
	bool can_defer;
	bool no_power_contrib;
	bool last_close;

	lockdep_assert_held(&tt_dev->chardev_mutex);

	can_defer = tt_dev->dev_class->defer_idle_powerdown && (idle_power_down_grace_ms > 0);
	no_power_contrib = (priv->power_state.validity == TT_POWER_VALIDITY(15, 0) && priv->power_state.power_flags == 0);
	last_close = list_empty(&tt_dev->open_fds_list);

	if (tt_dev->detached || tt_dev->needs_hw_init)
		return;

	if (!power_policy)
		return;

	if (no_power_contrib)
		return;

	if (can_defer && last_close)
		mod_delayed_work(system_wq, &tt_dev->power_down_work, msecs_to_jiffies(idle_power_down_grace_ms));
	else
		tenstorrent_set_aggregated_power_state_locked(tt_dev);
}

static int tt_cdev_release(struct inode *inode, struct file *file)
{
	struct chardev_private *priv = file->private_data;
	struct tenstorrent_device *tt_dev = priv->device;

	// Hold reset_rwsem (shared) across the body so the reset ioctl (which holds
	// it exclusive) cannot interleave with the device-touching cleanup.
	down_read(&tt_dev->reset_rwsem);

	tt_cdev_release_noc_cleanup(priv);
	tenstorrent_memory_cleanup(priv);
	tt_cdev_release_resource_locks(priv);
	tt_cdev_release_tlbs(priv);

	mutex_lock(&tt_dev->chardev_mutex);

	list_del(&priv->open_fd);

	// Must follow the list_del: release_power reads open_fds_list.
	tt_cdev_release_power(priv);

	if (list_empty(&tt_dev->open_fds_list)) {
		WRITE_ONCE(tt_dev->chardev_excl_held, false);
		wake_up_interruptible(&tt_dev->chardev_excl_waitqueue);
	}

	mutex_unlock(&tt_dev->chardev_mutex);

	up_read(&tt_dev->reset_rwsem);

	tenstorrent_device_put(tt_dev);
	put_pid(priv->pid);
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
