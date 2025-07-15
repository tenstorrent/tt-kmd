// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <memory>
#include <random>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"

namespace
{

static constexpr size_t ONE_MEG = 1 << 20;
static constexpr size_t TWO_MEG = 1 << 21;
static constexpr size_t SIXTEEN_MEG = 1 << 24;
static constexpr size_t FOUR_GIG = 1ULL << 32;

struct xy_t
{
    uint32_t x;
    uint32_t y;
};

class TlbHandle
{
    int fd;
    int tlb_id;
    uint8_t *tlb_base;
    size_t tlb_size;

public:
    TlbHandle(int fd, size_t size, const tenstorrent_noc_tlb_config &config)
        : fd(fd)
        , tlb_size(size)
    {
        tenstorrent_allocate_tlb allocate_tlb{};
        allocate_tlb.in.size = size;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");

        tlb_id = allocate_tlb.out.id;

        tenstorrent_configure_tlb configure_tlb{};
        configure_tlb.in.id = tlb_id;
        configure_tlb.in.config = config;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &configure_tlb) != 0)
            THROW_TEST_FAILURE("Failed to configure TLB");

        void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, allocate_tlb.out.mmap_offset_uc);
        if (mem == MAP_FAILED) {
            tenstorrent_free_tlb free_tlb{};
            free_tlb.in.id = tlb_id;
            ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb);
            THROW_TEST_FAILURE("Failed to mmap TLB");
        }

        tlb_base = reinterpret_cast<uint8_t *>(mem);
    }

    uint8_t* data() { return tlb_base; }
    size_t size() const { return tlb_size; }

    ~TlbHandle() noexcept
    {
        tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = tlb_id;

        munmap(tlb_base, tlb_size);
        ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb);
    }
};

template <size_t WINDOW_SIZE>
class TlbWindow
{
    static constexpr size_t WINDOW_MASK = WINDOW_SIZE - 1;
    static_assert((WINDOW_SIZE & WINDOW_MASK) == 0, "WINDOW_SIZE must be a power of 2");

    const uint64_t offset;  // Within the window, to reach target address.
    std::unique_ptr<TlbHandle> window;

public:
    TlbWindow(int fd, uint32_t x, uint32_t y, uint64_t addr)
        : offset(addr & WINDOW_MASK)
    {
        tenstorrent_noc_tlb_config config{
            .addr = addr & ~WINDOW_MASK,
            .x_end = x,
            .y_end = y,
        };

        window = std::make_unique<TlbHandle>(fd, WINDOW_SIZE, config);
    }

    void write32(uint64_t addr, uint32_t value)
    {
        if (addr & 3)
            THROW_TEST_FAILURE("Misaligned write");

        uint8_t *ptr = window->data() + offset + addr;
        *reinterpret_cast<volatile uint32_t *>(ptr) = value;
    }

    uint32_t read32(uint64_t addr)
    {
        if (addr & 3)
            THROW_TEST_FAILURE("Misaligned read");

        uint8_t *ptr = window->data() + offset + addr;
        return *reinterpret_cast<volatile uint32_t *>(ptr);
    }
};

using TlbWindow1M = TlbWindow<ONE_MEG>;
using TlbWindow2M = TlbWindow<TWO_MEG>;
using TlbWindow16M = TlbWindow<SIXTEEN_MEG>;
using TlbWindow4G = TlbWindow<FOUR_GIG>;

static std::random_device RD;
static std::mt19937_64 RNG(RD());

uint64_t random_aligned_address(uint64_t maximum, uint64_t alignment = 0x4)
{
    std::uniform_int_distribution<uint64_t> dist(0, maximum / alignment - 1);
    return dist(RNG) * alignment;
}

void fill_with_random_data(std::vector<uint32_t> &data)
{
    std::generate(data.begin(), data.end(), [&]{ return RNG(); });
}

bool is_blackhole_noc_translation_enabled(const EnumeratedDevice &dev)
{
    static constexpr uint64_t BAR0_UC_OFFSET = 0; // HACK: avoids a QUERY_MAPPINGS
    static constexpr size_t BAR0_SIZE = 1 << 29;
    static constexpr uint64_t NIU_CFG_BAR0_OFFSET = 0x1FD04100;

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    if (dev.type != Blackhole)
        THROW_TEST_FAILURE("BUG: is_blackhole_noc_translation_enabled() called for a non-Blackhole device");

    void *mem = mmap(nullptr, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BAR0_UC_OFFSET);
    if (mem == MAP_FAILED)
        THROW_TEST_FAILURE("Failed to mmap BAR0 for NOC translation check");

    uint8_t *bar0_base = static_cast<uint8_t *>(mem);
    auto niu_cfg = *reinterpret_cast<volatile uint32_t *>(bar0_base + NIU_CFG_BAR0_OFFSET);
    bool translated = (niu_cfg >> 14) & 1;

    munmap(mem, BAR0_SIZE);

    return translated;
}

