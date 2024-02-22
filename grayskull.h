// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_GRAYSKULL_H_INCLUDED
#define TTDRIVER_GRAYSKULL_H_INCLUDED

#include <linux/types.h>
#include "device.h"
#include "tlb.h"

struct grayskull_device {
	struct tenstorrent_device tt;
	u8 __iomem *bar0_mapping;
	u8 __iomem *reg_iomap;	// everything after the TLB windows
	u8 __iomem *kernel_tlb;	// covers one TLB window
	u8 __iomem *reset_unit_regs;
	u32 enabled_rows;	// bitmap of enabled Tensix rows (NOC0-indexed)
	u32 watchdog_fw_reset_vec;
	u32 smbus_fw_reset_vec;
	struct tt_hwmon_context *hwmon_context;
	struct tlb_pool tlb_pool;
};

#define tt_dev_to_gs_dev(ttdev) \
	container_of((tt_dev), struct grayskull_device, tt)

bool grayskull_shutdown_firmware(struct pci_dev *pdev, u8 __iomem* reset_unit_regs);

bool grayskull_send_arc_fw_message(u8 __iomem* reset_unit_regs, u8 message_id, u32 timeout_us, u16* exit_code);
bool grayskull_send_arc_fw_message_with_args(u8 __iomem* reset_unit_regs,
					     u8 message_id, u16 arg0, u16 arg1,
					     u32 timeout_us, u16* exit_code);
bool arc_l2_is_running(u8 __iomem* reset_unit_regs);
bool grayskull_read_fw_telemetry_offset(u8 __iomem *reset_unit_regs, u32 *offset);
void grayskull_send_curr_date(u8 __iomem* reset_unit_regs);

#endif
