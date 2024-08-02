// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TENSTORRENT_MEMORY_H_INCLUDED
#define TENSTORRENT_MEMORY_H_INCLUDED

#include <linux/compiler.h>

#define MAX_DMA_BUF_SIZE_LOG2 28

struct chardev_private;
struct tenstorrent_query_mappings;
struct tenstorrent_allocate_dma_buf;
struct tenstorrent_free_dma_buf;
struct tenstorrent_pin_pages;
struct tenstorrent_map_peer_bar;
struct vm_area_struct;

long ioctl_query_mappings(struct chardev_private *priv,
			  struct tenstorrent_query_mappings __user *arg);
long ioctl_allocate_dma_buf(struct chardev_private *priv,
			    struct tenstorrent_allocate_dma_buf __user *arg);
long ioctl_free_dma_buf(struct chardev_private *priv,
			struct tenstorrent_free_dma_buf __user *arg);
long ioctl_pin_pages(struct chardev_private *priv,
		     struct tenstorrent_pin_pages __user *arg);
long ioctl_map_peer_bar(struct chardev_private *priv,
			struct tenstorrent_map_peer_bar __user *arg);

int tenstorrent_mmap(struct chardev_private *priv, struct vm_area_struct *vma);
void tenstorrent_memory_cleanup(struct chardev_private *priv);

#endif
