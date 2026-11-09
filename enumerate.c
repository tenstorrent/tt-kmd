// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/pm.h>
#include <linux/list.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/proc_fs.h>

#include "enumerate.h"
#include "interrupt.h"
#include "chardev.h"
#include "device.h"
#include "module.h"
#include "memory.h"
#include "chardev_private.h"
#include "wormhole.h"
#include "tlb.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0) || TT_RHEL_RELEASE_GE(9, 4)
#define pci_enable_pcie_error_reporting(dev) do { } while (0)
#define pci_disable_pcie_error_reporting(dev) do { } while (0)
#else
#include <linux/aer.h>
#endif

static DEFINE_XARRAY_ALLOC(tenstorrent_dev_xa);

// Galaxy systems have 32 Tenstorrent chips across 4 boards with 8 chips each.
// The high nibble of the PCI bus number identifies the board; the low nibble
// identifies the chip within that board (1-based). These tables map high nibble
// to board index in physical board order.
#define GALAXY_CHIPS_PER_UBB 8
#define GALAXY_NUM_UBBS 4

#define PCI_SUBSYSTEM_ID_GALAXY_WH 0x0035
#define PCI_SUBSYSTEM_ID_GALAXY_BH 0x0047
#define PCI_SUBSYSTEM_ID_GALAXY_BH_CF 0x0202

static const u8 wh_galaxy_ubb_bus_prefix[GALAXY_NUM_UBBS] = { 0xC, 0x8, 0x0, 0x4 };
static const u8 bh_galaxy_ubb_bus_prefix[GALAXY_NUM_UBBS] = { 0x0, 0x4, 0xC, 0x8 };

static bool is_galaxy(struct pci_dev *dev)
{
	return dev->subsystem_device == PCI_SUBSYSTEM_ID_GALAXY_WH ||
	       dev->subsystem_device == PCI_SUBSYSTEM_ID_GALAXY_BH ||
	       dev->subsystem_device == PCI_SUBSYSTEM_ID_GALAXY_BH_CF;
}

static int galaxy_bdf_to_ordinal(struct pci_dev *pdev)
{
	const u8 *ubb_table;
	u8 bus = pdev->bus->number;
	u8 high = bus >> 4;
	u8 low = bus & 0x0F;
	int ubb;

	switch (pdev->subsystem_device) {
	case PCI_SUBSYSTEM_ID_GALAXY_WH:
		ubb_table = wh_galaxy_ubb_bus_prefix;
		break;
	case PCI_SUBSYSTEM_ID_GALAXY_BH:
	case PCI_SUBSYSTEM_ID_GALAXY_BH_CF:
		ubb_table = bh_galaxy_ubb_bus_prefix;
		break;
	default:
		return -1;
	}

	if (low < 1 || low > GALAXY_CHIPS_PER_UBB)
		return -1;

	for (ubb = 0; ubb < GALAXY_NUM_UBBS; ubb++) {
		if (ubb_table[ubb] == high)
			return ubb * GALAXY_CHIPS_PER_UBB + (low - 1);
	}

	return -1;
}

#if !IS_ENABLED(CONFIG_HWMON)
struct device *devm_hwmon_device_register_with_info(struct device *,
	const char *, void *, const struct hwmon_chip_info *, const struct
	attribute_group **) { return NULL; }
#endif

