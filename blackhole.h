// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "device.h"

// DMA writes from host memory to the device, served by HDMA read channel 0.
struct blackhole_dma {
	spinlock_t lock;	// Guards queue, window and the channel registers
	struct list_head queue;	// Commands oldest first; the engine runs the head
	int window;		// 2M TLB id the engine writes through while queue is not empty, or -1
	struct mutex window_mutex; // Serializes taking and returning window, which may sleep
	struct work_struct release_window; // Returns window to the pool once queue drains
	unsigned long started;	// jiffies when the running transfer started
	struct delayed_work timeout;
};

struct blackhole_device {
	struct tenstorrent_device tt;

	struct mutex kernel_tlb_mutex;	// Guards access to kernel_tlb
	u8 __iomem *tlb_regs;   // All TLB registers
	u8 __iomem *kernel_tlb; // Topmost 2M window, reserved for kernel
	u8 __iomem *noc2axi_cfg;
	u8 __iomem *bar2_mapping;

	u8 saved_mps;

	struct blackhole_dma dma;

	bool pcie_perf_group_registered;
	bool telemetry_group_registered;
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((ttdev), struct blackhole_device, tt)

#endif
