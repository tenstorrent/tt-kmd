// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_DMA_H_INCLUDED
#define TTDRIVER_BLACKHOLE_DMA_H_INCLUDED

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "ioctl.h"

struct blackhole_device;
struct chardev_private;
struct io_uring_cmd;
struct tenstorrent_device;
struct bh_dma_request;

// Host-to-device DMA on HDMA read channel 0 (TENSTORRENT_URING_CMD_DMA_WRITE).
struct bh_dma {
	// Fixed at probe and held while the driver is bound.
	unsigned int data_tlb[TENSTORRENT_DMA_WINDOWS];	// 2M windows bound to a request's regions
	unsigned int list_tlb;		// 2M window the engine reads its list through
	void *list;			// Engine linked list, coherent; NULL if allocation failed
	dma_addr_t list_iova;

	// Set up by bh_dma_enable() once firmware has left channel 0 to the
	// host, while init_hardware runs.
	u64 list_axi;			// Engine address of the list
	unsigned int vector;		// MSI vector the engine signals completion on

	// lock guards everything below. enabled is set only by bh_dma_enable()
	// (init_hardware) and cleared by bh_dma_quiesce() or a channel that won't
	// stop; a submitter tests it without the lock only as a shortcut.
	spinlock_t lock;
	bool enabled;
	struct list_head queue;		// Requests waiting for the engine, in order
	struct bh_dma_request *active;	// Request the engine is running, or NULL
	unsigned long deadline;		// jiffies by which active must be done
	struct delayed_work timeout_work;
};

// 2M windows taken out of the pool for DMA: the data windows, then the list window.
#define BH_DMA_TLB_COUNT (TENSTORRENT_DMA_WINDOWS + 1)

void bh_dma_init(struct blackhole_device *bh, unsigned int first_tlb);
void bh_dma_cleanup(struct blackhole_device *bh);
void bh_dma_enable(struct blackhole_device *bh);
void bh_dma_quiesce(struct tenstorrent_device *tt_dev, bool reachable);
void bh_dma_interrupt(struct blackhole_device *bh);
int bh_dma_write(struct chardev_private *priv, struct io_uring_cmd *cmd,
		 const struct tenstorrent_dma_write *args);

#endif