template <size_t WINDOW_SIZE>
void VerifyNodeId(int fd, const xy_t &tile, uint64_t noc_reg_base)
{
    TlbWindow<WINDOW_SIZE> window(fd, tile.x, tile.y, noc_reg_base);
    auto node_id = window.read32(0);
    auto x = (node_id >> 0x0) & 0x3f;
    auto y = (node_id >> 0x6) & 0x3f;
    if (x != tile.x || y != tile.y)
        THROW_TEST_FAILURE("Node id mismatch");
}


// Wormhole has 156x 1M, 10x 2M, and 20x 16M windows; all but the last 16M
// window should be available for allocation on an unused device.
static void VerifyTlbQuantitiesWormhole(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    std::vector<uint32_t> ids;

    for (size_t i = 0; i < 156; ++i) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = ONE_MEG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");

        ids.push_back(tlb.out.id);
    }

    for (size_t i = 0; i < 10; ++i) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = TWO_MEG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");

        ids.push_back(tlb.out.id);
    }

    for (size_t i = 0; i < 19; ++i) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = SIXTEEN_MEG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");

        ids.push_back(tlb.out.id);
    }

    // The last 16M window should be off-limits to userspace.
    {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = SIXTEEN_MEG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) == 0)
            THROW_TEST_FAILURE("Allocated TLB in off-limits region");
    }

    for (uint32_t id : ids) {
        struct tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = id;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
            THROW_TEST_FAILURE("Failed to free TLB");
    }
}

void VerifyTlbSizesWormhole(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    std::array<size_t, 3> sizes = {ONE_MEG, TWO_MEG, SIXTEEN_MEG};

    for (size_t size : sizes) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");
    }
}

void VerifyTlbAccessWormhole(const EnumeratedDevice &dev)
{
    constexpr xy_t ARC = { 0, 10 };
    constexpr xy_t DDR = { 0, 11 };

    constexpr uint64_t ARC_NOC_NODE_ID = 0xFFFB2002CULL;
    constexpr uint64_t DDR_NOC_NODE_ID = 0x10009002CULL;

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    VerifyNodeId<ONE_MEG>(fd, ARC, ARC_NOC_NODE_ID);
    VerifyNodeId<ONE_MEG>(fd, DDR, DDR_NOC_NODE_ID);

    VerifyNodeId<TWO_MEG>(fd, ARC, ARC_NOC_NODE_ID);
    VerifyNodeId<TWO_MEG>(fd, DDR, DDR_NOC_NODE_ID);

    VerifyNodeId<SIXTEEN_MEG>(fd, ARC, ARC_NOC_NODE_ID);
    VerifyNodeId<SIXTEEN_MEG>(fd, DDR, DDR_NOC_NODE_ID);
}

void VerifyManyWindowsWormhole(const EnumeratedDevice &dev)
{
    std::vector<std::unique_ptr<TlbWindow1M>> windows1M;
    std::vector<std::unique_ptr<TlbWindow2M>> windows2M;
    std::vector<std::unique_ptr<TlbWindow16M>> windows16M;
    std::vector<uint32_t> random_data(0x1000);

    // Use DDR at (x=0, y=0) as a test target.  Pick a random address within the
    // first 1GB, aligned to a 4-byte boundary.
    uint32_t x = 0;
    uint32_t y = 0;
    uint64_t addr = random_aligned_address(1ULL << 30, 0x4);

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    for (size_t i = 0; i < 156; ++i)
        windows1M.push_back(std::make_unique<TlbWindow1M>(fd, x, y, addr));
    for (size_t i = 0; i < 10; ++i)
        windows2M.push_back(std::make_unique<TlbWindow2M>(fd, x, y, addr));
    for (size_t i = 0; i < 18; ++i)
        windows16M.push_back(std::make_unique<TlbWindow16M>(fd, x, y, addr));

    fill_with_random_data(random_data);

    TlbWindow16M writer_window(fd, x, y, addr);
    for (size_t i = 0; i < random_data.size(); i += 4)
        writer_window.write32(i, random_data.at(i / 4));

    for (size_t i = 0; i < random_data.size(); i += 4) {
        for (auto &reader_window : windows1M) {
            if (reader_window->read32(i) != random_data.at(i / 4))
                THROW_TEST_FAILURE("Window data mismatch");
        }

        for (auto &reader_window : windows2M) {
            if (reader_window->read32(i) != random_data.at(i / 4))
                THROW_TEST_FAILURE("Window data mismatch");
        }

        for (auto &reader_window : windows16M) {
            if (reader_window->read32(i) != random_data.at(i / 4))
                THROW_TEST_FAILURE("Window data mismatch");
        }
    }
}

