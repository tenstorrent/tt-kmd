// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_KERAUNOS_H_INCLUDED
#define TTDRIVER_KERAUNOS_H_INCLUDED

#include <linux/io.h>
#include <linux/spinlock.h>

#include "device.h"

// --- BAR2 (TLBSysIn0) layout -------------------------------------------------
// BAR2 is exactly covered by SysIn0: 64 windows of 16KB, BAR2 offset =
// (entry << 14) | within. The top KERAUNOS_BAR2_WC_ENTRIES windows are
// kernel-owned and mapped write-combining for bulk transfers (firmware
// staging into SMC SRAM); everything below is the uncached control plane.
//
// The WC region must be PAGE_SIZE-aligned on every supported page size: on
// x86 PAT folds a WC ioremap of pages that already carry a UC mapping back
// into UC, so the two mappings may not share a page. 64KB covers the largest
// arm64 page size.
#define KERAUNOS_SYSIN0_WINDOW_SHIFT	14
#define KERAUNOS_SYSIN0_WINDOW_SIZE	(1u << KERAUNOS_SYSIN0_WINDOW_SHIFT)	// 16KB
#define KERAUNOS_SYSIN0_ENTRIES		64
#define KERAUNOS_BAR2_SIZE		(KERAUNOS_SYSIN0_ENTRIES << KERAUNOS_SYSIN0_WINDOW_SHIFT)	// 1MB
#define KERAUNOS_BAR2_WC_ENTRIES	4
#define KERAUNOS_BAR2_WC_FIRST_ENTRY	(KERAUNOS_SYSIN0_ENTRIES - KERAUNOS_BAR2_WC_ENTRIES)	// 60
#define KERAUNOS_BAR2_WC_OFFSET		(KERAUNOS_BAR2_WC_FIRST_ENTRY << KERAUNOS_SYSIN0_WINDOW_SHIFT)	// 0xF0000
#define KERAUNOS_BAR2_WC_SIZE		(KERAUNOS_BAR2_WC_ENTRIES << KERAUNOS_SYSIN0_WINDOW_SHIFT)	// 64KB

// --- SMC scratch registers / host-visible boot state -------------------------
// SMC CPU-control scratch registers: 16 x 32-bit at SMC-local 0xC001_0100,
// 8-byte stride; SMC-local 0xC000_0000 is KLA 0x0800_0000.
//
// K-SMC scratch[12] is the host-visible boot state word of the Grendel Boot
// Specification §6.6 (the host staging handshake). The host reads it to
// learn when a firmware bundle staging window is open and writes exactly
// one value, BUNDLE_STAGED, after copying a bundle in.
#define KERAUNOS_SMC_SCRATCH_KLA(n)	(0x08010100u + 8u * (n))
#define KERAUNOS_SMC_BOOT_STATE_KLA	KERAUNOS_SMC_SCRATCH_KLA(12)

#define KERAUNOS_BOOT_STATE_MASK	0x0000000Fu	// HOST_BOOT_STATE
#define KERAUNOS_BOOT_BUNDLE_SEQ_MASK	0x000000F0u	// HOST_BUNDLE_SEQ
#define KERAUNOS_BOOT_BUNDLE_SEQ_SHIFT	4
#define KERAUNOS_BOOT_ERROR_CODE_MASK	0x0000FF00u	// BOOT_ERROR_CODE, valid in BOOT_ERROR
#define KERAUNOS_BOOT_ERROR_CODE_SHIFT	8

enum keraunos_host_boot_state {
	KERAUNOS_BOOT_RESET_OR_UNINITIALIZED	= 0,
	KERAUNOS_BOOT_WAIT_FOR_BUNDLE		= 1,	// window for HOST_BUNDLE_SEQ open, host-writable
	KERAUNOS_BOOT_BUNDLE_STAGED		= 2,	// written by the host, nothing else is
	KERAUNOS_BOOT_BUNDLE_CONSUMED		= 3,
	KERAUNOS_BOOT_SIP_READY			= 4,
	KERAUNOS_BOOT_ERROR			= 15,
};

struct keraunos_device {
	struct tenstorrent_device tt;

	// Kernel-owned TLB plumbing for NOC_READ/NOC_WRITE scalar SPA/KLA access.
	//
	// bar2 maps the uncached part of the BAR2 control plane (TLBSysIn0),
	// [0, KERAUNOS_BAR2_WC_OFFSET), through which the pre-programmed
	// SysIn0[2] bootstrap window lets us (re)program any TLBAppIn0 config
	// entry. bar2_wc maps the remaining top windows write-combining; see
	// keraunos_sysin0_window() for addressing either. bar0_kernel_tlb maps
	// the single 16MB BAR0 window belonging to our reserved AppIn0 entry;
	// we point that entry at the target SPA, then access it through this
	// window. A separate reserved SysIn0 entry provides a 16KB BAR2
	// aperture for KLA accesses.
	//
	// kernel_tlb_lock serializes the program-then-access sequence.
	u8 __iomem *bar0_kernel_tlb;
	u8 __iomem *bar2;
	u8 __iomem *bar2_wc;
	spinlock_t kernel_tlb_lock;

	bool boot_group_registered;
};

#define tt_dev_to_keraunos_dev(tt_dev) container_of((tt_dev), struct keraunos_device, tt)

#endif
