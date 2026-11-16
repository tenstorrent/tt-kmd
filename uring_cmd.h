// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_URING_CMD_H_INCLUDED
#define TTDRIVER_URING_CMD_H_INCLUDED

#include <linux/version.h>

// io_uring driver commands exist since 5.19, but their kernel API kept changing.
// 6.6, the oldest LTS that passes the whole SQE and takes issue_flags in every
// callback, is the oldest one supported.
#define TT_URING_CMD_SUPPORTED \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && IS_ENABLED(CONFIG_IO_URING))

#if TT_URING_CMD_SUPPORTED

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#include <linux/io_uring/cmd.h>
#else
#include <linux/io_uring.h>
#endif

// 7.0 turned io_uring_sqe_cmd() into a macro that takes the payload type.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define tt_uring_cmd_payload(cmd, type) io_uring_sqe_cmd((cmd)->sqe, type)
#else
#define tt_uring_cmd_payload(cmd, type) ((const type *)io_uring_sqe_cmd((cmd)->sqe))
#endif

// 6.18 dropped the second result from io_uring_cmd_done().
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define tt_uring_cmd_done(cmd, ret, issue_flags) io_uring_cmd_done(cmd, ret, issue_flags)
#else
#define tt_uring_cmd_done(cmd, ret, issue_flags) io_uring_cmd_done(cmd, ret, 0, issue_flags)
#endif

// Defines name() for io_uring_cmd_complete_in_task(), calling fn(cmd, issue_flags).
// 6.19 changed the callback signature.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
#define TT_URING_CMD_TASK_WORK(name, fn)					\
	static void name(struct io_tw_req tw_req, io_tw_token_t tw)		\
	{									\
		fn(io_uring_cmd_from_tw(tw_req), IO_URING_CMD_TASK_WORK_ISSUE_FLAGS); \
	}
#else
#define TT_URING_CMD_TASK_WORK(name, fn)					\
	static void name(struct io_uring_cmd *cmd, unsigned int issue_flags)	\
	{									\
		fn(cmd, issue_flags);						\
	}
#endif

#endif // TT_URING_CMD_SUPPORTED

#endif
