// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_KERAUNOS_H_INCLUDED
#define TTDRIVER_KERAUNOS_H_INCLUDED

#include <linux/io.h>
#include <linux/spinlock.h>

#include "device.h"

struct keraunos_device {
	struct tenstorrent_device tt;

	// Kernel-owned TLB plumbing for NOC_READ/NOC_WRITE scalar SPA access.
	//
	// bar2 maps the BAR2 control plane (TLBSysIn0), through which the
	// pre-programmed SysIn0[2] bootstrap window lets us (re)program any
	// TLBAppIn0 config entry. bar0_kernel_tlb maps the single 16MB BAR0
	// window belonging to our reserved AppIn0 entry; we point that entry
	// at the target address, then access it through this window.
	//
	// kernel_tlb_lock serializes the program-then-access sequence.
	u8 __iomem *bar0_kernel_tlb;
	u8 __iomem *bar2;
	spinlock_t kernel_tlb_lock;
};

#define tt_dev_to_keraunos_dev(tt_dev) container_of((tt_dev), struct keraunos_device, tt)

#endif
