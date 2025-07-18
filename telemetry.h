// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_TELEMETRY_H_INCLUDED
#define TTDRIVER_TELEMETRY_H_INCLUDED

#include <linux/types.h>
#include <linux/device.h>

enum tt_telemetry_tags {
    TELEMETRY_BOARD_ID = 1,
    TELEMETRY_VCORE = 6,
    TELEMETRY_POWER = 7,
    TELEMETRY_CURRENT = 8,
    TELEMETRY_ASIC_TEMP = 11,
    TELEMETRY_AICLK = 14,
    TELEMETRY_AXICLK = 15,
    TELEMETRY_ARCCLK = 16,
    TELEMETRY_ETH_FW_VERSION = 24,
    TELEMETRY_BM_APP_FW_VERSION = 26,
    TELEMETRY_BM_BL_FW_VERSION = 27,
    TELEMETRY_FLASH_BUNDLE_VERSION = 28,
    TELEMETRY_CM_FW_VERSION = 29,
    TELEMETRY_FAN_RPM = 41,
    TELEMETRY_TT_FLASH_VERSION = 58,
    TELEMETRY_ASIC_ID = 61
};

struct tenstorrent_sysfs_attr {
    u32 tag_id;
    struct device_attribute attr;
};

#endif // TTDRIVER_TELEMETRY_H_INCLUDED

