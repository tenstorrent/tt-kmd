// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Host-to-device DMA on Blackhole's HDMA read channel 1. Firmware keeps
// channel 0 for its own DMA messages.
//
// The engine runs in linked-list mode. Its list lives in host memory that the
// driver allocates, and the engine reads it through a 2M TLB window pointed at
// the chip's own PCIe tile, at NOC address (0 << 58) | IOVA, which bypasses the
// outbound iATU. Data goes out through TENSTORRENT_DMA_WINDOWS more 2M windows,
// each bound to one region (endpoint plus 2M-aligned range) of a request.
//
// Requests from every fd share one FIFO and run one at a time, in order: each
// is one chain, with one list element per descriptor. The engine's stop or
// abort MSI ends the chain, on vector 1 when the device got two vectors and on
// vector 0 otherwise. The handler starts the next chain and posts the CQE from
// task context, as io_uring_cmd_done() can't be called from an interrupt.

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/msi.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "blackhole.h"
#include "blackhole_dma.h"
#include "chardev_private.h"
#include "uring_compat.h"

// One element of the engine's linked list (DesignWare HDMA v0 layout).
struct bh_hdma_lli {
	__le32 control;
	__le32 size;
	__le64 sar;
	__le64 dar;
} __packed;

#define LLI_CB	BIT(0)	// Cycle bit: the engine runs an element only if it matches its CCS

// Room for a full request plus the element that ends the chain. A coherent
// buffer is aligned to its size when that is a power of two, so the list never
// crosses a 2M boundary and one window covers it.
#define BH_DMA_LIST_SIZE SZ_32K

static_assert((TENSTORRENT_DMA_MAX_DESCS + 1) * sizeof(struct bh_hdma_lli) <= BH_DMA_LIST_SIZE);
static_assert(BH_DMA_LIST_SIZE <= TENSTORRENT_DMA_WINDOW_SIZE);

#define DMA_WINDOW_MASK ((u64)TENSTORRENT_DMA_WINDOW_SIZE - 1)

// Engine (AXI) address of 2M window i.
#define TLB_2M_AXI_ADDR(i) ((u64)(i) * TENSTORRENT_DMA_WINDOW_SIZE)

// HDMA read channel 1 registers, in BAR2.
#define HDMA_RDCH1		0x300
#define HDMA_EN			0x00
#define HDMA_DOORBELL		0x04
#define HDMA_ELEM_PF		0x08
#define HDMA_LLP_LOW		0x10
#define HDMA_LLP_HIGH		0x14
#define HDMA_CYCLE_SYNC		0x18
#define HDMA_CONTROL1		0x34
#define HDMA_CH_STAT		0x80
#define HDMA_INT_STAT		0x84
#define HDMA_INT_SETUP		0x88
#define HDMA_INT_CLEAR		0x8C
#define HDMA_MSI_STOP_LOW	0x90
#define HDMA_MSI_STOP_HIGH	0x94
#define HDMA_MSI_ABORT_LOW	0xA0
#define HDMA_MSI_ABORT_HIGH	0xA4
#define HDMA_MSI_MSGD		0xA8

#define HDMA_EN_ENABLE		BIT(0)
#define HDMA_DOORBELL_START	BIT(0)
#define HDMA_DOORBELL_STOP	BIT(1)
#define HDMA_CYCLE_SYNC_CB	BIT(0)
#define HDMA_CYCLE_SYNC_CCS	BIT(1)
#define HDMA_CONTROL1_LLEN	BIT(0)
#define HDMA_CH_STAT_MASK	GENMASK(1, 0)
#define HDMA_CH_STAT_RUNNING	1
#define HDMA_INT_STOP		BIT(0)
#define HDMA_INT_ABORT		BIT(2)
#define HDMA_INT_ALL		(HDMA_INT_STOP | BIT(1) | HDMA_INT_ABORT)
// Bits 0-2 mask the local interrupt pin, which isn't ours. Stop and abort
// must also be enabled locally (LSIE, LAIE) for INT_STAT to latch them.
#define HDMA_INT_SETUP_MASK_LOCAL	GENMASK(2, 0)
#define HDMA_INT_SETUP_RSIE	BIT(3)	// MSI on stop
#define HDMA_INT_SETUP_LSIE	BIT(4)
#define HDMA_INT_SETUP_RAIE	BIT(5)	// MSI on abort
#define HDMA_INT_SETUP_LAIE	BIT(6)
#define HDMA_INT_SETUP_VALUE	(HDMA_INT_SETUP_MASK_LOCAL | HDMA_INT_SETUP_RSIE | HDMA_INT_SETUP_LSIE | \
				 HDMA_INT_SETUP_RAIE | HDMA_INT_SETUP_LAIE)

