// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tenstorrent Blackhole driver: module init/exit and the global objects
 * (char device region, device class, debugfs and procfs roots).
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>

#include "device.h"

struct class *tt_class;
dev_t tt_devt_base;

static int __init tt_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&tt_devt_base, 0, TT_MAX_DEVICES, TT_DRIVER_NAME);
	if (ret)
		return ret;

	tt_class = class_create(TT_DRIVER_NAME);
	if (IS_ERR(tt_class)) {
		ret = PTR_ERR(tt_class);
		goto err_region;
	}

	tt_debugfs_init();	/* optional: failures leave debugfs out */

	ret = tt_procfs_init();
	if (ret)
		goto err_debugfs;

	ret = tt_pci_register();
	if (ret)
		goto err_procfs;

	return 0;

err_procfs:
	tt_procfs_exit();
err_debugfs:
	tt_debugfs_exit();
	class_destroy(tt_class);
err_region:
	unregister_chrdev_region(tt_devt_base, TT_MAX_DEVICES);
	return ret;
}

static void __exit tt_exit(void)
{
	tt_pci_unregister();
	tt_procfs_exit();
	tt_debugfs_exit();
	class_destroy(tt_class);
	unregister_chrdev_region(tt_devt_base, TT_MAX_DEVICES);
}

module_init(tt_init);
module_exit(tt_exit);

MODULE_AUTHOR("Tenstorrent");
MODULE_DESCRIPTION("Tenstorrent Blackhole AI accelerator driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(TT_DRIVER_VERSION_STR);
