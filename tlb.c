// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/sched.h>
#include <linux/slab.h>

#include "tlb.h"
#include "wormhole.h"

#define TLB_SIZE_FROM_INDEX(i) \
	((i) < TLB_COUNT_1M ? TLB_SIZE_1M : (i) < TLB_COUNT_1M + TLB_COUNT_2M ? TLB_SIZE_2M : TLB_SIZE_16M)

void tlb_pool_init(struct tlb_pool *pool)
{
	int i;

	spin_lock_init(&pool->lock);
	init_waitqueue_head(&pool->wait_queue);

	for (i = 0; i < TLB_COUNT; i++) {
		pool->avail[i] = false;
		pool->tlbs[i].index = i;
		pool->tlbs[i].size = TLB_SIZE_FROM_INDEX(i);
		pool->tlbs[i].pool = pool;
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

void tlb_free(struct tlb_t *tlb)
{
	struct tlb_pool *pool = tlb->pool;
	spin_lock(&pool->lock);
	pool->avail[tlb->index] = true;
	wake_up(&pool->wait_queue);
	spin_unlock(&pool->lock);
}

void tlb_set_config(struct tlb_t *tlb, struct noc_addr_t *noc_addr)
{
	struct tlb_config *config = &tlb->config;
	memset(config, 0, sizeof(*config));

	config->address = noc_addr->addr / tlb->size;
	config->x_end = noc_addr->x;
	config->y_end = noc_addr->y;
	// TODO: if noc_addr_t becomes more sophisticated, set the appropriate
	// fields in the tlb_config struct.
}

static void set_field(u64 *reg, u64 value, int *offset, int width)
{
	u64 mask = (1ULL << width) - 1;
	*reg |= (value & mask) << *offset;
	*offset += width;
}

u64 tlb_encode_config(struct tlb_t *tlb, int local_offset_width)
{
	struct tlb_config *config = &tlb->config;
	u64 local_offset = config->address / tlb->size;
	u64 encoded = 0;
	int offset = 0;

	set_field(&encoded, local_offset, &offset, local_offset_width);
	set_field(&encoded, config->x_end, &offset, 6);
	set_field(&encoded, config->y_end, &offset, 6);
	set_field(&encoded, config->x_start, &offset, 6);
	set_field(&encoded, config->y_start, &offset, 6);
	set_field(&encoded, config->noc_sel, &offset, 1);
	set_field(&encoded, config->mcast, &offset, 1);
	set_field(&encoded, config->ordering, &offset, 2);
	set_field(&encoded, config->linked, &offset, 1);

	return encoded;
}
