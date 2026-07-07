// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "msgqueue.h"

#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/pci.h>

#include "device.h"

int arc_msg_try_push(struct tenstorrent_device *tt_dev, const struct arc_msg *msg, u32 queue_base, u32 num_entries)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	u32 request_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE;
	u32 wptr;
	u32 rptr;
	u32 num_occupied;
	u32 slot;
	u32 req_offset;
	int i;

	if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_REQ_WPTR(queue_base), &wptr) != 0)
		return -EIO;

	if (wptr == U32_MAX) {
		dev_err(&tt_dev->pdev->dev, "ARC queue WPTR read returned all-1s; device gone?\n");
		return -EIO;
	}

	if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_REQ_RPTR(queue_base), &rptr) != 0)
		return -EIO;

	if (rptr == U32_MAX) {
		dev_err(&tt_dev->pdev->dev, "ARC queue RPTR read returned all-1s; device gone?\n");
		return -EIO;
	}

	num_occupied = (wptr - rptr) % (2 * num_entries);
	if (num_occupied >= num_entries)
		return -EAGAIN;

	slot = wptr % num_entries;
	req_offset = slot * sizeof(struct arc_msg);
	for (i = 0; i < 8; ++i) {
		u32 addr = request_base + req_offset + (i * sizeof(u32));
		u32 value = (i == 0) ? msg->header : msg->payload[i - 1];

		if (cls->csm_write32(tt_dev, addr, value) != 0)
			return -EIO;
	}

	wptr = (wptr + 1) % (2 * num_entries);
	if (cls->csm_write32(tt_dev, ARC_MSG_QUEUE_REQ_WPTR(queue_base), wptr) != 0)
		return -EIO;

	return 0;
}

int arc_msg_try_pop(struct tenstorrent_device *tt_dev, struct arc_msg *msg, u32 queue_base, u32 num_entries)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	u32 response_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE + (num_entries * sizeof(struct arc_msg));
	u32 rptr;
	u32 wptr;
	u32 num_occupied;
	u32 slot;
	u32 response_offset;
	int i;

	if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_RES_RPTR(queue_base), &rptr) != 0)
		return -EIO;

	if (rptr == U32_MAX) {
		dev_err(&tt_dev->pdev->dev, "ARC queue RPTR read returned all-1s; device gone?\n");
		return -EIO;
	}

	if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_RES_WPTR(queue_base), &wptr) != 0)
		return -EIO;

	if (wptr == U32_MAX) {
		dev_err(&tt_dev->pdev->dev, "ARC queue WPTR read returned all-1s; device gone?\n");
		return -EIO;
	}

	num_occupied = (wptr - rptr) % (2 * num_entries);
	if (num_occupied == 0)
		return -EAGAIN;

	slot = rptr % num_entries;
	response_offset = slot * sizeof(struct arc_msg);
	if (cls->csm_read32(tt_dev, response_base + response_offset, &msg->header) != 0)
		return -EIO;

	for (i = 0; i < 7; ++i) {
		u32 addr = response_base + response_offset + ((i + 1) * sizeof(u32));

		if (cls->csm_read32(tt_dev, addr, &msg->payload[i]) != 0)
			return -EIO;
	}

	rptr = (rptr + 1) % (2 * num_entries);
	if (cls->csm_write32(tt_dev, ARC_MSG_QUEUE_RES_RPTR(queue_base), rptr) != 0)
		return -EIO;

	return 0;
}

int arc_msg_send_sync(struct tenstorrent_device *tt_dev, struct arc_msg *msg)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	struct arc_msg drain;
	u32 queue_base;
	u32 num_entries;
	unsigned long timeout;
	int ret;

	if (!cls->arc_msg_locate_queue || !cls->arc_msg_trigger)
		return -EOPNOTSUPP;

	mutex_lock(&tt_dev->arc_msg_mutex);

	ret = cls->arc_msg_locate_queue(tt_dev, &queue_base, &num_entries);
	if (ret != 0)
		goto out;

	// A zero-length queue would divide by zero in the ring math below.
	if (num_entries == 0) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	// Discard any stale response left behind by a previous exchange.
	while (arc_msg_try_pop(tt_dev, &drain, queue_base, num_entries) == 0)
		;

	ret = arc_msg_try_push(tt_dev, msg, queue_base, num_entries);
	if (ret != 0)
		goto out;

	cls->arc_msg_trigger(tt_dev);

	timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS);
	for (;;) {
		ret = arc_msg_try_pop(tt_dev, msg, queue_base, num_entries);
		if (ret != -EAGAIN)
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(&tt_dev->pdev->dev, "Timeout waiting for ARC response\n");
			ret = -ETIMEDOUT;
			break;
		}

		usleep_range(100, 200);
	}

	if (ret == 0 && msg->header != 0)
		ret = -EREMOTEIO;

out:
	mutex_unlock(&tt_dev->arc_msg_mutex);
	return ret;
}
