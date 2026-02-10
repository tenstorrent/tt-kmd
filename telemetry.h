// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_TELEMETRY_H_INCLUDED
#define TTDRIVER_TELEMETRY_H_INCLUDED

#include <linux/types.h>
#include <linux/device.h>

// Maximum number of tag IDs in the per-device tag-to-address cache.
// Tag IDs are small integers (currently up to 64); 128 gives comfortable headroom.
#define TELEM_TAG_CACHE_SIZE 128

enum tt_telemetry_tags {
    TELEMETRY_BOARD_ID = 1,
    TELEMETRY_VCORE = 6,
    TELEMETRY_POWER = 7,
    TELEMETRY_CURRENT = 8,
    TELEMETRY_VDD_LIMITS = 9,
    TELEMETRY_THM_LIMIT_SHUTDOWN = 10,
    TELEMETRY_ASIC_TEMP = 11,
    TELEMETRY_AICLK = 14,
    TELEMETRY_AXICLK = 15,
    TELEMETRY_ARCCLK = 16,
    TELEMETRY_ETH_FW_VERSION = 24,
    TELEMETRY_BM_APP_FW_VERSION = 26,
    TELEMETRY_BM_BL_FW_VERSION = 27,
    TELEMETRY_FLASH_BUNDLE_VERSION = 28,
    TELEMETRY_CM_FW_VERSION = 29,
    TELEMETRY_FAN_SPEED = 31,
    TELEMETRY_TIMER_HEARTBEAT = 32,
    TELEMETRY_FAN_RPM = 41,
    TELEMETRY_TDC_LIMIT_MAX = 55,
    TELEMETRY_THM_LIMIT_THROTTLE = 56,
    TELEMETRY_TT_FLASH_VERSION = 58,
    TELEMETRY_THERM_TRIP_COUNT = 60,
    TELEMETRY_ASIC_ID = 61,
    TELEMETRY_AICLK_LIMIT_MAX = 63,
    TELEMETRY_TDP_LIMIT_MAX = 64,
};

struct tenstorrent_sysfs_attr {
    u32 tag_id;
    struct device_attribute attr;
};

struct tenstorrent_device;
int tt_telemetry_read32(struct tenstorrent_device *tt_dev, u16 tag_id, u32 *value);

// Common sysfs show callbacks for telemetry attributes.
ssize_t tt_sysfs_show_u32_dec(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t tt_sysfs_show_u64_hex(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t tt_sysfs_show_u32_ver(struct device *dev, struct device_attribute *attr, char *buf);
ssize_t tt_sysfs_show_card_type(struct device *dev, struct device_attribute *attr, char *buf);
umode_t tt_sysfs_telemetry_is_visible(struct kobject *kobj, struct attribute *attr, int n);

#define ARC_CSM_BASE 0x10000000
#define ARC_CSM_SIZE (1 << 19)
static inline bool is_range_within_csm(u64 addr, size_t len)
{
	return (addr >= ARC_CSM_BASE) && (addr <= (ARC_CSM_BASE + ARC_CSM_SIZE) - len);
}

#endif // TTDRIVER_TELEMETRY_H_INCLUDED

