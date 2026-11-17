/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * NOC_READ / NOC_WRITE / SET_NOC_CLEANUP ioctls (batch B3). B1 provides stubs.
 * The kernel NOC access these build on is tt_noc_read()/tt_noc_write() in
 * blackhole.h.
 */
#ifndef TT_NOC_H
#define TT_NOC_H

#include <linux/types.h>
#include <linux/compiler.h>

struct tenstorrent_device;
struct chardev_private;

struct tt_noc_dev_state {
};

struct tt_noc_fd_state {
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
