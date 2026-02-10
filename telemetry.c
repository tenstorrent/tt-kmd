// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/rwsem.h>
#include <linux/sysfs.h>

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

ssize_t tt_sysfs_show_u32_dec(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	u32 value;
	int r;

	r = tt_telemetry_read32(tt_dev, data->tag_id, &value);
	if (r)
		return r;

	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

ssize_t tt_sysfs_show_u64_hex(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	u32 hi, lo;
	int r;

	r = tt_telemetry_read32(tt_dev, data->tag_id, &hi);
	if (r)
		return r;

	r = tt_telemetry_read32(tt_dev, data->tag_id + 1, &lo);
	if (r)
		return r;

	return scnprintf(buf, PAGE_SIZE, "%08X%08X\n", hi, lo);
}

ssize_t tt_sysfs_show_u32_ver(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	u32 value;
	u32 major, minor, patch, ver;
	int r;

	r = tt_telemetry_read32(tt_dev, data->tag_id, &value);
	if (r)
		return r;

	// ETH firmware uses a different version packing.
	if (data->tag_id == TELEMETRY_ETH_FW_VERSION) {
		major = (value >> 16) & 0xFF;
		minor = (value >> 12) & 0xF;
		patch = (value >>  0) & 0xFFF;
		return scnprintf(buf, PAGE_SIZE, "%u.%u.%u\n", major, minor, patch);
	}

	major = (value >> 24) & 0xFF;
	minor = (value >> 16) & 0xFF;
	patch = (value >>  8) & 0xFF;
	ver = value & 0xFF;

	return scnprintf(buf, PAGE_SIZE, "%u.%u.%u.%u\n", major, minor, patch, ver);
}

ssize_t tt_sysfs_show_card_type(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	u32 value;
	u16 card_type;
	const char *card_name;
	int r;

	r = tt_telemetry_read32(tt_dev, data->tag_id, &value);
	if (r)
		return r;

	card_type = (value >> 4) & 0xFFFF;
	switch (card_type) {
	// Wormhole
	case 0x14: card_name = "n300"; break;
	case 0x18: card_name = "n150"; break;
	case 0x35: card_name = "galaxy-wormhole"; break;
	// Blackhole
	case 0x36: card_name = "p100"; break;
	case 0x40: card_name = "p150a"; break;
	case 0x41: card_name = "p150b"; break;
	case 0x42: card_name = "p150c"; break;
	case 0x43: card_name = "p100a"; break;
	case 0x44: card_name = "p300b"; break;
	case 0x45: card_name = "p300a"; break;
	case 0x46: card_name = "p300c"; break;
	case 0x47: card_name = "galaxy-blackhole"; break;
	default: card_name = "unknown"; break;
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", card_name);
}

umode_t tt_sysfs_telemetry_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct device_attribute *dev_attr = container_of(attr, struct device_attribute, attr);
	struct tenstorrent_sysfs_attr *ts_attr = container_of(dev_attr, struct tenstorrent_sysfs_attr, attr);
	bool visible;

	if (ts_attr->tag_id >= TELEM_TAG_CACHE_SIZE)
		return 0;

	visible = tt_dev->telemetry_tag_cache[ts_attr->tag_id] != 0;
	return visible ? attr->mode : 0;
}
