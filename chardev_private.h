// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TENSTORRENT_CHARDEV_PRIVATE_H_INCLUDED
#define TENSTORRENT_CHARDEV_PRIVATE_H_INCLUDED

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>

#include "ioctl.h"

struct file;
struct tenstorrent_device;

#define DMABUF_HASHTABLE_BITS 4
struct dmabuf {
	struct hlist_node hash_chain;

	void *ptr;	// kernel address for dma buffer
	dma_addr_t phys;
	u64 size;	// always a multiple of PAGE_SIZE
	u8 index;
};

// This is our device-private data assocated with each open character device fd.
// Accessed through struct file::private_data.
struct chardev_private {
	struct tenstorrent_device *device;
	struct mutex mutex;
	DECLARE_HASHTABLE(dmabufs, DMABUF_HASHTABLE_BITS);	// keyed on by dmabuf.index, chained on struct dmabuf.hash_chain
	struct list_head pinnings;	// struct pinned_page_range.list
	struct list_head peer_mappings; // struct peer_resource_mapping.list

	DECLARE_BITMAP(resource_lock, TENSTORRENT_RESOURCE_LOCK_COUNT);

	struct list_head node;	// for struct tenstorrent_device::open_fds
};

struct chardev_private *get_tenstorrent_priv(struct file *f);

#endif