static int mappings_seq_show(struct seq_file *s, void *v)
{
	struct tenstorrent_device *tt_dev = s->private;
	struct chardev_private *priv;
	struct pinned_page_range *pinning;
	unsigned int tlb_id;
	struct tlb_descriptor desc;
	bool sensitive = capable(CAP_SYS_ADMIN);

	seq_printf(s, "WARNING: This file is for diagnostic purposes only.\n"
		      "Its format is not stable and may change in future driver versions.\n"
		      "Do not write scripts that parse this file.\n\n");

	seq_printf(s, "%-8s %-16s %-14s %s\n", "PID", "Comm", "Type", "Mapping Details");
	seq_printf(s, "%-8s %-16s %-14s %s\n", "----", "----", "----", "---------------");

	mutex_lock(&tt_dev->chardev_mutex);
	list_for_each_entry(priv, &tt_dev->open_fds_list, open_fd) {
		struct tenstorrent_mmap_vma *mmap_vma;
		struct dmabuf *dmabuf;
		unsigned int bkt;
		pid_t pid;

		pid = pid_vnr(priv->pid);

		// Open file descriptors.
		seq_printf(s, "%-8d %-16s %-14s\n", pid, priv->comm, "OPEN_FD");

		if (!mutex_trylock(&priv->mutex)) {
			seq_printf(s, "%-8s %-16s %-14s\n", "", "", "...locked, skipping details...");
			continue;
		}

		if (!mutex_trylock(&priv->device->iatu_mutex)) {
			seq_printf(s, "%-8s %-16s %-14s\n", "", "", "...IATU busy, skipping details...");
			mutex_unlock(&priv->mutex);
			continue;
		}

		// User pinnings, including iATU entries.
		list_for_each_entry(pinning, &priv->pinnings, list) {
			unsigned long long va_start = pinning->virtual_address;
			unsigned long size_bytes = pinning->page_count * PAGE_SIZE;
			unsigned long long addr = 0;
			const char *addr_label;

			if (pinning->dma_mapping.sgl) {
				// IOMMU path: show IOVA
				addr_label = "IOVA";
				if (sensitive)
					addr = pinning->dma_mapping.sgl->dma_address;
			} else {
				// Non-IOMMU path: show physical address
				addr_label = "PA";
				if (sensitive && pinning->pages)
					addr = page_to_phys(pinning->pages[0]);
			}

			if (pinning->outbound_iatu_region >= 0) {
				const struct tenstorrent_outbound_iatu_region *region;
				region = &priv->device->outbound_iatus[pinning->outbound_iatu_region];

				seq_printf(
					s,
					"%-8d %-16s %-14s VA: 0x%016llx -> %s: 0x%016llx -> NOC: 0x%llx (size=0x%lx)\n",
					pid, priv->comm, "PIN_PAGES+IATU", va_start, addr_label, addr,
					sensitive ? region->base : 0, size_bytes);
			} else {
				seq_printf(s, "%-8d %-16s %-14s VA: 0x%016llx -> %s: 0x%016llx (size=0x%lx)\n", pid,
					   priv->comm, "PIN_PAGES", va_start, addr_label, addr, size_bytes);
			}
		}

		// Driver-allocated DMA buffers, including iATU entries.
		hash_for_each(priv->dmabufs, bkt, dmabuf, hash_chain) {
			unsigned long long addr = sensitive ? dmabuf->phys : 0;
			unsigned long size_bytes = dmabuf->size;
			const char *addr_label = is_iommu_translated(&priv->device->pdev->dev) ? "IOVA" : "PA";

			if (dmabuf->outbound_iatu_region >= 0) {
				const struct tenstorrent_outbound_iatu_region *region;
				region = &priv->device->outbound_iatus[dmabuf->outbound_iatu_region];

				seq_printf(s,
					   "%-8d %-16s %-14s ID: %-3u -> %s: 0x%016llx -> NOC: 0x%llx (size=0x%lx)\n",
					   pid, priv->comm, "DMA_BUF+IATU", dmabuf->index, addr_label, addr,
					   sensitive ? region->base : 0, size_bytes);
			} else {
				seq_printf(s, "%-8d %-16s %-14s ID: %-3u -> %s: 0x%016llx (size=0x%lx)\n", pid,
					   priv->comm, "DMA_BUF", dmabuf->index, addr_label, addr, size_bytes);
			}
		}

		// Allocated TLB windows.
		if (!mutex_trylock(&priv->tlb_mutex)) {
			seq_printf(s, "%-8s %-16s %-14s\n", "", "", "...TLBs busy, skipping...");
		} else {
			for_each_set_bit(tlb_id, priv->tlbs, TENSTORRENT_MAX_INBOUND_TLBS) {
				seq_printf(s, "%-8d %-16s %-14s ID: %-3u\n", pid, priv->comm, "TLB-alloc", tlb_id);
			}
			mutex_unlock(&priv->tlb_mutex);
		}

		// BAR/TLB mappings.
		if (!mutex_trylock(&priv->vma_lock)) {
			seq_printf(s, "%-8s %-16s %-14s\n", "", "", "...VMA list busy, skipping...");
		} else {
			list_for_each_entry(mmap_vma, &priv->vma_list, list) {
				const char *cache_str = mmap_vma->cache_mode == BAR_MAPPING_WC ? "WC" : "UC";

				if (mmap_vma->type == TT_VMA_BAR) {
					seq_printf(s, "%-8d %-16s %-14s BAR%u %-2s (offset=0x%llx, size=0x%llx)\n",
						   pid, priv->comm, "BAR",
						   mmap_vma->bar.bar_index, cache_str,
						   mmap_vma->bar.offset,
						   mmap_vma->bar.size);
				} else if (mmap_vma->type == TT_VMA_TLB) {
					tlb_id = mmap_vma->tlb.id;
					if (tt_dev->dev_class->describe_tlb &&
					    tt_dev->dev_class->describe_tlb(tt_dev, tlb_id, &desc) == 0) {
						seq_printf(s,
							   "%-8d %-16s %-14s ID: %-3u %-2s -> BAR%d + 0x%lx (size=0x%lx)\n",
							   pid, priv->comm, "TLB", tlb_id, cache_str,
							   desc.bar, desc.bar_offset, desc.size);
					}
				}
			}
			mutex_unlock(&priv->vma_lock);
		}

		mutex_unlock(&priv->device->iatu_mutex);
		mutex_unlock(&priv->mutex);
	}
	mutex_unlock(&tt_dev->chardev_mutex);

	return 0;
}

