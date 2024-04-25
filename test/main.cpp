// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <iostream>
#include <string>

#include "util.h"
#include "enumeration.h"
#include "test_failure.h"

void TestGetDeviceInfo(const EnumeratedDevice &dev);
void TestConfigSpace(const EnumeratedDevice &dev, bool check_aer);
void TestQueryMappings(const EnumeratedDevice &dev);
void TestDmaBuf(const EnumeratedDevice &dev);
void TestPinPages(const EnumeratedDevice &dev);
void TestLock(const EnumeratedDevice &dev);
void TestHwmon(const EnumeratedDevice &dev);
void TestHugePages(const EnumeratedDevice &dev);

int main(int argc, char *argv[])
{
    bool at_least_one_device = false;

    // When running inside a VM aer seems to be disabled, this argument skips that check
    // so the rest of the tests will run.
    bool check_aer = true;
    if (argc >= 2 && argv[1] == std::string("--skip-aer")) { check_aer = false; }

    auto devs = EnumerateDevices();
    for (const auto &d : devs)
    {
        std::cout << "Testing " << d.path << " @ " << d.location.format() << '\n';

        TestGetDeviceInfo(d);
        TestConfigSpace(d, check_aer);
        TestQueryMappings(d);
        TestDmaBuf(d);
        TestPinPages(d);
        TestLock(d);
        TestHwmon(d);
        TestHugePages(d);

        at_least_one_device = true;
    }

    if (!at_least_one_device)
        THROW_TEST_FAILURE("No devices found.");

    return 0;
}
