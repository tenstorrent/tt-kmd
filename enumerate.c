// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define pci_enable_pcie_error_reporting(dev) do { } while (0)
#define pci_disable_pcie_error_reporting(dev) do { } while (0)
#else
#include <linux/aer.h>
#endif

static DEFINE_XARRAY_ALLOC(tenstorrent_dev_xa);

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
		struct bar_mapping *bar_mapping;
		struct dmabuf *dmabuf;
		unsigned int bkt;

		// Open file descriptors.
		seq_printf(s,
			   "%-8d %-16s %-14s\n", priv->pid, priv->comm,
			   "OPEN_FD");

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
					priv->pid, priv->comm, "PIN_PAGES+IATU", va_start, addr_label, addr,
					sensitive ? region->base : 0, size_bytes);
			} else {
				seq_printf(s, "%-8d %-16s %-14s VA: 0x%016llx -> %s: 0x%016llx (size=0x%lx)\n",
					   priv->pid, priv->comm, "PIN_PAGES", va_start, addr_label, addr, size_bytes);
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
					   priv->pid, priv->comm, "DMA_BUF+IATU", dmabuf->index, addr_label, addr,
					   sensitive ? region->base : 0, size_bytes);
			} else {
				seq_printf(s, "%-8d %-16s %-14s ID: %-3u -> %s: 0x%016llx (size=0x%lx)\n", priv->pid,
					   priv->comm, "DMA_BUF", dmabuf->index, addr_label, addr, size_bytes);
			}
		}

		// BAR mappings.
		list_for_each_entry(bar_mapping, &priv->bar_mappings, list) {
			seq_printf(s, "%-8d %-16s %-14s BAR%u %-2s (offset=0x%llx, size=0x%llx, refs=%d)\n", priv->pid,
				   priv->comm, "BAR", bar_mapping->bar_index,
				   bar_mapping->type == BAR_MAPPING_WC ? "WC" : "UC", bar_mapping->offset,
				   bar_mapping->size, refcount_read(&bar_mapping->refs));
		}

		// Individual inbound TLB window mappings.
		for_each_set_bit(tlb_id, priv->tlbs, TENSTORRENT_MAX_INBOUND_TLBS) {
			if (tt_dev->dev_class->describe_tlb &&
			    tt_dev->dev_class->describe_tlb(tt_dev, tlb_id, &desc) == 0) {
				seq_printf(s,
					   "%-8d %-16s %-14s ID: %-3u -> BAR%d + 0x%lx (size=0x%lx, refs=%d)\n",
					   priv->pid, priv->comm, "TLB",
					   tlb_id, desc.bar, desc.bar_offset,
					   desc.size,
					   atomic_read(&tt_dev->tlb_refs[tlb_id]));
			}
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
		seq_printf(s, "%d\n", priv->pid);
	mutex_unlock(&tt_dev->chardev_mutex);

	return 0;
}

static int tenstorrent_reboot_notifier(struct notifier_block *nb,
				       unsigned long action, void *data) {
	struct tenstorrent_device *tt_dev = container_of(nb, struct tenstorrent_device, reboot_notifier);

	if (action != SYS_POWER_OFF)
		tt_dev->dev_class->reboot(tt_dev);

	return NOTIFY_DONE;
}

static int tenstorrent_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct tenstorrent_device *tt_dev = NULL;
	u32 ordinal;
	int err;
	const struct tenstorrent_device_class *device_class;

	if (!id->driver_data) {
		dev_warn(&dev->dev, "Unsupported device\n");
		return -ENODEV;
	}

	device_class = (const struct tenstorrent_device_class *)id->driver_data;

	printk(KERN_INFO "Found a Tenstorrent %s device at bus %04x:%02x.\n",
	       device_class->name, (unsigned)pci_domain_nr(dev->bus), (unsigned)dev->bus->number);

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

	err = xa_alloc(&tenstorrent_dev_xa, &ordinal, tt_dev, xa_limit_31b, GFP_KERNEL);
	if (err) {
		kfree(tt_dev);
		pci_disable_device(dev);
		return err;
	}

	// The refcount created here persists until remove.
	kref_init(&tt_dev->kref);

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

	// Use dma_address_bits from module parameter or device class for coherent
	// DMA mask, but use a 64-bit mask for streaming mappings. The problem this
	// solves is that legacy Wormhole software assumes it will get 32-bit DMA
	// addresses from the ALLOCATE_DMA_BUF API, but a 32 bit DMA mask is too
	// limiting for user pinnings when IOMMU is enabled.
	// linux/dma-mapping.h says, "the DMA API guarantees that the coherent DMA
	// mask can be set to the same or smaller than the streaming DMA mask" so
	// only set_dma_mask() return value is checked.
	tt_dev->dma_capable = (dma_set_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: 64)) == 0);
	dma_set_coherent_mask(&dev->dev, DMA_BIT_MASK(dma_address_bits ?: device_class->dma_address_bits));

	// Max these to ensure the IOVA allocator will not split large pinned regions.
	dma_set_max_seg_size(&dev->dev, UINT_MAX);
	dma_set_seg_boundary(&dev->dev, ULONG_MAX);

	pci_set_master(dev);
	pci_enable_pcie_error_reporting(dev);

	pci_set_drvdata(dev, tt_dev);
	dev_set_drvdata(&tt_dev->dev, tt_dev);

	tt_dev->interrupt_enabled = tenstorrent_enable_interrupts(tt_dev);

	if (device_class->init_device(tt_dev))
		tt_dev->needs_hw_init = !device_class->init_hardware(tt_dev);

	pci_save_state(dev);
	device_class->save_reset_state(tt_dev);

	tenstorrent_register_device(tt_dev);

	if (device_class->reboot) {
		tt_dev->reboot_notifier.notifier_call = tenstorrent_reboot_notifier;
		register_reboot_notifier(&tt_dev->reboot_notifier);
	}

	if (!tt_dev->needs_hw_init)
		device_class->init_telemetry(tt_dev);

	debugfs_create_file("mappings", 0444, tt_dev->debugfs_root, tt_dev, &mappings_fops);

	// Set initial low-power state via aggregation logic.
	if (power_policy)
		tenstorrent_set_aggregated_power_state(tt_dev);

	// Enable runtime PM. Device will auto-suspend after 5 seconds of idleness.
	// PCI core disables this by default, so put_noidle + allow turn it on.
	pm_runtime_set_autosuspend_delay(&dev->dev, 5000);
	pm_runtime_use_autosuspend(&dev->dev);
	pm_runtime_put_noidle(&dev->dev);
	pm_runtime_allow(&dev->dev);

	return 0;
}

