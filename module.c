// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/debugfs.h>
#include <linux/proc_fs.h>

#include "chardev.h"
#include "enumerate.h"

#include "module.h"

#define TENSTORRENT_DRIVER_VERSION_STRING \
	__stringify(TENSTORRENT_DRIVER_VERSION_MAJOR) "." \
	__stringify(TENSTORRENT_DRIVER_VERSION_MINOR) "." \
	__stringify(TENSTORRENT_DRIVER_VERSION_PATCH) \
	TENSTORRENT_DRIVER_VERSION_SUFFIX

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tenstorrent AI kernel driver");
MODULE_VERSION(TENSTORRENT_DRIVER_VERSION_STRING);

struct dentry *tt_debugfs_root;
struct proc_dir_entry *tt_procfs_root;

static uint max_devices = 32;
module_param(max_devices, uint, 0444);
MODULE_PARM_DESC(max_devices, "Maximum number of tenstorrent devices (chips) to support.");

uint dma_address_bits = 0;
module_param(dma_address_bits, uint, 0444);
MODULE_PARM_DESC(dma_address_bits, "DMA address bits, 0 for automatic.");

uint reset_limit = 10;
module_param(reset_limit, uint, 0444);
MODULE_PARM_DESC(reset_limit, "Maximum number of times to reset device during boot.");

unsigned char auto_reset_timeout = 10;
module_param(auto_reset_timeout, byte, 0444);
MODULE_PARM_DESC(auto_reset_timeout, "Timeout duration in seconds for M3 auto reset to occur.");

bool power_policy = true;
module_param(power_policy, bool, 0644);
MODULE_PARM_DESC(power_policy, "Enable power policy: low power at probe, re-aggregate on close (default=on).");

const struct pci_device_id tenstorrent_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_GRAYSKULL),
	  .driver_data=(kernel_ulong_t)NULL}, // Deprecated
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_WORMHOLE),
	  .driver_data=(kernel_ulong_t)&wormhole_class },
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_BLACKHOLE),
	  .driver_data=(kernel_ulong_t)&blackhole_class },
	{ 0 },
};

MODULE_DEVICE_TABLE(pci, tenstorrent_ids);

static int __init ttdriver_init(void)
{
	int err = 0;

	printk(KERN_INFO "Loading Tenstorrent AI driver module v%s.\n", TENSTORRENT_DRIVER_VERSION_STRING);

	tt_debugfs_root = debugfs_create_dir("tenstorrent", NULL);

	tt_procfs_root = proc_mkdir("driver/tenstorrent", NULL);
	if (!tt_procfs_root) {
		err = -ENOMEM;
		goto fail_procfs;
	}

	err = init_char_driver(max_devices);
	if (err != 0)
		goto fail_char_driver;

	err = tenstorrent_pci_register_driver();
	if (err != 0)
		goto fail_pci_register;

	return 0;

fail_pci_register:
	cleanup_char_driver();
fail_char_driver:
	proc_remove(tt_procfs_root);
fail_procfs:
	debugfs_remove(tt_debugfs_root);

	return err;
}

static void __exit ttdriver_cleanup(void)
{
	printk(KERN_INFO "Unloading Tenstorrent AI driver module.\n");

	tenstorrent_pci_unregister_driver();
	cleanup_char_driver();
	debugfs_remove(tt_debugfs_root);
	proc_remove(tt_procfs_root);
}

module_init(ttdriver_init);
module_exit(ttdriver_cleanup);
