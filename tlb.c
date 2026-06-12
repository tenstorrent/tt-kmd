// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "tlb.h"
#include "device.h"

int tenstorrent_device_allocate_tlb(struct tenstorrent_device *tt_dev, size_t size)
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

	// Find a free window and claim it. tlb_mutex keeps find-and-claim atomic
	// against other allocations and against the refcount drops in
	// tenstorrent_device_free_tlb() / tenstorrent_tlb_export_put().
	mutex_lock(&tt_dev->tlb_mutex);

	id = find_next_zero_bit(tt_dev->tlbs, offset + n, offset);
	if (id == offset + n) {
		mutex_unlock(&tt_dev->tlb_mutex);
		return -ENOMEM;
	}

	set_bit(id, tt_dev->tlbs);
	WARN_ON(tt_dev->tlb_refcount[id] != 0);
	tt_dev->tlb_refcount[id] = 1; // The owning fd holds the first reference.

	mutex_unlock(&tt_dev->tlb_mutex);

	return id;
}

int tenstorrent_device_free_tlb(struct tenstorrent_device *tt_dev, unsigned int id)
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

	mutex_lock(&tt_dev->tlb_mutex);

	if (!test_bit(id, tt_dev->tlbs)) {
		mutex_unlock(&tt_dev->tlb_mutex);
		return -EPERM;
	}

	// Drop the owning fd's reference. If a dma-buf export of this window is
	// still live, the window's bit stays set and the window is returned to
	// the pool only when the last export is released.
	tt_dev->tlb_refcount[id]--;
	if (tt_dev->tlb_refcount[id] == 0)
		clear_bit(id, tt_dev->tlbs);

	mutex_unlock(&tt_dev->tlb_mutex);

	return 0;
}

// Take an export reference on an allocated TLB window, keeping it allocated
// (and out of the free pool) for the lifetime of a dma-buf export, even across
// FREE_TLB or close() of the owning fd. The caller must currently own the
// window. Pairs with tenstorrent_tlb_export_put().
void tenstorrent_tlb_export_get(struct tenstorrent_device *tt_dev, unsigned int id)
{
	mutex_lock(&tt_dev->tlb_mutex);

	tt_dev->tlb_refcount[id]++;

	mutex_unlock(&tt_dev->tlb_mutex);
}

// Release an export reference taken by tenstorrent_tlb_export_get(). When the
// last reference (owning fd or final export) goes away, the window returns to
// the pool.
void tenstorrent_tlb_export_put(struct tenstorrent_device *tt_dev, unsigned int id)
{
	mutex_lock(&tt_dev->tlb_mutex);

	if (WARN_ON(tt_dev->tlb_refcount[id] == 0)) {
		mutex_unlock(&tt_dev->tlb_mutex);
		return;
	}

	tt_dev->tlb_refcount[id]--;
	if (tt_dev->tlb_refcount[id] == 0)
		clear_bit(id, tt_dev->tlbs);

	mutex_unlock(&tt_dev->tlb_mutex);
}

int tenstorrent_device_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				     struct tenstorrent_noc_tlb_config *config)
{
	if (tt_dev->dev_class->configure_tlb)
		return tt_dev->dev_class->configure_tlb(tt_dev, tlb, config);

	return -EINVAL;
}
