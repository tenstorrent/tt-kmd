// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_CHARDEV_H_INCLUDED
#define TTDRIVER_CHARDEV_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;
struct tenstorrent_event;
struct work_struct;

extern int init_char_driver(unsigned int max_devices);
extern void cleanup_char_driver(void);

int tenstorrent_register_device(struct tenstorrent_device *gs_dev);
void tenstorrent_unregister_device(struct tenstorrent_device *gs_dev);
int tenstorrent_set_aggregated_power_state(struct tenstorrent_device *tt_dev);
void tenstorrent_power_down_work_func(struct work_struct *work);
void tenstorrent_push_event(struct tenstorrent_device *tt_dev, const struct tenstorrent_event *event);
void tenstorrent_push_driver_event(struct tenstorrent_device *tt_dev, u16 type, u32 data0);

#endif
