// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/spinlock.h>

#include "keraunos.h"
#include "module.h"

// --- TLBAppIn0 (BAR0) geometry ----------------------------------------------
// 256 entries, each a 16MB window: BAR0 offset = (entry << 24) | within.
#define APPIN0_WINDOW_SHIFT	24
#define APPIN0_WINDOW_SIZE	(1u << APPIN0_WINDOW_SHIFT)	// 16MB
#define APPIN0_WINDOW_MASK	(APPIN0_WINDOW_SIZE - 1)
#define APPIN0_ENTRIES_PER_BANK	64

// The inbound TLB target-address field is 52 bits wide.
#define KERAUNOS_TLB_ADDRESS_SIZE	(1ULL << 52)

// Entries 0-163 are statically programmed by emulation/FW bring-up
// (Tensix/Mimir/Keraunos config). 164-255 are genuinely free in every code
// path, so claim the first free one for the kernel's scalar access window.
#define KERNEL_APPIN0_ENTRY	164

// --- TLBSysIn0 (BAR2) bootstrap ---------------------------------------------
// BAR2 offset = (sysin0_entry << 14) | within_16KB_window. Bring-up programs
// SysIn0[2] -> 0x1804_4000, the base of the four APPIN0 config banks, so a
// write into that window reaches any AppIn0 entry's config registers.
#define SYSIN0_WINDOW_SHIFT	14
#define SYSIN0_WINDOW_SIZE	(1u << SYSIN0_WINDOW_SHIFT)	// 16KB
#define SYSIN0_WINDOW_MASK	(SYSIN0_WINDOW_SIZE - 1)
#define SYSIN0_TLBCFG_ENTRY	1
#define SYSIN0_APPIN0_ENTRY	2
#define KERNEL_SYSIN0_ENTRY	13
#define TLBCFG_SYSIN0_OFFSET	0x3000
#define APPIN0_BANK_STRIDE	0x1000	// distance between APPIN00..APPIN03 banks
#define TLB_CFG_ENTRY_SIZE	0x40	// 64 bytes per TLB config entry
#define TLB_CFG_ADDR_OFFSET	0x00	// qword: target_addr | valid
#define TLB_CFG_ATTR_OFFSET	0x20	// qword: attributes (0 = normal)
#define TLB_CFG_VALID		0x1

// BAR2 offset of the config qword for AppIn0 entry, reached via SysIn0[2].
static u32 keraunos_appin0_cfg_offset(u32 entry)
{
	u32 bank = entry / APPIN0_ENTRIES_PER_BANK;
	u32 idx = entry % APPIN0_ENTRIES_PER_BANK;

	return (SYSIN0_APPIN0_ENTRY << SYSIN0_WINDOW_SHIFT)
	       + bank * APPIN0_BANK_STRIDE
	       + idx * TLB_CFG_ENTRY_SIZE;
}

// SysIn0[1] exposes the first 16KB of TLBCFG, including all SysIn0 entries.
static u32 keraunos_sysin0_cfg_offset(u32 entry)
{
	return (SYSIN0_TLBCFG_ENTRY << SYSIN0_WINDOW_SHIFT)
	       + TLBCFG_SYSIN0_OFFSET + entry * TLB_CFG_ENTRY_SIZE;
}

// Keraunos uses a flat SPA rather than NOC coordinates. Validate both the
// generic NOC request fields and the Keraunos address range.
static int keraunos_check_access(struct keraunos_device *kd, u32 x, u32 y, u64 addr, u32 width, int noc)
{
	if (x != 0 || y != 0 || noc != 0)
		return -EINVAL;

	if (width != 1 && width != 2 && width != 4 && width != 8)
		return -EINVAL;

	if (addr & (width - 1))
		return -EINVAL;

	if (addr > KERAUNOS_TLB_ADDRESS_SIZE - width)
		return -EINVAL;

	if (!kd->bar0_kernel_tlb || !kd->bar2)
		return -ENODEV;		// kernel TLB window unavailable

	return 0;
}

static int keraunos_check_kla_access(struct keraunos_device *kd, u64 addr, u32 width)
{
	if (width != 1 && width != 2 && width != 4 && width != 8)
		return -EINVAL;

	if (addr & (width - 1))
		return -EINVAL;

	if (addr > KERAUNOS_TLB_ADDRESS_SIZE - width)
		return -EINVAL;

	if (!kd->bar2)
		return -ENODEV;

	return 0;
}

