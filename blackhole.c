// SPDX-License-Identifier: GPL-2.0-only
/*
 * Blackhole hardware access: TLB window geometry and config encoding, the
 * kernel-reserved TLB window used for kernel NOC reads/writes, NOC
 * translation detection and firmware readiness.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/delay.h>
#include <linux/jiffies.h>

#include "device.h"
#include "blackhole.h"

static unsigned int fw_ready_timeout_ms = 15000;
module_param(fw_ready_timeout_ms, uint, 0444);
MODULE_PARM_DESC(fw_ready_timeout_ms,
		 "Maximum time probe waits for the firmware message queue to become ready (ms)");

/* ---- TLB windows ---- */

unsigned int bh_tlb_window_count(struct tenstorrent_device *dev)
{
	return BH_TLB_2M_COUNT + dev->num_4g_windows;
}

u64 bh_tlb_window_size(unsigned int index)
{
	return index < BH_TLB_4G_FIRST ? BH_TLB_2M_SIZE : BH_TLB_4G_SIZE;
}

unsigned int bh_tlb_window_bar(unsigned int index)
{
	return index < BH_TLB_4G_FIRST ? 0 : 4;
}

u64 bh_tlb_window_bar_offset(unsigned int index)
{
	if (index < BH_TLB_4G_FIRST)
		return (u64)index * BH_TLB_2M_SIZE;
	return (u64)(index - BH_TLB_4G_FIRST) * BH_TLB_4G_SIZE;
}

/* OR @value (which fits in @width bits) into the 128-bit v[] at bit @shift. */
static void bh_set_field(u64 v[2], unsigned int shift, unsigned int width, u64 value)
{
	if (shift >= 64) {
		v[1] |= value << (shift - 64);
		return;
	}
	v[0] |= value << shift;
	if (shift + width > 64)
		v[1] |= value >> (64 - shift);
}

int bh_tlb_encode(unsigned int index, const struct tenstorrent_noc_tlb_config *cfg,
		  u32 regs[3])
{
	bool is_4g = index >= BH_TLB_4G_FIRST;
	u64 size = bh_tlb_window_size(index);
	unsigned int addr_bits = is_4g ? 32 : 43;
	unsigned int addr_shift = is_4g ? 32 : 21;
	unsigned int b = addr_bits;	/* first bit after local_offset */
	u64 v[2] = { 0, 0 };

	if (index >= BH_TLB_4G_FIRST + BH_TLB_4G_MAX)
		return -EINVAL;
	if (cfg->addr & (size - 1))
		return -EINVAL;
	if (cfg->x_end > BH_NOC_MAX_COORD || cfg->y_end > BH_NOC_MAX_COORD ||
	    cfg->x_start > BH_NOC_MAX_COORD || cfg->y_start > BH_NOC_MAX_COORD)
		return -EINVAL;
	if (cfg->noc > 1 || cfg->mcast > 1 || cfg->linked > 1 || cfg->static_vc > 1 ||
	    cfg->ordering > 3)
		return -EINVAL;

	bh_set_field(v, 0, addr_bits, cfg->addr >> addr_shift);
	bh_set_field(v, b + 0, 6, cfg->x_end);
	bh_set_field(v, b + 6, 6, cfg->y_end);
	bh_set_field(v, b + 12, 6, cfg->x_start);
	bh_set_field(v, b + 18, 6, cfg->y_start);
	bh_set_field(v, b + 24, 1, cfg->noc);
	/* b + 25 reserved */
	bh_set_field(v, b + 26, 1, cfg->mcast);
	bh_set_field(v, b + 27, 2, cfg->ordering);
	bh_set_field(v, b + 29, 1, cfg->linked);
	bh_set_field(v, b + 30, 1, cfg->static_vc);
	/* b + 31 reserved, b + 32 static_vc_buddy = 0 */
	if (cfg->static_vc && cfg->mcast)
		bh_set_field(v, b + 33, 2, 2);	/* static_vc_class: multicast */

	regs[0] = lower_32_bits(v[0]);
	regs[1] = upper_32_bits(v[0]);
	regs[2] = lower_32_bits(v[1]);
	return 0;
}

static void bh_tlb_write_regs(struct tenstorrent_device *dev, unsigned int index,
			      const u32 regs[3])
{
	void __iomem *reg = dev->bar0_regs +
		(BH_TLB_CONFIG_BAR0_OFFSET - BH_REG_REGION_BAR0_OFFSET) +
		index * BH_TLB_CONFIG_STRIDE;

	iowrite32(regs[0], reg + 0);
	iowrite32(regs[1], reg + 4);
	iowrite32(regs[2], reg + 8);
	/* Read back: the posted config writes complete before any later access
	 * through the window. */
	ioread32(reg + 8);
}