static int mappings_open(struct inode *inode, struct file *file)
{
	return single_open(file, mappings_seq_show, inode->i_private);
}

static const struct file_operations mappings_fops = {
	.owner   = THIS_MODULE,
	.open    = mappings_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

int pids_proc_show(struct seq_file *s, void *v)
{
	struct tenstorrent_device *tt_dev = s->private;
	struct chardev_private *priv;

	mutex_lock(&tt_dev->chardev_mutex);
	list_for_each_entry(priv, &tt_dev->open_fds_list, open_fd)
		seq_printf(s, "%d\n", pid_vnr(priv->pid));
	mutex_unlock(&tt_dev->chardev_mutex);

	return 0;
}

static int tenstorrent_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct tenstorrent_device *tt_dev = NULL;
	u32 ordinal;
	int galaxy_ord;
	int err;
	const struct tenstorrent_device_class *device_class;

	if (!id->driver_data) {
		dev_warn(&dev->dev, "Unsupported device\n");
		return -ENODEV;
	}

	device_class = (const struct tenstorrent_device_class *)id->driver_data;

	dev_info(&dev->dev, "Found a Tenstorrent %s device\n", device_class->name);

	// During pre-test, unflashed boards have no class code which trips up __dev_sort_resources.
	// Assign the proper class code and rerun resource assignment to clear things up.
	if (dev->class >> 8 == PCI_CLASS_NOT_DEFINED) {
		dev->class = 0x120000;	// Processing Accelerator - vendor-specific interface
		pci_assign_unassigned_bus_resources(dev->bus);
	}

	if (pci_enable_device(dev) < 0)
		return -EIO;

	tt_dev = kzalloc(device_class->instance_size, GFP_KERNEL);
	if (tt_dev == NULL)
		return -ENOMEM;

	galaxy_ord = galaxy_bdf_to_ordinal(dev);
	if (galaxy_ord >= 0) {
		ordinal = galaxy_ord;
		err = xa_insert(&tenstorrent_dev_xa, ordinal, tt_dev, GFP_KERNEL);
		if (err == -EBUSY) {
			dev_warn(&dev->dev, "Galaxy ordinal %u in use, falling back to dynamic allocation\n", ordinal);
			err = xa_alloc(&tenstorrent_dev_xa, &ordinal, tt_dev, xa_limit_31b, GFP_KERNEL);
		}
	} else {
		err = xa_alloc(&tenstorrent_dev_xa, &ordinal, tt_dev, xa_limit_31b, GFP_KERNEL);
	}
	if (err) {
		kfree(tt_dev);
		pci_disable_device(dev);
		return err;
	}

	tt_dev->detached = false;
	tt_dev->needs_hw_init = true;
	tt_dev->dev_class = device_class;
	tt_dev->pdev = pci_dev_get(dev);
	tt_dev->ordinal = ordinal;
	atomic_long_set(&tt_dev->reset_gen, 0);
	init_rwsem(&tt_dev->reset_rwsem);

	// Initialize per-device TLB counts from device class defaults.
	// Device-specific init may adjust these.
	memcpy(tt_dev->tlb_counts, device_class->tlb_counts, sizeof(tt_dev->tlb_counts));

	mutex_init(&tt_dev->chardev_mutex);
	mutex_init(&tt_dev->iatu_mutex);
	mutex_init(&tt_dev->dmabuf_export_lock);
	mutex_init(&tt_dev->arc_msg_mutex);
	INIT_LIST_HEAD(&tt_dev->arc_msg_queue);
	INIT_LIST_HEAD(&tt_dev->dmabuf_exports);
	INIT_DELAYED_WORK(&tt_dev->power_down_work, tenstorrent_power_down_work_func);

	// Both DMA masks come from the device class; the reasons for each chip's
	// numbers are next to them in the class definitions. The
	// dma_address_bits module parameter overrides both.
	//
	// linux/dma-mapping.h says the coherent mask may be set the same as or
	// smaller than the streaming mask, so only dma_set_mask()'s return value
	// is checked. The class table must keep coherent <= streaming.
	WARN_ON(device_class->coherent_dma_bits > device_class->streaming_dma_bits);
	tt_dev->dma_capable = (dma_set_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: device_class->streaming_dma_bits)) == 0);
	dma_set_coherent_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: device_class->coherent_dma_bits));

	// Max these to ensure the IOVA allocator will not split large pinned regions.
	dma_set_max_seg_size(&dev->dev, UINT_MAX);
	dma_set_seg_boundary(&dev->dev, ULONG_MAX);

	// Thunderbolt-connected devices are marked "untrusted" by the kernel,
	// which forces SWIOTLB bounce buffering even with IOMMU enabled. The
	// IOMMU itself provides DMA isolation; clear the flag only when IOMMU
	// is active and the untrust is due to an external-facing port.
	// pci_dev.external_facing was added in v5.9.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
	if (dev->untrusted && is_iommu_translated(&dev->dev)) {
		struct pci_dev *root = pcie_find_root_port(dev);
		if (root && root->external_facing) {
			dev_info(&dev->dev, "Clearing untrusted flag (external-facing port, IOMMU active)\n");
			dev->untrusted = 0;
		}
	}
