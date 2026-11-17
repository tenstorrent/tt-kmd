// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCI driver: probe/remove of the Blackhole device and the device registry
 * (minor -> struct tenstorrent_device) used by open().
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include "device.h"
#include "blackhole.h"
#include "chardev.h"

static DEFINE_IDA(tt_ordinal_ida);
static DEFINE_MUTEX(tt_devices_lock);
static struct tenstorrent_device *tt_devices[TT_MAX_DEVICES];

static void tt_device_release(struct kref *kref)
{
	struct tenstorrent_device *dev = container_of(kref, struct tenstorrent_device, kref);

	cleanup_srcu_struct(&dev->detach_srcu);
	pci_dev_put(dev->pdev);
	kfree(dev);
}

void tt_device_get(struct tenstorrent_device *dev)
{
	kref_get(&dev->kref);
}

void tt_device_put(struct tenstorrent_device *dev)
{
	kref_put(&dev->kref, tt_device_release);
}

struct tenstorrent_device *tt_device_lookup(unsigned int minor)
{
	struct tenstorrent_device *dev = NULL;

	if (minor >= TT_MAX_DEVICES)
		return NULL;

	mutex_lock(&tt_devices_lock);
	dev = tt_devices[minor];
	if (dev)
		tt_device_get(dev);
	mutex_unlock(&tt_devices_lock);
	return dev;
}

static void tt_devices_publish(struct tenstorrent_device *dev)
{
	mutex_lock(&tt_devices_lock);
	tt_devices[dev->ordinal] = dev;
	mutex_unlock(&tt_devices_lock);
}

static void tt_devices_unpublish(struct tenstorrent_device *dev)
{
	mutex_lock(&tt_devices_lock);
	tt_devices[dev->ordinal] = NULL;
	mutex_unlock(&tt_devices_lock);
}

static irqreturn_t tt_irq_handler(int irq, void *data)
{
	/* No interrupt source is used yet; the vector exists so MSI is enabled
	 * and programmed. Later subsystems hook their handling in here. */
	return IRQ_HANDLED;
}

/* Stop new device sections, wait for the running ones, wake all waiters. */
static void tt_mark_detached(struct tenstorrent_device *dev)
{
	WRITE_ONCE(dev->detached, true);
	synchronize_srcu(&dev->detach_srcu);
	wake_up_all(&dev->open_wq);
	wake_up_all(&dev->lock_wq);
}

static void tt_build_mappings(struct tenstorrent_device *dev)
{
	struct tenstorrent_mapping *m = dev->mappings;

	m[0] = (struct tenstorrent_mapping){
		.mapping_id = TENSTORRENT_MAPPING_RESOURCE0_UC,
		.mapping_base = TT_MMAP_BAR0_UC,
		.mapping_size = dev->bar0_len,
	};
	/* No RESOURCE0_WC: see the PAT note in device.h. */
	m[1] = (struct tenstorrent_mapping){
		.mapping_id = TENSTORRENT_MAPPING_RESOURCE1_UC,
		.mapping_base = TT_MMAP_BAR2_UC,
		.mapping_size = dev->bar2_len,
	};
	dev->num_mappings = 2;
}

/* Enable PCIe error reporting in Device Control if the platform left it off. */
static void tt_enable_error_reporting(struct tenstorrent_device *dev)
{
	const u16 bits = PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE |
			 PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE;
	struct pci_dev *pdev = dev->pdev;
	u16 devctl;

	if (!pci_is_pcie(pdev))
		return;
	if (pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &devctl))
		return;
	if (devctl & bits)
		return;

	dev->devctl_orig = devctl;
	if (!pcie_capability_set_word(pdev, PCI_EXP_DEVCTL, bits))
		dev->devctl_changed = true;
}

static void tt_restore_error_reporting(struct tenstorrent_device *dev)
{
	if (dev->devctl_changed)
		pcie_capability_clear_and_set_word(dev->pdev, PCI_EXP_DEVCTL,
						   PCI_EXP_DEVCTL_CERE | PCI_EXP_DEVCTL_NFERE |
						   PCI_EXP_DEVCTL_FERE | PCI_EXP_DEVCTL_URRE,
						   dev->devctl_orig);
}

static int tt_setup_irq(struct tenstorrent_device *dev)
{
	struct pci_dev *pdev = dev->pdev;
	int ret;

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_info(&pdev->dev, "MSI not available (%d)\n", ret);
		return 0;
	}

	dev->irq = pci_irq_vector(pdev, 0);
	ret = request_irq(dev->irq, tt_irq_handler, 0, TT_DRIVER_NAME, dev);
	if (ret) {
		pci_free_irq_vectors(pdev);
		dev_info(&pdev->dev, "cannot request MSI vector (%d)\n", ret);
		return 0;
	}

	dev->msi_enabled = true;
	return 0;
}

