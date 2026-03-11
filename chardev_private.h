// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TENSTORRENT_CHARDEV_PRIVATE_H_INCLUDED
#define TENSTORRENT_CHARDEV_PRIVATE_H_INCLUDED

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/sched.h>
#include <linux/refcount.h>

#include "ioctl.h"
#include "msgqueue.h"

struct file;
struct tenstorrent_device;

enum arc_msg_state {
	ARC_MSG_NONE,
	ARC_MSG_QUEUED,
	ARC_MSG_IN_FW,
	ARC_MSG_IN_FW_ABANDONED,
	ARC_MSG_RESPONSE_READY,
};

struct chardev_msg {
	enum arc_msg_state state;
	struct arc_msg request;
	struct arc_msg response;
	u32 queue_index;
	bool reset_occurred;
};

enum bar_mapping_type { BAR_MAPPING_UC, BAR_MAPPING_WC };
enum tenstorrent_vma_type { TT_VMA_BAR, TT_VMA_TLB };

struct tenstorrent_mmap_vma {
	struct list_head list;
	struct vm_area_struct *vma;
	enum tenstorrent_vma_type type;
	enum bar_mapping_type cache_mode;

	union {
		// BAR mapping metadata
		struct {
			unsigned int bar_index;
			u64 offset;
			u64 size;
		} bar;

		// TLB mapping metadata
		struct {
			int id;
		} tlb;
	};
};

#define DMABUF_HASHTABLE_BITS 4
struct dmabuf {
	struct hlist_node hash_chain;

	void *ptr;	// kernel address for dma buffer
	dma_addr_t phys;
	u64 size;	// always a multiple of PAGE_SIZE
	u8 index;
	int outbound_iatu_region;
};

// This is our device-private data assocated with each open character device fd.
// Accessed through struct file::private_data.
struct chardev_private {
	struct tenstorrent_device *device;
	struct mutex mutex;
	DECLARE_HASHTABLE(dmabufs, DMABUF_HASHTABLE_BITS);	// keyed on by dmabuf.index, chained on struct dmabuf.hash_chain
	struct list_head pinnings;	// struct pinned_page_range.list
	struct list_head peer_mappings; // struct peer_resource_mapping.list

	struct list_head vma_list;	// struct tenstorrent_mmap_vma.list
	struct mutex vma_lock;		// Protects vma_list

	struct pid *pid;
	char comm[TASK_COMM_LEN];

	DECLARE_BITMAP(resource_lock, TENSTORRENT_RESOURCE_LOCK_COUNT);

	struct list_head open_fd;	// node in struct tenstorrent_device.open_fds_list

	DECLARE_BITMAP(tlbs, TENSTORRENT_MAX_INBOUND_TLBS);	// TLBs owned by this fd

	struct tenstorrent_set_noc_cleanup noc_cleanup; // NOC write on release action
	struct tenstorrent_power_state power_state; // Power state for this fd

	struct chardev_msg msg;			// ARC message slot
	struct list_head msg_queue_node;	// node in tenstorrent_device.msg_queue

	long open_reset_gen; // Reset generation at open time
};

struct chardev_private *get_tenstorrent_priv(struct file *f);

#endif
