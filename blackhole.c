// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/types.h>

#include "blackhole.h"
#include "pcie.h"
#include "module.h"

#define MAX_MRRS 4096

#define TLB_2M_WINDOW_COUNT 202
#define TLB_2M_SHIFT 21
#define TLB_2M_REG_SIZE 12
#define TLB_2M_WINDOW_SIZE (1 << TLB_2M_SHIFT)
#define TLB_2M_WINDOW_MASK (TLB_2M_WINDOW_SIZE - 1)

#define TLB_REGS_START 0x1FC00000   // BAR0
#define TLB_REGS_LEN 0x00001000     // Covers all TLB registers

#define KERNEL_TLB_INDEX (TLB_2M_WINDOW_COUNT - 1)	// Last 2M window is ours
#define KERNEL_TLB_START (KERNEL_TLB_INDEX * TLB_2M_WINDOW_SIZE)
#define KERNEL_TLB_LEN TLB_2M_WINDOW_SIZE

struct TLB_2M_REG {
	union {
		struct {
			u32 low32;
			u32 mid32;
			u32 high32;
		};
		// packed to make y_start straddle mid32 and high32
		struct __attribute__((packed)) {
			u64 address : 43;
			u64 x_end : 6;
			u64 y_end : 6;
			u64 x_start : 6;
			u64 y_start : 6;
			u64 noc : 2;
			u64 multicast : 1;
			u64 ordering : 2;
			u64 linked : 1;
			u64 use_static_vc : 1;
			u64 stream_header : 1;
			u64 static_vc : 3;
			u64 reserved : 18;
		};
	};
};
static_assert(sizeof(struct TLB_2M_REG) == TLB_2M_REG_SIZE, "TLB_2M_REG size mismatch");

static u64 program_tlb(struct blackhole_device *bh, u32 x, u32 y, u64 addr) {
	struct TLB_2M_REG conf = {0};
	u8 __iomem *regs = bh->tlb_regs + (KERNEL_TLB_INDEX * TLB_2M_REG_SIZE);

	conf.address = addr >> TLB_2M_SHIFT;
	conf.x_end = x;
	conf.y_end = y;
	conf.ordering = 1;	// strict

	iowrite32(conf.low32, regs + 0);
	iowrite32(conf.mid32, regs + 4);
	iowrite32(conf.high32, regs + 8);

	return addr & TLB_2M_WINDOW_MASK;
}

static u32 noc_read32(struct blackhole_device *bh, u32 x, u32 y, u64 addr) {
	u64 offset;
	u32 val;

	mutex_lock(&bh->kernel_tlb_mutex);

	offset = program_tlb(bh, x, y, addr);
	val = ioread32(bh->kernel_tlb + offset);

	mutex_unlock(&bh->kernel_tlb_mutex);

	return val;
}

static bool blackhole_init(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	bh->tlb_regs = pci_iomap_range(bh->tt.pdev, 0, TLB_REGS_START, TLB_REGS_LEN);
	bh->kernel_tlb = pci_iomap_range(bh->tt.pdev, 0, KERNEL_TLB_START, KERNEL_TLB_LEN);

	if (!bh->tlb_regs || !bh->kernel_tlb) {
		if (bh->tlb_regs)
			pci_iounmap(bh->tt.pdev, bh->tlb_regs);

		if (bh->kernel_tlb)
			pci_iounmap(bh->tt.pdev, bh->kernel_tlb);
		return false;
	}

	return true;
}

static bool blackhole_init_hardware(struct tenstorrent_device *tt_dev) {
	struct pci_dev *pdev = tt_dev->pdev;
	pcie_set_readrq(pdev, MAX_MRRS);
	return true;
}

static bool blackhole_post_hardware_init(struct tenstorrent_device *tt_dev) {
	return true;
}

static void blackhole_cleanup_hardware(struct tenstorrent_device *tt_dev) {
}

static void blackhole_cleanup(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (bh->tlb_regs)
		pci_iounmap(tt_dev->pdev, bh->tlb_regs);
	if (bh->kernel_tlb)
		pci_iounmap(tt_dev->pdev, bh->kernel_tlb);
}

struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	.dma_address_bits = 58,
	.init_device = blackhole_init,
	.init_hardware = blackhole_init_hardware,
	.post_hardware_init = blackhole_post_hardware_init,
	.cleanup_hardware = blackhole_cleanup_hardware,
	.cleanup_device = blackhole_cleanup,
};