static void tt_teardown_irq(struct tenstorrent_device *dev)
{
	if (!dev->msi_enabled)
		return;
	free_irq(dev->irq, dev);
	pci_free_irq_vectors(dev->pdev);
	dev->msi_enabled = false;
}

static void tt_unmap_bars(struct tenstorrent_device *dev)
{
	mutex_lock(&dev->kernel_tlb_mutex);
	dev->mmio_gone = true;
	mutex_unlock(&dev->kernel_tlb_mutex);

	if (dev->bar2)
		pci_iounmap(dev->pdev, dev->bar2);
	if (dev->kernel_tlb)
		pci_iounmap(dev->pdev, dev->kernel_tlb);
	if (dev->bar0_regs)
		pci_iounmap(dev->pdev, dev->bar0_regs);
	dev->bar2 = NULL;
	dev->kernel_tlb = NULL;
	dev->bar0_regs = NULL;
}

static int tt_map_bars(struct tenstorrent_device *dev)
{
	struct pci_dev *pdev = dev->pdev;

	dev->bar0_start = pci_resource_start(pdev, 0);
	dev->bar0_len = pci_resource_len(pdev, 0);
	dev->bar2_start = pci_resource_start(pdev, 2);
	dev->bar2_len = pci_resource_len(pdev, 2);
	dev->bar4_start = pci_resource_start(pdev, 4);
	dev->bar4_len = pci_resource_len(pdev, 4);

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) || dev->bar0_len < BH_BAR0_SIZE ||
	    !(pci_resource_flags(pdev, 2) & IORESOURCE_MEM) || dev->bar2_len < BH_BAR2_SIZE) {
		dev_err(&pdev->dev, "unexpected BARs: BAR0 %pa, BAR2 %pa\n",
			&dev->bar0_len, &dev->bar2_len);
		return -ENODEV;
	}

	if (pci_resource_flags(pdev, 4) & IORESOURCE_MEM)
		dev->num_4g_windows = min_t(u64, dev->bar4_len / BH_TLB_4G_SIZE, BH_TLB_4G_MAX);
	else
		dev->bar4_len = 0;

	dev->bar0_regs = pci_iomap_range(pdev, 0, BH_REG_REGION_BAR0_OFFSET, BH_REG_REGION_SIZE);
	dev->kernel_tlb = pci_iomap_range(pdev, 0, bh_tlb_window_bar_offset(BH_KERNEL_TLB_INDEX),
					  BH_TLB_2M_SIZE);
	dev->bar2 = pci_iomap_range(pdev, 2, 0, dev->bar2_len);
	if (!dev->bar0_regs || !dev->kernel_tlb || !dev->bar2) {
		dev_err(&pdev->dev, "cannot map BARs\n");
		tt_unmap_bars(dev);
		return -ENOMEM;
	}
	dev->mmio_gone = false;
	return 0;
}

/* Subsystem device init/fini, in init order. */
struct tt_subsys {
	int (*init)(struct tenstorrent_device *dev);
	void (*fini)(struct tenstorrent_device *dev);
};

static const struct tt_subsys tt_subsystems[] = {
	{ tt_memory_device_init, tt_memory_device_fini },
	{ tt_tlb_device_init, tt_tlb_device_fini },
	{ tt_noc_device_init, tt_noc_device_fini },
	{ tt_smc_device_init, tt_smc_device_fini },
	{ tt_power_device_init, tt_power_device_fini },
};

static void tt_subsystems_fini(struct tenstorrent_device *dev, int count)
{
	while (count--)
		tt_subsystems[count].fini(dev);
}

static int tt_subsystems_init(struct tenstorrent_device *dev)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(tt_subsystems); i++) {
		ret = tt_subsystems[i].init(dev);
		if (ret) {
			tt_subsystems_fini(dev, i);
			return ret;
		}
	}
	return 0;
}

static struct tenstorrent_device *tt_device_alloc(struct pci_dev *pdev)
{
	struct tenstorrent_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

