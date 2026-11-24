// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Tests for TENSTORRENT_URING_CMD_DMA_WRITE, host-to-device DMA on Blackhole.
// They need liburing 2.2 or newer at build time and an IOMMU, and are skipped
// without them, or when the device answers EOPNOTSUPP (old firmware or kernel).
//
// Verify that data written by DMA reads back through a TLB window, for one
// descriptor and for many descriptors spread over every DMA window, including
// one that ends exactly on a window boundary.
// Verify that a request fails with EINVAL for non-zero flags or reserved bytes,
// a count of 0 or over the limit, a descriptor crossing a window boundary, more regions than windows, an unknown command, and a source
// that is unpinned, runs past the end of a pinning, or was pinned by another fd.
// Verify that unreadable descriptors fail with EFAULT.
// Verify that UNPIN_PAGES fails with EBUSY while the fd has DMA in flight and
// succeeds once its CQEs are in.
// Verify that closing the fd with DMA in flight lets the DMA finish.

#include <iostream>

#include "enumeration.h"

#ifdef HAVE_LIBURING

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <liburing.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "ioctl.h"

#include "devfd.h"
#include "test_failure.h"
#include "tlbs.h"
#include "util.h"

namespace
{

constexpr uint64_t WINDOW_SIZE = TENSTORRENT_DMA_WINDOW_SIZE;

std::mt19937_64 RNG{std::random_device{}()};

class Ring
{
public:
    Ring()
    {
        int r = io_uring_queue_init(64, &ring, 0);
        if (r < 0) {
            errno = -r;
            throw_system_error("io_uring_queue_init failed");
        }
    }

    ~Ring() { io_uring_queue_exit(&ring); }

    Ring(const Ring &) = delete;
    Ring &operator=(const Ring &) = delete;

    void submit(int fd, const tenstorrent_dma_desc *descs, uint32_t count, uint32_t flags = 0,
                uint32_t cmd_op = TENSTORRENT_URING_CMD_DMA_WRITE)
    {
        tenstorrent_dma_write args{};
        args.descs = reinterpret_cast<uintptr_t>(descs);
        args.count = count;
        args.flags = flags;

        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (!sqe)
            THROW_TEST_FAILURE("io_uring SQ full");

        // liburing 2.2's io_uring_sqe has no names for the command fields:
        // cmd_op is the first four bytes of off, and the payload starts at
        // addr3.
        io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, nullptr, 0, 0);
        std::memcpy(reinterpret_cast<uint8_t *>(sqe) + offsetof(io_uring_sqe, off), &cmd_op, sizeof(cmd_op));
        static_assert(sizeof(args) == 16, "DMA_WRITE payload must fit a 64-byte SQE");
        std::memcpy(reinterpret_cast<uint8_t *>(sqe) + offsetof(io_uring_sqe, addr3), &args, sizeof(args));

        int r = io_uring_submit(&ring);
        if (r != 1) {
            errno = r < 0 ? -r : EIO;
            throw_system_error("io_uring_submit failed");
        }
    }

    // Wait for the next CQE and return its res.
    int wait()
    {
        io_uring_cqe *cqe;
        int r = io_uring_wait_cqe(&ring, &cqe);
        if (r < 0) {
            errno = -r;
            throw_system_error("io_uring_wait_cqe failed");
        }

        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
        return res;
    }

    int run(int fd, const tenstorrent_dma_desc *descs, uint32_t count, uint32_t flags = 0,
            uint32_t cmd_op = TENSTORRENT_URING_CMD_DMA_WRITE)
    {
        submit(fd, descs, count, flags, cmd_op);
        return wait();
    }

private:
    io_uring ring;
};

int pin_pages(int fd, void *p, size_t size, uint64_t *iova)
{
    tenstorrent_pin_pages pin{};
    pin.in.output_size_bytes = sizeof(pin.out);
    pin.in.virtual_address = reinterpret_cast<uintptr_t>(p);
    pin.in.size = size;

    if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0)
        return -1;

    *iova = pin.out.physical_address;
    return 0;
}