int bh_tlb_configure(struct tenstorrent_device *dev, unsigned int index,
		     const struct tenstorrent_noc_tlb_config *cfg)
{
	u32 regs[3];
	int ret;

	if (index == BH_KERNEL_TLB_INDEX || index >= bh_tlb_window_count(dev))
		return -EINVAL;

	ret = bh_tlb_encode(index, cfg, regs);
	if (ret)
		return ret;

	/* Plain rectangular multicast only: clear any strided config left by
	 * a previous user of windows 0-31. */
	if (index < BH_TLB_STRIDED_COUNT)
		iowrite32(0, dev->bar0_regs +
			  (BH_TLB_STRIDED_BAR0_OFFSET - BH_REG_REGION_BAR0_OFFSET) + 4 * index);

	bh_tlb_write_regs(dev, index, regs);
	return 0;
}

/* ---- Kernel window ---- */

static int noc_check_target(u8 noc, u16 x, u16 y)
{
	if (noc > 1 || x > BH_NOC_MAX_COORD || y > BH_NOC_MAX_COORD)
		return -EINVAL;
	return 0;
}

/*
 * Point the kernel window at the 2 MiB-aligned block containing @addr and
 * return the MMIO address of @addr. The config is rewritten every time: the
 * window registers are reachable through BAR0 and nothing guarantees they
 * still hold what we last wrote. Caller holds kernel_tlb_mutex and has
 * checked mmio_gone.
 */
static void __iomem *bh_kernel_tlb_target(struct tenstorrent_device *dev, u8 noc,
					  u16 x, u16 y, u64 addr)
{
	struct tenstorrent_noc_tlb_config cfg = {
		.addr = addr & ~(BH_TLB_2M_SIZE - 1),
		.x_end = x,
		.y_end = y,
		.noc = noc,
		.ordering = BH_TLB_ORDERING_STRICT,
	};
	u32 regs[3];

	lockdep_assert_held(&dev->kernel_tlb_mutex);

	/* Cannot fail: target and alignment were validated by the caller. */
	bh_tlb_encode(BH_KERNEL_TLB_INDEX, &cfg, regs);
	bh_tlb_write_regs(dev, BH_KERNEL_TLB_INDEX, regs);

	return dev->kernel_tlb + (addr & (BH_TLB_2M_SIZE - 1));
}

static int noc_check_access(u8 noc, u16 x, u16 y, u64 addr, int width)
{
	if (width != 1 && width != 2 && width != 4 && width != 8)
		return -EINVAL;
	if (addr & (width - 1))
		return -EINVAL;
	return noc_check_target(noc, x, y);
}

/* Take the kernel window; @intr: give up with -ERESTARTSYS on a signal. */
static int bh_kernel_tlb_lock(struct tenstorrent_device *dev, bool intr)
{
	if (!intr) {
		mutex_lock(&dev->kernel_tlb_mutex);
		return 0;
	}
	return mutex_lock_interruptible(&dev->kernel_tlb_mutex) ? -ERESTARTSYS : 0;
}

static int __tt_noc_read(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
			 void *val, int width, bool intr)
{
	void __iomem *p;
	int ret;

	ret = noc_check_access(noc, x, y, addr, width);
	if (ret)
		return ret;

	ret = bh_kernel_tlb_lock(dev, intr);
	if (ret)
		return ret;
	if (dev->mmio_gone) {
		ret = -ENODEV;
		goto out;
	}

	p = bh_kernel_tlb_target(dev, noc, x, y, addr);
	switch (width) {
	case 1:
		*(u8 *)val = ioread8(p);
		break;
	case 2:
		*(u16 *)val = ioread16(p);
		break;
	case 4:
		*(u32 *)val = ioread32(p);
		break;
	case 8:
		*(u64 *)val = readq(p);
		break;
	}
out:
	mutex_unlock(&dev->kernel_tlb_mutex);
	return ret;
}

static int __tt_noc_write(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
			  const void *val, int width, bool intr)
{
	void __iomem *p;
	int ret;

	ret = noc_check_access(noc, x, y, addr, width);
	if (ret)
		return ret;

	ret = bh_kernel_tlb_lock(dev, intr);
	if (ret)
		return ret;
	if (dev->mmio_gone) {
		ret = -ENODEV;
		goto out;
	}

	p = bh_kernel_tlb_target(dev, noc, x, y, addr);
	switch (width) {
	case 1:
		iowrite8(*(const u8 *)val, p);
		break;
	case 2:
		iowrite16(*(const u16 *)val, p);
		break;
	case 4:
		iowrite32(*(const u32 *)val, p);
		break;
	case 8:
		writeq(*(const u64 *)val, p);
		break;
	}
out:
	mutex_unlock(&dev->kernel_tlb_mutex);
	return ret;
}

int tt_noc_read(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		void *val, int width)
{
	return __tt_noc_read(dev, noc, x, y, addr, val, width, false);
}

int tt_noc_write(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		 const void *val, int width)
{
	return __tt_noc_write(dev, noc, x, y, addr, val, width, false);
}

int tt_noc_read_interruptible(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y,
			      u64 addr, void *val, int width)
{
	return __tt_noc_read(dev, noc, x, y, addr, val, width, true);
}

int tt_noc_write_interruptible(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y,
			       u64 addr, const void *val, int width)
{
	return __tt_noc_write(dev, noc, x, y, addr, val, width, true);
}