	if (init_srcu_struct(&dev->detach_srcu)) {
		kfree(dev);
		return NULL;
	}
	kref_init(&dev->kref);
	dev->pdev = pci_dev_get(pdev);
	dev->ordinal = -1;
	mutex_init(&dev->kernel_tlb_mutex);
	dev->mmio_gone = true;
	mutex_init(&dev->open_lock);
	INIT_LIST_HEAD(&dev->open_fds);
	init_waitqueue_head(&dev->open_wq);
	spin_lock_init(&dev->resource_lock);
	init_waitqueue_head(&dev->lock_wq);
	return dev;
}

static int tt_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct tenstorrent_device *dev;
	int ret;

	dev = tt_device_alloc(pdev);
	if (!dev)
		return -ENOMEM;

	ret = ida_alloc_max(&tt_ordinal_ida, TT_MAX_DEVICES - 1, GFP_KERNEL);
	if (ret < 0)
		goto err_put;
	dev->ordinal = ret;
	dev->devt = MKDEV(MAJOR(tt_devt_base), dev->ordinal);

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed (%d)\n", ret);
		goto err_ida;
	}

	ret = pci_request_regions(pdev, TT_DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "cannot claim BARs (%d)\n", ret);
		goto err_disable;
	}

	ret = tt_map_bars(dev);
	if (ret)
		goto err_regions;

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "no usable DMA mask (%d)\n", ret);
		goto err_master;
	}

	tt_enable_error_reporting(dev);

	pci_save_state(pdev);

	ret = bh_init(dev);
	if (ret)
		goto err_devctl;

	tt_build_mappings(dev);

	ret = tt_subsystems_init(dev);
	if (ret) {
		dev_err(&pdev->dev, "subsystem init failed (%d)\n", ret);
		goto err_devctl;
	}

	/* After subsystem init, so the handler only ever sees initialised state. */
	ret = tt_setup_irq(dev);
	if (ret)
		goto err_subsys;

	pci_set_drvdata(pdev, dev);

	tt_devices_publish(dev);
	ret = tt_chardev_add(dev);
	if (ret)
		goto err_unpublish;

	tt_procfs_device_add(dev);
	tt_debugfs_device_add(dev);

	dev_info(&pdev->dev, "tenstorrent %d: Blackhole, BAR4 %u x 4 GiB windows, MSI %s\n",
		 dev->ordinal, dev->num_4g_windows, dev->msi_enabled ? "on" : "off");
	return 0;

err_unpublish:
	tt_devices_unpublish(dev);
	pci_set_drvdata(pdev, NULL);
	tt_mark_detached(dev);
	tt_teardown_irq(dev);
err_subsys:
	tt_subsystems_fini(dev, ARRAY_SIZE(tt_subsystems));
err_devctl:
	tt_restore_error_reporting(dev);
err_master:
	pci_clear_master(pdev);
	tt_unmap_bars(dev);
err_regions:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
err_ida:
	ida_free(&tt_ordinal_ida, dev->ordinal);
err_put:
	tt_device_put(dev);
	return ret;
}

static void tt_pci_remove(struct pci_dev *pdev)
{
	struct tenstorrent_device *dev = pci_get_drvdata(pdev);

	/* No new users: debugfs/procfs readers are drained, the node is gone. */
	tt_debugfs_device_remove(dev);
	tt_procfs_device_remove(dev);
	tt_devices_unpublish(dev);
	tt_chardev_remove(dev);

	/* Wait for in-flight ioctls/mmaps; later ones fail with -ENODEV. */
	tt_mark_detached(dev);

	/* Existing user mappings of the BARs must not outlive the BARs; faults
	 * after this point SIGBUS. */
	tt_chardev_zap_mappings(dev);

	/* Fds that stay open: drop their DMA mappings etc. while we can. */
	tt_chardev_detach_fds(dev);

	tt_teardown_irq(dev);
	tt_subsystems_fini(dev, ARRAY_SIZE(tt_subsystems));

	tt_restore_error_reporting(dev);
	pci_clear_master(pdev);
	tt_unmap_bars(dev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

	ida_free(&tt_ordinal_ida, dev->ordinal);
	tt_device_put(dev);
}

static const struct pci_device_id tt_pci_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_BLACKHOLE) },
	{ }
};
MODULE_DEVICE_TABLE(pci, tt_pci_ids);

static struct pci_driver tt_pci_driver = {
	.name = TT_DRIVER_NAME,
	.id_table = tt_pci_ids,
	.probe = tt_pci_probe,
	.remove = tt_pci_remove,
};

int tt_pci_register(void)
{
	return pci_register_driver(&tt_pci_driver);
}

void tt_pci_unregister(void)
{
	pci_unregister_driver(&tt_pci_driver);
}