// Read channel 0's PF_DEPTH, which firmware sets, is the prefetch depth of
// every read channel: channel i owns PF_DEPTH + 1 of the engine's 16 prefetch
// entries, starting at entry i * (PF_DEPTH + 1). The driver only reads it.
#define HDMA_RDCH0_QOS		0x13C
#define HDMA_QOS_PF_DEPTH	GENMASK(25, 16)
#define HDMA_PF_ENTRIES		16
#define HDMA_CHANNEL		1

// How long a channel reset waits for a stopped chain to drain.
#define HDMA_STOP_TIMEOUT_US	100

// Timeout for one chain: 2 s plus 1 ms per MiB.
#define DMA_TIMEOUT_BASE_MS	2000

// TAG_FW_ACTIVE_CONFIG_0 bit: firmware uses only HDMA channel 0, keeps its
// PF_DEPTH fixed, and leaves channel 1 to the host.
#define FW_ACTIVE_CONFIG_0_HDMA_HOST_CHANNEL_1 BIT(3)

struct bh_dma_request {
	struct list_head node;		// In bh_dma.queue
	struct io_uring_cmd *cmd;
	int result;			// CQE res, set when the request ends
	u64 bytes;
	u32 count;
	u32 nregions;
	struct tenstorrent_noc_tlb_config region[TENSTORRENT_DMA_WINDOWS];
	struct bh_hdma_lli lli[];	// One data element per descriptor
};

#if TT_HAVE_URING_CMD
static void dma_timeout_work(struct work_struct *work);
#endif