// Point an AppIn0 entry at addr's 16MB window. The caller owns the entry and
// must serialize programming against data-side accesses through that aperture.
static void keraunos_program_appin0(struct keraunos_device *kd, u32 entry, u64 addr)
{
	u64 aligned = addr & ~(u64)APPIN0_WINDOW_MASK;
	u32 cfg_off = keraunos_appin0_cfg_offset(entry);

	writeq(aligned | TLB_CFG_VALID, kd->bar2 + cfg_off + TLB_CFG_ADDR_OFFSET);
	writeq(0, kd->bar2 + cfg_off + TLB_CFG_ATTR_OFFSET);

	(void)readq(kd->bar2 + cfg_off + TLB_CFG_ADDR_OFFSET);
}

// Point a SysIn0 entry at addr's 16KB KLA window. The caller owns the entry
// and must serialize programming against data-side accesses through it.
static void keraunos_program_sysin0(struct keraunos_device *kd, u32 entry, u64 addr)
{
	u64 aligned = addr & ~(u64)SYSIN0_WINDOW_MASK;
	u32 cfg_off = keraunos_sysin0_cfg_offset(entry);

	writeq(aligned | TLB_CFG_VALID, kd->bar2 + cfg_off + TLB_CFG_ADDR_OFFSET);
	writeq(0, kd->bar2 + cfg_off + TLB_CFG_ATTR_OFFSET);

	(void)readq(kd->bar2 + cfg_off + TLB_CFG_ADDR_OFFSET);
}

// Point the reserved kernel entry at addr and return its BAR0 iomem address.
// Caller must hold kernel_tlb_lock.
static u8 __iomem *keraunos_program_window(struct keraunos_device *kd, u64 addr)
{
	keraunos_program_appin0(kd, KERNEL_APPIN0_ENTRY, addr);
	return kd->bar0_kernel_tlb + (addr & APPIN0_WINDOW_MASK);
}

// Point the reserved kernel SysIn0 entry at addr and return its BAR2 address.
// Caller must hold kernel_tlb_lock.
static u8 __iomem *keraunos_program_kla_window(struct keraunos_device *kd, u64 addr)
{
	keraunos_program_sysin0(kd, KERNEL_SYSIN0_ENTRY, addr);
	return kd->bar2 + (KERNEL_SYSIN0_ENTRY << SYSIN0_WINDOW_SHIFT)
	       + (addr & SYSIN0_WINDOW_MASK);
}

// Kernel-mediated scalar read of an arbitrary system physical address.
static int keraunos_noc_read(struct tenstorrent_device *tt_dev, u32 x, u32 y,
			     u64 addr, void *value, u32 width, int noc)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u8 __iomem *win;
	int ret = keraunos_check_access(kd, x, y, addr, width, noc);

	if (ret)
		return ret;

	spin_lock(&kd->kernel_tlb_lock);
	win = keraunos_program_window(kd, addr);

	switch (width) {
	case 1: *(u8 *)value = ioread8(win); break;
	case 2: *(u16 *)value = ioread16(win); break;
	case 4: *(u32 *)value = ioread32(win); break;
	case 8: *(u64 *)value = ioread64(win); break;
	}

	spin_unlock(&kd->kernel_tlb_lock);

	return 0;
}

// Kernel-mediated scalar write to an arbitrary system physical address.
static int keraunos_noc_write(struct tenstorrent_device *tt_dev, u32 x, u32 y,
			      u64 addr, const void *value, u32 width, int noc)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u8 __iomem *win;
	int ret = keraunos_check_access(kd, x, y, addr, width, noc);

	if (ret)
		return ret;

	spin_lock(&kd->kernel_tlb_lock);
	win = keraunos_program_window(kd, addr);

	switch (width) {
	case 1: iowrite8(*(const u8 *)value, win); break;
	case 2: iowrite16(*(const u16 *)value, win); break;
	case 4: iowrite32(*(const u32 *)value, win); break;
	case 8: iowrite64(*(const u64 *)value, win); break;
	}

	spin_unlock(&kd->kernel_tlb_lock);

	return 0;
}

static int keraunos_kla_read(struct tenstorrent_device *tt_dev, u64 addr, void *value, u32 width)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u8 __iomem *win;
	int ret = keraunos_check_kla_access(kd, addr, width);

	if (ret)
		return ret;

	spin_lock(&kd->kernel_tlb_lock);
	win = keraunos_program_kla_window(kd, addr);

	switch (width) {
	case 1:
		*(u8 *)value = ioread8(win);
		break;
	case 2:
		*(u16 *)value = ioread16(win);
		break;
	case 4:
		*(u32 *)value = ioread32(win);
		break;
	case 8:
		*(u64 *)value = ioread64(win);
		break;
	}

	spin_unlock(&kd->kernel_tlb_lock);

	return 0;
}

