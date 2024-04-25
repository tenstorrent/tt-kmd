// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Verify that hugepages can be allocated by userspace and passed to the driver for use as sysmem buffer.
// Verify that the driver can present the sysmem buffer to userspace as virtually contiguous.
// Verify that the device can read and write to the sysmem buffer.

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <numa.h>

#include "ioctl.h"

#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"

static const off_t BAR0_MMAP_OFFSET = 0;     // MMAP_OFFSET_RESOURCE0_UC
static const size_t BAR0_SIZE = (512 * 1024 * 1024);
static const uint64_t TLB_CONFIG_BASE = 0x1FC00000;
static const size_t TLB_1M_SIZE = (1024 * 1024);

struct DeviceSpecificAttributes
{
    uint64_t num_hugepages;
    uint64_t tlb_local_offset_width_1M;
    uint64_t pcie_y_tile;
    uint64_t pcie_noc_offset;
};

// Keyed by PCI device ID.
static const std::map<uint16_t, DeviceSpecificAttributes> DEVICE_SPECIFIC_ATTRIBUTES = {
    { 0xFACA, { 1, 12, 4, 0x0'0000'0000ULL } },    // Grayskull
    { 0x401E, { 4, 16, 3, 0x8'0000'0000ULL } },    // Wormhole
};

static uint64_t generate_random_sysmem_address()
{
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dis(0x0, 0xFFFD'FFFF);
    uint32_t address = dis(gen);
    return address & 0xFFFF'FFFC;   // Align to 4-byte boundary.
}

static uint64_t encode_1M_tlb_config(int32_t x, int32_t y, uint64_t address, uint16_t device_id)
{
    auto set_field = [](uint64_t &reg, uint64_t value, uint32_t &offset, uint32_t width) {
        uint64_t mask = (1ULL << width) - 1;
        reg |= (value & mask) << offset;
        offset += width;
    };

    uint32_t offset = 0;
    uint64_t encoded = 0;
    uint64_t l = address / TLB_1M_SIZE;
    uint32_t w = DEVICE_SPECIFIC_ATTRIBUTES.at(device_id).tlb_local_offset_width_1M;

    set_field(encoded, l, offset, w);               // local offset
    set_field(encoded, x, offset, 6);               // x_end
    set_field(encoded, y, offset, 6);               // y_end
    set_field(encoded, x, offset, 6);               // x_start
    set_field(encoded, y, offset, 6);               // y_start

    return encoded;
}

class SimpleDevice
{
    const int fd;
    void *bar0;

public:
    SimpleDevice(int fd)
        : fd(fd)
        , bar0(mmap(nullptr, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BAR0_MMAP_OFFSET))
    {
        if (bar0 == MAP_FAILED)
            THROW_TEST_FAILURE("mmap of BAR0 failed.");
    }

    void noc_write32(int32_t x, int32_t y, uint64_t address, uint32_t value)
    {
        auto offset = configure_tlb(x, y, address);
        auto *tlb_access = reinterpret_cast<uint8_t *>(bar0) + offset;
        *reinterpret_cast<volatile uint32_t *>(tlb_access) = value;
        noc_read32(x, y, address);  // Flush the write.
    }

    uint32_t noc_read32(int32_t x, int32_t y, uint64_t address)
    {
        auto offset = configure_tlb(x, y, address);
        auto *tlb_access = reinterpret_cast<uint8_t *>(bar0) + offset;
        return *reinterpret_cast<volatile uint32_t *>(tlb_access);
    }

    uint64_t configure_tlb(int32_t x, int32_t y, uint64_t address)
    {
        auto conf = encode_1M_tlb_config(x, y, address, pci_device_id());
        auto offset = address % TLB_1M_SIZE;
        auto *tlb_config = reinterpret_cast<uint8_t *>(bar0) + TLB_CONFIG_BASE;
        *reinterpret_cast<volatile uint64_t *>(tlb_config) = conf;
        return offset;
    }

    uint16_t pci_device_id() const
    {
        tenstorrent_get_device_info info{};
        info.in.output_size_bytes = sizeof(info.out);

        if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0)
            THROW_TEST_FAILURE("TENSTORRENT_IOCTL_GET_DEVICE_INFO failed.");

        return info.out.device_id;
    }

    uint16_t numa_node() const
    {
        tenstorrent_get_device_info info{};
        info.in.output_size_bytes = sizeof(info.out);

        if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0)
            THROW_TEST_FAILURE("TENSTORRENT_IOCTL_GET_DEVICE_INFO failed.");

        return info.out.numa_node;
    }

    ~SimpleDevice() noexcept
    {
        munmap(bar0, BAR0_SIZE);
    }
};

