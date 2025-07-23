// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <asm/io.h>

#include "hwmon.h"

static umode_t tt_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel) {
	const struct tt_hwmon_context *ctx = drvdata;
	const struct tt_hwmon_attr *attribute = ctx->attributes;
	const struct tt_hwmon_label *label = ctx->labels;

	while (attribute->reg_offset != TT_HWMON_ATTR_END) {
		if (attribute->type == type && attribute->attr == attr)
			return S_IRUGO;
		attribute++;
	}

	while (label->name != NULL) {
		if (label->type == type && label->attr == attr)
			return S_IRUGO;
		label++;
	}

	return 0;
}

static int tt_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val) {
	const struct tt_hwmon_context *ctx = dev_get_drvdata(dev);
	const struct tt_hwmon_attr *attribute = ctx->attributes;

	while (attribute->reg_offset != TT_HWMON_ATTR_END) {
		if (attribute->type == type && attribute->attr == attr) {
			u32 value = ioread32(ctx->telemetry_base + attribute->reg_offset);
			value >>= attribute->shift;
			value &= attribute->mask;
			value *= attribute->multiplier;
			value /= attribute->divisor;
			*val = value;
			return 0;
		}
		attribute++;
	}

	return -EOPNOTSUPP;
}

static int tt_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, const char **str) {
	const struct tt_hwmon_context *ctx = dev_get_drvdata(dev);
	const struct tt_hwmon_label *label = ctx->labels;

	while (label->name != NULL) {
		if (label->type == type && label->attr == attr) {
			*str = label->name;
			return 0;
		}
		label++;
	}

	return -EOPNOTSUPP;
}

const struct hwmon_ops tt_hwmon_ops = {
	.is_visible = tt_hwmon_is_visible,
	.read = tt_hwmon_read,
	.read_string = tt_hwmon_read_string,
};
