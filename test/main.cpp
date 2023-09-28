// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <iostream>

#include "util.h"
#include "enumeration.h"
#include "test_failure.h"

void TestGetDeviceInfo(const EnumeratedDevice &dev);
void TestConfigSpace(const EnumeratedDevice &dev);
void TestQueryMappings(const EnumeratedDevice &dev);
void TestDmaBuf(const EnumeratedDevice &dev);
void TestPinPages(const EnumeratedDevice &dev);

int main(int argc, char *argv[])
{
    bool at_least_one_device = false;

    auto devs = EnumerateDevices();
    for (const auto &d : devs)
    {
        std::cout << "Testing " << d.path << " @ " << d.location.format() << '\n';

        TestGetDeviceInfo(d);
        TestConfigSpace(d);
        TestQueryMappings(d);
        TestDmaBuf(d);
        TestPinPages(d);

        at_least_one_device = true;
    }

    if (!at_least_one_device)
        THROW_TEST_FAILURE("No devices found.");

    return 0;
}
