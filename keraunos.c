// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/pci.h>
#include <linux/io.h>
#include <linux/mutex.h>

#include "keraunos.h"
#include "module.h"

// --- TLBAppIn0 (BAR0) geometry ----------------------------------------------
// 256 entries, each a 16MB window: BAR0 offset = (entry << 24) | within.
#define APPIN0_WINDOW_SHIFT	24
#define APPIN0_WINDOW_SIZE	(1u << APPIN0_WINDOW_SHIFT)	// 16MB
#define APPIN0_WINDOW_MASK	(APPIN0_WINDOW_SIZE - 1)
#define APPIN0_ENTRIES_PER_BANK	64

// AppIn0 covers system addresses below the GDDR/AppIn1 base. Anything at or
// above this needs BAR4/AppIn1, which read32 does not handle yet.
#define APPIN0_SPA_LIMIT	0x1000000000000ULL		// 0x1_0000_0000_0000

// Entries 0-163 are statically programmed by emulation/FW bring-up
// (Tensix/Mimir/Keraunos config). 164-255 are genuinely free in every code
// path, so claim the first free one for the kernel's read32 window.
#define KERNEL_APPIN0_ENTRY	164

// --- TLBSysIn0 (BAR2) bootstrap ---------------------------------------------
// BAR2 offset = (sysin0_entry << 14) | within_16KB_window. Bring-up programs
// SysIn0[2] -> 0x1804_4000, the base of the four APPIN0 config banks, so a
// write into that window reaches any AppIn0 entry's config registers.
#define SYSIN0_WINDOW_SHIFT	14
#define SYSIN0_APPIN0_ENTRY	2
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

// Validate a 32-bit access target. Only the AppIn0 range is supported for now,
// and the kernel TLB window must have mapped successfully at probe time.
static int keraunos_check_access(struct keraunos_device *kd, u64 addr)
{
	if (addr & 0x3)
		return -EINVAL;		// 32-bit accesses must be 4-byte aligned

	if (addr >= APPIN0_SPA_LIMIT)
		return -EINVAL;		// only AppIn0 range is supported for now

	if (!kd->bar0_kernel_tlb || !kd->bar2)
		return -ENODEV;		// kernel TLB window unavailable

	return 0;
}

// Point the reserved AppIn0 entry at addr's 16MB window and return the BAR0
// iomem address of the target word. Caller must hold kernel_tlb_mutex.
static u8 __iomem *keraunos_program_window(struct keraunos_device *kd, u64 addr)
{
	u64 aligned = addr & ~(u64)APPIN0_WINDOW_MASK;
	u32 within = addr & APPIN0_WINDOW_MASK;
	u32 cfg_off = keraunos_appin0_cfg_offset(KERNEL_APPIN0_ENTRY);

	writeq(aligned | TLB_CFG_VALID, kd->bar2 + cfg_off + TLB_CFG_ADDR_OFFSET);
	writeq(0, kd->bar2 + cfg_off + TLB_CFG_ATTR_OFFSET);

	// Read the config qword back to force the (posted) BAR2 writes to land
	// before we issue the data-side access through BAR0.
	(void)readq(kd->bar2 + cfg_off + TLB_CFG_ADDR_OFFSET);

	return kd->bar0_kernel_tlb + within;
}

// Kernel-mediated 32-bit read of an arbitrary AppIn0-range system address.
// Serialized against itself / write32 by kernel_tlb_mutex.
static int keraunos_read32(struct tenstorrent_device *tt_dev, u64 addr, u32 *value)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	int ret = keraunos_check_access(kd, addr);

	if (ret)
		return ret;

	mutex_lock(&kd->kernel_tlb_mutex);
	*value = ioread32(keraunos_program_window(kd, addr));
	mutex_unlock(&kd->kernel_tlb_mutex);

	return 0;
}

// Kernel-mediated 32-bit write to an arbitrary AppIn0-range system address.
static int keraunos_write32(struct tenstorrent_device *tt_dev, u64 addr, u32 value)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u8 __iomem *win;
	int ret = keraunos_check_access(kd, addr);

	if (ret)
		return ret;

	mutex_lock(&kd->kernel_tlb_mutex);
	win = keraunos_program_window(kd, addr);
	iowrite32(value, win);
	(void)ioread32(win);	// flush the posted write to the device
	mutex_unlock(&kd->kernel_tlb_mutex);

	return 0;
}

static bool keraunos_init(struct tenstorrent_device *tt_dev)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;
	resource_size_t bar0_len = pci_resource_len(pdev, 0);
	unsigned long window_off = (unsigned long)KERNEL_APPIN0_ENTRY << APPIN0_WINDOW_SHIFT;

	dev_info(&pdev->dev, "Keraunos init_device\n");

	mutex_init(&kd->kernel_tlb_mutex);

	// Map BAR2 (control plane) so we can reprogram TLB config via SysIn0.
	kd->bar2 = pci_iomap(pdev, 2, 0);
	if (!kd->bar2)
		dev_warn(&pdev->dev, "Keraunos: BAR2 iomap failed; READ32 disabled\n");

	// Map just the 16MB BAR0 window owned by our reserved AppIn0 entry.
	if (window_off + APPIN0_WINDOW_SIZE <= bar0_len) {
		kd->bar0_kernel_tlb = pci_iomap_range(pdev, 0, window_off, APPIN0_WINDOW_SIZE);
		if (!kd->bar0_kernel_tlb)
			dev_warn(&pdev->dev, "Keraunos: BAR0 kernel TLB window iomap failed; READ32 disabled\n");
	} else {
		dev_warn(&pdev->dev,
			 "Keraunos: BAR0 (0x%llx) too small for AppIn0 entry %u window; READ32 disabled\n",
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
	.dma_address_bits = 64,
	.init_device = keraunos_init,
	.init_hardware = keraunos_init_hardware,
	.save_reset_state = keraunos_save_reset_state,
	.cleanup_hardware = keraunos_cleanup_hardware,
	.cleanup_device = keraunos_cleanup,
	.set_power_state = keraunos_set_power_state,
	.init_telemetry = keraunos_init_telemetry,
	.read32 = keraunos_read32,
	.write32 = keraunos_write32,
};
