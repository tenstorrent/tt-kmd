// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Test the debugfs mappings file that shows resource visibility.
// This file shows:
// - Open file descriptors with PID and command name
// - Pinned user pages
// - Driver-allocated DMA buffers
// - BAR mappings
// - TLB allocations

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstdint>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include "ioctl.h"
#include "util.h"
#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"
#include "tlbs.h"

namespace
{

std::string get_debugfs_path(const EnumeratedDevice &dev)
{
    // Extract ordinal from device path like "/dev/tenstorrent/0"
    std::string path = dev.path;
    size_t last_slash = path.find_last_of('/');
    if (last_slash == std::string::npos)
        THROW_TEST_FAILURE("Could not parse device path: " + path);

    std::string ordinal = path.substr(last_slash + 1);
    return "/sys/kernel/debug/tenstorrent/" + ordinal + "/mappings";
}

bool is_file_readable(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;

    return access(path.c_str(), R_OK) == 0;
}

bool contains(const std::string &haystack, const std::string &needle)
{
    return haystack.find(needle) != std::string::npos;
}

void VerifyBasicFormat(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    std::string content = read_file(debugfs_path);

    // Check for warning header
    if (!contains(content, "WARNING: This file is for diagnostic purposes only"))
        THROW_TEST_FAILURE("Missing warning header in mappings file");

    if (!contains(content, "not stable"))
        THROW_TEST_FAILURE("Missing stability warning in mappings file");

    // Check for column headers
    if (!contains(content, "PID"))
        THROW_TEST_FAILURE("Missing PID column header");

    if (!contains(content, "Comm"))
        THROW_TEST_FAILURE("Missing Comm column header");

    if (!contains(content, "Type"))
        THROW_TEST_FAILURE("Missing Type column header");

    if (!contains(content, "Mapping Details"))
        THROW_TEST_FAILURE("Missing Mapping Details column header");
}

void VerifyOpenFdAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    // Open a device file descriptor
    DevFd dev_fd(dev.path);
    pid_t pid = getpid();

    std::string content = read_file(debugfs_path);

    // Check that our PID appears
    std::string pid_str = std::to_string(pid);
    if (!contains(content, pid_str))
        THROW_TEST_FAILURE("PID not found in mappings file");

    // Check that OPEN_FD type appears
    if (!contains(content, "OPEN_FD"))
        THROW_TEST_FAILURE("OPEN_FD entry not found in mappings file");

    // The file descriptor is closed when dev_fd goes out of scope
}

void VerifyPinPagesAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    auto page_size = getpagesize();

    void *p = std::aligned_alloc(page_size, page_size);
    if (!p)
        throw_system_error("aligned_alloc failed");

    std::unique_ptr<void, Freer> page(p);

    DevFd dev_fd(dev.path);

    struct tenstorrent_pin_pages pin_pages;
    zero(&pin_pages);
    pin_pages.in.output_size_bytes = sizeof(pin_pages.out);
    pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
    pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
    pin_pages.in.size = page_size;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != 0)
        THROW_TEST_FAILURE("PIN_PAGES failed");

    std::string content = read_file(debugfs_path);

    // Check that PIN_PAGES type appears
    if (!contains(content, "PIN_PAGES"))
        THROW_TEST_FAILURE("PIN_PAGES entry not found in mappings file");
}

void VerifyPinPagesIatuAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    auto page_size = getpagesize();

    void *p = std::aligned_alloc(page_size, page_size);
    if (!p)
        throw_system_error("aligned_alloc failed");

    std::unique_ptr<void, Freer> page(p);

    DevFd dev_fd(dev.path);

    struct tenstorrent_pin_pages pin_pages;
    zero(&pin_pages);
    pin_pages.in.output_size_bytes = sizeof(pin_pages.out);
    pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS | TENSTORRENT_PIN_PAGES_NOC_DMA;
    pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
    pin_pages.in.size = page_size;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != 0)
        THROW_TEST_FAILURE("PIN_PAGES with NOC_DMA flag failed");

    std::string content = read_file(debugfs_path);

    // Check that PIN_PAGES+IATU type appears
    if (!contains(content, "PIN_PAGES+IATU"))
        THROW_TEST_FAILURE("PIN_PAGES+IATU entry not found in mappings file");
}

void VerifyDmaBufAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    DevFd dev_fd(dev.path);

    struct tenstorrent_allocate_dma_buf allocate_dma_buf;
    zero(&allocate_dma_buf);
    allocate_dma_buf.in.requested_size = page_size();
    allocate_dma_buf.in.buf_index = 0;
    allocate_dma_buf.in.flags = 0;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, &allocate_dma_buf) != 0)
        THROW_TEST_FAILURE("ALLOCATE_DMA_BUF failed");

    std::string content = read_file(debugfs_path);

    // Check that DMA_BUF type appears
    if (!contains(content, "DMA_BUF"))
        THROW_TEST_FAILURE("DMA_BUF entry not found in mappings file");

    // Check that the buffer ID appears
    if (!contains(content, "ID: 0"))
        THROW_TEST_FAILURE("DMA_BUF ID not found in mappings file");
}

void VerifyDmaBufIatuAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    DevFd dev_fd(dev.path);

    struct tenstorrent_allocate_dma_buf allocate_dma_buf;
    zero(&allocate_dma_buf);
    allocate_dma_buf.in.requested_size = page_size();
    allocate_dma_buf.in.buf_index = 2;
    allocate_dma_buf.in.flags = TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, &allocate_dma_buf) != 0)
        THROW_TEST_FAILURE("ALLOCATE_DMA_BUF with NOC_DMA flag failed");

    std::string content = read_file(debugfs_path);

    // Check that DMA_BUF+IATU type appears
    if (!contains(content, "DMA_BUF+IATU"))
        THROW_TEST_FAILURE("DMA_BUF+IATU entry not found in mappings file");

    // Check that the buffer ID appears
    if (!contains(content, "ID: 2"))
        THROW_TEST_FAILURE("DMA_BUF+IATU ID not found in mappings file");
}

void VerifyBarMappingAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    DevFd dev_fd(dev.path);

    // Map BAR0 UC directly - we know this always exists
    // BAR0 UC starts at offset 0 in the mmap space
    size_t map_size = page_size();

    void *mem = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     dev_fd.get(), 0);
    if (mem == MAP_FAILED)
        THROW_TEST_FAILURE("mmap of BAR0 failed");

    std::string content = read_file(debugfs_path);

    // Check that BAR type appears
    if (!contains(content, "BAR"))
        THROW_TEST_FAILURE("BAR entry not found in mappings file");

    munmap(mem, map_size);
}

void VerifyTlbAppears(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    DevFd dev_fd(dev.path);

    struct tenstorrent_allocate_tlb allocate_tlb;
    zero(&allocate_tlb);
    allocate_tlb.in.size = TWO_MEG;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_TLB, &allocate_tlb) != 0)
        THROW_TEST_FAILURE("ALLOCATE_TLB failed");

    std::string content = read_file(debugfs_path);

    // Check that TLB type appears
    if (!contains(content, "TLB"))
        THROW_TEST_FAILURE("TLB entry not found in mappings file");
}

void VerifyMultipleResourcesAppear(const EnumeratedDevice &dev, const std::string &debugfs_path)
{
    auto page_size_val = page_size();

    // Allocate pinned pages
    void *p = std::aligned_alloc(page_size_val, page_size_val);
    if (!p)
        throw_system_error("aligned_alloc failed");
    std::unique_ptr<void, Freer> page(p);

    DevFd dev_fd(dev.path);

    // Pin pages
    struct tenstorrent_pin_pages pin_pages;
    zero(&pin_pages);
    pin_pages.in.output_size_bytes = sizeof(pin_pages.out);
    pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
    pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
    pin_pages.in.size = page_size_val;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != 0)
        THROW_TEST_FAILURE("PIN_PAGES failed");

    // Allocate DMA buffer
    struct tenstorrent_allocate_dma_buf allocate_dma_buf;
    zero(&allocate_dma_buf);
    allocate_dma_buf.in.requested_size = page_size_val;
    allocate_dma_buf.in.buf_index = 1;
    allocate_dma_buf.in.flags = 0;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, &allocate_dma_buf) != 0)
        THROW_TEST_FAILURE("ALLOCATE_DMA_BUF failed");

    std::string content = read_file(debugfs_path);

    // Check that both resources appear
    if (!contains(content, "PIN_PAGES"))
        THROW_TEST_FAILURE("PIN_PAGES not found in multi-resource test");

    if (!contains(content, "DMA_BUF"))
        THROW_TEST_FAILURE("DMA_BUF not found in multi-resource test");

    if (!contains(content, "OPEN_FD"))
        THROW_TEST_FAILURE("OPEN_FD not found in multi-resource test");
}

} // anonymous namespace

void TestMappingsDebugfs(const EnumeratedDevice &dev)
{
    std::string debugfs_path = get_debugfs_path(dev);

    // Check if debugfs file is accessible
    if (!is_file_readable(debugfs_path)) {
        std::cout << "Debugfs mappings file not accessible, skipping test.\n";
        return;
    }

    VerifyBasicFormat(dev, debugfs_path);
    VerifyOpenFdAppears(dev, debugfs_path);
    VerifyPinPagesAppears(dev, debugfs_path);
    VerifyPinPagesIatuAppears(dev, debugfs_path);
    VerifyDmaBufAppears(dev, debugfs_path);
    VerifyDmaBufIatuAppears(dev, debugfs_path);
    VerifyBarMappingAppears(dev, debugfs_path);
    VerifyTlbAppears(dev, debugfs_path);
    VerifyMultipleResourcesAppear(dev, debugfs_path);
}
