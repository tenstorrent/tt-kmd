// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_GRENDEL_H_INCLUDED
#define TTDRIVER_GRENDEL_H_INCLUDED

#include "device.h"

struct grendel_device {
	struct tenstorrent_device tt;
};

#define tt_dev_to_grendel_dev(tt_dev) container_of((tt_dev), struct grendel_device, tt)

#endif
