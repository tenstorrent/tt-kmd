// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_NOC_BLOCK_IO_H_INCLUDED
#define TTDRIVER_NOC_BLOCK_IO_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;
struct tenstorrent_noc_block_io;

long tenstorrent_noc_block_io(struct tenstorrent_device *tt_dev, struct tenstorrent_noc_block_io __user *arg,
			      bool write);

#endif
