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
};

#define tt_dev_to_wh_dev(ttdev) \
	container_of((tt_dev), struct wormhole_device, tt)

#endif
