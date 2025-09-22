// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_WORMHOLE_H_INCLUDED
#define TTDRIVER_WORMHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct wormhole_device {
	struct tenstorrent_device tt;
	struct mutex kernel_tlb_mutex;	// Guards access to kernel_tlb

	u8 __iomem *bar2_mapping;
	u8 __iomem *bar4_mapping;

	u8 saved_mps;

	u64 *sysfs_attr_offsets;

	struct delayed_work fw_ready_work;
	int telemetry_retries;
};

#define tt_dev_to_wh_dev(ttdev) \
	container_of((tt_dev), struct wormhole_device, tt)

bool wormhole_send_arc_fw_message_with_args(u8 __iomem *reset_unit_regs, u8 message_id, u16 arg0, u16 arg1,
					    u32 timeout_us, u16 *exit_code);

#endif
