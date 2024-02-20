// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_HWMON_H_INCLUDED
#define TTDRIVER_HWMON_H_INCLUDED

#include <linux/hwmon.h>

// Sentinel for tt_hwmon_attr's reg_offset field.
#define TT_HWMON_ATTR_END 0xffffffff

struct tt_hwmon_attr {
	enum hwmon_sensor_types type;
	u32 attr;
	u32 reg_offset;	// Use TT_HWMON_ATTR_END here to indicate end of array.
	u32 shift;
	u32 mask;
	u32 multiplier;
	int channel;
};

struct tt_hwmon_label {
	enum hwmon_sensor_types type;
	u32 attr;
	const char *name;
	int channel;
};

struct tt_hwmon_context {
	struct tenstorrent_device *tt_dev;
	const struct tt_hwmon_label *labels;
	const struct tt_hwmon_attr *attributes;
	
	// telemetry_offset is relative to the start of ARC CSM for local reads;
	// for remote reads, add it to the NOC endpoint address for the remote ARC.
	u32 telemetry_offset;	// Relative to ARC_CSM_START

	u32 (*read32)(u64 offset, struct tt_hwmon_context *context, int channel);
};

extern const struct hwmon_ops tt_hwmon_ops;

#endif
