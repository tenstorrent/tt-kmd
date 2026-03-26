// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct blackhole_device {
	struct tenstorrent_device tt;

	struct mutex kernel_tlb_mutex;
	u8 __iomem *tlb_regs;
	u8 __iomem *kernel_tlb;    // UC-mapped 2M window for reads and 32-bit ops
	u8 __iomem *kernel_tlb_wc; // WC-mapped 2M window for block writes
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;

	u64 *hwmon_attr_addrs;
	u64 *sysfs_attr_addrs;

	u8 saved_mps;

	bool pcie_perf_group_registered;
	bool telemetry_group_registered;
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((tt_dev), struct blackhole_device, tt)

#endif
