// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_MSGQUEUE_H_INCLUDED
#define TTDRIVER_MSGQUEUE_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;

struct arc_msg {
	u32 header;
	u32 payload[7];
};

#define ARC_MSG_QUEUE_HEADER_SIZE 32
#define ARC_MSG_TIMEOUT_MS        100

#define ARC_MSG_QUEUE_REQ_WPTR(base) ((base) + 0x00)
#define ARC_MSG_QUEUE_RES_RPTR(base) ((base) + 0x04)
#define ARC_MSG_QUEUE_REQ_RPTR(base) ((base) + 0x10)
#define ARC_MSG_QUEUE_RES_WPTR(base) ((base) + 0x14)

bool arc_msg_push(struct tenstorrent_device *tt_dev, const struct arc_msg *msg, u32 queue_base, u32 num_entries);
bool arc_msg_pop(struct tenstorrent_device *tt_dev, struct arc_msg *msg, u32 queue_base, u32 num_entries);

#endif
