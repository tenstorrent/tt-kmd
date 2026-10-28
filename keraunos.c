// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

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
#define SYSIN0_WINDOW_SHIFT	KERAUNOS_SYSIN0_WINDOW_SHIFT
#define SYSIN0_WINDOW_SIZE	KERAUNOS_SYSIN0_WINDOW_SIZE
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

// iomem base of SysIn0 entry's 16KB BAR2 window. Windows at or above
// KERAUNOS_BAR2_WC_FIRST_ENTRY come from the write-combining mapping; the
// rest from the uncached one. NULL if the relevant mapping is absent.
static u8 __iomem *keraunos_sysin0_window(struct keraunos_device *kd, u32 entry)
{
	u32 off = entry << SYSIN0_WINDOW_SHIFT;

	if (entry >= KERAUNOS_BAR2_WC_FIRST_ENTRY)
		return kd->bar2_wc ? kd->bar2_wc + (off - KERAUNOS_BAR2_WC_OFFSET) : NULL;

	return kd->bar2 ? kd->bar2 + off : NULL;
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

// Write one inbound TLB config entry in 32-bit pieces: libttsim's Keraunos
// model rejects 64-bit accesses to TLB config. The high address word goes
// first so the entry never turns valid with a stale upper address.
static void keraunos_write_tlb_cfg(u8 __iomem *cfg, u64 aligned)
{
	writel(upper_32_bits(aligned), cfg + TLB_CFG_ADDR_OFFSET + 4);
	writel(lower_32_bits(aligned) | TLB_CFG_VALID, cfg + TLB_CFG_ADDR_OFFSET);
	writel(0, cfg + TLB_CFG_ATTR_OFFSET);
	writel(0, cfg + TLB_CFG_ATTR_OFFSET + 4);

	(void)readl(cfg + TLB_CFG_ADDR_OFFSET);
}

// Point an AppIn0 entry at addr's 16MB window. The caller owns the entry and
// must serialize programming against data-side accesses through that aperture.
static void keraunos_program_appin0(struct keraunos_device *kd, u32 entry, u64 addr)
{
	u64 aligned = addr & ~(u64)APPIN0_WINDOW_MASK;

	keraunos_write_tlb_cfg(kd->bar2 + keraunos_appin0_cfg_offset(entry), aligned);
}

// Point a SysIn0 entry at addr's 16KB KLA window. The caller owns the entry
// and must serialize programming against data-side accesses through it.
static void keraunos_program_sysin0(struct keraunos_device *kd, u32 entry, u64 addr)
{
	u64 aligned = addr & ~(u64)SYSIN0_WINDOW_MASK;

	keraunos_write_tlb_cfg(kd->bar2 + keraunos_sysin0_cfg_offset(entry), aligned);
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
	return keraunos_sysin0_window(kd, KERNEL_SYSIN0_ENTRY)
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

	// The UC and WC halves of BAR2 must not share a page (see keraunos.h),
	// and the kernel's scalar KLA aperture must live in the UC half.
	BUILD_BUG_ON(!IS_ALIGNED(KERAUNOS_BAR2_WC_OFFSET, PAGE_SIZE));
	BUILD_BUG_ON(KERNEL_SYSIN0_ENTRY >= KERAUNOS_BAR2_WC_FIRST_ENTRY);

	dev_info(&pdev->dev, "Keraunos init_device\n");

	spin_lock_init(&kd->kernel_tlb_lock);
	mutex_init(&kd->staging_mutex);

	// Map BAR2 (control plane) in two pieces: the uncached low part, through
	// which we reprogram TLB config via SysIn0, and the write-combining top
	// windows reserved for bulk kernel transfers.
	if (bar2_len >= KERAUNOS_BAR2_SIZE) {
		kd->bar2 = pci_iomap_range(pdev, 2, 0, KERAUNOS_BAR2_WC_OFFSET);
		if (!kd->bar2)
			dev_warn(&pdev->dev, "Keraunos: BAR2 iomap failed; NOC access disabled\n");

		kd->bar2_wc = pci_iomap_wc_range(pdev, 2, KERAUNOS_BAR2_WC_OFFSET, KERAUNOS_BAR2_WC_SIZE);
		if (!kd->bar2_wc)
			dev_warn(&pdev->dev, "Keraunos: BAR2 WC iomap failed; WC aperture unavailable\n");
	} else {
		dev_warn(&pdev->dev,
			 "Keraunos: BAR2 (0x%llx) smaller than SysIn0 (0x%x); NOC access disabled\n",
			 (unsigned long long)bar2_len, KERAUNOS_BAR2_SIZE);
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

// --- SMC host-visible boot state --------------------------------------------

// Read K-SMC scratch[12], the host-visible boot state word. Raw; callers
// decode with the KERAUNOS_BOOT_* masks. Fails if BAR2 is unmapped.
static int keraunos_read_boot_state(struct keraunos_device *kd, u32 *word)
{
	return keraunos_kla_read(&kd->tt, KERAUNOS_SMC_BOOT_STATE_KLA, word, sizeof(*word));
}

// Raw hex on purpose: the field layout is the boot spec's ABI, not ours, and
// it is still moving pre-silicon. Decode in userspace.
static ssize_t boot_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u32 word;
	int ret;

	ret = keraunos_read_boot_state(kd, &word);
	if (ret)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "0x%08x\n", word);
}
static DEVICE_ATTR_RO(boot_state);

static struct attribute *keraunos_boot_attrs[] = {
	&dev_attr_boot_state.attr,
	NULL,
};

// Unnamed: attributes appear directly in the device's sysfs directory.
static const struct attribute_group keraunos_boot_group = {
	.attrs = keraunos_boot_attrs,
};

// --- Host bundle staging (Grendel Boot Specification §6.6) ------------------
//
// TODO: this is the development ioctl's engine. Long term the same sequence
// runs from an async worker at probe, fed by request_firmware_nowait()
// (boot spec §8.4), walking HOST_BUNDLE_SEQ 0, 1, ... until SIP_READY.

#define STAGING_POLL_MS			10
#define STAGING_DEFAULT_TIMEOUT_MS	60000

static u32 boot_state_of(u32 word)
{
	return word & KERAUNOS_BOOT_STATE_MASK;
}

static u32 boot_seq_of(u32 word)
{
	return (word & KERAUNOS_BOOT_BUNDLE_SEQ_MASK) >> KERAUNOS_BOOT_BUNDLE_SEQ_SHIFT;
}

// Poll the KLA word at addr until (word & mask) == want, or with !equal until
// it differs. *word receives the last value read. Fails early if scratch[12]
// reports BOOT_ERROR.
static int keraunos_poll_kla(struct keraunos_device *kd, u64 addr, u32 mask, u32 want,
			     bool equal, unsigned long deadline, u32 *word)
{
	for (;;) {
		int ret = keraunos_kla_read(&kd->tt, addr, word, sizeof(*word));

		if (ret)
			return ret;
		if (addr == KERAUNOS_SMC_BOOT_STATE_KLA &&
		    boot_state_of(*word) == KERAUNOS_BOOT_ERROR)
			return -EIO;
		if (((*word & mask) == want) == equal)
			return 0;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		if (msleep_interruptible(STAGING_POLL_MS))
			return -EINTR;
	}
}

// Wait for posted writes through the WC aperture to reach the device: wmb()
// drains the CPU's combining buffers, and a config read, which is
// non-posted, cannot complete ahead of the writes before it. Config space
// rather than a BAR read because it is safe if the device is going away.
static int keraunos_flush_wc(struct keraunos_device *kd)
{
	u16 vendor_id;

	wmb();
	if (pci_read_config_word(kd->tt.pdev, PCI_VENDOR_ID, &vendor_id) || vendor_id == U16_MAX)
		return -ENODEV;

	return 0;
}

// Copy len bytes from userspace to KLA addr through the WC SysIn0 entries,
// one KERAUNOS_BAR2_WC_SIZE span at a time. Caller holds staging_mutex.
//
// TODO: DMA. CPU copies through a 64KB aperture are the simple first cut;
// this is the only function that needs to change for another transport.
static int keraunos_copy_to_kla(struct keraunos_device *kd, u64 addr,
				const u8 __user *src, size_t len, void *bounce)
{
	while (len) {
		u64 base = addr & ~(u64)SYSIN0_WINDOW_MASK;
		u32 off = addr - base;
		size_t chunk = min_t(size_t, len, KERAUNOS_BAR2_WC_SIZE - off);
		u32 i;
		int ret;

		if (copy_from_user(bounce, src, chunk))
			return -EFAULT;

		// The WC entries are adjacent in BAR2, so pointing them at
		// adjacent 16KB targets gives one contiguous 64KB aperture. Each
		// config write ends in a read-back, so the new targets are in
		// place before any data goes through them.
		for (i = 0; i < KERAUNOS_BAR2_WC_ENTRIES; i++)
			keraunos_program_sysin0(kd, KERAUNOS_BAR2_WC_FIRST_ENTRY + i,
						base + (u64)i * SYSIN0_WINDOW_SIZE);

		// 64-bit stores: SMC SRAM takes them, and fixed-width writes
		// avoid the byte/halfword tails memcpy_toio() may emit.
		__iowrite64_copy(kd->bar2_wc + off, bounce, chunk / 8);

		// Drain before the entries are retargeted or released.
		ret = keraunos_flush_wc(kd);
		if (ret)
			return ret;

		addr += chunk;
		src += chunk;
		len -= chunk;
	}

	return 0;
}

static int keraunos_stage_bundle_locked(struct keraunos_device *kd,
					struct tenstorrent_stage_bundle *args, void *bounce)
{
	struct device *dev = &kd->tt.pdev->dev;
	u32 timeout_ms = args->timeout_ms ?: STAGING_DEFAULT_TIMEOUT_MS;
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	u32 word;
	u32 val;
	int ret;

	// 1. Firmware opens the window.
	ret = keraunos_poll_kla(kd, KERAUNOS_SMC_BOOT_STATE_KLA, KERAUNOS_BOOT_STATE_MASK,
				KERAUNOS_BOOT_WAIT_FOR_BUNDLE, true, deadline, &word);
	args->boot_state = word;
	if (ret)
		return ret;

	// TODO: enforce once firmware sets HOST_BUNDLE_SEQ; today every bundle
	// arrives with seq 0.
	if (boot_seq_of(word) != args->seq)
		dev_info(dev, "Keraunos: staging bundle seq %u but firmware shows seq %u\n",
			 args->seq, boot_seq_of(word));

	// 2. Copy, flushed by keraunos_copy_to_kla() before it returns.
	ret = keraunos_copy_to_kla(kd, KERAUNOS_SMC_STAGING_KLA,
				   u64_to_user_ptr(args->data), args->size, bounce);
	if (ret)
		return ret;

	// 3. The one value the producer writes. HOST_BUNDLE_SEQ stays as
	// firmware set it; BOOT_ERROR_CODE is meaningless outside BOOT_ERROR.
	word &= ~(KERAUNOS_BOOT_STATE_MASK | KERAUNOS_BOOT_ERROR_CODE_MASK);
	word |= KERAUNOS_BOOT_BUNDLE_STAGED;
	ret = keraunos_kla_write(&kd->tt, KERAUNOS_SMC_BOOT_STATE_KLA, &word, sizeof(word));
	if (ret)
		return ret;
	args->boot_state = word;

	dev_info(dev, "Keraunos: staged %llu byte bundle (seq %u)\n", args->size, boot_seq_of(word));

	// 4. Development: no K-SEP to validate the bundle, so stand in for it.
	if (args->flags & TENSTORRENT_STAGE_BUNDLE_FAKE_SEP_VALIDATION) {
		ret = keraunos_poll_kla(kd, KERAUNOS_SMC_BUNDLE_VALIDATION_KLA,
					KERAUNOS_BUNDLE_READY_FOR_VALIDATION,
					KERAUNOS_BUNDLE_READY_FOR_VALIDATION, true, deadline, &val);
		if (ret)
			goto out;

		val |= KERAUNOS_BUNDLE_VALIDATED;
		ret = keraunos_kla_write(&kd->tt, KERAUNOS_SMC_BUNDLE_VALIDATION_KLA, &val, sizeof(val));
		if (ret)
			goto out;
	}

	// 5. Consumer takes the bundle. Wait for any move off BUNDLE_STAGED:
	// CONSUMED can be brief, and the next WAIT_FOR_BUNDLE or SIP_READY
	// also means the bundle was taken.
	// TODO: drop NO_WAIT once firmware writes BUNDLE_CONSUMED for every
	// bundle; K-BL1 currently jumps to MIS without doing so for BUN3.
	if (!(args->flags & TENSTORRENT_STAGE_BUNDLE_NO_WAIT)) {
		ret = keraunos_poll_kla(kd, KERAUNOS_SMC_BOOT_STATE_KLA, KERAUNOS_BOOT_STATE_MASK,
					KERAUNOS_BOOT_BUNDLE_STAGED, false, deadline, &word);
		args->boot_state = word;
		return ret;
	}

out:
	if (keraunos_kla_read(&kd->tt, KERAUNOS_SMC_BOOT_STATE_KLA, &word, sizeof(word)) == 0)
		args->boot_state = word;

	return ret;
}

static int keraunos_stage_bundle(struct tenstorrent_device *tt_dev, struct tenstorrent_stage_bundle *args)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	void *bounce;
	int ret;

	if (!kd->bar2 || !kd->bar2_wc)
		return -ENODEV;

	if (args->size == 0 || args->size % 8 || args->size > KERAUNOS_SMC_STAGING_SIZE)
		return -EINVAL;

	bounce = kvmalloc(KERAUNOS_BAR2_WC_SIZE, GFP_KERNEL);
	if (!bounce)
		return -ENOMEM;

	// TODO: this holds reset_rwsem (shared) across firmware waits of up to
	// timeout_ms, so a device reset waits for staging to finish.
	ret = mutex_lock_interruptible(&kd->staging_mutex);
	if (!ret) {
		ret = keraunos_stage_bundle_locked(kd, args, bounce);
		mutex_unlock(&kd->staging_mutex);
	}

	kvfree(bounce);
	return ret;
}

