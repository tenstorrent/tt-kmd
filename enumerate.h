#ifndef TTDRIVER_ENUMERATE_H_INCLUDED
#define TTDRIVER_ENUMERATE_H_INCLUDED

#include <linux/list.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/cdev.h>

#define TENSTORRENT "tenstorrent"

#define PCI_VENDOR_ID_TENSTORRENT 0x1E52
#define PCI_DEVICE_ID_GRAYSKULL	0xFACA
#define PCI_DEVICE_ID_WORMHOLE	0x401E

struct pci_dev;
struct cdev;

struct grayskull_device {
	struct device dev;
	struct cdev chardev;
	struct pci_dev *pdev;
	unsigned int ordinal;
	bool dma_capable;
	bool interrupt_enabled;

	u8 __iomem *reset_unit_regs;
};

int tenstorrent_pci_register_driver(void);
void tenstorrent_pci_unregister_driver(void);

struct grayskull_device *tenstorrent_lookup_device(unsigned minor);

#endif
