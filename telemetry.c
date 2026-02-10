// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/rwsem.h>

#include "device.h"

int tt_telemetry_read32(struct tenstorrent_device *tt_dev, u16 tag_id, u32 *value)
{
	int r;

	if (tag_id >= TELEM_TAG_CACHE_SIZE)
		return -EINVAL;

	down_read(&tt_dev->reset_rwsem);

	if (tt_dev->detached) {
		r = -ENODEV;
		goto out;
	}

	if (tt_dev->needs_hw_init) {
		r = -ENODATA;
		goto out;
	}

	r = tt_dev->dev_class->read_telemetry_tag(tt_dev, tag_id, value);

out:
	up_read(&tt_dev->reset_rwsem);
	return r;
}