#endif

	pci_set_master(dev);
	pci_enable_pcie_error_reporting(dev);

	// HACK: Suppress hotplug for Galaxy systems.
	if (is_galaxy(dev))
		pci_ignore_hotplug(dev);

	pci_set_drvdata(dev, tt_dev);
	dev_set_drvdata(&tt_dev->dev, tt_dev);

	tt_dev->interrupt_enabled = tenstorrent_enable_interrupts(tt_dev);

	if (!device_class->init_device(tt_dev)) {
		dev_err(&dev->dev, "Device initialization failed\n");
		err = -EIO;
		goto fail_init_device;
	}

	tt_dev->needs_hw_init = !device_class->init_hardware(tt_dev);

	pci_save_state(dev);
	device_class->save_reset_state(tt_dev);

	tenstorrent_register_device(tt_dev);

	if (!tt_dev->needs_hw_init)
		device_class->init_telemetry(tt_dev);

	debugfs_create_file("mappings", 0444, tt_dev->debugfs_root, tt_dev, &mappings_fops);

	// Set initial low-power state via aggregation logic.
	if (power_policy) {
		down_read(&tt_dev->reset_rwsem);
		tenstorrent_update_power_state(tt_dev);
		up_read(&tt_dev->reset_rwsem);
	}

	return 0;