// Take the DMA windows out of the pool and allocate the list. Failing to
// allocate the list leaves DMA unavailable but is not fatal for the device.
void bh_dma_init(struct blackhole_device *bh, unsigned int first_tlb)
{
	struct tenstorrent_device *tt_dev = &bh->tt;
	struct bh_dma *dma = &bh->dma;
	unsigned int i;

	spin_lock_init(&dma->lock);
	INIT_LIST_HEAD(&dma->queue);
#if TT_HAVE_URING_CMD
	INIT_DELAYED_WORK(&dma->timeout_work, dma_timeout_work);
#endif

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

#if TT_HAVE_URING_CMD

static void __iomem *hdma_reg(struct blackhole_device *bh, u32 reg)
{
	return bh->bar2_mapping + HDMA_RDCH1 + reg;
}

static void hdma_write(struct blackhole_device *bh, u32 reg, u32 value)
{
	iowrite32(value, hdma_reg(bh, reg));
}

static u32 hdma_read(struct blackhole_device *bh, u32 reg)
{
	return ioread32(hdma_reg(bh, reg));
}

// Channel settings that outlive a chain. Also restores them after a channel
// reset.
static void hdma_setup_channel(struct blackhole_device *bh)
{
	hdma_write(bh, HDMA_EN, HDMA_EN_ENABLE);
	// List elements the engine reads ahead. More than channel 0's PF_DEPTH
	// overruns the channel's prefetch entries and hangs it.
	hdma_write(bh, HDMA_ELEM_PF, bh->dma.pf_depth);
	hdma_write(bh, HDMA_INT_SETUP, HDMA_INT_SETUP_VALUE);
	hdma_write(bh, HDMA_CONTROL1, HDMA_CONTROL1_LLEN);
}

static bool hdma_running(struct blackhole_device *bh)
{
	return FIELD_GET(HDMA_CH_STAT_MASK, hdma_read(bh, HDMA_CH_STAT)) == HDMA_CH_STAT_RUNNING;
}

// Stop whatever the channel is doing and leave it ready for the next chain.
// Returns false if it wouldn't stop.
static bool hdma_reset_channel(struct blackhole_device *bh)
{
	unsigned int us;

	hdma_write(bh, HDMA_DOORBELL, HDMA_DOORBELL_STOP);

	for (us = 0; us < HDMA_STOP_TIMEOUT_US && hdma_running(bh); us++)
		udelay(1);

	if (hdma_running(bh))
		return false;

	hdma_write(bh, HDMA_EN, 0);
	hdma_write(bh, HDMA_INT_CLEAR, HDMA_INT_ALL);
	hdma_setup_channel(bh);
	return true;
}

// Run when firmware is ready, from init_hardware: at probe, after a reset
// and at resume. DMA stays off unless firmware has left channel 1 to us.
void bh_dma_enable(struct blackhole_device *bh)
{
	struct tenstorrent_device *tt_dev = &bh->tt;
	struct bh_dma *dma = &bh->dma;
	struct tenstorrent_noc_tlb_config config = { 0 };
	struct device *dev = &tt_dev->pdev->dev;
	u32 active_config;
	u32 pf_depth;
	u32 pcie_x;

	if (!dma->list)
		return;

	// The chip may have been reset behind the driver's back (RESTORE_STATE
	// after a reset that didn't go through RESET_DEVICE), so start over.
	bh_dma_quiesce(tt_dev, true);

	if (blackhole_read_fw_active_config(bh, &active_config) != 0 ||
	    !(active_config & FW_ACTIVE_CONFIG_0_HDMA_HOST_CHANNEL_1))
		return;

	// Channel 1 has prefetch entries only if two channels' worth fit.
	pf_depth = FIELD_GET(HDMA_QOS_PF_DEPTH, ioread32(bh->bar2_mapping + HDMA_RDCH0_QOS));
	if ((HDMA_CHANNEL + 1) * (pf_depth + 1) > HDMA_PF_ENTRIES) {
		dev_info(dev, "HDMA PF_DEPTH %u leaves channel %u no prefetch entries; DMA is unavailable\n",
			 pf_depth, HDMA_CHANNEL);
		return;
	}
	dma->pf_depth = pf_depth;

	// The engine signals completion with an MSI it writes itself.
	if (!tt_dev->interrupt_enabled || !tt_dev->pdev->msi_enabled) {
		dev_info(dev, "DMA needs MSI interrupts; DMA is unavailable\n");
		return;
	}

	if (!blackhole_detect_pcie_noc_x(bh, &pcie_x)) {
		dev_warn(dev, "Can't find the PCIe tile; DMA is unavailable\n");
		return;
	}

	// The list window reaches host memory at its IOVA, bypassing the iATU.
	config.addr = dma->list_iova & ~DMA_WINDOW_MASK;
	config.x_end = pcie_x;
	config.y_end = 0;
	blackhole_configure_tlb_2M(bh, dma->list_tlb, &config);
	dma->list_axi = TLB_2M_AXI_ADDR(dma->list_tlb) + (dma->list_iova & DMA_WINDOW_MASK);

	// With one vector, completions share vector 0 with firmware.
	dma->vector = tt_dev->interrupt_vectors > BH_HDMA_MSI_VECTOR ? BH_HDMA_MSI_VECTOR : 0;

	hdma_write(bh, HDMA_INT_CLEAR, HDMA_INT_ALL);
	hdma_setup_channel(bh);

	spin_lock_irq(&dma->lock);
	dma->enabled = true;
	spin_unlock_irq(&dma->lock);

	dev_info(dev, "DMA available, completions on vector %u\n", dma->vector);
}

// Program the engine for the request at the head of the queue and start it.
static void dma_start_locked(struct blackhole_device *bh)
{
	struct bh_dma *dma = &bh->dma;
	struct bh_dma_request *req;
	struct bh_hdma_lli *list = dma->list;
	struct msi_msg msg;
	unsigned int timeout_ms;
	u32 data;
	u32 i;

	lockdep_assert_held(&dma->lock);

	if (dma->active || list_empty(&dma->queue))
		return;

	req = list_first_entry(&dma->queue, struct bh_dma_request, node);
	list_del(&req->node);
	dma->active = req;

	for (i = 0; i < req->nregions; i++)
		blackhole_configure_tlb_2M(bh, dma->data_tlb[i], &req->region[i]);

	// The engine, not the CPU, uses the windows, so make sure their
	// configuration has landed before it can start.
	ioread32(bh->tlb_regs);

	memcpy(list, req->lli, req->count * sizeof(*list));

	// The element after the last one has a cycle bit that doesn't match, so
	// the engine stops there and raises the stop MSI.
	memset(&list[req->count], 0, sizeof(*list));

	// The MSI's address and data can change with its affinity, so take
	// them fresh. Multi-message MSI puts the vector in the low data bits.
	get_cached_msi_msg(bh->tt.irq_base, &msg);
	data = msg.data | dma->vector;

	hdma_write(bh, HDMA_MSI_STOP_LOW, msg.address_lo);
	hdma_write(bh, HDMA_MSI_STOP_HIGH, msg.address_hi);
	hdma_write(bh, HDMA_MSI_ABORT_LOW, msg.address_lo);
	hdma_write(bh, HDMA_MSI_ABORT_HIGH, msg.address_hi);
	hdma_write(bh, HDMA_MSI_MSGD, data);

	hdma_write(bh, HDMA_LLP_LOW, lower_32_bits(dma->list_axi));
	hdma_write(bh, HDMA_LLP_HIGH, upper_32_bits(dma->list_axi));
	hdma_write(bh, HDMA_CYCLE_SYNC, HDMA_CYCLE_SYNC_CCS | HDMA_CYCLE_SYNC_CB);

	// The list must be in memory before the doorbell.
	dma_wmb();
	hdma_write(bh, HDMA_DOORBELL, HDMA_DOORBELL_START);

	timeout_ms = DMA_TIMEOUT_BASE_MS + DIV_ROUND_UP_ULL(req->bytes, SZ_1M);
	dma->deadline = jiffies + msecs_to_jiffies(timeout_ms);
	mod_delayed_work(system_wq, &dma->timeout_work, msecs_to_jiffies(timeout_ms));
}

static void dma_complete_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	struct bh_dma_request *req = *(struct bh_dma_request **)cmd->pdu;
	int result = req->result;

	kvfree(req);
	tt_uring_cmd_done(cmd, result, issue_flags);
}

