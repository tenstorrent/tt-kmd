/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SET_POWER_STATE aggregation and open/close power behaviour (batch B4).
 * B1 provides stubs.
 */
#ifndef TT_POWER_H
#define TT_POWER_H

#include <linux/types.h>
#include <linux/compiler.h>

struct tenstorrent_device;
struct chardev_private;

struct tt_power_dev_state {
};

struct tt_power_fd_state {
};

int tt_power_device_init(struct tenstorrent_device *dev);
void tt_power_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_power_fd_init(struct chardev_private *priv);

/*
 * Called at the end of open(), after the fd has been admitted and added to
 * dev->open_fds (so it is visible to aggregation over that list), inside a
 * detach SRCU section with the device attached. @o_append: open() had
 * O_APPEND (start at power state 0, no aggregation); otherwise legacy
 * high-power + aggregate.
 * A non-zero return fails the open() (power_fd_release is NOT called then).
 */
int tt_power_fd_open(struct chardev_private *priv, bool o_append);

/*
 * Called last among the release hooks; the fd is still on dev->open_fds,
 * so drop this fd's contribution before re-aggregating.
 */
void tt_power_fd_release(struct chardev_private *priv);

/*
 * PCI remove, for every fd still open, after dev->detached is set and with
 * dev->open_lock held: release everything that uses the device (DMA
 * mappings, iATU regions, exports' device side); the PCI device is gone once
 * remove returns. May run concurrently with this fd's release hook: both
 * take the subsystem's per-fd lock; release afterwards only frees software
 * state (and must not call the DMA API once detached).
 */
void tt_power_fd_detach(struct chardev_private *priv);

long tt_power_ioctl_set_state(struct chardev_private *priv, void __user *arg);

#endif
