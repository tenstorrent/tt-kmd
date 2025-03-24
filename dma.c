// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/dmaengine.h>

#include "chardev_private.h"
#include "device.h"
#include "dma.h"
#include "ioctl.h"
#include "tlb.h"

static void dma_complete_callback(void *param)
{
	struct chardev_private *priv = param;
	complete(&priv->dma_completion);
}

static dma_addr_t calculate_device_addr(u32 tlb_id, u64 offset)
{
	// TODO: do this rite
	return (tlb_id * (1 << 20)) + offset;
}

long ioctl_dma(struct chardev_private *priv, struct tenstorrent_dma __user *arg)
{
	struct tenstorrent_dma in = {0};
	struct dma_chan *dma_chan;
	struct dma_slave_config dma_config = {0};
	struct dma_async_tx_descriptor *desc;
	enum dma_status status;
	dma_cap_mask_t mask;
	dma_cookie_t cookie;


	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	if (copy_from_user(&in, &arg->in, sizeof(in)) != 0)
		return -EFAULT;

	dma_chan = dma_request_channel(mask, NULL, NULL);
	if (!dma_chan) {
		pr_err("dma_request_channel failed\n");
		return -EBUSY;
	}

	dma_config.dst_addr = calculate_device_addr(in.in.tlb_id, in.in.offset);
	dma_config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dma_config.device_fc = false;

	int ret = dmaengine_slave_config(dma_chan, &dma_config);
	if (ret) {
		pr_err("dmaengine_slave_config failed\n");
		dma_release_channel(dma_chan);
		return ret;
	}

	if (!dma_chan || !dma_chan->device || !dma_chan->device->device_prep_slave_sg) {
		pr_err("dma_chan or device or device_prep_slave_sg is NULL\n");
	} else {
		pr_info("OK\n");
	}

	desc = dmaengine_prep_slave_single(dma_chan, in.in.iova, in.in.size, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
	if (!desc) {
		pr_err("dmaengine_prep_slave_single failed\n");
		dma_release_channel(dma_chan);
		return -EINVAL;
	}

	init_completion(&priv->dma_completion);
	desc->callback_param = priv;
	desc->callback = dma_complete_callback;

	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		pr_err("dmaengine_submit failed\n");
		dma_release_channel(dma_chan);
		return -EINVAL;
	}

	dma_async_issue_pending(dma_chan);

	if (wait_for_completion_timeout(&priv->dma_completion, msecs_to_jiffies(1000)) == 0) {
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
	pr_info("Finis\n");
	return 0;
}

