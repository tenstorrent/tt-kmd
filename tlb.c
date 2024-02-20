// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sched.h>
#include <linux/slab.h>

#include "tlb.h"
#include "wormhole.h"

#define TLB_SIZE_FROM_INDEX(i) \
	((i) < TLB_COUNT_1M ? TLB_SIZE_1M : (i) < TLB_COUNT_1M + TLB_COUNT_2M ? TLB_SIZE_2M : TLB_SIZE_16M)

#define TLB_1M_BASE 0
#define TLB_2M_BASE (TLB_1M_BASE + TLB_SIZE_1M * TLB_COUNT_1M)
#define TLB_16M_BASE (TLB_2M_BASE + TLB_SIZE_2M * TLB_COUNT_2M)
#define TLB_OFFSET(index)                                                                             \
	((index) < TLB_COUNT_1M		       ? TLB_1M_BASE + (index) * TLB_SIZE_1M :                \
	 (index) < TLB_COUNT_1M + TLB_COUNT_2M ? TLB_2M_BASE + ((index)-TLB_COUNT_1M) * TLB_SIZE_2M : \
						 TLB_16M_BASE + ((index)-TLB_COUNT_1M - TLB_COUNT_2M) * TLB_SIZE_16M)

static u32 tlb_read32(u8 __iomem *bar0, struct tlb_t *tlb, u32 offset)
{
	return ioread32(bar0 + TLB_OFFSET(tlb->index) + (offset % tlb->size));
}

static void tlb_write32(u8 __iomem *bar0, struct tlb_t *tlb, u32 offset, u32 value)
{
	iowrite32(value, bar0 + TLB_OFFSET(tlb->index) + (offset % tlb->size));
}

static void tlb_memcpy_toio(u8 __iomem *bar0, struct tlb_t *tlb, u32 offset, const void *src, size_t n)
{
	memcpy_toio(bar0 + TLB_OFFSET(tlb->index) + (offset % tlb->size), src, n);
}

void tlb_pool_init(struct tlb_pool *pool)
{
	int i;

	spin_lock_init(&pool->lock);
	init_waitqueue_head(&pool->wait_queue);

	for (i = 0; i < TLB_COUNT; i++) {
		pool->avail[i] = false;
		pool->tlbs[i].index = i;
		pool->tlbs[i].size = TLB_SIZE_FROM_INDEX(i);
	}

	// This is the current (Feb 2024) convention held by UMD/KMD: UMD gets all
	// except the last TLB, which is 16MB and reserved for KMD.
	pool->avail[TLB_COUNT - 1] = true;
}

struct tlb_t *tlb_alloc(struct tlb_pool *pool)
{
	struct tlb_t *tlb = NULL;
	DECLARE_WAITQUEUE(wait, current);

	spin_lock(&pool->lock);

	for (;;) {
		int index;
		for (index = 0; index < TLB_COUNT; index++) {
			if (pool->avail[index]) {
				pool->avail[index] = false;
				tlb = &pool->tlbs[index];
				goto done;
			}
		}

		// No TLBs free - wait for one to become available.
		set_current_state(TASK_UNINTERRUPTIBLE);
		add_wait_queue(&pool->wait_queue, &wait);
		spin_unlock(&pool->lock);
		schedule();
		remove_wait_queue(&pool->wait_queue, &wait);
		spin_lock(&pool->lock);
		// FIXME: Add a timeout.
	}

done:
	spin_unlock(&pool->lock);
	return tlb;
}

void tlb_free(struct tlb_pool *pool, struct tlb_t *tlb)
{
	spin_lock(&pool->lock);
	pool->avail[tlb->index] = true;
	wake_up(&pool->wait_queue);
	spin_unlock(&pool->lock);
}

void tlb_set_config(struct tlb_t *tlb, u64 address, u64 x, u64 y)
{
	struct tlb_config *config = &tlb->config;
	memset(config, 0, sizeof(*config));

	config->address = address / tlb->size;
	config->x_end = x;
	config->y_end = y;
}

u32 wh_read32(struct wormhole_device *wh, struct tlb_t *tlb, u32 x, u32 y, u64 addr)
{
	tlb_set_config(tlb, addr, x, y);
	wh_program_tlb(wh, tlb);
	return tlb_read32(wh->bar0_mapping, tlb, addr);
}

void wh_write32(struct wormhole_device *wh, struct tlb_t *tlb, u32 x, u32 y, u64 addr, u32 value)
{
	tlb_set_config(tlb, addr, x, y);
	wh_program_tlb(wh, tlb);
	tlb_write32(wh->bar0_mapping, tlb, addr, value);
}

void wh_memcpy_toio(struct wormhole_device *wh, struct tlb_t *tlb, u32 x, u32 y, u64 addr, const void *src, size_t size)
{
	tlb_set_config(tlb, addr, x, y);
	wh_program_tlb(wh, tlb);
	tlb_memcpy_toio(wh->bar0_mapping, tlb, addr, src, size);
}