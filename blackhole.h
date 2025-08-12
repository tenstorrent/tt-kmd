// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct blackhole_device {
	struct tenstorrent_device tt;

	struct mutex kernel_tlb_mutex;	// Guards access to kernel_tlb
	u8 __iomem *tlb_regs;   // All TLB registers
	u8 __iomem *kernel_tlb; // Topmost 2M window, reserved for kernel
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;

	u64 *hwmon_attr_addrs;
	u64 *sysfs_attr_addrs;
	struct attribute **telemetry_attrs;
	struct attribute_group telemetry_group;

	u8 saved_mps;
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((tt_dev), struct blackhole_device, tt)

#endif