static bool keraunos_init_telemetry(struct tenstorrent_device *tt_dev)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);
	u32 word;
	int r;

	r = device_add_group(&tt_dev->dev, &keraunos_boot_group);
	if (r)
		dev_err(&tt_dev->pdev->dev, "Keraunos: boot state sysfs unavailable: %d\n", r);
	else
		kd->boot_group_registered = true;

	r = keraunos_read_boot_state(kd, &word);
	if (r)
		dev_info(&tt_dev->pdev->dev, "Keraunos: SMC boot state unreadable: %d\n", r);
	else
		dev_info(&tt_dev->pdev->dev, "Keraunos: SMC boot state 0x%08x (state %u, bundle seq %u)\n",
			 word, word & KERAUNOS_BOOT_STATE_MASK,
			 (word & KERAUNOS_BOOT_BUNDLE_SEQ_MASK) >> KERAUNOS_BOOT_BUNDLE_SEQ_SHIFT);

	return true;
}

static void keraunos_cleanup_telemetry(struct tenstorrent_device *tt_dev)
{
	struct keraunos_device *kd = tt_dev_to_keraunos_dev(tt_dev);

	if (kd->boot_group_registered) {
		device_remove_group(&tt_dev->dev, &keraunos_boot_group);
		kd->boot_group_registered = false;
	}
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
	if (kd->bar2_wc) {
		pci_iounmap(tt_dev->pdev, kd->bar2_wc);
		kd->bar2_wc = NULL;
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
	.cleanup_telemetry = keraunos_cleanup_telemetry,
	.noc_read = keraunos_noc_read,
	.noc_write = keraunos_noc_write,
	.kla_read = keraunos_kla_read,
	.kla_write = keraunos_kla_write,
	.stage_bundle = keraunos_stage_bundle,
};