fail_init_device:
	tenstorrent_disable_interrupts(tt_dev);
	pci_disable_pcie_error_reporting(dev);
	pci_set_drvdata(dev, NULL);
	pci_dev_put(dev);
	xa_erase(&tenstorrent_dev_xa, ordinal);
	// Direct kfree: this path runs before tenstorrent_register_device,
	// so tt_dev->dev is not yet initialized and cannot be put_device'd.
	kfree(tt_dev);
	pci_disable_device(dev);
	return err;
}

static void tenstorrent_pci_remove(struct pci_dev *dev)
{
	struct tenstorrent_device *tt_dev = pci_get_drvdata(dev);
	struct chardev_private *priv;
	u16 vendor_id;

	// Detach.  Everything that enters the driver from outside holds
	// reset_rwsem while it works and checks detached under it, so taking it
	// for write drains whatever is in flight, and setting detached before
	// releasing it turns away whatever comes next: ioctl, mmap and open fail
	// with -ENODEV, and close() and telemetry reads stay off the hardware.
	// Nothing below has to consider a concurrent ioctl or reset.
	//
	// While here, revoke what userspace and dma-buf importers already hold:
	// - power_down_work is armed only with reset_rwsem held and detached
	//   clear, so after this cancel it can never be armed again.
	// - No BAR or TLB mapping can be created after the zap.
	// - Every TLB dma-buf export was created by an ioctl, so the revoke walk
	//   sees them all and no more can appear.
	down_write(&tt_dev->reset_rwsem);
	WRITE_ONCE(tt_dev->detached, true);
	cancel_delayed_work_sync(&tt_dev->power_down_work);
	tenstorrent_vma_zap(tt_dev);
	tenstorrent_revoke_tlb_dmabufs(tt_dev);
	up_write(&tt_dev->reset_rwsem);

	// Waiters in open() and LOCK_CTL ACQUIRE_BLOCKING hold no locks; wake
	// them so they observe detached and return -ENODEV.
	wake_up_interruptible_all(&tt_dev->chardev_excl_waitqueue);
	wake_up_interruptible(&tt_dev->resource_lock_waitqueue);

	// Unpublish.  This must follow the detach: telemetry is re-probed by the
	// reset ioctl, which could otherwise bring it back after its cleanup.
	// Telemetry goes before the chardev because its sysfs groups hang off
	// tt_dev->dev, which tenstorrent_unregister_device deletes.
	tt_telemetry_cleanup(tt_dev);
	tenstorrent_unregister_device(tt_dev);

	// Quiesce the hardware.  Remove is the only thing still using it, so no
	// lock is needed from here on.
	//
	// Interrupts go first, as in suspend: a device-class interrupt handler
	// may schedule work, which cleanup_hardware() and cleanup_device() drain.
	// free_irq() waits for in-flight handlers, so no interrupt can schedule
	// more work behind those drains.
	tenstorrent_disable_interrupts(tt_dev);

	// Tell firmware to stop, unless it cannot hear us.  A device that is
	// mid-reset (needs_hw_init) has not been reinitialized, and where reset
	// is seen as a hotplug event that is why we are here.  A device that has
	// dropped off the bus reads back all-ones.  Neither should be sent
	// messages; any other device must be, or its firmware carries on doing
	// DMA into memory that is about to be freed.
	pci_read_config_word(dev, PCI_VENDOR_ID, &vendor_id);
	if (!tt_dev->needs_hw_init && vendor_id != U16_MAX)
		tt_dev->dev_class->cleanup_hardware(tt_dev);

	// Whatever firmware made of that, the device can issue no more DMA once
	// bus mastering is off.  Only now is it safe to release the memory it
	// could reach.
	pci_clear_master(dev);

	// Release the DMA memory of the fds that are still open: they may stay
	// open indefinitely, and their pinnings and buffers must not outlive the
	// driver's binding to the device.  No fd has joined the list since the
	// detach; close() can still take one off it, hence chardev_mutex.
	mutex_lock(&tt_dev->chardev_mutex);
	list_for_each_entry(priv, &tt_dev->open_fds_list, open_fd) {
		tenstorrent_memory_cleanup(priv);
	}
	mutex_unlock(&tt_dev->chardev_mutex);

	tt_dev->dev_class->cleanup_device(tt_dev); // unmap BARs

	pci_disable_pcie_error_reporting(dev);
	pci_disable_device(dev);

	pci_set_drvdata(dev, NULL);

	// If this is postponed, a subsequent probe is forced to use a different ordinal.
	xa_erase(&tenstorrent_dev_xa, tt_dev->ordinal);

	// Drop the reference from device_initialize (tenstorrent_register_device).
	// tt_dev is freed here unless open fds or dma-buf exports still hold it.
	put_device(&tt_dev->dev);
}