TT_URING_CMD_TASK_WORK(dma_complete_task_work, dma_complete_cmd)

// End a request that is off the queue. Safe from any context.
static void dma_end_request(struct bh_dma_request *req, int result)
{
	req->result = result;
	io_uring_cmd_complete_in_task(req->cmd, dma_complete_task_work);
}

// End the active request and start the next.
static void dma_finish_active_locked(struct blackhole_device *bh, int result)
{
	struct bh_dma *dma = &bh->dma;
	struct bh_dma_request *req = dma->active;

	lockdep_assert_held(&dma->lock);

	dma->active = NULL;
	dma_end_request(req, result);
	dma_start_locked(bh);
}

// End the active request after the engine stopped it or ran out of time:
// reset the channel and go on with the next. A channel that won't stop is
// beyond the driver until the device is reset, so DMA goes off and what is
// queued fails.
static void dma_abort_active_locked(struct blackhole_device *bh, int result)
{
	struct bh_dma *dma = &bh->dma;
	struct bh_dma_request *req, *tmp;

	lockdep_assert_held(&dma->lock);

	if (hdma_reset_channel(bh)) {
		dma_finish_active_locked(bh, result);
		return;
	}

	dev_err(&bh->tt.pdev->dev, "DMA channel won't stop; DMA is off until the device is reset\n");
	dma->enabled = false;

	req = dma->active;
	dma->active = NULL;
	dma_end_request(req, result);

	list_for_each_entry_safe(req, tmp, &dma->queue, node) {
		list_del(&req->node);
		dma_end_request(req, result);
	}
}

