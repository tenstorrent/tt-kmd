#ifndef TTDRIVER_DEVICE_H_INCLUDED
#define TTDRIVER_DEVICE_H_INCLUDED

#include <linux/types.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/reboot.h>

struct tenstorrent_device_class;

struct tenstorrent_device {
	struct device dev;
	struct cdev chardev;
	struct pci_dev *pdev;
	const struct tenstorrent_device_class *dev_class;

	unsigned int ordinal;
	bool dma_capable;
	bool interrupt_enabled;

	struct mutex chardev_mutex;
	unsigned int chardev_open_count;

	struct notifier_block reboot_notifier;
};

struct tenstorrent_device_class {
	const char *name;
	u32 instance_size;
	bool (*init_device)(struct tenstorrent_device *ttdev);
	bool (*init_hardware)(struct tenstorrent_device *ttdev);
	void (*cleanup_device)(struct tenstorrent_device *ttdev);
	void (*first_open_cb)(struct tenstorrent_device *ttdev);
	void (*last_release_cb)(struct tenstorrent_device *ttdev);
	void (*reboot)(struct tenstorrent_device *ttdev);
};

#endif
