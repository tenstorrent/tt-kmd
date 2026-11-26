// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_DMA_H_INCLUDED
#define TTDRIVER_BLACKHOLE_DMA_H_INCLUDED

#include <linux/types.h>

#include "ioctl.h"

struct blackhole_device;

// Host-to-device DMA on HDMA read channel 0 (TENSTORRENT_URING_CMD_DMA_WRITE).
struct bh_dma {
	// Fixed at probe and held while the driver is bound.
	unsigned int data_tlb[TENSTORRENT_DMA_WINDOWS];	// 2M windows bound to a request's regions
	unsigned int list_tlb;		// 2M window the engine reads its list through
	void *list;			// Engine linked list, coherent; NULL if allocation failed
	dma_addr_t list_iova;
};

// 2M windows taken out of the pool for DMA: the data windows, then the list window.
#define BH_DMA_TLB_COUNT (TENSTORRENT_DMA_WINDOWS + 1)

void bh_dma_init(struct blackhole_device *bh, unsigned int first_tlb);
void bh_dma_cleanup(struct blackhole_device *bh);

#endif