// Act on the channel's interrupt status. Returns false if the chain is still
// running.
static bool dma_handle_status_locked(struct blackhole_device *bh, u32 status)
{
	struct bh_dma *dma = &bh->dma;

	if (!(status & (HDMA_INT_STOP | HDMA_INT_ABORT)))
		return false;

	hdma_write(bh, HDMA_INT_CLEAR, status);

	if (status & HDMA_INT_ABORT) {
		dev_warn_ratelimited(&bh->tt.pdev->dev, "DMA aborted (status 0x%x)\n", status);
		dma_abort_active_locked(bh, -EIO);
	} else {
		dma_finish_active_locked(bh, 0);
	}

	if (!dma->active)
		cancel_delayed_work(&dma->timeout_work);

	return true;
}

// From the MSI handler, for the vector completions arrive on.
void bh_dma_interrupt(struct blackhole_device *bh)
{
	struct bh_dma *dma = &bh->dma;
	unsigned long flags;
	u32 status;

	// While DMA is off, the channel may not be ours or the chip may be
	// mid-reset, so leave it alone.
	if (!READ_ONCE(dma->enabled))
		return;

	spin_lock_irqsave(&dma->lock, flags);

	if (!dma->enabled || !dma->active)
		goto out;

	// Vector 0 is shared with firmware, so the MSI may not be ours.
	status = hdma_read(bh, HDMA_INT_STAT);
	dma_handle_status_locked(bh, status);

out:
	spin_unlock_irqrestore(&dma->lock, flags);
}

static void dma_timeout_work(struct work_struct *work)
{
	struct bh_dma *dma = container_of(to_delayed_work(work), struct bh_dma, timeout_work);
	struct blackhole_device *bh = container_of(dma, struct blackhole_device, dma);

	spin_lock_irq(&dma->lock);

	if (!dma->enabled || !dma->active)
		goto out;

	// The work was armed for an earlier chain; wait for this one's deadline.
	if (time_before(jiffies, dma->deadline)) {
		mod_delayed_work(system_wq, &dma->timeout_work, dma->deadline - jiffies);
		goto out;
	}

	// The chain may have ended with its MSI lost on the way.
	if (dma_handle_status_locked(bh, hdma_read(bh, HDMA_INT_STAT)))
		goto out;

	dev_warn_ratelimited(&bh->tt.pdev->dev, "DMA timed out (channel status 0x%x)\n",
			     hdma_read(bh, HDMA_CH_STAT));
	dma_abort_active_locked(bh, -ETIMEDOUT);

out:
	spin_unlock_irq(&dma->lock);
}

// Stop DMA and fail everything queued or running with -ENODEV. DMA stays off
// until bh_dma_enable(). Called with reset_rwsem held for write, before a
// reset, suspend or removal; reachable says whether the chip can be touched.
void bh_dma_quiesce(struct tenstorrent_device *tt_dev, bool reachable)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct bh_dma *dma = &bh->dma;
	struct bh_dma_request *req, *tmp;
	LIST_HEAD(failed);

	spin_lock_irq(&dma->lock);

	if (dma->active && reachable)
		hdma_reset_channel(bh);
	dma->enabled = false;

	if (dma->active)
		list_add(&dma->active->node, &failed);
	dma->active = NULL;
	list_splice_tail_init(&dma->queue, &failed);

	spin_unlock_irq(&dma->lock);

	cancel_delayed_work_sync(&dma->timeout_work);

	list_for_each_entry_safe(req, tmp, &failed, node) {
		list_del(&req->node);
		dma_end_request(req, -ENODEV);
	}
}