static void tenstorrent_pci_remove(struct pci_dev *dev)
{
	struct tenstorrent_device *tt_dev = pci_get_drvdata(dev);
	struct chardev_private *priv, *tmp;
	u16 vendor_id;

	// Disable runtime PM and ensure device is awake for teardown.
	pm_runtime_forbid(&dev->dev);
	pm_runtime_get_sync(&dev->dev);

	if (tt_dev->dev_class == &wormhole_class) {
		struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
		cancel_delayed_work_sync(&wh->fw_ready_work);
	}

	// In a hotplug scenario, the device may not be accessible anymore. Check
	// if it is still accessible by reading the vendor ID. If it is not, set the
	// detached flag to prevent further hardware access.
	pci_read_config_word(dev, PCI_VENDOR_ID, &vendor_id);
	if (vendor_id == U16_MAX)
		tt_dev->detached = true;
	else
		tt_dev->dev_class->cleanup_hardware(tt_dev); // Put FW into A3 state

	tt_dev->dev_class->cleanup_device(tt_dev); // unmap BARs

	list_for_each_entry_safe(priv, tmp, &tt_dev->open_fds_list, open_fd) {
		tenstorrent_memory_cleanup(priv);
	}

	// These remove child sysfs entries which must happen before remove returns.
	tenstorrent_unregister_device(tt_dev);
	tenstorrent_disable_interrupts(tt_dev);

	pci_disable_pcie_error_reporting(dev);
	pci_disable_device(dev);
	tt_dev->detached = true;

	pci_set_drvdata(dev, NULL);

	// If this is postponed, a subsequent probe is forced to use a different ordinal.
	xa_erase(&tenstorrent_dev_xa, tt_dev->ordinal);

	// Balance the get_sync/forbid from start of remove.
	pm_runtime_put_sync(&dev->dev);
	pm_runtime_allow(&dev->dev);

	tenstorrent_device_put(tt_dev);
}

static void tt_dev_release(struct kref *tt_dev_kref) {
	struct tenstorrent_device *tt_dev = container_of(tt_dev_kref, struct tenstorrent_device, kref);
	struct pci_dev *pdev = tt_dev->pdev;

	if (tt_dev->dev_class->reboot)
		unregister_reboot_notifier(&tt_dev->reboot_notifier);


	pci_dev_put(pdev);
	kfree(tt_dev);
}

void tenstorrent_device_put(struct tenstorrent_device *tt_dev) {
	kref_put(&tt_dev->kref, tt_dev_release);
}

static int tenstorrent_suspend(struct device *dev) {
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tenstorrent_device *tt_dev = pci_get_drvdata(pdev);

	tt_dev->dev_class->cleanup_hardware(tt_dev);

	return 0;
}

static int tenstorrent_resume(struct device *dev) {
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tenstorrent_device *tt_dev = pci_get_drvdata(pdev);

	bool ok = tt_dev->dev_class->init_hardware(tt_dev);

	// Suspend invalidates the saved state.
	if (ok)
		pci_save_state(pdev);

	return ok ? 0 : -EIO;
}

static int tenstorrent_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tenstorrent_device *tt_dev = pci_get_drvdata(pdev);

	tt_dev->dev_class->cleanup_hardware(tt_dev);

	return 0;
}

static int tenstorrent_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct tenstorrent_device *tt_dev = pci_get_drvdata(pdev);

	if (!tt_dev->dev_class->init_hardware(tt_dev))
		return -EIO;

	pci_save_state(pdev);

	return 0;
}

static const struct dev_pm_ops tenstorrent_pm_ops = {
	.suspend = tenstorrent_suspend,
	.resume = tenstorrent_resume,
	.runtime_suspend = tenstorrent_runtime_suspend,
	.runtime_resume = tenstorrent_runtime_resume,
};

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
