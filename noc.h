/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NOC_READ / NOC_WRITE / SET_NOC_CLEANUP ioctls (batch B3).
 * The kernel NOC access these build on is tt_noc_read()/tt_noc_write() in
 * blackhole.h.
 */
#ifndef TT_NOC_H
#define TT_NOC_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>

struct tenstorrent_device;
struct chardev_private;

/*
 * NOC addresses user space must not reach through the kernel window: the
 * PCIe controller's DBI (config/iATU registers) and SII (low-level controller
 * and PHY configuration) ranges, on any tile. Writes there can wedge the
 * PCIe tile and with it all host access to the card. The NIU registers just
 * above SII (0xFFFF_FFFF_FF00_0000+, e.g. NOC_NODE_ID) stay reachable.
 * User TLB windows may not be pointed into DBI (CONFIGURE_TLB); SII is
 * covered by the 4 GiB window that also holds the NIU registers, so only the
 * NOC_READ/NOC_WRITE/SET_NOC_CLEANUP paths refuse it.
 */
#define TT_NOC_PCIE_DBI_START		0xF800000000000000ULL
#define TT_NOC_PCIE_DBI_END		0xFC00000000000000ULL
#define TT_NOC_PCIE_SII_START		0xFFFFFFFFF0000000ULL
#define TT_NOC_PCIE_SII_END		0xFFFFFFFFFF000000ULL

static inline bool tt_noc_addr_in_dbi(u64 addr)
{
	return addr >= TT_NOC_PCIE_DBI_START && addr < TT_NOC_PCIE_DBI_END;
}

/* For accesses of at most 8 naturally aligned bytes (the range ends are
 * 8-byte aligned, so checking the first byte suffices). */
static inline bool tt_noc_addr_forbidden(u64 addr)
{
	return tt_noc_addr_in_dbi(addr) ||
	       (addr >= TT_NOC_PCIE_SII_START && addr < TT_NOC_PCIE_SII_END);
}

struct tt_noc_dev_state {
};

/* The fd's SET_NOC_CLEANUP action, performed by tt_noc_fd_release(). */
struct tt_noc_cleanup {
	bool enabled;
	u8 noc;
	u8 x;
	u8 y;
	u64 addr;
	u32 data;
};

struct tt_noc_fd_state {
	spinlock_t lock;		/* protects cleanup */
	struct tt_noc_cleanup cleanup;
};

int tt_noc_device_init(struct tenstorrent_device *dev);
void tt_noc_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_noc_fd_init(struct chardev_private *priv);

int tt_noc_fd_open(struct chardev_private *priv);
/* Runs first among the release hooks: perform the registered cleanup write. */
void tt_noc_fd_release(struct chardev_private *priv);

/*
 * PCI remove, for every fd still open, after dev->detached is set and with
 * dev->open_lock held: release everything that uses the device (DMA
 * mappings, iATU regions, exports' device side); the PCI device is gone once
 * remove returns. May run concurrently with this fd's release hook: both
 * take the subsystem's per-fd lock; release afterwards only frees software
 * state (and must not call the DMA API once detached).
 */
void tt_noc_fd_detach(struct chardev_private *priv);

long tt_noc_ioctl_read(struct chardev_private *priv, void __user *arg);
long tt_noc_ioctl_write(struct chardev_private *priv, void __user *arg);
long tt_noc_ioctl_set_cleanup(struct chardev_private *priv, void __user *arg);

#endif
