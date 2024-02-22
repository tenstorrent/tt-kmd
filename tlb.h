// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "device.h"

#ifndef TTDRIVER_TLB_H_INCLUDED
#define TTDRIVER_TLB_H_INCLUDED

#define TLB_SIZE_1M (1 * (1024 * 1024))
#define TLB_SIZE_2M (2 * (1024 * 1024))
#define TLB_SIZE_16M (16 * (1024 * 1024))
#define TLB_COUNT_1M 156
#define TLB_COUNT_2M 10
#define TLB_COUNT_16M 20
#define TLB_COUNT (156 + 10 + 20)

#define TLB_1M_BASE 0
#define TLB_2M_BASE (TLB_1M_BASE + TLB_SIZE_1M * TLB_COUNT_1M)
#define TLB_16M_BASE (TLB_2M_BASE + TLB_SIZE_2M * TLB_COUNT_2M)
#define TLB_OFFSET(index)                                                                             \
	((index) < TLB_COUNT_1M		       ? TLB_1M_BASE + (index) * TLB_SIZE_1M :                \
	 (index) < TLB_COUNT_1M + TLB_COUNT_2M ? TLB_2M_BASE + ((index)-TLB_COUNT_1M) * TLB_SIZE_2M : \
						 TLB_16M_BASE + ((index)-TLB_COUNT_1M - TLB_COUNT_2M) * TLB_SIZE_16M)


struct wormhole_device;

struct tlb_config {
	u64 address;
	u64 x_end;
	u64 y_end;
	u64 x_start;
	u64 y_start;
	u64 noc_sel;
	u64 mcast;
	u64 ordering;
	u64 linked;
};

// TLB represents a mapping from a region of BAR0 to a chip endpoint.
struct tlb_t {
	struct tlb_pool *pool;
	u32 index;	// 0 <= index < TLB_COUNT
	u32 size;	// 1MB, 2MB, or 16MB

	struct tlb_config config;
	u64 encoded_config;		// for hardware
};

struct tlb_pool {
	spinlock_t lock;
	bool avail[TLB_COUNT];
	struct tlb_t tlbs[TLB_COUNT];
	wait_queue_head_t wait_queue;
};

void tlb_pool_init(struct tlb_pool *);
struct tlb_t *tlb_alloc(struct tlb_pool *);
void tlb_free(struct tlb_t *);
void tlb_set_config(struct tlb_t *tlb, struct noc_addr_t *noc_addr);
u64 tlb_encode_config(struct tlb_t *tlb, int local_offset_width);


#endif