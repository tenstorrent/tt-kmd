// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/dmaengine.h>
#include <linux/dma/edma.h>

#include "chardev_private.h"
#include "device.h"
#include "dma.h"
#include "ioctl.h"
#include "tlb.h"
#include "wormhole.h"

static void dma_complete_callback(void *param)
{
	struct completion *comp = param;
	complete(comp);
}

struct tt_dma_filter {
	struct device *dev;
	u32 dma_mask;
};

static bool tt_dma_filter_fn(struct dma_chan *chan, void *node)
{
	struct dma_slave_caps caps = {0};
	struct tt_dma_filter *filter = node;
	dma_get_slave_caps(chan, &caps);

	return chan->device->dev == filter->dev && (filter->dma_mask & caps.directions);
}

long ioctl_dma(struct chardev_private *priv, struct tenstorrent_dma __user *arg)
{
	struct tenstorrent_device *tt_dev = priv->device;
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	struct device *dma_dev = wh_dev->edma_chip->dev;
	struct tlb_descriptor tlb_desc = {0};
	struct tenstorrent_dma tt_dma = {0};
	struct dma_slave_config dma_config = {0};
	struct dma_async_tx_descriptor *desc;
	struct dma_chan *dma_chan;
	enum dma_status status;
	enum dma_transfer_direction direction;
	dma_cap_mask_t mask;
	dma_cookie_t cookie;
	u64 chip_addr;
	int ret;
	struct tt_dma_filter filter = {0};


	// Driver-driven DMA will only work with driver-managed TLBs.
	// TODO: is the TLB that the user requested a driver-managed TLB?
	// Does it belong to the fd?
	if (!tt_dev->dev_class->describe_tlb)
		return -EINVAL;

	if (copy_from_user(&tt_dma, &arg->in, sizeof(tt_dma)) != 0)
		return -EFAULT;

	// Retrieve information about the TLB window the user requested.
	if (tt_dev->dev_class->describe_tlb(tt_dev, tt_dma.in.tlb_id, &tlb_desc))
		return -EINVAL;

	// Do not allow the DMA engine to walk off the end of the TLB window.
	if (tt_dma.in.offset >= tlb_desc.size)
		return -EINVAL;

	filter.dev = dma_dev;
	if (tt_dma.in.flags & TENSTORRENT_DMA_H2D)
		filter.dma_mask = BIT(DMA_MEM_TO_DEV);
	else
		filter.dma_mask = BIT(DMA_DEV_TO_MEM);

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);
	dma_chan = dma_request_channel(mask, tt_dma_filter_fn, &filter);
	if (IS_ERR_OR_NULL(dma_chan)) {
		return -EINVAL;
	}
	// Chip-side AXI addresses of TLB windows are are equivalent to their offset
	// into BAR0, with one notable exception: 4G windows in BH BAR4 have an
	// additional offset of 0x20_0000_0000
	//
	// TODO: that's just what I remember, so make sure this is right when you
	// add BH support.
	chip_addr = tlb_desc.bar_offset + tt_dma.in.offset;
	if (tt_dma.in.flags & TENSTORRENT_DMA_H2D) {
		dma_config.dst_addr = chip_addr;
		direction = DMA_MEM_TO_DEV;
	} else {
		dma_config.src_addr = chip_addr;
		direction = DMA_DEV_TO_MEM;
	}
	ret = dmaengine_slave_config(dma_chan, &dma_config);
	if (ret) {
		dma_release_channel(dma_chan);
		return ret;
	}

	desc = dmaengine_prep_slave_single(dma_chan, tt_dma.in.iova, tt_dma.in.size, direction, DMA_PREP_INTERRUPT);
	if (!desc) {
		pr_info("prep_slave_single failed\n");
		dma_release_channel(dma_chan);
		return -EINVAL;
	}

	struct completion dma_completion;
	init_completion(&dma_completion);
	desc->callback_param = &dma_completion;
	desc->callback = dma_complete_callback;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		pr_err("dmaengine_submit failed\n");
		dma_release_channel(dma_chan);
		return -EINVAL;
	}

	dma_async_issue_pending(dma_chan);

	if (wait_for_completion_timeout(&dma_completion, msecs_to_jiffies(5000)) == 0) {
		pr_err("DMA timed out\n");
		dmaengine_terminate_all(dma_chan);
		dma_release_channel(dma_chan);
		return -ETIMEDOUT;
	}

	status = dma_async_is_tx_complete(dma_chan, cookie, NULL, NULL);
	if (status != DMA_COMPLETE) {
		pr_err("DMA not complete\n");
		dmaengine_terminate_all(dma_chan);
		dma_release_channel(dma_chan);
		return -EIO;
	}

	dma_release_channel(dma_chan);
	return 0;
}

