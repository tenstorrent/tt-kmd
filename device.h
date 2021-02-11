#ifndef TTDRIVER_DEVICE_H_INCLUDED
#define TTDRIVER_DEVICE_H_INCLUDED

#include <linux/types.h>
#include <linux/device.h>
#include <linux/cdev.h>

struct tenstorrent_device_class;

struct tenstorrent_device {
	struct device dev;
	struct cdev chardev;
	struct pci_dev *pdev;
	const struct tenstorrent_device_class *dev_class;

	unsigned int ordinal;
	bool dma_capable;
	bool interrupt_enabled;
};

struct tenstorrent_device_class {
	const char *name;
	u32 instance_size;
	bool (*init_device)(struct tenstorrent_device *ttdev);
	void (*cleanup_device)(struct tenstorrent_device *ttdev);
};

#endif
