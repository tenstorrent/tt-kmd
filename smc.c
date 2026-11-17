// SPDX-License-Identifier: GPL-2.0-only
/* Firmware message queue (SMC_MSG): stubs until batch B4. */
#include <linux/errno.h>

#include "device.h"
#include "smc.h"

int tt_smc_device_init(struct tenstorrent_device *dev)
{
	return 0;
}

void tt_smc_device_fini(struct tenstorrent_device *dev)
{
}

void tt_smc_fd_init(struct chardev_private *priv)
{
}

int tt_smc_fd_open(struct chardev_private *priv)
{
	return 0;
}

void tt_smc_fd_release(struct chardev_private *priv)
{
}

void tt_smc_fd_detach(struct chardev_private *priv)
{
}

long tt_smc_ioctl_msg(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}
