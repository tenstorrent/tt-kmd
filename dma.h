// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TENSTORRENT_DMA_H_INCLUDED
#define TENSTORRENT_DMA_H_INCLUDED

struct chardev_private;
struct tenstorrent_dma;

long ioctl_dma(struct chardev_private *priv, struct tenstorrent_dma __user *arg);

#endif
