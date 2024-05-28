// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/stat.h>
#include <asm/io.h>

#include "device.h"
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

ssize_t tt_show_attribute(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tt_attribute_data *data = container_of(attr, struct tt_attribute_data, attr);
	struct tt_hwmon_context *ctx = &tt_dev->hwmon_context;
	u32 value = ioread32(ctx->telemetry_base + data->reg_offset);
	value &= data->mask;
	return sprintf(buf, "%u\n", value);
}

ssize_t tt_show_card_type(struct device *dev, struct device_attribute *attr, char *buf) {
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tt_attribute_data *data = container_of(attr, struct tt_attribute_data, attr);
	struct tt_hwmon_context *ctx = &tt_dev->hwmon_context;

	u32 board_id_hi = ioread32(ctx->telemetry_base + data->reg_offset);
	u16 card_type = (board_id_hi >> 4) & 0xFFFF;
	char *card_name;
	switch (card_type) {
	case 0x3: card_name = "e150"; break;
	case 0x7: card_name = "e75"; break;
	case 0x14: card_name = "n300"; break;
	case 0x18: card_name = "n150"; break;
	default: card_name = "unknown"; break;
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", card_name);
}

ssize_t tt_show_card_serial(struct device *dev, struct device_attribute *attr, char *buf) {
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tt_attribute_data *data = container_of(attr, struct tt_attribute_data, attr);
	struct tt_hwmon_context *ctx = &tt_dev->hwmon_context;

	u32 board_id_hi = ioread32(ctx->telemetry_base + data->reg_offset);
	u32 board_id_lo = ioread32(ctx->telemetry_base + data->reg_offset + 4);
	return scnprintf(buf, PAGE_SIZE, "%08X%08X\n", board_id_hi, board_id_lo);
}

ssize_t tt_show_fw_ver(struct device *dev, struct device_attribute *attr, char *buf) {
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tt_attribute_data *data = container_of(attr, struct tt_attribute_data, attr);
	struct tt_hwmon_context *ctx = &tt_dev->hwmon_context;

	u32 fw_ver = ioread32(ctx->telemetry_base + data->reg_offset);
	u32 major = (fw_ver >> 24) & 0xFF;
	u32 minor = (fw_ver >> 16) & 0xFF;
	u32 patch = (fw_ver >>  8) & 0xFF;
	u32 ver = fw_ver & 0xFF;
	return scnprintf(buf, PAGE_SIZE, "%u.%u.%u.%u\n", major, minor, patch, ver);
}

ssize_t tt_show_eth_fw_ver(struct device *dev, struct device_attribute *attr, char *buf) {
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tt_attribute_data *data = container_of(attr, struct tt_attribute_data, attr);
	struct tt_hwmon_context *ctx = &tt_dev->hwmon_context;

	u32 fw_ver = ioread32(ctx->telemetry_base + data->reg_offset);
	u32 major = (fw_ver >> 16) & 0xFF;
	u32 minor = (fw_ver >> 12) & 0xF;
	u32 patch = (fw_ver >>  0) & 0xFFF;
	return scnprintf(buf, PAGE_SIZE, "%u.%u.%u\n", major, minor, patch);
}