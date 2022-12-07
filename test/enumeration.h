#pragma once

#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

#include "util.h"

struct EnumeratedDevice
{
    std::string path;
    PciBusDeviceFunction location;
    dev_t node;
};

std::vector<EnumeratedDevice> EnumerateDevices(void);
