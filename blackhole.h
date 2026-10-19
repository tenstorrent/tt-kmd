// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include <linux/workqueue.h>
#include "device.h"

struct blackhole_device {
	struct tenstorrent_device tt;

	struct mutex kernel_tlb_mutex;	// Guards access to kernel_tlb
	u8 __iomem *tlb_regs;   // All TLB registers
	u8 __iomem *kernel_tlb; // Topmost 2M window, reserved for kernel
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;

	u8 saved_mps;

	bool pcie_perf_group_registered;
	bool telemetry_group_registered;

	// Uncorrectable L1 ECC error detection: on each MSI, compare firmware's
	// L1 DBE total against the last value read.
	struct work_struct l1_dbe_work;
	bool l1_dbe_enabled;
	bool l1_dbe_baselined;	// l1_dbe_count holds a value read from firmware
	u32 l1_dbe_count;
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((ttdev), struct blackhole_device, tt)

#endif
