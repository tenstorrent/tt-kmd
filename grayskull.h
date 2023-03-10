#ifndef TTDRIVER_GRAYSKULL_H_INCLUDED
#define TTDRIVER_GRAYSKULL_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct grayskull_device {
	struct tenstorrent_device tt;
	u8 __iomem *reg_iomap;	// everything after the TLB windows
	u8 __iomem *kernel_tlb;	// covers one TLB window
	u8 __iomem *reset_unit_regs;
	u32 enabled_rows;	// bitmap of enabled Tensix rows (NOC0-indexed)
};

#define tt_dev_to_gs_dev(ttdev) \
	container_of((tt_dev), struct grayskull_device, tt)

bool grayskull_shutdown_firmware(struct pci_dev *pdev, u8 __iomem* reset_unit_regs);

bool grayskull_send_arc_fw_message_with_args(u8 __iomem* reset_unit_regs,
					     u8 message_id, u16 arg0, u16 arg1,
					     u32 timeout_us, u16* exit_code);
bool poll_pcie_link_up_completion(struct pci_dev *pdev, u32 timeout_ms);
bool complete_pcie_init(struct tenstorrent_device *tt_dev, u8 __iomem* reset_unit_regs);
bool arc_l2_is_running(u8 __iomem* reset_unit_regs);

#endif
