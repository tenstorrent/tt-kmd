// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "msgqueue.h"

#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/pci.h>

#include "chardev_private.h"
#include "device.h"

bool arc_msg_push(struct tenstorrent_device *tt_dev, const struct arc_msg *msg, u32 queue_base, u32 num_entries)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	u32 request_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE;
	unsigned long timeout;
	u32 wptr;
	u32 slot;
	u32 req_offset;
	int i;

	if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_REQ_WPTR(queue_base), &wptr) != 0)
		return false;

	timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS);
	for (;;) {
		u32 rptr;
		u32 num_occupied;

		if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_REQ_RPTR(queue_base), &rptr) != 0)
			return false;

		num_occupied = (wptr - rptr) % (2 * num_entries);
		if (num_occupied < num_entries)
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(&tt_dev->pdev->dev, "Timeout waiting for space in ARC message queue\n");
			return false;
		}

		usleep_range(100, 200);
	}

	slot = wptr % num_entries;
	req_offset = slot * sizeof(struct arc_msg);
	for (i = 0; i < 8; ++i) {
		u32 addr = request_base + req_offset + (i * sizeof(u32));
		u32 value = (i == 0) ? msg->header : msg->payload[i - 1];

		if (cls->csm_write32(tt_dev, addr, value) != 0)
			return false;
	}

	wptr = (wptr + 1) % (2 * num_entries);
	if (cls->csm_write32(tt_dev, ARC_MSG_QUEUE_REQ_WPTR(queue_base), wptr) != 0)
		return false;

	return true;
}

bool arc_msg_pop(struct tenstorrent_device *tt_dev, struct arc_msg *msg, u32 queue_base, u32 num_entries)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	u32 response_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE + (num_entries * sizeof(struct arc_msg));
	unsigned long timeout;
	u32 rptr;
	u32 slot;
	u32 response_offset;
	int i;

	if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_RES_RPTR(queue_base), &rptr) != 0)
		return false;

	timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS);
	for (;;) {
		u32 wptr;
		u32 num_occupied;

		if (cls->csm_read32(tt_dev, ARC_MSG_QUEUE_RES_WPTR(queue_base), &wptr) != 0)
			return false;

		num_occupied = (wptr - rptr) % (2 * num_entries);
		if (num_occupied > 0)
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(&tt_dev->pdev->dev, "Timeout waiting for ARC response\n");
			return false;
		}

		usleep_range(100, 200);
	}

	slot = rptr % num_entries;
	response_offset = slot * sizeof(struct arc_msg);
	if (cls->csm_read32(tt_dev, response_base + response_offset, &msg->header) != 0)
		return false;

	for (i = 0; i < 7; ++i) {
		u32 addr = response_base + response_offset + ((i + 1) * sizeof(u32));

		if (cls->csm_read32(tt_dev, addr, &msg->payload[i]) != 0)
			return false;
	}

	rptr = (rptr + 1) % (2 * num_entries);
	if (cls->csm_write32(tt_dev, ARC_MSG_QUEUE_RES_RPTR(queue_base), rptr) != 0)
		return false;

	return true;
}

int arc_msg_try_push(struct tenstorrent_device *tt_dev, const struct arc_msg *msg)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	u32 queue_base = tt_dev->arc_msg_queue_base;
	u32 num_entries = tt_dev->arc_msg_num_entries;
	u32 request_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE;
	u32 wptr, rptr, num_occupied;
	u32 slot, req_offset;
	int i, ret;

	if (queue_base == 0)
		return -EOPNOTSUPP;

	ret = cls->csm_read32(tt_dev, ARC_MSG_QUEUE_REQ_WPTR(queue_base), &wptr);
	if (ret)
		return ret;

	ret = cls->csm_read32(tt_dev, ARC_MSG_QUEUE_REQ_RPTR(queue_base), &rptr);
	if (ret)
		return ret;

	num_occupied = (wptr - rptr) % (2 * num_entries);
	if (num_occupied >= num_entries)
		return -EAGAIN;

	slot = wptr % num_entries;
	req_offset = slot * sizeof(struct arc_msg);
	for (i = 0; i < 8; ++i) {
		u32 addr = request_base + req_offset + (i * sizeof(u32));
		u32 value = (i == 0) ? msg->header : msg->payload[i - 1];

		ret = cls->csm_write32(tt_dev, addr, value);
		if (ret)
			return ret;
	}

	wptr = (wptr + 1) % (2 * num_entries);
	return cls->csm_write32(tt_dev, ARC_MSG_QUEUE_REQ_WPTR(queue_base), wptr);
}

