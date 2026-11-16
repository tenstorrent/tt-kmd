// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "device.h"

// Most data windows DMA holds at a time.
#define BLACKHOLE_DMA_MAX_WINDOWS 16

struct chardev_private;

// DMA writes from host memory to the device, served by HDMA read channel 0
// running chains of descriptors from a linked list.
struct blackhole_dma {
	spinlock_t lock;	// Guards the fields below up to deadline, and the channel registers
	struct list_head queue;	// Commands oldest first; the engine runs the first one
	int list_window;	// 2M TLB id the engine fetches the list through, or -1
	int windows[BLACKHOLE_DMA_MAX_WINDOWS]; // TLB ids the engine writes through
	unsigned int window_count;
	u64 window_size;	// Of every data window
	u64 list_axi;		// Address of the list as the engine fetches it
	u64 msi_addr;
	u32 msi_data;
	u32 saved_qos;		// QOS before the queue went busy, restored when it drains
	unsigned long deadline;	// jiffies by which the running chain must stop

	struct mutex window_mutex; // Serializes taking and returning windows, which may sleep
	// The fd whose SET_DMA_WINDOWS set is held, or NULL while the windows are
	// the default ones. Guarded by window_mutex.
	struct chardev_private *window_owner;
	struct work_struct release_windows; // Returns the windows once the queue drains
	struct delayed_work timeout;
	void *list;		// Elements of the running chain, in host memory
	dma_addr_t list_iova;
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
