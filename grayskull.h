// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_GRAYSKULL_H_INCLUDED
#define TTDRIVER_GRAYSKULL_H_INCLUDED

#include <linux/types.h>
#include "device.h"

#define SCRATCH_REG(n) (0x60 + (n)*sizeof(u32))	/* byte offset */

bool grayskull_shutdown_firmware(struct pci_dev *pdev, u8 __iomem* reset_unit_regs);

bool grayskull_send_arc_fw_message(u8 __iomem* reset_unit_regs, u8 message_id, u32 timeout_us, u16* exit_code);
bool grayskull_send_arc_fw_message_with_args(u8 __iomem* reset_unit_regs,
					     u8 message_id, u16 arg0, u16 arg1,
					     u32 timeout_us, u16* exit_code);
bool arc_l2_is_running(u8 __iomem* reset_unit_regs);
bool grayskull_read_fw_telemetry_offset(u8 __iomem *reset_unit_regs, u32 *offset);
void grayskull_send_curr_date(u8 __iomem* reset_unit_regs);

#endif