// dev.release callback: runs when the last reference on tt_dev->dev drops.
// Set by tenstorrent_register_device.
void tt_dev_release(struct device *dev)
{
	struct tenstorrent_device *tt_dev = container_of(dev, struct tenstorrent_device, dev);
	struct pci_dev *pdev = tt_dev->pdev;

	pci_dev_put(pdev);
	kfree(tt_dev);
}

static int tenstorrent_suspend(struct device *dev) {
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tenstorrent_device *tt_dev = pci_get_drvdata(pdev);

	cancel_delayed_work_sync(&tt_dev->power_down_work);
	tenstorrent_revoke_tlb_dmabufs(tt_dev);

	// Device-class cleanup may need to drain work scheduled by its IRQ
	// handler before firmware is suspended. free_irq() prevents a late MSI
	// from queuing more work after that drain.
	tenstorrent_disable_interrupts(tt_dev);
	tt_dev->dev_class->cleanup_hardware(tt_dev);

	return 0;
}

static int tenstorrent_resume(struct device *dev) {
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tenstorrent_device *tt_dev = pci_get_drvdata(pdev);
	bool ok;

	// Re-establish the handler before init_hardware() re-enables firmware
	// features that signal completion through MSI (such as FW log batches).
	if (!tenstorrent_enable_interrupts(tt_dev))
		dev_warn(dev, "Unable to re-enable interrupts after resume\n");

	ok = tt_dev->dev_class->init_hardware(tt_dev);

	// Suspend invalidates the saved state.
	if (ok)
		pci_save_state(pdev);

	return ok ? 0 : -EIO;
}

static SIMPLE_DEV_PM_OPS(tenstorrent_pm_ops, tenstorrent_suspend, tenstorrent_resume);

extern const struct pci_device_id tenstorrent_ids[];
static struct pci_driver tenstorrent_pci_driver = {
	.name = TENSTORRENT,
	.id_table = tenstorrent_ids,
	.probe = tenstorrent_pci_probe,
	.remove = tenstorrent_pci_remove,
	.shutdown = tenstorrent_pci_remove,

	.driver.pm = &tenstorrent_pm_ops,
};

int tenstorrent_pci_register_driver(void)
{
	return pci_register_driver(&tenstorrent_pci_driver);
}

void tenstorrent_pci_unregister_driver(void)
{
	pci_unregister_driver(&tenstorrent_pci_driver);
}
