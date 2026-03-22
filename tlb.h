// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_TLB_H_INCLUDED
#define TTDRIVER_TLB_H_INCLUDED

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mutex.h>

struct tenstorrent_device;
struct tenstorrent_noc_tlb_config;

struct tlb_descriptor {
	int bar;
	unsigned long size;
	unsigned long bar_offset;
};

int tenstorrent_device_allocate_tlb(struct tenstorrent_device *tt_dev,
				    size_t size);
int tenstorrent_device_free_tlb(struct tenstorrent_device *tt_dev,
				unsigned int id);
int tenstorrent_device_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				     struct tenstorrent_noc_tlb_config *config);

/*
 * TLB pool for kernel-mediated I/O.
 *
 * Ownership model: the caller holds pool->lock for the entire acquire + MMIO +
 * completion sequence. The tlb_pool_window returned by acquire is a transient
 * view — it has no back-reference to the pool entry and becomes invalid when
 * the lock is released (a subsequent acquire may evict and reprogram the
 * underlying TLB).
 *
 * TODO: Pin/unpin model for concurrent I/O. Acquire pins an entry and releases
 * the lock; the caller does MMIO without the lock; release unpins. Eviction
 * skips pinned entries. This moves the lock off the MMIO critical path.
 */

#define TT_IO_TLB_POOL_SIZE 4

struct tlb_pool_entry {
	struct list_head lru_node;
	int tlb_index;
	bool valid;
	u32 x, y;
	u64 aligned_addr;
	u8 __iomem *io_base;
};

struct tlb_pool {
	struct mutex lock;
	struct list_head lru_list;
	struct tlb_pool_entry entries[TT_IO_TLB_POOL_SIZE];
	unsigned int count;
	struct tenstorrent_device *tt_dev;
	u64 window_size;
};

struct tlb_pool_window {
	u8 __iomem *io_addr;
	size_t remaining;
};

int tlb_pool_init(struct tlb_pool *pool, struct tenstorrent_device *tt_dev, const int *tlb_indices, unsigned int count);
void tlb_pool_destroy(struct tlb_pool *pool);
int tlb_pool_acquire(struct tlb_pool *pool, u32 x, u32 y, u64 addr, struct tlb_pool_window *window);

#endif // TTDRIVER_TLB_H_INCLUDED
