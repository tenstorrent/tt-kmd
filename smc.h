/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Firmware (SMC) message queue: SMC_MSG POST/POLL/ABANDON and kernel-internal
 * messages (batch B4). B1 provides stubs.
 */
#ifndef TT_SMC_H
#define TT_SMC_H

#include <linux/types.h>
#include <linux/compiler.h>

struct tenstorrent_device;
struct chardev_private;

struct tt_smc_dev_state {
};

struct tt_smc_fd_state {
};

int tt_smc_device_init(struct tenstorrent_device *dev);
void tt_smc_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_smc_fd_init(struct chardev_private *priv);

int tt_smc_fd_open(struct chardev_private *priv);
/* Abandon any outstanding message of this fd. */
void tt_smc_fd_release(struct chardev_private *priv);

/*
 * PCI remove, for every fd still open, after dev->detached is set and with
 * dev->open_lock held: release everything that uses the device (DMA
 * mappings, iATU regions, exports' device side); the PCI device is gone once
 * remove returns. May run concurrently with this fd's release hook: both
 * take the subsystem's per-fd lock; release afterwards only frees software
 * state (and must not call the DMA API once detached).
 */
void tt_smc_fd_detach(struct chardev_private *priv);

long tt_smc_ioctl_msg(struct chardev_private *priv, void __user *arg);

#endif
