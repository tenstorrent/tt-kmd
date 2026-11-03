// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Tests for TENSTORRENT_URING_CMD_DMA_WRITE, submitted through io_uring.

#include <liburing.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

#include "ioctl.h"

#include "devfd.h"
#include "enumeration.h"
#include "test_failure.h"
#include "tlbs.h"
#include "util.h"

namespace
{

constexpr std::size_t BUFFER_SIZE = 64 * 1024;
constexpr uint64_t DRAM_ADDR = 0x1000000;

struct DramCore
{
    uint16_t x;
    uint16_t y;
};

DramCore dram_core(const EnumeratedDevice &dev)
{
    // DRAM bank 0, a scratch endpoint as in noc_io.cpp.
    return is_blackhole_noc_translation_enabled(dev) ? DramCore{17, 12} : DramCore{0, 0};
}

class Ring
{
public:
    Ring()
    {
        int ret = io_uring_queue_init(8, &ring, 0);
        if (ret != 0)
            throw std::system_error(-ret, std::generic_category(), "io_uring_queue_init failed");
    }

    ~Ring() { io_uring_queue_exit(&ring); }

    // Queues one DMA_WRITE command; descs must stay alive until it completes.
    void queue(int fd, const std::vector<tenstorrent_dma_desc> &descs)
    {
        io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, nullptr, 0, 0);
        sqe->cmd_op = TENSTORRENT_URING_CMD_DMA_WRITE;

        tenstorrent_dma_write write{};
        write.descs = reinterpret_cast<uintptr_t>(descs.data());
        write.count = descs.size();
        std::memcpy(sqe->cmd, &write, sizeof(write));
        queued++;
    }

    // Submits the queued commands and returns their CQE results, in completion order.
    std::vector<int> submit_and_wait()
    {
        if (io_uring_submit(&ring) != static_cast<int>(queued))
            THROW_TEST_FAILURE("io_uring_submit failed");

        std::vector<int> results;
        for (; queued > 0; queued--) {
            io_uring_cqe *cqe;
            if (io_uring_wait_cqe(&ring, &cqe) != 0)
                THROW_TEST_FAILURE("io_uring_wait_cqe failed");

            results.push_back(cqe->res);
            io_uring_cqe_seen(&ring, cqe);
        }
        return results;
    }

    int dma_write(int fd, const std::vector<tenstorrent_dma_desc> &descs)
    {
        queue(fd, descs);
        return submit_and_wait()[0];
    }

private:
    io_uring ring;
    unsigned int queued = 0;
};

struct PinnedBuffer
{
    std::unique_ptr<uint8_t, Freer> memory;
    uint64_t iova;
};

PinnedBuffer pin_buffer(int fd)
{
    PinnedBuffer buffer;
    buffer.memory.reset(static_cast<uint8_t *>(std::aligned_alloc(page_size(), BUFFER_SIZE)));

    tenstorrent_pin_pages pin{};
    pin.in.output_size_bytes = sizeof(pin.out);
    pin.in.virtual_address = reinterpret_cast<uintptr_t>(buffer.memory.get());
    pin.in.size = BUFFER_SIZE;

    if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0)
        THROW_TEST_FAILURE("PIN_PAGES failed");

    buffer.iova = pin.out.physical_address;
    return buffer;
}

uint64_t noc_read64(int fd, DramCore core, uint64_t addr)
{
    tenstorrent_noc_io r{};
    r.argsz = sizeof(r);
    r.x = core.x;
    r.y = core.y;
    r.width = 8;
    r.addr = addr;

    if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &r) != 0)
        THROW_TEST_FAILURE("NOC_READ failed");

    return r.value;
}

tenstorrent_dma_desc make_desc(uint64_t src, DramCore core, uint64_t addr, uint32_t size)
{
    tenstorrent_dma_desc desc{};
    desc.src = src;
    desc.addr = addr;
    desc.size = size;
    desc.x = core.x;
    desc.y = core.y;
    return desc;
}

constexpr std::size_t CHUNKS = 16;
constexpr uint32_t CHUNK_SIZE = BUFFER_SIZE / CHUNKS;

