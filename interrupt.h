// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_INTERRUPT_H_INCLUDED
#define TTDRIVER_INTERRUPT_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;

bool tenstorrent_enable_interrupts(struct tenstorrent_device *tt_dev);
void tenstorrent_disable_interrupts(struct tenstorrent_device *tt_dev);

#endif
