/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Blackhole hardware: BAR layout, TLB windows and their config registers,
 * kernel NOC access through the kernel-reserved TLB window, firmware
 * readiness and NOC translation detection.
 */
#ifndef TT_BLACKHOLE_H
#define TT_BLACKHOLE_H

#include <linux/types.h>
#include <linux/bits.h>

#include "ioctl.h"

struct tenstorrent_device;

/* BARs. */
#define BH_BAR0_SIZE			0x20000000ULL	/* 512 MiB */
#define BH_BAR2_SIZE			0x00100000ULL	/* 1 MiB */

/* Host-to-NOC TLB windows: 202 x 2 MiB in BAR0, then up to 8 x 4 GiB in BAR4. */
#define BH_TLB_2M_COUNT			202
#define BH_TLB_2M_SIZE			(1ULL << 21)
#define BH_TLB_4G_FIRST			202
#define BH_TLB_4G_MAX			8
#define BH_TLB_4G_SIZE			(1ULL << 32)
#define BH_KERNEL_TLB_INDEX		201	/* last 2 MiB window, BAR0 0x1920_0000 */

/* Register region of BAR0 mapped by the kernel: TLB config + NIU config. */
#define BH_REG_REGION_BAR0_OFFSET	0x1FC00000ULL
#define BH_REG_REGION_SIZE		0x00200000ULL
#define BH_TLB_CONFIG_BAR0_OFFSET	0x1FC00000ULL	/* windows[i] at + 12 * i */
#define BH_TLB_CONFIG_STRIDE		12
#define BH_TLB_STRIDED_BAR0_OFFSET	0x1FC009D8ULL	/* strided[j] at + 4 * j */
#define BH_TLB_STRIDED_COUNT		32		/* windows 0-31 */
#define BH_NIU0_CFG_BAR0_OFFSET		0x1FD04000ULL	/* PCIe tile NIU #0 config */
#define BH_NIU_CFG_0			0x100
#define BH_NIU_CFG_0_NOC_ID_TRANSLATE_EN	BIT(14)

/* TLB ordering modes. */
#define BH_TLB_ORDERING_DEFAULT		0
#define BH_TLB_ORDERING_STRICT		1
#define BH_TLB_ORDERING_POSTED		2
#define BH_TLB_ORDERING_COUNTED		3

/* ARC tile (NoC0 coordinates; row 0 is never translated). */
#define BH_ARC_X			8
#define BH_ARC_Y			0
#define BH_ARC_NOC_NODE_ID		0x80050044ULL
#define BH_ARC_RESET_UNIT		0x80030000ULL
#define BH_ARC_SCRATCH_RAM(n)		(BH_ARC_RESET_UNIT + 0x400 + 4 * (n))
#define BH_ARC_BOOT_STATUS0		BH_ARC_SCRATCH_RAM(2)	/* 0x80030408 */
#define BH_BOOT_STATUS0_MSG_QUEUE_READY	BIT(0)

#define BH_NOC_MAX_COORD		63

/* ---- TLB windows (used by tlb.c for user windows) ---- */

/* Total windows on this device: 202 + num_4g_windows. */
unsigned int bh_tlb_window_count(struct tenstorrent_device *dev);
/* Size of window @index (2 MiB or 4 GiB). */
u64 bh_tlb_window_size(unsigned int index);
/* PCI BAR index (0 or 4) and offset within it of window @index. */
unsigned int bh_tlb_window_bar(unsigned int index);
u64 bh_tlb_window_bar_offset(unsigned int index);

/*
 * Encode @cfg for window @index into the 96-bit register value (regs[0] =
 * low32, regs[1] = mid32, regs[2] = high32). Rejects (-EINVAL) values that do
 * not fit: addr not aligned to the window size, x/y > 63, noc > 1, mcast,
 * linked or static_vc > 1, ordering > 3. Reserved bits are written as zero;
 * static_vc_class is 0b10 for multicast, 0b00 otherwise. Does not look at
 * cfg->reserved*: the caller validates the ABI.
 */
int bh_tlb_encode(unsigned int index, const struct tenstorrent_noc_tlb_config *cfg,
		  u32 regs[3]);

/*
 * Encode and program window @index (for windows 0-31 also zero its strided
 * multicast register). Refuses BH_KERNEL_TLB_INDEX and indices beyond
 * bh_tlb_window_count(). Caller must hold the device attached
 * (tt_dev_enter()) or be in probe/remove before MMIO teardown.
 */
int bh_tlb_configure(struct tenstorrent_device *dev, unsigned int index,
		     const struct tenstorrent_noc_tlb_config *cfg);

/* ---- Kernel NOC access through window BH_KERNEL_TLB_INDEX ----
 *
 * Serialized by dev->kernel_tlb_mutex, so callable from process context
 * only (may sleep). @width is 1, 2, 4 or 8; @addr must be aligned to @width;
 * @noc 0 or 1; @x, @y <= 63. Values are little-endian native integers of
 * @width bytes at @val. Returns 0, -EINVAL or -ENODEV (MMIO torn down).
 * The hardware gives no error indication: a read of an unreachable tile may
 * return all ones.
 */
int tt_noc_read(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		void *val, int width);
int tt_noc_write(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		 const void *val, int width);
/* Same, for user-initiated accesses: waiting for the window is interruptible
 * (-ERESTARTSYS on a signal, nothing accessed). */
int tt_noc_read_interruptible(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y,
			      u64 addr, void *val, int width);
int tt_noc_write_interruptible(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y,
			       u64 addr, const void *val, int width);

static inline int tt_noc_read32(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y,
				u64 addr, u32 *val)
{
	return tt_noc_read(dev, noc, x, y, addr, val, 4);
}

static inline int tt_noc_write32(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y,
				 u64 addr, u32 val)
{
	return tt_noc_write(dev, noc, x, y, addr, &val, 4);
}

/* Block transfers in 32-bit accesses; @addr and @len multiples of 4. Holds
 * the kernel window for the whole transfer (reschedules every 4 KiB). */
int tt_noc_read_block(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		      void *buf, size_t len);
int tt_noc_write_block(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		       const void *buf, size_t len);

/* ---- Probe-time hardware bring-up ---- */

/* NIU_CFG_0 bit 14 of the PCIe tile's NIU #0, read through BAR0. */
bool bh_noc_translation_enabled(struct tenstorrent_device *dev);

/*
 * Probe-time init: detect NOC translation, check the ARC is reachable over
 * the kernel window (NOC_NODE_ID), wait (bounded by module param
 * fw_ready_timeout_ms) for BOOT_STATUS0.msg_queue_ready. Problems are logged
 * and recorded (dev->fw_ready) but do not fail the probe, so the device stays
 * reachable for recovery.
 */
int bh_init(struct tenstorrent_device *dev);

#endif
