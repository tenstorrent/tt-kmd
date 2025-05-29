// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sched/signal.h>

#include "tlb.h"
#include "device.h"

int tenstorrent_device_allocate_tlb(struct tenstorrent_device *tt_dev,
				    size_t *size)
{
	const struct tenstorrent_device_class *dev_class = tt_dev->dev_class;
	unsigned long id = 0;
	unsigned long offset = 0; // Offset into the TLB bitmap.
	unsigned long n = 0; // Total number of TLBs of the requested size.
	int kind;

	if (dev_class->tlb_kinds == 0)
		return -EINVAL;

	for (kind = 0; kind < dev_class->tlb_kinds; ++kind) {
		long tlb_count = dev_class->tlb_counts[kind];
		long tlb_size = dev_class->tlb_sizes[kind];

		if (*size <= tlb_size) {
			*size = tlb_size;
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
		total_tlbs += dev_class->tlb_counts[i];

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
