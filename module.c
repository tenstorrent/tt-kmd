#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>

#include "chardev.h"
#include "enumerate.h"

#define TTDRIVER_VER "1.4"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Tenstorrent AI kernel driver");
MODULE_VERSION(TTDRIVER_VER);

static uint max_devices = 16;
module_param(max_devices, uint, 0444);
MODULE_PARM_DESC(max_devices, "Maximum number of tenstorrent devices (chips) to support.");

bool arc_fw_init = true;
module_param(arc_fw_init, bool, 0444);
MODULE_PARM_DESC(arc_fw_init, "Automatically initialize tenstorrent devices' ARC FW. Default Y.");

bool arc_fw_override = false;
module_param(arc_fw_override, bool, 0444);
MODULE_PARM_DESC(arc_fw_override, "Override ARC FW from filesystem instead of auto load from SPI: Y/N. Default N.");

bool arc_fw_stage2_init = true;
module_param(arc_fw_stage2_init, bool, 0444);
MODULE_PARM_DESC(arc_fw_stage2_init, "Enable ARC FW stage 2 initialization. Default Y.");

bool ddr_train_en = true;
module_param(ddr_train_en, bool, 0444);
MODULE_PARM_DESC(ddr_train_en, "Enable DRAM training: Y/N. Default Y.");

uint ddr_frequency_override = 0;
module_param(ddr_frequency_override, uint, 0444);
MODULE_PARM_DESC(ddr_frequency_override, "DDR frequency override in MHz or 0 to auto-detect.");

bool aiclk_ppm_en = true;
module_param(aiclk_ppm_en, bool, 0444);
MODULE_PARM_DESC(aiclk_ppm_en, "Enable dynamic AICLK: Y/N. Default Y.");

uint aiclk_fmax_override = 0;
module_param(aiclk_fmax_override, uint, 0444);
MODULE_PARM_DESC(aiclk_fmax_override, "AICLK override in MHz or 0 to auto-detect.");

uint arc_fw_feat_dis_override = 0;
module_param(arc_fw_feat_dis_override, uint, 0444);
MODULE_PARM_DESC(arc_fw_feat_dis_override, "Feature disable bitmap. Default 0.");

bool watchdog_fw_en = true;
module_param(watchdog_fw_en, bool, 0444);
MODULE_PARM_DESC(watchdog_fw_en, "Enable watchdog FW: Y/N. Default Y.");

bool watchdog_fw_override = false;
module_param(watchdog_fw_override, bool, 0444);
MODULE_PARM_DESC(watchdog_fw_override, "Override watchdog FW from filesystem instead of auto load from SPI: Y/N. Default N.");

uint axiclk_override = 0;
module_param(axiclk_override, uint, 0444);
MODULE_PARM_DESC(axiclk_override, "AXICLK override in MHz or 0 to auto-detect.");

struct tenstorrent_device_class;
extern struct tenstorrent_device_class grayskull_class;
extern struct tenstorrent_device_class wormhole_class;

const struct pci_device_id tenstorrent_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_GRAYSKULL),
	  .driver_data=(kernel_ulong_t)&grayskull_class },
	{ PCI_DEVICE(PCI_VENDOR_ID_TENSTORRENT, PCI_DEVICE_ID_WORMHOLE),
	  .driver_data=(kernel_ulong_t)&wormhole_class },
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