int unpin_pages(int fd, void *p, size_t size)
{
    tenstorrent_unpin_pages unpin{};
    unpin.in.virtual_address = reinterpret_cast<uintptr_t>(p);
    unpin.in.size = size;

    return ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin);
}

// Page-aligned host memory full of random bytes, pinned by one fd.
class PinnedBuffer
{
public:
    PinnedBuffer(int fd, size_t size)
        : fd(fd), size(size), mem(std::aligned_alloc(page_size(), size))
    {
        if (!mem)
            THROW_TEST_FAILURE("aligned_alloc failed");

        for (size_t i = 0; i < size; i++)
            bytes()[i] = static_cast<uint8_t>(RNG());

        if (pin_pages(fd, mem.get(), size, &iova_) != 0)
            throw_system_error("PIN_PAGES failed");
        pinned = true;
    }

    ~PinnedBuffer()
    {
        if (pinned)
            unpin_pages(fd, mem.get(), size);
    }

    int unpin()
    {
        int r = unpin_pages(fd, mem.get(), size);
        if (r == 0)
            pinned = false;
        return r;
    }

    uint64_t iova() const { return iova_; }
    uint8_t *bytes() { return static_cast<uint8_t *>(mem.get()); }

private:
    int fd;
    size_t size;
    std::unique_ptr<void, Freer> mem;
    uint64_t iova_ = 0;
    bool pinned = false;
};

// DRAM the tests may scribble on: TENSTORRENT_DMA_WINDOWS + 1 regions from base.
struct Target
{
    uint16_t x;
    uint16_t y;
    uint64_t base;

    uint64_t region(unsigned int i) const { return base + i * WINDOW_SIZE; }
};

Target pick_target(const EnumeratedDevice &dev)
{
    bool translated = is_blackhole_noc_translation_enabled(dev);
    Target t;

    // The DRAM core the TLB tests use, somewhere in its first 512M.
    t.x = translated ? 17 : 0;
    t.y = translated ? 12 : 0;
    t.base = (RNG() % 256) * WINDOW_SIZE;
    return t;
}

tenstorrent_dma_desc make_desc(uint64_t src, const Target &t, uint64_t addr, uint32_t size)
{
    tenstorrent_dma_desc d{};
    d.src = src;
    d.addr = addr;
    d.size = size;
    d.x = t.x;
    d.y = t.y;
    d.noc = 0;
    return d;
}

void verify_device_data(int fd, const Target &t, uint64_t addr, const uint8_t *expected, size_t size)
{
    TlbWindow2M window(fd, t.x, t.y, addr);

    for (size_t i = 0; i < size; i += 4) {
        uint32_t want;
        std::memcpy(&want, expected + i, sizeof(want));
        if (window.read32(i) != want)
            THROW_TEST_FAILURE("DMA_WRITE data mismatch");
    }
}

void expect_result(int got, int want, const char *what)
{
    if (got != want)
        THROW_TEST_FAILURE(std::string("DMA_WRITE ") + what + ": got " + std::to_string(got) +
                           ", expected " + std::to_string(want));
}

void VerifyDmaWriteOne(const EnumeratedDevice &dev, const Target &t)
{
    constexpr uint32_t SIZE = 64 * 1024;
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();
    PinnedBuffer src(fd, SIZE);
    Ring ring;

    uint64_t addr = t.region(0) + 0x1000;
    tenstorrent_dma_desc d = make_desc(src.iova(), t, addr, SIZE);

    expect_result(ring.run(fd, &d, 1), 0, "of one descriptor");
    verify_device_data(fd, t, addr, src.bytes(), SIZE);
}

