// SPDX-License-Identifier: GPL-2.0-only
/*
 * NOC_READ / NOC_WRITE: single 1/2/4/8-byte NOC accesses through the
 * kernel-reserved TLB window (tt_noc_read()/tt_noc_write(), blackhole.c).
 * SET_NOC_CLEANUP: a 32-bit NOC write performed when the fd is released.
 *
 * The ioctls run inside the detach section of tt_cdev_ioctl(), so after PCI
 * remove they fail with -ENODEV before reaching this file.
 */
#include <linux/errno.h>
#include <linux/uaccess.h>

#include "device.h"
#include "chardev.h"
#include "blackhole.h"
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
	spin_lock_init(&priv->noc.lock);
	priv->noc.cleanup.enabled = false;
}

int tt_noc_fd_open(struct chardev_private *priv)
{
	return 0;
}

/* Called inside an SRCU detach read section (see tt_cdev_release()): if the
 * device is not detached now, the kernel window stays usable until we return.
 * Not interruptible: a dying process (fatal signal pending) must still get
 * its cleanup write. */
void tt_noc_fd_release(struct chardev_private *priv)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_noc_cleanup c;
	int ret;

	spin_lock(&priv->noc.lock);
	c = priv->noc.cleanup;
	priv->noc.cleanup.enabled = false;
	spin_unlock(&priv->noc.lock);

	if (!c.enabled || READ_ONCE(dev->detached))
		return;

	ret = tt_noc_write32(dev, c.noc, c.x, c.y, c.addr, c.data);
	if (ret)
		dev_dbg(&dev->pdev->dev, "NOC cleanup write failed (%d)\n", ret);
}

/* Nothing device-side to tear down: the cleanup write is simply not done
 * once the device is gone (release checks dev->detached). */
void tt_noc_fd_detach(struct chardev_private *priv)
{
}

/* Common validation of struct tenstorrent_noc_io; copies it into @io. */
static int tt_noc_io_get(struct tenstorrent_noc_io *io, void __user *arg)
{
	struct tenstorrent_noc_io __user *uio = arg;
	u32 argsz;

	if (get_user(argsz, &uio->argsz))
		return -EFAULT;
	if (argsz != sizeof(*io))
		return -EINVAL;
	if (copy_from_user(io, uio, sizeof(*io)))
		return -EFAULT;

	if (io->flags || io->reserved0[0] || io->reserved0[1])
		return -EINVAL;
	if (io->x > BH_NOC_MAX_COORD || io->y > BH_NOC_MAX_COORD || io->noc > 1)
		return -EINVAL;
	if (io->width != 1 && io->width != 2 && io->width != 4 && io->width != 8)
		return -EINVAL;
	/* Natural alignment. Blackhole NOC addresses are 64-bit: no range
	 * limit, but the PCIe controller's register ranges are off limits. */
	if (io->addr & (io->width - 1))
		return -EINVAL;
	if (tt_noc_addr_forbidden(io->addr))
		return -EPERM;
	return 0;
}

long tt_noc_ioctl_read(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_noc_io __user *uio = arg;
	struct tenstorrent_noc_io io;
	u64 value = 0;
	int ret;

	ret = tt_noc_io_get(&io, arg);
	if (ret)
		return ret;

	/* Little-endian host: the low @width bytes of value, zero-extended. */
	ret = tt_noc_read_interruptible(priv->dev, io.noc, io.x, io.y, io.addr, &value, io.width);
	if (ret)
		return ret;

	if (put_user(value, &uio->value))
		return -EFAULT;
	return 0;
}

long tt_noc_ioctl_write(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_noc_io io;
	int ret;

	ret = tt_noc_io_get(&io, arg);
	if (ret)
		return ret;

	return tt_noc_write_interruptible(priv->dev, io.noc, io.x, io.y, io.addr, &io.value, io.width);
}

long tt_noc_ioctl_set_cleanup(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_set_noc_cleanup __user *uarg = arg;
	struct tenstorrent_set_noc_cleanup in;
	struct tt_noc_cleanup c = {};
	u32 argsz;

	if (get_user(argsz, &uarg->argsz))
		return -EFAULT;
	if (argsz != sizeof(in))
		return -EINVAL;
	if (copy_from_user(&in, uarg, sizeof(in)))
		return -EFAULT;

	if (in.flags || in.reserved0 || in.enabled > 1)
		return -EINVAL;
	if (in.enabled) {
		if (in.noc > 1 || in.x > BH_NOC_MAX_COORD || in.y > BH_NOC_MAX_COORD ||
		    (in.addr & 3))
			return -EINVAL;
		if (tt_noc_addr_forbidden(in.addr))
			return -EPERM;
		c.enabled = true;
		c.noc = in.noc;
		c.x = in.x;
		c.y = in.y;
		c.addr = in.addr;
		c.data = lower_32_bits(in.data);
	}

	spin_lock(&priv->noc.lock);
	priv->noc.cleanup = c;
	spin_unlock(&priv->noc.lock);
	return 0;
}
