// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Tests for the small reset-safe NOC transfer ioctls
// (TENSTORRENT_IOCTL_NOC_READ / TENSTORRENT_IOCTL_NOC_WRITE).

#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <random>

#include "ioctl.h"

#include "devfd.h"
#include "enumeration.h"
#include "test_failure.h"
#include "tlbs.h"

namespace
{

static std::mt19937_64 RNG{std::random_device{}()};

uint64_t random_aligned_address(uint64_t maximum, uint64_t alignment)
{
    std::uniform_int_distribution<uint64_t> dist(0, maximum / alignment - 1);
    return dist(RNG) * alignment;
}

uint64_t noc_read(int fd, uint16_t x, uint16_t y, uint8_t noc, uint8_t size, uint64_t addr)
{
    tenstorrent_noc_read r{};
    r.argsz = sizeof(r);
    r.x = x;
    r.y = y;
    r.noc = noc;
    r.size = size;
    r.addr = addr;

    if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &r) != 0)
        THROW_TEST_FAILURE("NOC_READ ioctl failed");

    return r.data;
}

void noc_write(int fd, uint16_t x, uint16_t y, uint8_t noc, uint8_t size, uint64_t addr, uint64_t value)
{
    tenstorrent_noc_write w{};
    w.argsz = sizeof(w);
    w.x = x;
    w.y = y;
    w.noc = noc;
    w.size = size;
    w.addr = addr;
    w.data = value;

    if (ioctl(fd, TENSTORRENT_IOCTL_NOC_WRITE, &w) != 0)
        THROW_TEST_FAILURE("NOC_WRITE ioctl failed");
}

// A NOC tile's node_id register encodes its own coordinates: x in bits [5:0]
// and y in bits [11:6].  Reading it back is a self-checking way to verify that
// a read of a given width reached the requested endpoint.
void VerifyNodeId(int fd, uint16_t x, uint16_t y, uint64_t node_id_addr)
{
    uint32_t node_id = static_cast<uint32_t>(noc_read(fd, x, y, 0, 4, node_id_addr));
    uint32_t read_x = (node_id >> 0) & 0x3f;
    uint32_t read_y = (node_id >> 6) & 0x3f;

    if (read_x != x || read_y != y)
        THROW_TEST_FAILURE("NOC_READ node id mismatch");

    // The same register, read at narrower widths, must agree with the low
    // bytes of the 32-bit value.
    if ((noc_read(fd, x, y, 0, 1, node_id_addr) & 0xff) != (node_id & 0xff))
        THROW_TEST_FAILURE("NOC_READ 1-byte node id mismatch");

    if ((noc_read(fd, x, y, 0, 2, node_id_addr) & 0xffff) != (node_id & 0xffff))
        THROW_TEST_FAILURE("NOC_READ 2-byte node id mismatch");
}

// Write then read back each transfer width at a DRAM endpoint.
void VerifyRoundtrip(int fd, uint16_t x, uint16_t y)
{
    static constexpr std::array<std::pair<uint8_t, uint64_t>, 4> cases = {{
        {1, 0xffull},
        {2, 0xffffull},
        {4, 0xffffffffull},
        {8, ~0ull},
    }};

    // Pick a random 8-byte-aligned base within the first 1 GiB; give each
    // width its own 8-byte slot so the writes don't overlap.
    uint64_t base = random_aligned_address(1ULL << 30, 8);
    uint64_t offset = 0;

    for (const auto &[size, mask] : cases) {
        uint64_t addr = base + offset;
        uint64_t value = RNG() & mask;

        noc_write(fd, x, y, 0, size, addr, value);
        uint64_t got = noc_read(fd, x, y, 0, size, addr) & mask;

        if (got != value)
            THROW_TEST_FAILURE("NOC roundtrip data mismatch");

        offset += 8;
    }
}

void ExpectReadRejected(int fd, const tenstorrent_noc_read &request, const char *what)
{
    tenstorrent_noc_read r = request;
    if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &r) == 0)
        THROW_TEST_FAILURE(std::string("NOC_READ accepted invalid request: ") + what);
}

void ExpectWriteRejected(int fd, const tenstorrent_noc_write &request, const char *what)
{
    tenstorrent_noc_write w = request;
    if (ioctl(fd, TENSTORRENT_IOCTL_NOC_WRITE, &w) == 0)
        THROW_TEST_FAILURE(std::string("NOC_WRITE accepted invalid request: ") + what);
}

void VerifyValidation(int fd)
{
    auto good = [] {
        tenstorrent_noc_read r{};
        r.argsz = sizeof(r);
        r.x = 0;
        r.y = 0;
        r.noc = 0;
        r.size = 4;
        r.addr = 0;
        return r;
    };

    { auto r = good(); r.argsz = sizeof(r) - 1;     ExpectReadRejected(fd, r, "bad argsz"); }
    { auto r = good(); r.flags = 1;                 ExpectReadRejected(fd, r, "nonzero flags"); }
    { auto r = good(); r.reserved0[0] = 1;          ExpectReadRejected(fd, r, "nonzero reserved0"); }
    { auto r = good(); r.noc = 2;                   ExpectReadRejected(fd, r, "noc > 1"); }
    { auto r = good(); r.size = 3;                  ExpectReadRejected(fd, r, "unsupported size"); }
    { auto r = good(); r.size = 0;                  ExpectReadRejected(fd, r, "zero size"); }
    { auto r = good(); r.size = 4; r.addr = 2;      ExpectReadRejected(fd, r, "misaligned 4-byte"); }
    { auto r = good(); r.size = 2; r.addr = 1;      ExpectReadRejected(fd, r, "misaligned 2-byte"); }
    { auto r = good(); r.size = 8; r.addr = 4;      ExpectReadRejected(fd, r, "misaligned 8-byte"); }

    // The write request shares the same layout and validation.
    tenstorrent_noc_write w{};
    w.argsz = sizeof(w) + 1;
    w.size = 4;
    ExpectWriteRejected(fd, w, "bad argsz");
}

void VerifyWormhole(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    // ARC at (0,10) and DRAM at (0,11) expose node_id registers at these
    // addresses (see the TLB tests).
    VerifyNodeId(fd, 0, 10, 0xFFFB2002CULL);
    VerifyNodeId(fd, 0, 11, 0x10009002CULL);

    // DRAM at (0,0) is a safe scratch endpoint for the roundtrip.
    VerifyRoundtrip(fd, 0, 0);
}

void VerifyBlackhole(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();
    bool translated = is_blackhole_noc_translation_enabled(dev);

    // ARC is at (8,0) regardless of NOC translation.
    VerifyNodeId(fd, 8, 0, 0x0000000080050044ULL);

    // Use a valid DRAM core as a scratch endpoint for the roundtrip.
    uint16_t dram_x = translated ? 17 : 0;
    uint16_t dram_y = translated ? 12 : 0;
    VerifyRoundtrip(fd, dram_x, dram_y);
}

} // namespace

void TestNocIo(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    VerifyValidation(dev_fd.get());

    switch (dev.type)
    {
    case Wormhole:
        VerifyWormhole(dev);
        break;
    case Blackhole:
        VerifyBlackhole(dev);
        break;
    default:
        THROW_TEST_FAILURE("Unknown device type");
    }
}