void fill_random(PinnedBuffer &buffer)
{
    std::mt19937_64 rng{std::random_device{}()};
    auto *words = reinterpret_cast<uint64_t *>(buffer.memory.get());
    for (std::size_t i = 0; i < BUFFER_SIZE / sizeof(uint64_t); i++)
        words[i] = rng();
}

// Descriptors that copy the buffer's chunks to DRAM in reverse order.
std::vector<tenstorrent_dma_desc> reversed_chunks(const PinnedBuffer &buffer, DramCore core)
{
    std::vector<tenstorrent_dma_desc> descs;
    for (std::size_t i = 0; i < CHUNKS; i++)
        descs.push_back(make_desc(buffer.iova + (CHUNKS - 1 - i) * CHUNK_SIZE, core, DRAM_ADDR + i * CHUNK_SIZE, CHUNK_SIZE));
    return descs;
}

void verify_reversed_chunks(int fd, const PinnedBuffer &buffer, DramCore core)
{
    const auto *words = reinterpret_cast<const uint64_t *>(buffer.memory.get());

    for (std::size_t offset = 0; offset < BUFFER_SIZE; offset += sizeof(uint64_t)) {
        std::size_t chunk = offset / CHUNK_SIZE;
        std::size_t src_offset = (CHUNKS - 1 - chunk) * CHUNK_SIZE + offset % CHUNK_SIZE;
        if (noc_read64(fd, core, DRAM_ADDR + offset) != words[src_offset / sizeof(uint64_t)])
            THROW_TEST_FAILURE("DMA_WRITE data mismatch");
    }
}

// One command whose descriptors run as one chain.
void VerifyChainLands(int fd, Ring &ring, DramCore core, PinnedBuffer &buffer)
{
    fill_random(buffer);

    if (ring.dma_write(fd, reversed_chunks(buffer, core)) != 0)
        THROW_TEST_FAILURE("DMA_WRITE failed");

    verify_reversed_chunks(fd, buffer, core);
}

// Commands submitted together, which the engine chains across.
void VerifyQueuedCommandsLand(int fd, Ring &ring, DramCore core, PinnedBuffer &buffer)
{
    constexpr std::size_t COMMANDS = 4;
    fill_random(buffer);

    std::vector<tenstorrent_dma_desc> descs = reversed_chunks(buffer, core);
    std::vector<std::vector<tenstorrent_dma_desc>> commands;
    for (std::size_t i = 0; i < COMMANDS; i++)
        commands.emplace_back(descs.begin() + i * CHUNKS / COMMANDS, descs.begin() + (i + 1) * CHUNKS / COMMANDS);

    for (const auto &command : commands)
        ring.queue(fd, command);

    for (int result : ring.submit_and_wait()) {
        if (result != 0)
            THROW_TEST_FAILURE("DMA_WRITE failed");
    }

    verify_reversed_chunks(fd, buffer, core);
}

void VerifyForeignSourceRejected(int fd, Ring &ring, DramCore core, const PinnedBuffer &buffer)
{
    // Starts inside the pinning but runs past its end.
    std::vector<tenstorrent_dma_desc> descs = {
        make_desc(buffer.iova + BUFFER_SIZE / 2, core, DRAM_ADDR, BUFFER_SIZE),
    };

    if (ring.dma_write(fd, descs) != -EINVAL)
        THROW_TEST_FAILURE("DMA_WRITE accepted a source outside the fd's pinnings");
}

} // namespace

void TestDmaWrite(const EnumeratedDevice &dev)
{
    if (dev.type != Blackhole)
        return;

    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();
    Ring ring;
    DramCore core = dram_core(dev);

    // Kernels older than 6.6 have no driver commands on this device.
    if (ring.dma_write(fd, {}) == -EOPNOTSUPP) {
        std::cout << "DMA_WRITE not supported, skipping DMA_WRITE tests.\n";
        return;
    }

    // Shared by the tests: memory freed and reallocated can come back at the
    // same address, which PIN_PAGES rejects while the fd holds the old pinning.
    PinnedBuffer buffer = pin_buffer(fd);

    VerifyChainLands(fd, ring, core, buffer);
    VerifyQueuedCommandsLand(fd, ring, core, buffer);
    VerifyForeignSourceRejected(fd, ring, core, buffer);
}
