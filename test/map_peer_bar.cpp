// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Verify that mapping the same device (two different fds) is rejected.
// Verify that mapping two different chips is rejected.
// Verify mapping each BAR of the peer.

#include <fstream>
#include <map>
#include <string>
#include <cstddef>
#include <cstdint>

#include <sys/ioctl.h>

#include "ioctl.h"

#include "util.h"
#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"

namespace
{

struct PciBar
{
    std::uint64_t base;
    std::uint64_t size;
    bool memory;
    bool io;
    bool prefetch;
};

std::map<unsigned int, PciBar> QueryResources(const EnumeratedDevice &dev)
{
    std::ifstream resource(sysfs_dir_for_bdf(dev.location) + "/resource");

    // The contents of resource are undocumented. Each line represents
    // one resource with 3 hex numbers: physical start, physical end, flags.
    // Flags are in include/linux/ioport.h, but this is not in uapi.

    std::map<unsigned int, PciBar> resources;

    std::string line;
    unsigned resource_index = 0;
    while (std::getline(resource, line))
    {
        std::size_t endpos = 0;
        std::uint64_t start = std::stoull(line, &endpos, 16);
        line = line.substr(endpos);
        std::uint64_t end = std::stoull(line, &endpos, 16);
        line = line.substr(endpos);
        std::uint64_t flags = std::stoull(line, &endpos, 16);

        bool io = false;
        bool memory = false;

        const std::uint64_t resource_flags_type = 0x1F00;
        const std::uint64_t resource_flags_type_io = 0x100;
        const std::uint64_t resource_flags_type_memory = 0x200;
        const std::uint64_t resource_flags_prefetchable = 0x2000;

        switch (flags & resource_flags_type)
        {
            case resource_flags_type_io: io = true; break;
            case resource_flags_type_memory: memory = true; break;
        }

        if (io || memory)
        {
            bool prefetch = flags & resource_flags_prefetchable;

            resources[resource_index] = PciBar{start, end - start + 1, memory, io, prefetch};
        }

        resource_index++;
    }

    return resources;
}

void VerifyBasic(const EnumeratedDevice &d1, const EnumeratedDevice &d2,
                 const std::map<unsigned int, PciBar> &d2_bars)
{
    for (const auto &bar : d2_bars)
    {
        if (bar.second.memory)
        {
            DevFd fd1(d1.path);
            DevFd fd2(d2.path);

            struct tenstorrent_map_peer_bar map_peer_bar;
            zero(&map_peer_bar);

            // Cap to the largest page-aligned size the u32 ABI field can hold.
            uint32_t size = std::min(bar.second.size, (uint64_t)0xFFFFF000);

            map_peer_bar.in.peer_fd = fd2.get();
            map_peer_bar.in.peer_bar_index = bar.first;
            map_peer_bar.in.peer_bar_offset = 0;
            map_peer_bar.in.peer_bar_length = size;

            if (ioctl(fd1.get(), TENSTORRENT_IOCTL_MAP_PEER_BAR, &map_peer_bar) != 0)
                THROW_TEST_FAILURE("MAP_PEER_BAR failed.");
        }
    }
}

void VerifySameDeviceRejected(const EnumeratedDevice &d1, const EnumeratedDevice &d2)
{
    DevFd fd1(d1.path);
    DevFd fd2(d2.path);

    struct tenstorrent_map_peer_bar map_peer_bar;
    zero(&map_peer_bar);

    map_peer_bar.in.peer_fd = fd2.get();
    map_peer_bar.in.peer_bar_index = 0;
    map_peer_bar.in.peer_bar_offset = 0;
    map_peer_bar.in.peer_bar_length = page_size();

    if (ioctl(fd1.get(), TENSTORRENT_IOCTL_MAP_PEER_BAR, &map_peer_bar) != -1)
        THROW_TEST_FAILURE("MAP_PEER_BAR succeeded with two fds for the same device.");
}

void VerifyDifferentChipRejected(const EnumeratedDevice &d1, const EnumeratedDevice &d2)
{
    DevFd fd1(d1.path);
    DevFd fd2(d2.path);

    struct tenstorrent_map_peer_bar map_peer_bar;
    zero(&map_peer_bar);

    map_peer_bar.in.peer_fd = fd2.get();
    map_peer_bar.in.peer_bar_index = 0;
    map_peer_bar.in.peer_bar_offset = 0;
    map_peer_bar.in.peer_bar_length = page_size();

    if (ioctl(fd1.get(), TENSTORRENT_IOCTL_MAP_PEER_BAR, &map_peer_bar) != -1)
        THROW_TEST_FAILURE("MAP_PEER_BAR succeeded on two different chips.");
}

std::uint16_t DeviceId(const EnumeratedDevice &dev)
{
    return std::stoul(read_file(sysfs_dir_for_bdf(dev.location) + "/device"), nullptr, 16);
}

}

void TestMapPeerBar(const EnumeratedDevice &d1, const EnumeratedDevice &d2)
{
    if (d1.location == d2.location)
    {
        VerifySameDeviceRejected(d1, d2);
    }
    else if (DeviceId(d1) != DeviceId(d2))
    {
        VerifyDifferentChipRejected(d1, d2);
    }
    else
    {
        auto d2_bars = QueryResources(d2);

        VerifyBasic(d1, d2, d2_bars);
    }
}
