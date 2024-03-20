// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <limits>
#include <variant>
#include <cerrno>
#include <cstdint>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ioctl.h"
#include "util.h"
#include "test_failure.h"
#include "devfd.h"
#include "enumeration.h"

tenstorrent_get_device_info_out GetDeviceInfo(int dev_fd)
{
    tenstorrent_get_device_info get_device_info;
    zero(&get_device_info);
    get_device_info.in.output_size_bytes = sizeof(get_device_info.out);

    if (ioctl(dev_fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &get_device_info) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_GET_DEVICE_INFO failed.");

    return get_device_info.out;
}

std::size_t MaxDmaBufSize(int dev_fd)
{
    return (std::size_t)1 << GetDeviceInfo(dev_fd).max_dma_buf_size_log2;
}

std::variant<tenstorrent_allocate_dma_buf_out, int>
AllocateDmaBuf(int dev_fd, std::uint32_t size, std::uint8_t index)
{
    tenstorrent_allocate_dma_buf allocate_dma_buf;
    zero(&allocate_dma_buf);

    allocate_dma_buf.in.requested_size = size;
    allocate_dma_buf.in.buf_index = index;

    if (ioctl(dev_fd, TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, &allocate_dma_buf) != 0)
        return errno;

    return allocate_dma_buf.out;
}

std::variant<tenstorrent_allocate_dma_buf_out, int>
AllocateDmaBufUpTo(int dev_fd, std::uint32_t size, std::uint8_t index)
{
    while (true)
    {
        auto alloc_result = AllocateDmaBuf(dev_fd, size, index);
        if (std::holds_alternative<tenstorrent_allocate_dma_buf_out>(alloc_result))
            return alloc_result;

        int error = std::get<int>(alloc_result);
        if (error != ENOMEM)
            return alloc_result;

        size /= 2;

        // The kernel driver would fail with EINVAL, but really the cause is failure to allocate.
        if (size < page_size())
            return ENOMEM;
    }
}

void VerifyTooLargeIndexFails(int dev_fd)
{
    if (TENSTORRENT_MAX_DMA_BUFS <= std::numeric_limits<decltype(tenstorrent_allocate_dma_buf_in::buf_index)>::max()) {
        auto buf_max = AllocateDmaBuf(dev_fd, page_size(), (std::uint8_t)TENSTORRENT_MAX_DMA_BUFS);
        if (!std::holds_alternative<int>(buf_max))
            THROW_TEST_FAILURE("DMA buf allocation with too-large index was permitted unexpectedly.");

        if (std::get<int>(buf_max) != EINVAL)
            THROW_TEST_FAILURE("DMA buf allocation with too-large index failed for a reason other than EINVAL.");
    }
}

void VerifyBufferMapping(int dev_fd, const std::vector<tenstorrent_allocate_dma_buf_out> &buffers)
{
    std::vector<void*> pointers;

    for (unsigned i = 0; i < buffers.size(); i++)
    {
        const auto &b = buffers[i];

        void *p = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, dev_fd, b.mapping_offset);
        if (p == MAP_FAILED)
            THROW_TEST_FAILURE("DMA buffer mapping failed.");

        pointers.push_back(p);

        std::memset(p, i, b.size);
    }

    for (unsigned i = 0; i < pointers.size(); i++)
    {
        unsigned char *p = static_cast<unsigned char*>(pointers[i]);
        if (p[0] != i)
            THROW_TEST_FAILURE("Wrong value in DMA buffer mapping.");

        munmap(p, buffers[i].size);
    }
}

// Allocate TENSTORRENT_MAX_DMA_BUFS tiny buffers.
// Allocate two buffers both for the same buf_index.
// Allocate for buf_index = TENSTORRENT_MAX_DMA_BUFS.

void TestDmaBuf(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    std::size_t max_dma_buf_size = MaxDmaBufSize(dev_fd.get());

    // Verify we can allocate a buffer.
    auto buf0 = AllocateDmaBufUpTo(dev_fd.get(), max_dma_buf_size, 0);
    if (std::holds_alternative<int>(buf0))
        THROW_TEST_FAILURE("Could not allocate first DMA buffer.");

    // Verify that duplicate buffer index is rejected.
    auto buf0_duplicate = AllocateDmaBuf(dev_fd.get(), page_size(), 0);
    if (!std::holds_alternative<int>(buf0_duplicate))
        THROW_TEST_FAILURE("Duplicate allocation in buffer index 0 was permitted unexpectedly.");

    if (std::get<int>(buf0_duplicate) != EINVAL)
        THROW_TEST_FAILURE("Duplicate allocation in buffer index 0 failed for a reason other than EINVAL.");

    // Verify that we can allocate a tiny buffer for every buffer index.
    std::vector<tenstorrent_allocate_dma_buf_out> buffers;
    buffers.push_back(std::get<tenstorrent_allocate_dma_buf_out>(buf0));

    for (unsigned int i = 1; i < TENSTORRENT_MAX_DMA_BUFS; i++)
    {
        auto buf = AllocateDmaBuf(dev_fd.get(), page_size(), i);

        if (!std::holds_alternative<tenstorrent_allocate_dma_buf_out>(buf))
            THROW_TEST_FAILURE("Tiny DMA buffer allocation failed.");

        buffers.push_back(std::get<tenstorrent_allocate_dma_buf_out>(buf));
    }

    VerifyTooLargeIndexFails(dev_fd.get());

    VerifyBufferMapping(dev_fd.get(), buffers);
}