void VerifyBadConfRejectedWormhole(const EnumeratedDevice &dev)
{
    static constexpr std::array<size_t, 3> sizes = {ONE_MEG, TWO_MEG, SIXTEEN_MEG};

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    std::vector<uint32_t> tlb_ids;
    for (size_t size : sizes) {
        tenstorrent_allocate_tlb allocate_tlb{};
        allocate_tlb.in.size = size;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");
        tlb_ids.push_back(allocate_tlb.out.id);
    }

    // Address must be aligned to the window size.
    for (size_t i = 0; i < tlb_ids.size(); ++i) {
        uint32_t tlb_id = tlb_ids.at(i);
        size_t size = sizes.at(i);
        tenstorrent_configure_tlb configure_tlb{};
        tenstorrent_noc_tlb_config config{
            .addr = size / 2,   // Not aligned to window size.
            .x_end = 0,
            .y_end = 0,
        };

        configure_tlb.in.id = tlb_id;
        configure_tlb.in.config = config;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &configure_tlb) == 0)
            THROW_TEST_FAILURE("Configured TLB with misaligned address");
    }

    // Addresses must fit in 36 bits.
    for (size_t i = 0; i < tlb_ids.size(); ++i) {
        uint32_t tlb_id = tlb_ids.at(i);
        tenstorrent_configure_tlb configure_tlb{};
        tenstorrent_noc_tlb_config config{
            .addr = 1ULL << 36,   // Address too large.
            .x_end = 0,
            .y_end = 0,
        };

        configure_tlb.in.id = tlb_id;
        configure_tlb.in.config = config;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &configure_tlb) == 0)
            THROW_TEST_FAILURE("Configured TLB with bad address");
    }

    for (uint32_t id : tlb_ids) {
        tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = id;

        if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
            THROW_TEST_FAILURE("Failed to free TLB");
    }
}

// Blackhole has 202x 2M and 8x 4G windows; all but the last 2M window should be
// available for allocation on an unused device.
void VerifyTlbQuantitiesBlackhole(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    std::vector<uint32_t> ids;

    for (size_t i = 0; i < 201; ++i) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = TWO_MEG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");

        ids.push_back(tlb.out.id);
    }

    // The last 2M window should be off-limits to userspace.
    {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = TWO_MEG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) == 0)
            THROW_TEST_FAILURE("Allocated TLB in off-limits region");
    }

    for (size_t i = 0; i < 8; ++i) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = FOUR_GIG;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");

        ids.push_back(tlb.out.id);
    }

    for (uint32_t id : ids) {
        struct tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = id;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
            THROW_TEST_FAILURE("Failed to free TLB");
    }
}

void VerifyTlbSizesBlackhole(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    std::array<size_t, 2> sizes = {TWO_MEG, FOUR_GIG};

    for (size_t size : sizes) {
        struct tenstorrent_allocate_tlb tlb{};
        tlb.in.size = size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");
    }
}

void VerifyTensixNodeIdsBlackhole(const EnumeratedDevice &dev)
{
    static constexpr uint32_t BH_GRID_X = 17;
    static constexpr uint32_t BH_GRID_Y = 12;
    static constexpr uint64_t NOC_NODE_ID_LOGICAL = 0xffb20148ULL;

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    auto is_tensix = [](uint32_t x, uint32_t y) -> bool {
        return (y >= 2 && y <= 11) &&   // Valid y range
            ((x >= 1 && x <= 7) ||      // Left block
            (x >= 10 && x <= 16));      // Right block
    };

    for (uint32_t x = 0; x < BH_GRID_X; ++x) {
        for (uint32_t y = 0; y < BH_GRID_Y; ++y) {

            if (!is_tensix(x, y))
                continue;

            {
                TlbWindow2M tlb(fd, x, y, NOC_NODE_ID_LOGICAL);
                uint32_t node_id = tlb.read32(0);
                uint32_t node_id_x = (node_id >> 0x0) & 0x3f;
                uint32_t node_id_y = (node_id >> 0x6) & 0x3f;

                if (node_id_x != x || node_id_y != y)
                    THROW_TEST_FAILURE("Node id mismatch");
            }

            {
                TlbWindow4G tlb(fd, x, y, NOC_NODE_ID_LOGICAL);
                uint32_t node_id = tlb.read32(0);
                uint32_t node_id_x = (node_id >> 0x0) & 0x3f;
                uint32_t node_id_y = (node_id >> 0x6) & 0x3f;

                if (node_id_x != x || node_id_y != y)
                    THROW_TEST_FAILURE("Node id mismatch");
            }
        }
    }
}

