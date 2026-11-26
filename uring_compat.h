// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// io_uring command support across kernel versions.
//
// DMA over io_uring needs 6.6 or newer. On older kernels, or without
// CONFIG_IO_URING, the driver has no uring_cmd handler and io_uring itself
// fails the command with -EOPNOTSUPP.

#ifndef TTDRIVER_URING_COMPAT_H_INCLUDED
#define TTDRIVER_URING_COMPAT_H_INCLUDED

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && IS_ENABLED(CONFIG_IO_URING)
#define TT_HAVE_URING_CMD 1
#else
#define TT_HAVE_URING_CMD 0
#endif

#if TT_HAVE_URING_CMD

// 6.8 moved the uring_cmd API out of linux/io_uring.h.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
#include <linux/io_uring/cmd.h>
#else
#include <linux/io_uring.h>
#endif

// The command's payload in the SQE. 7.0 made io_uring_sqe_cmd() take its type.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define tt_uring_cmd_payload(cmd, type) io_uring_sqe_cmd((cmd)->sqe, type)
#else
#define tt_uring_cmd_payload(cmd, type) ((const type *)io_uring_sqe_cmd((cmd)->sqe))
#endif

// Post the CQE. 6.18 dropped res2 from io_uring_cmd_done().
static inline void tt_uring_cmd_done(struct io_uring_cmd *cmd, s32 ret, unsigned int issue_flags)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	io_uring_cmd_done(cmd, ret, issue_flags);
#else
	io_uring_cmd_done(cmd, ret, 0, issue_flags);
#endif
}

// Define name() as a callback for io_uring_cmd_complete_in_task() that calls
// fn(cmd, issue_flags) in task context. 6.19 changed the callback's signature.
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

#endif // TT_HAVE_URING_CMD

#endif