// Two descriptors in every window's region, from different parts of the
// source, the second of each ending exactly at the end of its region.
void VerifyDmaWriteAllWindows(const EnumeratedDevice &dev, const Target &t)
{
    constexpr uint32_t SIZE = 4096;
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();
    PinnedBuffer src(fd, 2 * TENSTORRENT_DMA_WINDOWS * SIZE);
    Ring ring;
    std::vector<tenstorrent_dma_desc> descs;

    for (unsigned int w = 0; w < TENSTORRENT_DMA_WINDOWS; w++) {
        uint64_t first = t.region(w) + 0x2000;
        uint64_t last = t.region(w) + WINDOW_SIZE - SIZE;

        descs.push_back(make_desc(src.iova() + (2 * w) * SIZE, t, first, SIZE));
        descs.push_back(make_desc(src.iova() + (2 * w + 1) * SIZE, t, last, SIZE));
    }

    expect_result(ring.run(fd, descs.data(), descs.size()), 0, "over every window");

    for (unsigned int w = 0; w < TENSTORRENT_DMA_WINDOWS; w++) {
        verify_device_data(fd, t, descs[2 * w].addr, src.bytes() + (2 * w) * SIZE, SIZE);
        verify_device_data(fd, t, descs[2 * w + 1].addr, src.bytes() + (2 * w + 1) * SIZE, SIZE);
    }
}

void VerifyDmaWriteRejected(const EnumeratedDevice &dev, const Target &t)
{
    constexpr uint32_t SIZE = 64 * 1024;
    DevFd dev_fd(dev.path);
    DevFd other_fd(dev.path);
    int fd = dev_fd.get();
    PinnedBuffer src(fd, SIZE);
    PinnedBuffer other(other_fd.get(), SIZE);
    Ring ring;

    tenstorrent_dma_desc good = make_desc(src.iova(), t, t.region(0), 256);
    tenstorrent_dma_desc d;

    expect_result(ring.run(fd, &good, 1, 1), -EINVAL, "with non-zero flags");
    expect_result(ring.run(fd, &good, 0), -EINVAL, "with no descriptors");
    expect_result(ring.run(fd, &good, 1, 0, TENSTORRENT_URING_CMD_DMA_WRITE + 1), -EINVAL,
                  "with an unknown command");

    std::vector<tenstorrent_dma_desc> too_many(TENSTORRENT_DMA_MAX_DESCS + 1, good);
    expect_result(ring.run(fd, too_many.data(), too_many.size()), -EINVAL, "with too many descriptors");

    for (size_t i = 0; i < sizeof(d.reserved); i++) {
        d = good;
        d.reserved[i] = 1;
        expect_result(ring.run(fd, &d, 1), -EINVAL, "with a non-zero reserved byte");
    }

    d = make_desc(src.iova(), t, t.region(1) - 4, 8);
    expect_result(ring.run(fd, &d, 1), -EINVAL, "crossing a window boundary");

    std::vector<tenstorrent_dma_desc> regions;
    for (unsigned int w = 0; w <= TENSTORRENT_DMA_WINDOWS; w++)
        regions.push_back(make_desc(src.iova(), t, t.region(w), 256));
    expect_result(ring.run(fd, regions.data(), regions.size()), -EINVAL, "with too many regions");

    // The same range on the other NOC is another region.
    regions.back() = regions.front();
    regions.back().noc = 1;
    expect_result(ring.run(fd, regions.data(), regions.size()), -EINVAL, "with too many regions over two NOCs");

    d = make_desc(src.iova() + SIZE, t, t.region(0), 256);
    expect_result(ring.run(fd, &d, 1), -EINVAL, "from past the end of a pinning");

    d = make_desc(src.iova() + SIZE - 4, t, t.region(0), 8);
    expect_result(ring.run(fd, &d, 1), -EINVAL, "running off the end of a pinning");

    d = make_desc(other.iova(), t, t.region(0), 256);
    expect_result(ring.run(fd, &d, 1), -EINVAL, "from another fd's pinning");

    expect_result(ring.run(fd, nullptr, 1), -EFAULT, "with unreadable descriptors");

    // And the request that was right all along still runs.
    expect_result(ring.run(fd, &good, 1), 0, "after rejections");
}