void VerifyTlbAccessBlackhole(const EnumeratedDevice &dev)
{
    constexpr uint64_t PCI_NOC_NODE_ID_LOGICAL = 0xFFFFFFFFFF000148ULL;

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();
    bool is_translated = is_blackhole_noc_translation_enabled(dev);

    if (is_translated) {
        constexpr xy_t PCI = { 19, 24 };

        VerifyNodeId<TWO_MEG>(fd, PCI, PCI_NOC_NODE_ID_LOGICAL);
        VerifyNodeId<FOUR_GIG>(fd, PCI, PCI_NOC_NODE_ID_LOGICAL);
    } else {
        constexpr xy_t PCI = { 2, 0 };

        VerifyNodeId<TWO_MEG>(fd, PCI, PCI_NOC_NODE_ID_LOGICAL);
        VerifyNodeId<FOUR_GIG>(fd, PCI, PCI_NOC_NODE_ID_LOGICAL);
    }

    // ARC shows up at (x=8, y=0) regardless of whether translation is enabled.
    constexpr xy_t ARC = { 8, 0 };
    constexpr uint64_t ARC_NOC_NODE_ID = 0x0000000080050044ULL;
    VerifyNodeId<TWO_MEG>(fd, ARC, ARC_NOC_NODE_ID);
    VerifyNodeId<FOUR_GIG>(fd, ARC, ARC_NOC_NODE_ID);
}

void VerifyManyWindowsBlackhole(const EnumeratedDevice &dev)
{
    std::vector<std::unique_ptr<TlbWindow2M>> windows;
    std::vector<uint32_t> random_data(0x1000);
    bool translated = is_blackhole_noc_translation_enabled(dev);

    // Get coordinates for a valid DRAM core for this test.
    uint32_t x = translated ? 17 : 0; // Use (x=17, y=12) for translated, (x=0, y=0) for non-translated.
    uint32_t y = translated ? 12 : 0;

    // Pick a random address within the first 1GB, aligned to a 4-byte boundary.
    uint64_t addr = random_aligned_address(1ULL << 30, 0x4);

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    for (size_t i = 0; i < 200; ++i) {
        windows.push_back(std::make_unique<TlbWindow2M>(fd, x, y, addr));
    }

    fill_with_random_data(random_data);

    TlbWindow2M writer_window(fd, x, y, addr);
    for (size_t i = 0; i < random_data.size(); i += 4)
        writer_window.write32(i, random_data.at(i / 4));

    for (auto &reader_window : windows) {
        for (size_t i = 0; i < random_data.size(); i += 4) {
            if (reader_window->read32(i) != random_data.at(i / 4))
                THROW_TEST_FAILURE("Window data mismatch");
        }
    }
}

void VerifyBadConfiRejectedBlackhole(const EnumeratedDevice &dev)
{
    static constexpr std::array<size_t, 2> sizes = { TWO_MEG, FOUR_GIG };

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    std::vector<uint32_t> tlb_ids;
    for (size_t size : sizes) {
        tenstorrent_allocate_tlb allocate_tlb{};
        allocate_tlb.in.size = size;
        if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0)
            THROW_TEST_FAILURE("Failed to allocate TLB");
        tlb_ids.push_back(allocate_tlb.out.id);
    }

    // Address must be aligned to the window size.
    for (size_t i = 0; i < tlb_ids.size(); ++i) {
        uint32_t tlb_id = tlb_ids.at(i);
        size_t size = sizes.at(i);
        tenstorrent_configure_tlb configure_tlb{};
        tenstorrent_noc_tlb_config config{
            .addr = size / 2,   // Not aligned to window size.
            .x_end = 0,
            .y_end = 0,
        };

        configure_tlb.in.id = tlb_id;
        configure_tlb.in.config = config;
        if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &configure_tlb) == 0)
            THROW_TEST_FAILURE("Configured TLB with misaligned address");
    }

    for (uint32_t id : tlb_ids) {
        tenstorrent_free_tlb free_tlb{};
        free_tlb.in.id = id;

        if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
            THROW_TEST_FAILURE("Failed to free TLB");
    }
}

