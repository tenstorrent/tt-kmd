// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

#include "util.h"

enum DeviceType
{
    Grayskull,
    Wormhole,
    Blackhole,
};

struct EnumeratedDevice
{
    std::string path;
    PciBusDeviceFunction location;
    dev_t node;
    bool iommu_translated;
    DeviceType type;
};

std::vector<EnumeratedDevice> EnumerateDevices(void);