void VerifyDriverRejectsBogusHugepageSetup(int dev_fd)
{
    // Test that the driver rejects a variety of incorrect hugepage setups.

    tenstorrent_hugepage_setup setup{};

    setup.num_hugepages = 1;
    setup.virt_addrs[0] = reinterpret_cast<uint64_t>(&setup);   // Not a hugepage.

    if (ioctl(dev_fd, TENSTORRENT_IOCTL_HUGEPAGE_SETUP, &setup) == 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_HUGEPAGE_SETUP accepted a bogus vaddr.");

    setup.num_hugepages = TENSTORRENT_MAX_HUGEPAGES_PER_CARD + 1;
    if (ioctl(dev_fd, TENSTORRENT_IOCTL_HUGEPAGE_SETUP, &setup) == 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_HUGEPAGE_SETUP accepted a bogus num_hugepages.");

    int prot = PROT_READ | PROT_WRITE;
    int flags = MAP_SHARED | MAP_ANONYMOUS;
    void *not_hugepage = mmap(nullptr, 1UL << 30, prot, flags, -1, 0);

    if (not_hugepage != MAP_FAILED) {
        setup.virt_addrs[0] = reinterpret_cast<uint64_t>(not_hugepage);
        if (ioctl(dev_fd, TENSTORRENT_IOCTL_HUGEPAGE_SETUP, &setup) == 0)
            THROW_TEST_FAILURE("TENSTORRENT_IOCTL_HUGEPAGE_SETUP accepted a non-hugepage.");
    }

    flags |= MAP_HUGETLB;
    flags |= (21 << MAP_HUGE_SHIFT);    // 2M hugepage
    void *hugepage2M = mmap(nullptr, 1UL << 30, prot, flags, -1, 0);
    if (hugepage2M != MAP_FAILED) {
        setup.virt_addrs[0] = reinterpret_cast<uint64_t>(hugepage2M);

        if (ioctl(dev_fd, TENSTORRENT_IOCTL_HUGEPAGE_SETUP, &setup) == 0)
            THROW_TEST_FAILURE("TENSTORRENT_IOCTL_HUGEPAGE_SETUP accepted a 2M hugepage.");
    } else {
        // Couldn't map a 2M hugepage - not an error.
    }
}

void VerifyHugepageSetup(int dev_fd)
{
    SimpleDevice device(dev_fd);

    // 1. Clear existing hugepage configuration from the driver.
    tenstorrent_hugepage_setup setup{};
    setup.num_hugepages = 0;

    if (ioctl(dev_fd, TENSTORRENT_IOCTL_HUGEPAGE_SETUP, &setup) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_HUGEPAGE_SETUP (clearing existing config) failed.");

    // 2. Get the NUMA node and device ID.
    auto numa_node = device.numa_node();
    auto device_id = device.pci_device_id();

    // 3. Determine how many hugepages we will allocate.
    setup.num_hugepages = DEVICE_SPECIFIC_ATTRIBUTES.at(device_id).num_hugepages;

    // 4. Hop to the NUMA node associated with the device.
    if (numa_node >= 0) {
        if (numa_run_on_node(numa_node) != 0)
            THROW_TEST_FAILURE("numa_run_on_node failed.");
    }

    // 5. Allocate the hugepage(s).
    for (size_t i = 0; i < setup.num_hugepages; ++i) {
        int prot = PROT_READ | PROT_WRITE;
        int flags = MAP_SHARED | MAP_ANONYMOUS | MAP_HUGETLB | (30 << MAP_HUGE_SHIFT);
        void *hugepage = mmap(nullptr, 1UL << 30, prot, flags, -1, 0);
        if (hugepage == MAP_FAILED)
            THROW_TEST_FAILURE("mmap of 1G hugepage failed.");

        setup.virt_addrs[i] = reinterpret_cast<uint64_t>(hugepage);
    }

    // 6. Configure the driver with the hugepage(s).
    if (ioctl(dev_fd, TENSTORRENT_IOCTL_HUGEPAGE_SETUP, &setup) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_HUGEPAGE_SETUP (configuring new hugepages) failed.");

    // 7. Fill each hugepage with a pattern as we unmap them.
    uint64_t n = 0;
    for (size_t i = 0; i < setup.num_hugepages; ++i) {
        auto hugepage = reinterpret_cast<uint64_t *>(setup.virt_addrs[i]);
        for (size_t j = 0; j < (1UL << 30) / sizeof(uint64_t); ++j)
            hugepage[j] = n++;
        munmap(hugepage, 1UL << 30);
    }
}

void VerifySysmemHost(int dev_fd)
{
    SimpleDevice device(dev_fd);

    // 1. Get device id to determine number of hugepages.
    auto device_id = device.pci_device_id();
    size_t num_hugepages = DEVICE_SPECIFIC_ATTRIBUTES.at(device_id).num_hugepages;
    size_t sysmem_size = num_hugepages * (1UL << 30);

    // 2. Map the sysmem buffer now that we know its size.
    off_t sysmem_offset = 6ULL << 32;   // MMAP_OFFSET_RESOURCE_TENSIX_DMA
    void *sysmem = mmap(nullptr, sysmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, sysmem_offset);
    if (sysmem == MAP_FAILED)
        THROW_TEST_FAILURE("mmap of sysmem buffer failed.");

    // 3. Make sure it matches the pattern.
    for (size_t i = 0; i < sysmem_size / sizeof(uint64_t); ++i) {
        if (reinterpret_cast<uint64_t *>(sysmem)[i] != i)
            THROW_TEST_FAILURE("sysmem buffer pattern mismatch.");
    }

    // 4. Unmap it
    munmap(sysmem, sysmem_size);
}

void VerifySysmemDevice(int dev_fd)
{
    SimpleDevice dev(dev_fd);

    // 1. Get device id to determine number of hugepages and PCIE NOC tile location.
    auto device_id = dev.pci_device_id();
    auto num_hugepages = DEVICE_SPECIFIC_ATTRIBUTES.at(device_id).num_hugepages;
    size_t sysmem_size = num_hugepages * (1UL << 30);
    int32_t pcie_x = 0;
    int32_t pcie_y = DEVICE_SPECIFIC_ATTRIBUTES.at(device_id).pcie_y_tile;
    uint64_t offset = DEVICE_SPECIFIC_ATTRIBUTES.at(device_id).pcie_noc_offset;

    // 2. Map the sysmem buffer now that we know its size.
    off_t sysmem_offset = 6ULL << 32;   // MMAP_OFFSET_RESOURCE_TENSIX_DMA
    void *sysmem = mmap(nullptr, sysmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, sysmem_offset);
    if (sysmem == MAP_FAILED)
        THROW_TEST_FAILURE("mmap of sysmem buffer failed.");

    // 3. Read the sysmem buffer via the device.
    const size_t NUM_READS = 1024;
    for (size_t i = 0; i < NUM_READS; ++i) {
        auto address = generate_random_sysmem_address() & (sysmem_size - 1);
        auto actual = dev.noc_read32(pcie_x, pcie_y, offset + address);
        auto expected = reinterpret_cast<uint32_t *>(sysmem)[address / sizeof(uint32_t)];
        if (actual != expected)
             THROW_TEST_FAILURE("sysmem buffer read pattern mismatch.");
    }

    // 4. Write the sysmem buffer via the device.
    const size_t NUM_WRITES = 1024;
    for (size_t i = 0; i < NUM_WRITES; ++i) {
        auto address = generate_random_sysmem_address() & (sysmem_size - 1);
        auto value = 0xFFFF'FFFF;
        dev.noc_write32(pcie_x, pcie_y, address + offset, value);
        auto actual = reinterpret_cast<uint32_t *>(sysmem)[address / sizeof(uint32_t)];
         if (actual != value)
             THROW_TEST_FAILURE("sysmem buffer write pattern mismatch.");
    }

    // 5. Unmap it
    munmap(sysmem, sysmem_size);
}

void TestHugePages(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    VerifyDriverRejectsBogusHugepageSetup(dev_fd.get());
    VerifyHugepageSetup(dev_fd.get());
    VerifySysmemHost(dev_fd.get());
    VerifySysmemDevice(dev_fd.get());
}