static int noc_check_block(u8 noc, u16 x, u16 y, u64 addr, size_t len)
{
	if ((addr | len) & 3)
		return -EINVAL;
	if (addr + len < addr)
		return -EINVAL;
	return noc_check_target(noc, x, y);
}

int tt_noc_read_block(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		      void *buf, size_t len)
{
	u32 *out = buf;
	int ret;

	ret = noc_check_block(noc, x, y, addr, len);
	if (ret)
		return ret;

	mutex_lock(&dev->kernel_tlb_mutex);
	if (dev->mmio_gone) {
		ret = -ENODEV;
		goto out;
	}

	while (len) {
		u64 chunk = BH_TLB_2M_SIZE - (addr & (BH_TLB_2M_SIZE - 1));
		void __iomem *p = bh_kernel_tlb_target(dev, noc, x, y, addr);
		size_t i, n;

		if (chunk > len)
			chunk = len;
		n = chunk / 4;
		for (i = 0; i < n; i++) {
			*out++ = ioread32(p + 4 * i);
			if ((i & 1023) == 1023)
				cond_resched();
		}
		addr += chunk;
		len -= chunk;
	}
out:
	mutex_unlock(&dev->kernel_tlb_mutex);
	return ret;
}

int tt_noc_write_block(struct tenstorrent_device *dev, u8 noc, u16 x, u16 y, u64 addr,
		       const void *buf, size_t len)
{
	const u32 *in = buf;
	int ret;

	ret = noc_check_block(noc, x, y, addr, len);
	if (ret)
		return ret;

	mutex_lock(&dev->kernel_tlb_mutex);
	if (dev->mmio_gone) {
		ret = -ENODEV;
		goto out;
	}

	while (len) {
		u64 chunk = BH_TLB_2M_SIZE - (addr & (BH_TLB_2M_SIZE - 1));
		void __iomem *p = bh_kernel_tlb_target(dev, noc, x, y, addr);
		size_t i, n;

		if (chunk > len)
			chunk = len;
		n = chunk / 4;
		for (i = 0; i < n; i++) {
			iowrite32(*in++, p + 4 * i);
			if ((i & 1023) == 1023)
				cond_resched();
		}
		addr += chunk;
		len -= chunk;
	}
out:
	mutex_unlock(&dev->kernel_tlb_mutex);
	return ret;
}

/* ---- Probe-time init ---- */

bool bh_noc_translation_enabled(struct tenstorrent_device *dev)
{
	u32 niu_cfg = ioread32(dev->bar0_regs +
			       (BH_NIU0_CFG_BAR0_OFFSET - BH_REG_REGION_BAR0_OFFSET) +
			       BH_NIU_CFG_0);

	return niu_cfg != 0xffffffff && (niu_cfg & BH_NIU_CFG_0_NOC_ID_TRANSLATE_EN);
}

static int bh_check_arc(struct tenstorrent_device *dev)
{
	struct device *d = &dev->pdev->dev;
	u32 node_id;
	int ret;

	ret = tt_noc_read32(dev, 0, BH_ARC_X, BH_ARC_Y, BH_ARC_NOC_NODE_ID, &node_id);
	if (ret)
		return ret;

	if ((node_id & 0x3f) != BH_ARC_X || ((node_id >> 6) & 0x3f) != BH_ARC_Y) {
		dev_err(d, "ARC not reachable over the kernel TLB window: NOC_NODE_ID %#x\n",
			node_id);
		return -EIO;
	}
	return 0;
}

static void bh_wait_fw_ready(struct tenstorrent_device *dev)
{
	struct device *d = &dev->pdev->dev;
	unsigned long deadline = jiffies + msecs_to_jiffies(fw_ready_timeout_ms);
	u32 status = 0;

	for (;;) {
		if (!tt_noc_read32(dev, 0, BH_ARC_X, BH_ARC_Y, BH_ARC_BOOT_STATUS0, &status) &&
		    status != 0xffffffff && (status & BH_BOOT_STATUS0_MSG_QUEUE_READY)) {
			dev->fw_ready = true;
			break;
		}
		if (time_after(jiffies, deadline))
			break;
		msleep(20);
	}

	dev->boot_status0 = status;
	if (!dev->fw_ready)
		dev_warn(d, "firmware not ready after %u ms (BOOT_STATUS0 %#x)\n",
			 fw_ready_timeout_ms, status);
}

int bh_init(struct tenstorrent_device *dev)
{
	struct device *d = &dev->pdev->dev;

	dev->noc_translation_en = bh_noc_translation_enabled(dev);

	if (bh_check_arc(dev))
		return 0;	/* logged; keep the device reachable for recovery */

	bh_wait_fw_ready(dev);

	dev_info(d, "Blackhole: NOC translation %s, firmware %s (BOOT_STATUS0 %#x)\n",
		 dev->noc_translation_en ? "on" : "off",
		 dev->fw_ready ? "ready" : "not ready", dev->boot_status0);
	return 0;
}
