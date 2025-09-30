// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_CHARDEV_H_INCLUDED
#define TTDRIVER_CHARDEV_H_INCLUDED

#include "ioctl.h"

// Power features that the driver actively manages
#define TT_KMD_MANAGED_POWER_FLAGS_COUNT    2  // TT_POWER_FLAG_MAX_AI_CLK, TT_POWER_FLAG_MRISC_PHY_WAKEUP
#define TT_KMD_MANAGED_POWER_SETTINGS_COUNT 0  // None currently
#define TT_KMD_MANAGED_POWER_VALIDITY    TT_POWER_VALIDITY(TT_KMD_MANAGED_POWER_FLAGS_COUNT, TT_KMD_MANAGED_POWER_SETTINGS_COUNT)

struct tenstorrent_device;

extern int init_char_driver(unsigned int max_devices);
extern void cleanup_char_driver(void);

int tenstorrent_register_device(struct tenstorrent_device *gs_dev);
void tenstorrent_unregister_device(struct tenstorrent_device *gs_dev);

#endif
