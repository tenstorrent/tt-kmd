// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "tlbs.h"
#include "test_failure.h"

namespace
{

void VerifyReleaseHandler(const EnumeratedDevice &dev, uint32_t x, uint32_t y, uint64_t addr)
{
    static constexpr uint32_t pattern = 0xDEADBEEF;
    static constexpr uint32_t initial = 0x0;
    {
        DevFd dev_fd(dev.path);

        // First, clear whatever is at the target address.
        TlbWindow2M tlb(dev_fd.get(), x, y, addr);
        tlb.write32(0, initial);

        // Now set up the NOC write on release.
        tenstorrent_set_noc_cleanup noc_cleanup{};
        noc_cleanup.argsz = sizeof(noc_cleanup);
        noc_cleanup.enabled = true;
        noc_cleanup.data = pattern;
        noc_cleanup.x = x;
        noc_cleanup.y = y;
        noc_cleanup.addr = addr;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_SET_NOC_CLEANUP, &noc_cleanup) != 0)
            THROW_TEST_FAILURE("Failed to set NOC write on release for Wormhole device");

        // The device file descriptor will be closed as this scope ends.
    }
    // By now, the NOC write should have been triggered.
    // Reopen and read to verify.
    {
        DevFd dev_fd(dev.path);
        TlbWindow2M tlb(dev_fd.get(), x, y, addr);
        uint32_t value = tlb.read32(0);

        if (value != pattern)
            THROW_TEST_FAILURE("NOC write on release did not write the expected value");
    }
}

void VerifyReleaseHandlerDisabled(const EnumeratedDevice &dev, uint32_t x, uint32_t y, uint64_t addr)
{
    static constexpr uint32_t pattern = 0xDEADBEEF;
    static constexpr uint32_t initial = 0x0DDBA115;
    {
        DevFd dev_fd(dev.path);

        TlbWindow2M tlb(dev_fd.get(), x, y, addr);
        tlb.write32(0, initial);

        // Set up NOC write on release, and then disable it.
        tenstorrent_set_noc_cleanup noc_cleanup{};
        noc_cleanup.argsz = sizeof(noc_cleanup);
        noc_cleanup.enabled = true;
        noc_cleanup.data = pattern;
        noc_cleanup.x = x;
        noc_cleanup.y = y;
        noc_cleanup.addr = addr;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_SET_NOC_CLEANUP, &noc_cleanup) != 0)
            THROW_TEST_FAILURE("Failed to set NOC write on release for Wormhole device");

        noc_cleanup.enabled = false;
        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_SET_NOC_CLEANUP, &noc_cleanup) != 0)
            THROW_TEST_FAILURE("Failed to disable NOC write on release for Wormhole device");
    }
    // By now, the file descriptor has been closed, but the NOC write should NOT have been triggered.
    // Reopen and read to verify that the write did not happen.
    {
        DevFd dev_fd(dev.path);
        TlbWindow2M tlb(dev_fd.get(), x, y, addr);
        uint32_t value = tlb.read32(0);

        if (value != initial)
            THROW_TEST_FAILURE("NOC write on release did not write the expected value");
    }
}

void VerifyReleaseHandlerWormhole(const EnumeratedDevice &dev)
{
    // For Wormhole, we can use DRAM at (x=0, y=0) for the test.
    VerifyReleaseHandler(dev, 0, 0, 0x0);
    VerifyReleaseHandlerDisabled(dev, 0, 0, 0x0);
}

void VerifyReleaseHandlerBlackhole(const EnumeratedDevice &dev)
{
    // Determine a valid DRAM core to use for the test.
    bool translated = is_blackhole_noc_translation_enabled(dev);
    uint32_t x = translated ? 17 : 0; // Use (x=17, y=12) for translated, (x=0, y=0) for non-translated.
    uint32_t y = translated ? 12 : 0;

    VerifyReleaseHandler(dev, x, y, 0x0);
    VerifyReleaseHandlerDisabled(dev, x, y, 0x0);
}

} // namespace

void TestDeviceRelease(const EnumeratedDevice &dev)
{
    switch (dev.type)
    {
    case Wormhole:
        VerifyReleaseHandlerWormhole(dev);
        break;
    case Blackhole:
        VerifyReleaseHandlerBlackhole(dev);
        break;
    default:
        THROW_TEST_FAILURE("Unknown device type");
    }
}
