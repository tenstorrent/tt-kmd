// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include <linux/spinlock.h>
#include "device.h"

struct blackhole_device {
	struct tenstorrent_device tt;

	// Guards access to kernel_tlb and serializes aperture MMIO across CPUs
	// (spin_unlock issues mmiowb so posted writes drain before the TLB is reprogrammed).
	spinlock_t kernel_tlb_lock;
	u8 __iomem *tlb_regs;   // All TLB registers
	u8 __iomem *kernel_tlb; // Topmost 2M window, reserved for kernel
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;

	u8 saved_mps;

	bool pcie_perf_group_registered;
	bool telemetry_group_registered;
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((ttdev), struct blackhole_device, tt)

#endif