int arc_msg_try_pop(struct tenstorrent_device *tt_dev, struct arc_msg *msg)
{
	const struct tenstorrent_device_class *cls = tt_dev->dev_class;
	u32 queue_base = tt_dev->arc_msg_queue_base;
	u32 num_entries = tt_dev->arc_msg_num_entries;
	u32 response_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE + (num_entries * sizeof(struct arc_msg));
	u32 rptr, wptr, num_occupied;
	u32 slot, response_offset;
	int i, ret;

	if (queue_base == 0)
		return -EOPNOTSUPP;

	ret = cls->csm_read32(tt_dev, ARC_MSG_QUEUE_RES_RPTR(queue_base), &rptr);
	if (ret)
		return ret;

	ret = cls->csm_read32(tt_dev, ARC_MSG_QUEUE_RES_WPTR(queue_base), &wptr);
	if (ret)
		return ret;

	num_occupied = (wptr - rptr) % (2 * num_entries);
	if (num_occupied == 0)
		return -EAGAIN;

	slot = rptr % num_entries;
	response_offset = slot * sizeof(struct arc_msg);
	ret = cls->csm_read32(tt_dev, response_base + response_offset, &msg->header);
	if (ret)
		return ret;

	for (i = 0; i < 7; ++i) {
		u32 addr = response_base + response_offset + ((i + 1) * sizeof(u32));

		ret = cls->csm_read32(tt_dev, addr, &msg->payload[i]);
		if (ret)
			return ret;
	}

	rptr = (rptr + 1) % (2 * num_entries);
	return cls->csm_write32(tt_dev, ARC_MSG_QUEUE_RES_RPTR(queue_base), rptr);
}

// Abandon any outstanding ARC message for this fd.  Must be called with
// arc_msg_mutex held.  Safe to call in any state (no-op if IDLE).
void arc_msg_abandon(struct tenstorrent_device *tt_dev, struct chardev_msg *msg)
{
	switch (msg->state) {
	case ARC_MSG_IDLE:
		break;
	case ARC_MSG_QUEUED:
		list_del_init(&msg->link);
		msg->state = ARC_MSG_IDLE;
		break;
	case ARC_MSG_SUBMITTED:
		// Message is in the FW queue.  We can't pull it back, so clear
		// the inflight pointer and let the pump discard the response
		// when it arrives.  The fd is free to POST a new message
		// immediately.
		tt_dev->arc_msg_inflight = NULL;
		tt_dev->arc_msg_inflight_abandoned = true;
		msg->state = ARC_MSG_IDLE;
		break;
	case ARC_MSG_COMPLETED:
		msg->state = ARC_MSG_IDLE;
		break;
	}
}

// Pump the ARC message queue.  Must be called with arc_msg_mutex held.
//
// The driver keeps at most one message in the FW queue at a time.  This
// simplifies response matching: the next response always belongs to
// arc_msg_inflight.
//
// Step 1: if a message is in the FW queue, try to collect its response.
//   - No response yet (-EAGAIN): nothing to do, return.
//   - Response arrived: deliver it to the owning chardev_msg, or discard
//     it if the user called ABANDON while it was in flight.
//   - CSM error: the queue is in an unknown state.  Mark the message
//     completed with a synthetic error header and clear the inflight slot
//     so the queue can make progress.
//
// Step 2: if the FW queue is free and the SW queue is non-empty, take the
//   head of the SW queue, push it to the FW queue, trigger the ARC
//   interrupt, and mark it SUBMITTED.
void arc_msg_pump(struct tenstorrent_device *tt_dev)
{
	struct chardev_msg *inflight = tt_dev->arc_msg_inflight;
	struct arc_msg response;
	struct chardev_msg *next;
	int ret;

	// Step 1: try to collect a response for the message in the FW queue.
	// The FW queue is occupied if either inflight points to an owner or
	// the abandoned flag is set (owner detached, response still pending).
	if (inflight || tt_dev->arc_msg_inflight_abandoned) {
		ret = arc_msg_try_pop(tt_dev, &response);

		// FW hasn't responded yet — nothing more we can do this cycle.
		if (ret == -EAGAIN)
			return;

		if (ret)
			dev_err(&tt_dev->pdev->dev, "ARC message queue CSM error: %d\n", ret);

		if (tt_dev->arc_msg_inflight_abandoned) {
			// User called ABANDON (or closed the fd) while this message was in
			// FW. The response (if any) is discarded. inflight is already NULL
			// (cleared by the abandon operation).
			tt_dev->arc_msg_inflight_abandoned = false;
		} else if (ret == 0) {
			// Normal completion — deliver the FW response.
			inflight->response = response;
			inflight->state = ARC_MSG_COMPLETED;
			tt_dev->arc_msg_inflight = NULL;
		} else {
			// CSM error. We don't have a real response, but we must move the
			// message to COMPLETED so POLL can report the failure rather than
			// hanging on -EAGAIN.
			inflight->response.header = 0xFFFFFFFF;
			inflight->state = ARC_MSG_COMPLETED;
			tt_dev->arc_msg_inflight = NULL;
		}
	}

	// Step 2: submit the next queued message to the FW.
	if (list_empty(&tt_dev->arc_msg_queue))
		return;

	next = list_first_entry(&tt_dev->arc_msg_queue, struct chardev_msg, link);

	ret = arc_msg_try_push(tt_dev, &next->request);
	if (ret)
		return;

	tt_dev->dev_class->arc_msg_trigger(tt_dev);

	list_del_init(&next->link);
	next->state = ARC_MSG_SUBMITTED;
	tt_dev->arc_msg_inflight = next;
}
