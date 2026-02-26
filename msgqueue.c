// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "msgqueue.h"

#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/pci.h>

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
