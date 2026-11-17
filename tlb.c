// SPDX-License-Identifier: GPL-2.0-only
/* User TLB windows: stubs until batch B3. */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/seq_file.h>

#include "device.h"
#include "tlb.h"

int tt_tlb_device_init(struct tenstorrent_device *dev)
{
	return 0;
}

void tt_tlb_device_fini(struct tenstorrent_device *dev)
{
}

void tt_tlb_fd_init(struct chardev_private *priv)
{
}

int tt_tlb_fd_open(struct chardev_private *priv)
{
	return 0;
}

void tt_tlb_fd_release(struct chardev_private *priv)
{
}

void tt_tlb_fd_detach(struct chardev_private *priv)
{
}

long tt_tlb_ioctl_allocate(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_tlb_ioctl_free(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_tlb_ioctl_configure(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_tlb_ioctl_export_dmabuf(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

int tt_tlb_mmap(struct chardev_private *priv, struct vm_area_struct *vma)
{
	return -EINVAL;
}

void tt_tlb_debugfs_show(struct seq_file *s, struct chardev_private *priv)
{
}

int tt_tlb_reset_check(struct tenstorrent_device *dev)
{
	return 0;
}
