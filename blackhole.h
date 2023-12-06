// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_BLACKHOLE_H_INCLUDED
#define TTDRIVER_BLACKHOLE_H_INCLUDED

#include <linux/types.h>
#include "device.h"

struct blackhole_device {
	struct tenstorrent_device tt;
};

#define tt_dev_to_wh_dev(ttdev) \
	container_of((tt_dev), struct blackhole_device, tt)

#endif