// Check a request and turn it into list elements. Nothing is split: a request
// that doesn't fit the windows as given is rejected.
static int dma_build_request(struct bh_dma *dma, struct bh_dma_request *req,
			     const struct tenstorrent_dma_desc *descs)
{
	u32 i, r;

	for (i = 0; i < req->count; i++) {
		const struct tenstorrent_dma_desc *d = &descs[i];
		struct tenstorrent_noc_tlb_config *region;
		u64 base = d->addr & ~DMA_WINDOW_MASK;
		u64 offset = d->addr & DMA_WINDOW_MASK;

		if (memchr_inv(d->reserved, 0, sizeof(d->reserved)))
			return -EINVAL;

		if (offset + d->size > TENSTORRENT_DMA_WINDOW_SIZE)
			return -EINVAL;

		for (r = 0; r < req->nregions; r++) {
			region = &req->region[r];
			if (region->addr == base && region->x_end == d->x &&
			    region->y_end == d->y && region->noc == d->noc)
				break;
		}

		if (r == req->nregions) {
			if (r == TENSTORRENT_DMA_WINDOWS)
				return -EINVAL;

			// As user space's DMA window: relaxed ordering on the
			// unicast-write static VC.
			region = &req->region[r];
			region->addr = base;
			region->x_end = d->x;
			region->y_end = d->y;
			region->noc = d->noc;
			region->static_vc = 1;
			req->nregions++;
		}

		req->lli[i].control = cpu_to_le32(LLI_CB);
		req->lli[i].size = cpu_to_le32(d->size);
		req->lli[i].sar = cpu_to_le64(d->src);
		req->lli[i].dar = cpu_to_le64(TLB_2M_AXI_ADDR(dma->data_tlb[r]) + offset);
		req->bytes += d->size;
	}

	return 0;
}

// Queue a request. Called with reset_rwsem held shared on a current fd.
// Returns -EIOCBQUEUED once the request is queued; its CQE comes later.
int bh_dma_write(struct chardev_private *priv, struct io_uring_cmd *cmd,
		 const struct tenstorrent_dma_write *args)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(priv->device);
	struct bh_dma *dma = &bh->dma;
	struct tenstorrent_dma_desc *descs;
	struct bh_dma_request *req;
	unsigned long flags;
	int ret;

	BUILD_BUG_ON(sizeof(req) > sizeof(cmd->pdu));

	if (!READ_ONCE(dma->enabled))
		return -EOPNOTSUPP;

	if (args->flags != 0 || args->count == 0 || args->count > TENSTORRENT_DMA_MAX_DESCS)
		return -EINVAL;

	req = kvzalloc(struct_size(req, lli, args->count), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->cmd = cmd;
	req->count = args->count;

	// Everything is checked and run from this copy.
	descs = kvmalloc_array(args->count, sizeof(*descs), GFP_KERNEL);
	if (!descs) {
		ret = -ENOMEM;
		goto err_free_req;
	}

	if (copy_from_user(descs, u64_to_user_ptr(args->descs), args->count * sizeof(*descs))) {
		ret = -EFAULT;
		goto err_free_descs;
	}

	ret = dma_build_request(dma, req, descs);
	if (ret)
		goto err_free_descs;

	kvfree(descs);

	*(struct bh_dma_request **)cmd->pdu = req;

	spin_lock_irqsave(&dma->lock, flags);

	if (!dma->enabled) {
		spin_unlock_irqrestore(&dma->lock, flags);
		ret = -EOPNOTSUPP;
		goto err_free_req;
	}

	list_add_tail(&req->node, &dma->queue);
	dma_start_locked(bh);
	spin_unlock_irqrestore(&dma->lock, flags);

	return -EIOCBQUEUED;

err_free_descs:
	kvfree(descs);
err_free_req:
	kvfree(req);
	return ret;
}

#else // !TT_HAVE_URING_CMD

void bh_dma_enable(struct blackhole_device *bh)
{
}

void bh_dma_quiesce(struct tenstorrent_device *tt_dev, bool reachable)
{
}

void bh_dma_interrupt(struct blackhole_device *bh)
{
}

int bh_dma_write(struct chardev_private *priv, struct io_uring_cmd *cmd,
		 const struct tenstorrent_dma_write *args)
{
	return -EOPNOTSUPP;
}

#endif // TT_HAVE_URING_CMD

void bh_dma_cleanup(struct blackhole_device *bh)
{
	struct bh_dma *dma = &bh->dma;

	if (dma->list)
		dma_free_coherent(&bh->tt.pdev->dev, BH_DMA_LIST_SIZE, dma->list, dma->list_iova);
	dma->list = NULL;
}
