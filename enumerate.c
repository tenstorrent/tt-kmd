#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>

#include "enumerate.h"
#include "interrupt.h"
#include "chardev.h"
#include "grayskull.h"

DEFINE_IDR(tenstorrent_dev_idr);
DEFINE_MUTEX(tenstorrent_dev_idr_mutex);

static int tenstorrent_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct grayskull_device *gs_dev = NULL;
	int ordinal;

	printk(KERN_INFO "Found a Grayskull device at bus %d.", (int)dev->bus->number);

	if (pci_enable_device(dev) < 0)
		return -EIO;

	gs_dev = kzalloc(sizeof(*gs_dev), GFP_KERNEL);
	if (gs_dev == NULL)
		return -ENOMEM;

	mutex_lock(&tenstorrent_dev_idr_mutex);
	ordinal = idr_alloc(&tenstorrent_dev_idr, gs_dev, 0, 0, GFP_KERNEL);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

	if (ordinal < 0) {
		kfree(gs_dev);
		pci_disable_device(dev);
		return ordinal;
	}

	gs_dev->pdev = dev;
	gs_dev->ordinal = ordinal;

	gs_dev->dma_capable = (dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32)) == 0);
	pci_set_master(dev);

	pci_set_drvdata(dev, gs_dev);

	gs_dev->interrupt_enabled = tenstorrent_enable_interrupts(gs_dev);

	grayskull_init(gs_dev);

	tenstorrent_register_device(gs_dev);

	return 0;
}

static void tenstorrent_pci_remove(struct pci_dev *dev)
{
	struct grayskull_device *gs_dev = pci_get_drvdata(dev);

	tenstorrent_unregister_device(gs_dev);

	grayskull_cleanup(gs_dev);

	tenstorrent_disable_interrupts(gs_dev);

	pci_set_drvdata(dev, NULL);

	pci_disable_device(dev);

	mutex_lock(&tenstorrent_dev_idr_mutex);
	idr_remove(&tenstorrent_dev_idr, gs_dev->ordinal);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

	kfree(gs_dev);
}

extern const struct pci_device_id tenstorrent_ids[];
static struct pci_driver tenstorrent_pci_driver = {
	.name = TENSTORRENT,
	.id_table = tenstorrent_ids,
	.probe = tenstorrent_pci_probe,
	.remove = tenstorrent_pci_remove,
};

int tenstorrent_pci_register_driver(void)
{
	return pci_register_driver(&tenstorrent_pci_driver);
}

void tenstorrent_pci_unregister_driver(void)
{
	pci_unregister_driver(&tenstorrent_pci_driver);
}

struct grayskull_device *tenstorrent_lookup_device(unsigned minor)
{
	struct grayskull_device *dev;

	mutex_lock(&tenstorrent_dev_idr_mutex);
	dev = idr_find(&tenstorrent_dev_idr, minor);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

	return dev;
}
