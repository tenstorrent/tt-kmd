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
	struct tenstorrent_device *tt_dev = NULL;
	int ordinal;
	const struct tenstorrent_device_class *device_class = (const struct tenstorrent_device_class *)id->driver_data;

	printk(KERN_INFO "Found a Tenstorrent %s device at bus %d.\n", device_class->name, (int)dev->bus->number);

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

	tt_dev->dev_class = device_class;
	tt_dev->pdev = dev;
	tt_dev->ordinal = ordinal;

	mutex_init(&tt_dev->chardev_mutex);

	tt_dev->dma_capable = (dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(32)) == 0);
	pci_set_master(dev);

	pci_set_drvdata(dev, tt_dev);

	tt_dev->interrupt_enabled = tenstorrent_enable_interrupts(tt_dev);

	if (device_class->init_device(tt_dev))
		device_class->init_hardware(tt_dev);

	pci_save_state(dev);

	tenstorrent_register_device(tt_dev);

	return 0;
}

static void tenstorrent_pci_remove(struct pci_dev *dev)
{
	struct tenstorrent_device *tt_dev = pci_get_drvdata(dev);

	tenstorrent_unregister_device(tt_dev);

	tt_dev->dev_class->cleanup_device(tt_dev);

	tenstorrent_disable_interrupts(tt_dev);

	pci_set_drvdata(dev, NULL);

	pci_disable_device(dev);

	mutex_lock(&tenstorrent_dev_idr_mutex);
	idr_remove(&tenstorrent_dev_idr, tt_dev->ordinal);
	mutex_unlock(&tenstorrent_dev_idr_mutex);

	kfree(tt_dev);
}

extern const struct pci_device_id tenstorrent_ids[];
static struct pci_driver tenstorrent_pci_driver = {
	.name = TENSTORRENT,
	.id_table = tenstorrent_ids,
	.probe = tenstorrent_pci_probe,
	.remove = tenstorrent_pci_remove,
	.shutdown = tenstorrent_pci_remove,
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
