// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/pm.h>
#include <linux/list.h>

#include "enumerate.h"
#include "interrupt.h"
#include "chardev.h"
#include "device.h"
#include "module.h"
#include "memory.h"
#include "chardev_private.h"
#include "telemetry.h"
#include "wormhole.h"
#include "tlb.h"


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
#define pci_enable_pcie_error_reporting(dev) do { } while (0)
#define pci_disable_pcie_error_reporting(dev) do { } while (0)
#else
#include <linux/aer.h>
#endif

static DEFINE_IDR(tenstorrent_dev_idr);
static DEFINE_MUTEX(tenstorrent_dev_idr_mutex);

#if !IS_ENABLED(CONFIG_HWMON)
struct device *devm_hwmon_device_register_with_info(struct device *,
	const char *, void *, const struct hwmon_chip_info *, const struct
	attribute_group **) { return NULL; }
#endif

static ssize_t mappings_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct chardev_private *priv;
	struct pinned_page_range *pinning;
	unsigned int tlb_id;
	struct tlb_descriptor desc;
	size_t len = 0;
	bool sensitive = capable(CAP_SYS_ADMIN);

	len += scnprintf(buf + len, PAGE_SIZE - len, "%-8s %-16s %-14s %s\n", "PID", "Comm", "Type", "Mapping Details");
	len += scnprintf(buf + len, PAGE_SIZE - len, "%-8s %-16s %-14s %s\n", "----", "----", "----", "---------------");

	mutex_lock(&tt_dev->chardev_mutex);
	list_for_each_entry(priv, &tt_dev->open_fds_list, open_fd) {
		len += scnprintf(buf + len, (len < PAGE_SIZE) ? (PAGE_SIZE - len) : 0,
				 "%-8d %-16s %-14s\n", priv->pid, priv->comm,
				 "OPEN_FD");

		mutex_lock(&priv->mutex);
		list_for_each_entry(pinning, &priv->pinnings, list) {
			unsigned long long va_start = pinning->virtual_address;
			unsigned long size_kb = (pinning->page_count * PAGE_SIZE) / 1024;
			unsigned long long iova_start = sensitive ? pinning->dma_mapping.sgl->dma_address : 0;

			// Check if there is a corresponding IATU mapping
			if (pinning->outbound_iatu_region >= 0) {
				const struct tenstorrent_outbound_iatu_region *region;
				region = &priv->device->outbound_iatus[pinning->outbound_iatu_region];

				len += scnprintf(buf + len, (len < PAGE_SIZE) ? (PAGE_SIZE - len) : 0,
						 "%-8d %-16s %-14s VA: 0x%016llx -> IOVA: 0x%016llx -> NOC: 0x%llx (%lukB)\n",
						 priv->pid, priv->comm, "PIN_PAGES+IATU",
						 va_start, iova_start,
						 sensitive ? region->base : 0, size_kb);
			} else {
				// This is just a VA -> IOVA mapping
				len += scnprintf(buf + len, (len < PAGE_SIZE) ? (PAGE_SIZE - len) : 0,
						 "%-8d %-16s %-14s VA: 0x%016llx -> IOVA: 0x%016llx (%lukB)\n",
						 priv->pid, priv->comm, "PIN_PAGES",
						 va_start, iova_start, size_kb);
			}
		}
		for_each_set_bit(tlb_id, priv->tlbs, TENSTORRENT_MAX_INBOUND_TLBS) {
			if (tt_dev->dev_class->describe_tlb &&
			    tt_dev->dev_class->describe_tlb(tt_dev, tlb_id, &desc) == 0) {
				len += scnprintf(buf + len, (len < PAGE_SIZE) ? (PAGE_SIZE - len) : 0,
						 "%-8d %-16s %-14s ID: %-3u -> BAR%d + 0x%lx (%lukB) (refs: %d)\n",
						 priv->pid, priv->comm, "TLB",
						 tlb_id, desc.bar, desc.bar_offset,
						 desc.size / 1024,
						 atomic_read(&tt_dev->tlb_refs[tlb_id]));
			}
		}
		mutex_unlock(&priv->mutex);
	}
	mutex_unlock(&tt_dev->chardev_mutex);

	return len;
}
static DEVICE_ATTR_RO(mappings);


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
	int ordinal;
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

	mutex_lock(&tenstorrent_dev_idr_mutex);
	ordinal = idr_alloc(&tenstorrent_dev_idr, tt_dev, 0, 0, GFP_KERNEL);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

	if (ordinal < 0) {
		kfree(tt_dev);
		pci_disable_device(dev);
		return ordinal;
	}

	// The refcount created here persists until remove.
	kref_init(&tt_dev->kref);

	tt_dev->detached = false;
	tt_dev->dev_class = device_class;
	tt_dev->pdev = pci_dev_get(dev);
	tt_dev->ordinal = ordinal;

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

	device_create_file(&tt_dev->dev, &dev_attr_mappings);


	return 0;
}

static void tenstorrent_pci_remove(struct pci_dev *dev)
{
	struct tenstorrent_device *tt_dev = pci_get_drvdata(dev);
	struct chardev_private *priv, *tmp;
	u16 vendor_id;

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

	device_remove_file(&tt_dev->dev, &dev_attr_mappings);


	// These remove child sysfs entries which must happen before remove returns.
	tenstorrent_unregister_device(tt_dev);
	tenstorrent_disable_interrupts(tt_dev);

	pci_disable_pcie_error_reporting(dev);
	pci_disable_device(dev);
	tt_dev->detached = true;

	pci_set_drvdata(dev, NULL);

	// If this is postponed, a subsequent probe is forced to use a different ordinal.
	mutex_lock(&tenstorrent_dev_idr_mutex);
	idr_remove(&tenstorrent_dev_idr, tt_dev->ordinal);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

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

	int ret = tt_dev->dev_class->init_hardware(tt_dev);

	// Suspend invalidates the saved state.
	if (ret == 0)
		pci_save_state(pdev);

	return ret;
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

struct tenstorrent_device *tenstorrent_lookup_device(unsigned minor)
{
	struct tenstorrent_device *dev;

	mutex_lock(&tenstorrent_dev_idr_mutex);
	dev = idr_find(&tenstorrent_dev_idr, minor);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

	return dev;
}
