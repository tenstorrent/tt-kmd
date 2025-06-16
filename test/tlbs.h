// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <memory>

#include "ioctl.h"
#include "test_failure.h"

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

static constexpr size_t ONE_MEG = 1 << 20;
static constexpr size_t TWO_MEG = 1 << 21;
static constexpr size_t SIXTEEN_MEG = 1 << 24;
static constexpr size_t FOUR_GIG = 1ULL << 32;

using TlbWindow1M = TlbWindow<ONE_MEG>;
using TlbWindow2M = TlbWindow<TWO_MEG>;
using TlbWindow16M = TlbWindow<SIXTEEN_MEG>;
using TlbWindow4G = TlbWindow<FOUR_GIG>;
