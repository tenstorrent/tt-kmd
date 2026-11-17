// SPDX-License-Identifier: GPL-2.0-only
/* NOC_READ / NOC_WRITE / SET_NOC_CLEANUP: stubs until batch B3. */
#include <linux/errno.h>

#include "device.h"
#include "noc.h"

int tt_noc_device_init(struct tenstorrent_device *dev)
{
	return 0;
}

void tt_noc_device_fini(struct tenstorrent_device *dev)
{
}

void tt_noc_fd_init(struct chardev_private *priv)
{
}

int tt_noc_fd_open(struct chardev_private *priv)
{
	return 0;
}

void tt_noc_fd_release(struct chardev_private *priv)
{
}

void tt_noc_fd_detach(struct chardev_private *priv)
{
}

long tt_noc_ioctl_read(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_noc_ioctl_write(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_noc_ioctl_set_cleanup(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}
