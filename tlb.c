// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sched/signal.h>

#include "tlb.h"
#include "device.h"

int tenstorrent_device_allocate_tlb(struct tenstorrent_device *tt_dev,
				    size_t size)
{
	const struct tenstorrent_device_class *dev_class = tt_dev->dev_class;
	unsigned long id = 0;
	unsigned long offset = 0; // Offset into the TLB bitmap.
	unsigned long n = 0; // Total number of TLBs of the requested size.
	int kind;

	if (dev_class->tlb_kinds == 0)
		return -EINVAL;

	for (kind = 0; kind < dev_class->tlb_kinds; ++kind) {
		long tlb_count = tt_dev->tlb_counts[kind];
		long tlb_size = dev_class->tlb_sizes[kind];

		if (size == tlb_size) {
			n = tlb_count;
			break;
		}

		offset += tlb_count;
	}

	if (n == 0)
		return -EINVAL;

	// Find a free TLB and atomically claim it.
	for (;;) {
		id = find_next_zero_bit(tt_dev->tlbs, offset + n, offset);

		if (id == offset + n)
			return -ENOMEM;

		if (!test_and_set_bit(id, tt_dev->tlbs))
			return id; // Claimed this TLB.

		cond_resched();
		if (signal_pending(current))
			return -ERESTARTSYS;
	}

	// Unreachable.
	return -EINVAL;
}

int tenstorrent_device_free_tlb(struct tenstorrent_device *tt_dev,
				unsigned int id)
{
	const struct tenstorrent_device_class *dev_class = tt_dev->dev_class;
	u64 total_tlbs = 0;
	int i;

	if (dev_class->tlb_kinds == 0)
		return -EINVAL;

	for (i = 0; i < dev_class->tlb_kinds; ++i)
		total_tlbs += tt_dev->tlb_counts[i];

	if (id >= total_tlbs)
		return -EINVAL;

	if (!test_and_clear_bit(id, tt_dev->tlbs))
		return -EPERM;

	return 0;
}

int tenstorrent_device_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				     struct tenstorrent_noc_tlb_config *config)
{
	if (tt_dev->dev_class->configure_tlb)
		return tt_dev->dev_class->configure_tlb(tt_dev, tlb, config);

	return -EINVAL;
}

int tlb_pool_init(struct tlb_pool *pool, struct tenstorrent_device *tt_dev, const int *tlb_indices, unsigned int count)
{
	struct pci_dev *pdev = tt_dev->pdev;
	struct tlb_descriptor desc;
	unsigned int i;
	int ret;

	if (count > TT_IO_TLB_POOL_SIZE)
		return -EINVAL;

	if (!tt_dev->dev_class->describe_tlb)
		return -EOPNOTSUPP;

	mutex_init(&pool->lock);
	INIT_LIST_HEAD(&pool->lru_list);
	pool->tt_dev = tt_dev;
	pool->count = 0;
	pool->window_size = 0;

	for (i = 0; i < count; i++) {
		struct tlb_pool_entry *entry = &pool->entries[i];

		ret = tt_dev->dev_class->describe_tlb(tt_dev, tlb_indices[i], &desc);
		if (ret)
			goto fail;

		entry->io_base = pci_iomap_range(pdev, desc.bar, desc.bar_offset, desc.size);
		if (!entry->io_base) {
			ret = -ENOMEM;
			goto fail;
		}

		if (pool->window_size == 0)
			pool->window_size = desc.size;

		entry->tlb_index = tlb_indices[i];
		entry->valid = false;
		pool->count++;
		list_add_tail(&entry->lru_node, &pool->lru_list);
	}

	return 0;

fail:
	tlb_pool_destroy(pool);
	return ret;
}

void tlb_pool_destroy(struct tlb_pool *pool)
{
	struct pci_dev *pdev = pool->tt_dev->pdev;
	unsigned int i;

	for (i = 0; i < pool->count; i++) {
		if (pool->entries[i].io_base)
			pci_iounmap(pdev, pool->entries[i].io_base);
	}

	pool->count = 0;
}

// Caller must hold pool->lock.
int tlb_pool_acquire(struct tlb_pool *pool, u32 x, u32 y, u64 addr, struct tlb_pool_window *window)
{
	struct tlb_pool_entry *entry;
	struct tlb_pool_entry *victim;
	struct tenstorrent_noc_tlb_config config = { 0 };
	u64 aligned_addr = addr & ~(pool->window_size - 1);
	u64 offset_in_window = addr & (pool->window_size - 1);
	int ret;

	list_for_each_entry(entry, &pool->lru_list, lru_node) {
		if (entry->valid && entry->x == x && entry->y == y && entry->aligned_addr == aligned_addr) {
			list_move(&entry->lru_node, &pool->lru_list);
			window->io_addr = entry->io_base + offset_in_window;
			window->remaining = pool->window_size - offset_in_window;
			return 0;
		}
	}

	victim = list_last_entry(&pool->lru_list, struct tlb_pool_entry, lru_node);

	config.addr = aligned_addr;
	config.x_end = x;
	config.y_end = y;
	config.ordering = 1;

	ret = tenstorrent_device_configure_tlb(pool->tt_dev, victim->tlb_index, &config);
	if (ret)
		return ret;

	victim->valid = true;
	victim->x = x;
	victim->y = y;
	victim->aligned_addr = aligned_addr;

	list_move(&victim->lru_node, &pool->lru_list);

	window->io_addr = victim->io_base + offset_in_window;
	window->remaining = pool->window_size - offset_in_window;
	return 0;
}
