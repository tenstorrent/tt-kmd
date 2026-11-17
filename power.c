// SPDX-License-Identifier: GPL-2.0-only
/* SET_POWER_STATE and open/close power policy: stubs until batch B4. */
#include <linux/errno.h>

#include "device.h"
#include "power.h"

int tt_power_device_init(struct tenstorrent_device *dev)
{
	return 0;
}

void tt_power_device_fini(struct tenstorrent_device *dev)
{
}

void tt_power_fd_init(struct chardev_private *priv)
{
}

int tt_power_fd_open(struct chardev_private *priv, bool o_append)
{
	return 0;
}

void tt_power_fd_release(struct chardev_private *priv)
{
}

void tt_power_fd_detach(struct chardev_private *priv)
{
}

long tt_power_ioctl_set_state(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}
