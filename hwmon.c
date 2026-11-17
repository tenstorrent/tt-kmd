// SPDX-License-Identifier: GPL-2.0-only
/*
 * hwmon device under the PCI device, fed from firmware telemetry:
 * in0 "vcore" (mV), curr1 "current" (mA), temp1 "asic_temp" (millidegree C),
 * power1 "power" (microwatt), with maxes when the firmware publishes them.
 */
#include <linux/hwmon.h>
#include <linux/math64.h>
#include <linux/jiffies.h>

#include "device.h"
#include "telemetry.h"
#include "hwmon.h"

/* Telemetry tag behind an attribute, or 0 if none. */
static unsigned int tt_hwmon_tag(enum hwmon_sensor_types type, u32 attr)
{
	switch (type) {
	case hwmon_in:
		if (attr == hwmon_in_input || attr == hwmon_in_label)
			return TT_TAG_VCORE;
		if (attr == hwmon_in_max)
			return TT_TAG_VDD_LIMITS;
		break;
	case hwmon_curr:
		if (attr == hwmon_curr_input || attr == hwmon_curr_label)
			return TT_TAG_TDC;
		if (attr == hwmon_curr_max)
			return TT_TAG_TDC_LIMIT_MAX;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return TT_TAG_ASIC_TEMPERATURE;
		if (attr == hwmon_temp_max)
			return TT_TAG_THM_LIMIT_THROTTLE;
		if (attr == hwmon_temp_crit)
			return TT_TAG_THM_LIMIT_SHUTDOWN;
		break;
	case hwmon_power:
		if (attr == hwmon_power_input || attr == hwmon_power_label)
			return TT_TAG_TDP;
		if (attr == hwmon_power_max)
			return TT_TAG_TDP_LIMIT_MAX;
		break;
	default:
		break;
	}
	return 0;
}

/* Tags behind the cache slots (TT_HWMON_CACHE_TAGS). */
static const unsigned int tt_hwmon_cache_tags[TT_HWMON_CACHE_TAGS] = {
	TT_TAG_VCORE, TT_TAG_VDD_LIMITS, TT_TAG_TDC, TT_TAG_TDC_LIMIT_MAX,
	TT_TAG_ASIC_TEMPERATURE, TT_TAG_THM_LIMIT_THROTTLE, TT_TAG_THM_LIMIT_SHUTDOWN,
	TT_TAG_TDP, TT_TAG_TDP_LIMIT_MAX,
};

/* Firmware refreshes telemetry every 100 ms: serve repeated reads from a
 * cache that young instead of a NOC round trip each. */
#define TT_HWMON_CACHE_AGE	(HZ / 10)

static int tt_hwmon_cached_read(struct tenstorrent_device *dev, unsigned int tag, u32 *val)
{
	struct tt_hwmon_cache *c = &dev->smc.hwmon_cache;
	int i, ret, idx;

	for (i = 0; i < TT_HWMON_CACHE_TAGS; i++)
		if (tt_hwmon_cache_tags[i] == tag)
			break;
	if (i == TT_HWMON_CACHE_TAGS)
		return -EOPNOTSUPP;

	mutex_lock(&c->lock);
	if (c->stamp[i] && time_before(jiffies, c->stamp[i] + TT_HWMON_CACHE_AGE)) {
		*val = c->val[i];
		mutex_unlock(&c->lock);
		return 0;
	}

	ret = tt_dev_enter(dev, &idx);
	if (!ret) {
		ret = tt_telemetry_read(dev, tag, val);
		tt_dev_exit(dev, idx);
	}
	if (!ret) {
		c->val[i] = *val;
		c->stamp[i] = jiffies ? jiffies : 1;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static umode_t tt_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	struct tenstorrent_device *dev = (struct tenstorrent_device *)data;
	unsigned int tag = tt_hwmon_tag(type, attr);

	return tag && tt_telemetry_has(dev, tag) ? 0444 : 0;
}

static int tt_hwmon_read(struct device *hdev, enum hwmon_sensor_types type, u32 attr,
			 int channel, long *val)
{
	struct tenstorrent_device *dev = dev_get_drvdata(hdev);
	unsigned int tag = tt_hwmon_tag(type, attr);
	u32 raw;
	int ret;

	if (!tag)
		return -EOPNOTSUPP;

	ret = tt_hwmon_cached_read(dev, tag, &raw);
	if (ret)
		return ret;
	if (raw == 0xffffffff)		/* unreachable tile or invalid marker */
		return -ENODATA;

	switch (tag) {
	case TT_TAG_VCORE:			/* mV */
		*val = raw;
		break;
	case TT_TAG_VDD_LIMITS:			/* [31:16] vdd_max mV */
		if (!(raw >> 16))
			return -ENODATA;
		*val = raw >> 16;
		break;
	case TT_TAG_TDC:			/* A -> mA */
	case TT_TAG_TDC_LIMIT_MAX:
		*val = (long)raw * 1000;
		break;
	case TT_TAG_ASIC_TEMPERATURE:		/* signed 16.16 degC -> mdegC */
		if (raw == 0x80000000)
			return -ENODATA;
		*val = (long)div_s64((s64)(s32)raw * 1000, 65536);
		break;
	case TT_TAG_THM_LIMIT_THROTTLE:		/* degC -> mdegC */
	case TT_TAG_THM_LIMIT_SHUTDOWN:
		*val = (long)raw * 1000;
		break;
	case TT_TAG_TDP:			/* W -> uW */
	case TT_TAG_TDP_LIMIT_MAX:
		*val = (long)raw * 1000000;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int tt_hwmon_read_string(struct device *hdev, enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_in:
		*str = "vcore";
		return 0;
	case hwmon_curr:
		*str = "current";
		return 0;
	case hwmon_temp:
		*str = "asic_temp";
		return 0;
	case hwmon_power:
		*str = "power";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops tt_hwmon_ops = {
	.is_visible = tt_hwmon_is_visible,
	.read = tt_hwmon_read,
	.read_string = tt_hwmon_read_string,
};

static const struct hwmon_channel_info *const tt_hwmon_info[] = {
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX | HWMON_T_CRIT),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX),
	NULL
};

static const struct hwmon_chip_info tt_hwmon_chip_info = {
	.ops = &tt_hwmon_ops,
	.info = tt_hwmon_info,
};

void tt_hwmon_setup(struct tenstorrent_device *dev)
{
	mutex_init(&dev->smc.hwmon_cache.lock);
}

void tt_hwmon_init(struct tenstorrent_device *dev)
{
	struct device *hdev;

	dev->smc.hwmon = NULL;
	if (!dev->smc.telemetry.valid)
		return;

	hdev = hwmon_device_register_with_info(&dev->pdev->dev, "blackhole", dev,
					       &tt_hwmon_chip_info, NULL);
	if (IS_ERR(hdev)) {
		dev_info(&dev->pdev->dev, "hwmon registration failed (%ld)\n", PTR_ERR(hdev));
		return;
	}
	dev->smc.hwmon = hdev;
}

void tt_hwmon_fini(struct tenstorrent_device *dev)
{
	if (dev->smc.hwmon) {
		hwmon_device_unregister(dev->smc.hwmon);
		dev->smc.hwmon = NULL;
	}
}
