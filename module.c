#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>

#include "chardev.h"
#include "enumerate.h"

#define TTDRIVER_VER "1.0"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tenstorrent AI kernel driver");
MODULE_VERSION(TTDRIVER_VER);

static uint max_devices = 16;
module_param(max_devices, uint, 0444);
MODULE_PARM_DESC(max_devices, "Maximum number of tenstorrent devices (chips) to support.");

const struct pci_device_id tenstorrent_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_GRAYSKULL) },
	{ 0 },
};

MODULE_DEVICE_TABLE(pci, tenstorrent_ids);

static int __init ttdriver_init(void)
{
	int err = 0;

	printk(KERN_INFO "Loading Tenstorrent AI driver module v%s.\n", TTDRIVER_VER);

	err = init_char_driver(max_devices);
	if (err == 0)
		err = tenstorrent_pci_register_driver();

	return err;
}

static void __exit ttdriver_cleanup(void)
{
	printk(KERN_INFO "Unloading Tenstorrent AI driver module.\n");

	tenstorrent_pci_unregister_driver();
	cleanup_char_driver();
}

module_init(ttdriver_init);
module_exit(ttdriver_cleanup);
