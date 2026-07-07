// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "msgqueue.h"

#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/pci.h>

#include "device.h"
#include "chardev_private.h"

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

// Hand the in-flight response to its owning fd (or drop it if abandoned) and
// mark the FW queue free.  Caller holds arc_msg_mutex.
static void arc_msg_deliver(struct tenstorrent_device *tt_dev, const struct arc_msg *resp)
{
	struct chardev_private *owner = tt_dev->arc_msg_inflight;

	if (owner && !tt_dev->arc_msg_inflight_abandoned) {
		owner->arc_msg.buf = *resp;
		owner->arc_msg.state = CHARDEV_MSG_COMPLETED;
	}

	tt_dev->arc_msg_inflight_abandoned = false;
	tt_dev->arc_msg_inflight = NULL;
}

// Block until the in-flight message (if any) completes, delivering its response
// to the owning fd, so the FW queue is free for a synchronous kernel message to
// jump ahead of the queued user messages.  Caller holds arc_msg_mutex.
static void arc_msg_flush_inflight(struct tenstorrent_device *tt_dev, u32 queue_base, u32 num_entries)
{
	struct arc_msg resp;
	unsigned long timeout;
	int ret;

	if (!tt_dev->arc_msg_inflight)
		return;

	timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS);
	for (;;) {
		ret = arc_msg_try_pop(tt_dev, &resp, queue_base, num_entries);
		if (ret == 0)
			break;

		if (ret != -EAGAIN || time_after(jiffies, timeout)) {
			memset(&resp, 0, sizeof(resp));
			resp.header = ARC_MSG_STATUS_HW_ERROR;
			break;
		}

		usleep_range(100, 200);
	}

	arc_msg_deliver(tt_dev, &resp);
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

	// Let any outstanding user message finish so we don't consume its
	// response, then discard anything stale left behind by a prior exchange.
	arc_msg_flush_inflight(tt_dev, queue_base, num_entries);
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

int arc_msg_pump(struct tenstorrent_device *tt_dev)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	struct chardev_private *next;
	struct arc_msg resp;
	u32 queue_base;
	u32 num_entries;
	int ret;

	lockdep_assert_held(&tt_dev->arc_msg_mutex);

	if (!cls->arc_msg_locate_queue || !cls->arc_msg_trigger)
		return -EOPNOTSUPP;

	ret = cls->arc_msg_locate_queue(tt_dev, &queue_base, &num_entries);
	if (ret == -EOPNOTSUPP)
		return -EOPNOTSUPP;

	// Transient errors (FW not ready) leave queued messages in place to be
	// retried by a later pump.
	if (ret != 0 || num_entries == 0)
		return 0;

	// Collect the in-flight response if it has arrived.  While it hasn't,
	// the FW queue is busy and we cannot submit the next message.
	if (tt_dev->arc_msg_inflight) {
		ret = arc_msg_try_pop(tt_dev, &resp, queue_base, num_entries);
		if (ret == -EAGAIN)
			return 0;

		if (ret != 0) {
			memset(&resp, 0, sizeof(resp));
			resp.header = ARC_MSG_STATUS_HW_ERROR;
		}

		arc_msg_deliver(tt_dev, &resp);
	}

	// Submit the head of the SW queue, if any.
	if (!list_empty(&tt_dev->arc_msg_queue)) {
		next = list_first_entry(&tt_dev->arc_msg_queue, struct chardev_private, arc_msg.queue_node);

		ret = arc_msg_try_push(tt_dev, &next->arc_msg.buf, queue_base, num_entries);
		if (ret == -EAGAIN)
			return 0;

		list_del_init(&next->arc_msg.queue_node);

		if (ret != 0) {
			// Can't submit; complete with a failure so POLL reports it.
			next->arc_msg.buf.header = ARC_MSG_STATUS_HW_ERROR;
			next->arc_msg.state = CHARDEV_MSG_COMPLETED;
			return 0;
		}

		next->arc_msg.state = CHARDEV_MSG_SUBMITTED;
		tt_dev->arc_msg_inflight = next;
		cls->arc_msg_trigger(tt_dev);
	}

	return 0;
}

void arc_msg_abandon(struct chardev_private *priv)
{
	struct tenstorrent_device *tt_dev = priv->device;

	lockdep_assert_held(&tt_dev->arc_msg_mutex);

	switch (priv->arc_msg.state) {
	case CHARDEV_MSG_QUEUED:
		list_del_init(&priv->arc_msg.queue_node);
		break;
	case CHARDEV_MSG_SUBMITTED:
		// The response is still coming; let the pump discard it.  This
		// leaves tt_dev->arc_msg_inflight pointing at priv, but the
		// abandoned flag ensures the pump never dereferences it, so priv
		// is safe to free after this returns.
		if (tt_dev->arc_msg_inflight == priv)
			tt_dev->arc_msg_inflight_abandoned = true;
		break;
	default:
		break;
	}

	priv->arc_msg.state = CHARDEV_MSG_IDLE;
}
