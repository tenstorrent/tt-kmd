// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_HWMON_H_INCLUDED
#define TTDRIVER_HWMON_H_INCLUDED

#include <linux/hwmon.h>
#include <linux/device.h>

// Sentinel for tt_hwmon_attr's reg_offset field.
#define TT_HWMON_ATTR_END 0xffffffff

struct tt_hwmon_attr {
	enum hwmon_sensor_types type;
	u32 attr;
	u32 reg_offset;	// Use TT_HWMON_ATTR_END here to indicate end of array.
	u32 shift;
	u32 mask;
	u32 multiplier;
	u32 divisor;
};

struct tt_hwmon_label {
	enum hwmon_sensor_types type;
	u32 attr;
	const char *name;
};

struct tt_hwmon_context {
	const struct tt_hwmon_label *labels;
	const struct tt_hwmon_attr *attributes;
	u8 __iomem *telemetry_base;
};

extern const struct hwmon_ops tt_hwmon_ops;

#endif
