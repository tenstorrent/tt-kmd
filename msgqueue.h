// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_MSGQUEUE_H_INCLUDED
#define TTDRIVER_MSGQUEUE_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;
struct chardev_private;

struct arc_msg {
	u32 header;
	u32 payload[7];
};

#define ARC_MSG_QUEUE_HEADER_SIZE 32
#define ARC_MSG_TIMEOUT_MS        1000

#define ARC_MSG_STATUS_HW_ERROR   0xFFFFFFFFu

#define ARC_MSG_QUEUE_REQ_WPTR(base) ((base) + 0x00)
#define ARC_MSG_QUEUE_RES_RPTR(base) ((base) + 0x04)
#define ARC_MSG_QUEUE_REQ_RPTR(base) ((base) + 0x10)
#define ARC_MSG_QUEUE_RES_WPTR(base) ((base) + 0x14)

// Non-blocking primitives for the ARC FW message queue.  Return 0 on success,
// -EAGAIN when the request queue is full (push) or no response is ready (pop),
// or another negative errno on a hardware access error.  The caller is
// responsible for serialization; see arc_msg_send_sync().
int arc_msg_try_push(struct tenstorrent_device *tt_dev, const struct arc_msg *msg, u32 queue_base, u32 num_entries);
int arc_msg_try_pop(struct tenstorrent_device *tt_dev, struct arc_msg *msg, u32 queue_base, u32 num_entries);

// Synchronous send used for kernel-internal messages.  Takes arc_msg_mutex,
// locates the queue, drains any stale response, pushes the request, triggers
// the ARC, and polls for the response.  Returns 0 on success (with the response
// in *msg), -EOPNOTSUPP when the device has no usable queue, -EREMOTEIO when
// the firmware reports a nonzero status, or another negative errno.
int arc_msg_send_sync(struct tenstorrent_device *tt_dev, struct arc_msg *msg);

// Asynchronous per-fd messaging.  Both require arc_msg_mutex to be held by the
// caller.  arc_msg_pump advances the shared queue: it collects the in-flight
// response (if ready) and submits the next queued request.  It returns
// -EOPNOTSUPP when the device has no usable queue, otherwise 0.  arc_msg_abandon
// cancels the calling fd's outstanding message.
int arc_msg_pump(struct tenstorrent_device *tt_dev);
void arc_msg_abandon(struct chardev_private *priv);

#endif
