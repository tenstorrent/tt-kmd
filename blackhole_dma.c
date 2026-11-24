// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Host-to-device DMA on Blackhole's HDMA read channel 0.
//
// The engine runs in linked-list mode. Its list lives in host memory that the
// driver allocates, and the engine reads it through a 2M TLB window pointed at
// the chip's own PCIe tile, at NOC address (0 << 58) | IOVA, which bypasses the
// outbound iATU. Data goes out through TENSTORRENT_DMA_WINDOWS more 2M windows,
// each bound to one region (endpoint plus 2M-aligned range) of a request.

#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include "blackhole.h"
#include "blackhole_dma.h"

// One element of the engine's linked list (DesignWare HDMA v0 layout).
struct bh_hdma_lli {
	__le32 control;
	__le32 size;
	__le64 sar;
	__le64 dar;
} __packed;

// Room for a full request plus the element that ends the chain. A coherent
// buffer is aligned to its size when that is a power of two, so the list never
// crosses a 2M boundary and one window covers it.
#define BH_DMA_LIST_SIZE SZ_32K

static_assert((TENSTORRENT_DMA_MAX_DESCS + 1) * sizeof(struct bh_hdma_lli) <= BH_DMA_LIST_SIZE);
static_assert(BH_DMA_LIST_SIZE <= TENSTORRENT_DMA_WINDOW_SIZE);

// Take the DMA windows out of the pool and allocate the list. Failing to
// allocate the list leaves DMA unavailable but is not fatal for the device.
void bh_dma_init(struct blackhole_device *bh, unsigned int first_tlb)
{
	struct tenstorrent_device *tt_dev = &bh->tt;
	struct bh_dma *dma = &bh->dma;
	unsigned int i;

	for (i = 0; i < TENSTORRENT_DMA_WINDOWS; i++)
		dma->data_tlb[i] = first_tlb + i;
	dma->list_tlb = first_tlb + TENSTORRENT_DMA_WINDOWS;

	for (i = 0; i < BH_DMA_TLB_COUNT; i++)
		set_bit(first_tlb + i, tt_dev->tlbs);

	if (!tt_dev->dma_capable)
		return;

	dma->list = dma_alloc_coherent(&tt_dev->pdev->dev, BH_DMA_LIST_SIZE, &dma->list_iova, GFP_KERNEL);
	if (!dma->list)
		dev_warn(&tt_dev->pdev->dev, "Can't allocate the DMA list; DMA is unavailable\n");
}

void bh_dma_cleanup(struct blackhole_device *bh)
{
	struct bh_dma *dma = &bh->dma;

	if (dma->list)
		dma_free_coherent(&bh->tt.pdev->dev, BH_DMA_LIST_SIZE, dma->list, dma->list_iova);
	dma->list = NULL;
}
