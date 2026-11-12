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

	// Firmware event ring: on each MSI, copy records firmware appended since
	// the last read into the device's event records, and release them.
	struct work_struct events_work;
	bool events_enabled;		// events_work may run
	u32 event_ring_addr;		// CSM address of the ring, 0 if none
	u32 event_ring_entries;		// Records the ring holds, a power of 2
	u32 event_ring_entry_size;
	u32 event_ring_rptr;		// Firmware seq of the next record to read
	u32 event_ring_overflow;	// Firmware's overflow count as last read
	struct tenstorrent_event event_batch[TT_EVENT_RING_SIZE];	// One drain's records
};

#define tt_dev_to_bh_dev(ttdev) \
	container_of((ttdev), struct blackhole_device, tt)

#endif
