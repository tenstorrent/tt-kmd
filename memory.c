// SPDX-License-Identifier: GPL-2.0-only
/* DMA buffers, pinned pages, outbound iATU: stubs until batch B2. */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/seq_file.h>

#include "device.h"
#include "memory.h"

int tt_memory_device_init(struct tenstorrent_device *dev)
{
	return 0;
}

void tt_memory_device_fini(struct tenstorrent_device *dev)
{
}

void tt_memory_fd_init(struct chardev_private *priv)
{
}

int tt_memory_fd_open(struct chardev_private *priv)
{
	return 0;
}

void tt_memory_fd_release(struct chardev_private *priv)
{
}

void tt_memory_fd_detach(struct chardev_private *priv)
{
}

long tt_memory_ioctl_allocate_dma_buf(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_memory_ioctl_free_dma_buf(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_memory_ioctl_pin_pages(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

long tt_memory_ioctl_unpin_pages(struct chardev_private *priv, void __user *arg)
{
	return -ENOTTY;
}

int tt_memory_mmap(struct chardev_private *priv, struct vm_area_struct *vma)
{
	return -EINVAL;
}

void tt_memory_debugfs_show(struct seq_file *s, struct chardev_private *priv)
{
}