void VerifyPartialUnmappingDisallowed(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    tenstorrent_allocate_tlb allocate_tlb{};
    allocate_tlb.in.size = TWO_MEG;
    if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0)
        THROW_TEST_FAILURE("Failed to allocate TLB");

    uint32_t id = allocate_tlb.out.id;
    tenstorrent_free_tlb free_tlb{};
    free_tlb.in.id = id;

    void *mem = mmap(nullptr, TWO_MEG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, allocate_tlb.out.mmap_offset_uc);
    if (mem == MAP_FAILED) {
        ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb);
        THROW_TEST_FAILURE("Failed to mmap TLB");
    }

    // Attempt to unmap every page in the window.
    for (size_t i = 0; i < TWO_MEG; i += 0x1000) {
        if (munmap(static_cast<uint8_t *>(mem) + i, 0x1000) == 0) {
            THROW_TEST_FAILURE("Unmapped part of TLB");
        }
    }

    // Attempt to remap a page.  The mremap fails on 5.15.0 (fine), succeeds on
    // 5.4.0.  Test that the TLB is appropriately reference counted in the case
    // where the remap succeeds.
    void *target = mmap(nullptr, 0x1000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (target == MAP_FAILED)
        THROW_TEST_FAILURE("Failed to create anonymous mapping");
    void *page = mremap(static_cast<uint8_t*>(mem) + 0x1000, 0x1000, 0x1000,
                         MREMAP_MAYMOVE | MREMAP_FIXED, target);
    if (page != MAP_FAILED) {
        // Unmap the whole window.  Remapped page remains mapped.
        if (munmap(mem, TWO_MEG) != 0)
            THROW_TEST_FAILURE("Failed to munmap TLB");

        // Refcount due to remapped page should prevent freeing the window.
        if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) == 0)
            THROW_TEST_FAILURE("Freed mapped TLB");

        // Unmap the remapped page.
        if (munmap(page, 0x1000) != 0)
            THROW_TEST_FAILURE("Failed to munmap TLB");
    } else {
        if (munmap(mem, TWO_MEG) != 0)
            THROW_TEST_FAILURE("Failed to munmap TLB");
    }

    // Should be safe to free the TLB now.
    if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
        THROW_TEST_FAILURE("Failed to free TLB");
}

// If a window is mapped to userspace, attempting to free it should fail.
void VerifyMappedWindowCannotBeFreed(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    tenstorrent_allocate_tlb allocate_tlb{};
    allocate_tlb.in.size = TWO_MEG;
    if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0)
        THROW_TEST_FAILURE("Failed to allocate TLB");

    void *mem = mmap(nullptr, TWO_MEG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, allocate_tlb.out.mmap_offset_uc);
    if (mem == MAP_FAILED)
        THROW_TEST_FAILURE("Failed to mmap TLB");

    tenstorrent_free_tlb free_tlb{};
    free_tlb.in.id = allocate_tlb.out.id;
    if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) == 0)
        THROW_TEST_FAILURE("Freed mapped TLB");

    if (munmap(mem, TWO_MEG) != 0)
        THROW_TEST_FAILURE("Failed to munmap TLB");

    if (ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &free_tlb) != 0)
        THROW_TEST_FAILURE("Failed to free TLB");
}

} // namespace

void TestTlbs(const EnumeratedDevice &dev)
{
    switch (dev.type)
    {
    case Wormhole:
        VerifyTlbQuantitiesWormhole(dev);
        VerifyTlbSizesWormhole(dev);
        VerifyTlbAccessWormhole(dev);
        VerifyManyWindowsWormhole(dev);
        VerifyBadConfRejectedWormhole(dev);
        break;
    case Blackhole:
        VerifyTlbQuantitiesBlackhole(dev);
        VerifyTlbSizesBlackhole(dev);
        VerifyTensixNodeIdsBlackhole(dev);
        VerifyTlbAccessBlackhole(dev);
        VerifyManyWindowsBlackhole(dev);
        VerifyBadConfiRejectedBlackhole(dev);
        break;
    default:
        THROW_TEST_FAILURE("Unknown device type");
    }

    VerifyPartialUnmappingDisallowed(dev);
    VerifyMappedWindowCannotBeFreed(dev);
}