static int keraunos_kla_write(struct tenstorrent_device *tt_dev, u64 addr,
			      const void *value, u32 width)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u8 __iomem *win;
	int ret = keraunos_check_kla_access(kd, addr, width);

	if (ret)
		return ret;

	spin_lock(&kd->kernel_tlb_lock);
	win = keraunos_program_kla_window(kd, addr);

	switch (width) {
	case 1:
		iowrite8(*(const u8 *)value, win);
		break;
	case 2:
		iowrite16(*(const u16 *)value, win);
		break;
	case 4:
		iowrite32(*(const u32 *)value, win);
		break;
	case 8:
		iowrite64(*(const u64 *)value, win);
		break;
	}

	spin_unlock(&kd->kernel_tlb_lock);

	return 0;
}

static bool keraunos_init(struct tenstorrent_device *tt_dev)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;
	resource_size_t bar0_len = pci_resource_len(pdev, 0);
	resource_size_t bar2_len = pci_resource_len(pdev, 2);
	unsigned long window_off = (unsigned long)KERNEL_APPIN0_ENTRY << APPIN0_WINDOW_SHIFT;
	unsigned long sysin0_window_end =
		(unsigned long)(KERNEL_SYSIN0_ENTRY + 1) << SYSIN0_WINDOW_SHIFT;

	dev_info(&pdev->dev, "Keraunos init_device\n");

	spin_lock_init(&kd->kernel_tlb_lock);

	// Map BAR2 (control plane) so we can reprogram TLB config via SysIn0.
	if (sysin0_window_end <= bar2_len) {
		kd->bar2 = pci_iomap(pdev, 2, 0);
		if (!kd->bar2)
			dev_warn(&pdev->dev, "Keraunos: BAR2 iomap failed; NOC access disabled\n");
	} else {
		dev_warn(&pdev->dev,
			 "Keraunos: BAR2 (0x%llx) too small for SysIn0 entry %u window; NOC access disabled\n",
			 (unsigned long long)bar2_len, KERNEL_SYSIN0_ENTRY);
	}

	// Map just the 16MB BAR0 window owned by our reserved AppIn0 entry.
	if (window_off + APPIN0_WINDOW_SIZE <= bar0_len) {
		kd->bar0_kernel_tlb = pci_iomap_range(pdev, 0, window_off, APPIN0_WINDOW_SIZE);
		if (!kd->bar0_kernel_tlb)
			dev_warn(&pdev->dev, "Keraunos: BAR0 kernel TLB window iomap failed; NOC access disabled\n");
	} else {
		dev_warn(&pdev->dev,
			 "Keraunos: BAR0 (0x%llx) too small for AppIn0 entry %u window; NOC access disabled\n",
			 (unsigned long long)bar0_len, KERNEL_APPIN0_ENTRY);
	}

	// Reserve the entry so the TLB allocator never hands it to userspace.
	set_bit(KERNEL_APPIN0_ENTRY, tt_dev->tlbs);

	return true;
}

static bool keraunos_init_hardware(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Keraunos init_hardware (stub)\n");
	return true;
}

static bool keraunos_init_telemetry(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Keraunos init_telemetry (stub)\n");
	return true;
}

static void keraunos_save_reset_state(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Keraunos save_reset_state (stub)\n");
}

static void keraunos_cleanup_hardware(struct tenstorrent_device *tt_dev)
{
	dev_info(&tt_dev->pdev->dev, "Keraunos cleanup_hardware (stub)\n");
}

static void keraunos_cleanup(struct tenstorrent_device *tt_dev)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);

	dev_info(&tt_dev->pdev->dev, "Keraunos cleanup_device\n");

	if (kd->bar0_kernel_tlb) {
		pci_iounmap(tt_dev->pdev, kd->bar0_kernel_tlb);
		kd->bar0_kernel_tlb = NULL;
	}
	if (kd->bar2) {
		pci_iounmap(tt_dev->pdev, kd->bar2);
		kd->bar2 = NULL;
	}
}

static int keraunos_set_power_state(struct tenstorrent_device *tt_dev,
				    struct tenstorrent_power_state *power_state)
{
	dev_info(&tt_dev->pdev->dev, "Keraunos set_power_state (stub)\n");
	return 0;
}

struct tenstorrent_device_class keraunos_class = {
	.name = "Keraunos",
	.instance_size = sizeof(struct keraunos_device),
	.coherent_dma_bits = 44,
	.streaming_dma_bits = 44,
	.init_device = keraunos_init,
	.init_hardware = keraunos_init_hardware,
	.save_reset_state = keraunos_save_reset_state,
	.cleanup_hardware = keraunos_cleanup_hardware,
	.cleanup_device = keraunos_cleanup,
	.set_power_state = keraunos_set_power_state,
	.init_telemetry = keraunos_init_telemetry,
	.noc_read = keraunos_noc_read,
	.noc_write = keraunos_noc_write,
	.kla_read = keraunos_kla_read,
	.kla_write = keraunos_kla_write,
};