// Requests long enough to still be running when the test looks: each is the
// most descriptors a request takes, each a whole window.
std::vector<tenstorrent_dma_desc> long_request(uint64_t src, const Target &t)
{
    return std::vector<tenstorrent_dma_desc>(TENSTORRENT_DMA_MAX_DESCS,
                                             make_desc(src, t, t.region(0), WINDOW_SIZE));
}

constexpr unsigned int LONG_REQUESTS = 4;

void VerifyDmaUnpinInFlight(const EnumeratedDevice &dev, const Target &t)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();
    PinnedBuffer src(fd, WINDOW_SIZE);
    Ring ring;
    auto descs = long_request(src.iova(), t);

    for (unsigned int i = 0; i < LONG_REQUESTS; i++)
        ring.submit(fd, descs.data(), descs.size());

    if (src.unpin() == 0)
        THROW_TEST_FAILURE("UNPIN_PAGES succeeded with DMA in flight");
    if (errno != EBUSY)
        THROW_TEST_FAILURE("UNPIN_PAGES with DMA in flight did not fail with EBUSY");

    for (unsigned int i = 0; i < LONG_REQUESTS; i++)
        expect_result(ring.wait(), 0, "of a long request");

    if (src.unpin() != 0)
        THROW_TEST_FAILURE("UNPIN_PAGES failed after the DMA finished");
}

void VerifyDmaCloseInFlight(const EnumeratedDevice &dev, const Target &t)
{
    std::unique_ptr<void, Freer> mem(std::aligned_alloc(page_size(), WINDOW_SIZE));
    Ring ring;
    uint64_t iova;

    if (!mem)
        THROW_TEST_FAILURE("aligned_alloc failed");

    int fd = open(dev.path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0)
        throw_system_error("open " + dev.path + " failed");

    // The pinning goes with the fd.
    if (pin_pages(fd, mem.get(), WINDOW_SIZE, &iova) != 0) {
        close(fd);
        throw_system_error("PIN_PAGES failed");
    }

    auto descs = long_request(iova, t);
    for (unsigned int i = 0; i < LONG_REQUESTS; i++)
        ring.submit(fd, descs.data(), descs.size());

    close(fd);

    for (unsigned int i = 0; i < LONG_REQUESTS; i++)
        expect_result(ring.wait(), 0, "after close()");
}

} // namespace

void TestDmaWrite(const EnumeratedDevice &dev)
{
    if (dev.type != Blackhole)
        return;

    if (!dev.iommu_translated) {
        std::cout << "Skipping DMA_WRITE tests on " << dev.path << ": they need an IOMMU.\n";
        return;
    }

    Target t = pick_target(dev);

    // Old firmware or an old kernel can't do DMA at all.
    {
        DevFd dev_fd(dev.path);
        PinnedBuffer src(dev_fd.get(), page_size());
        Ring ring;
        tenstorrent_dma_desc d = make_desc(src.iova(), t, t.region(0), 256);

        int r = ring.run(dev_fd.get(), &d, 1);
        if (r == -EOPNOTSUPP) {
            std::cout << "Skipping DMA_WRITE tests on " << dev.path << ": the device can't do DMA.\n";
            return;
        }
        expect_result(r, 0, "of 256 bytes");
    }

    VerifyDmaWriteOne(dev, t);
    VerifyDmaWriteAllWindows(dev, t);
    VerifyDmaWriteRejected(dev, t);
    VerifyDmaUnpinInFlight(dev, t);
    VerifyDmaCloseInFlight(dev, t);
}

#else // !HAVE_LIBURING

void TestDmaWrite(const EnumeratedDevice &dev)
{
    static bool said;

    if (dev.type == Blackhole && !said) {
        std::cout << "Skipping DMA_WRITE tests: ttkmd_test was built without liburing 2.2 or newer.\n";
        said = true;
    }
}

#endif // HAVE_LIBURING
