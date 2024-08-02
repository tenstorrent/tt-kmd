// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Verify that pin pages accepts flags = 0 or TENSTORRENT_PIN_PAGES_CONTIGUOUS.
// Verify that pin pages rejects any other flags.
// Verify that pin pages rejects size == 0 and size not multiple of page size.
// Verify that pin pages rejects an unmapped range, a partially unmapped range.
// Verify that pin pages accepts a single page.
// Verify that pin pages can simultaneously pin many ranges.
// Verify that pin pages can pin multiple pages if they are contiguous.
// Verify that pin pages can pin discontiguous memory if and only if IOMMU is enabled.

#include <iostream>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstddef>
#include <cstdlib>

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

namespace
{

struct Freer
{
    template <class T>
    void operator() (T* p) const { std::free(p); }
};

struct Unmapper
{
    std::size_t page_count;

    template <class T>
    void operator() (T* p) const { munmap(p, getpagesize() * page_count); }
};

void VerifyPinPagesSimple(const EnumeratedDevice &dev)
{
    auto page_size = getpagesize();

    struct tenstorrent_pin_pages pin_pages;

    void *p = std::aligned_alloc(page_size, page_size);
    std::unique_ptr<void, Freer> page(p);

    static const unsigned int flags[] = { 0, TENSTORRENT_PIN_PAGES_CONTIGUOUS };
    for (auto f : flags)
    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = f;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
        pin_pages.in.size = page_size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != 0)
            THROW_TEST_FAILURE("PIN_PAGES failed single-page pin.");
    }
}

void VerifyPinPagesBadFlags(const EnumeratedDevice &dev)
{
    auto page_size = getpagesize();

    struct tenstorrent_pin_pages pin_pages;

    void *p = std::aligned_alloc(page_size, page_size);
    std::unique_ptr<void, Freer> page(p);

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = ~TENSTORRENT_PIN_PAGES_CONTIGUOUS;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
        pin_pages.in.size = page_size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1)
            THROW_TEST_FAILURE("PIN_PAGES succeeded with flags = ~TENSTORRENT_PIN_PAGES_CONTIGUOUS.");
    }
}

void VerifyPinPagesBadSize(const EnumeratedDevice &dev)
{
    auto page_size = getpagesize();

    struct tenstorrent_pin_pages pin_pages;

    void *p = std::aligned_alloc(page_size, page_size);
    std::unique_ptr<void, Freer> page(p);

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
        pin_pages.in.size = 0;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1)
            THROW_TEST_FAILURE("PIN_PAGES succeeded with size = 0.");
    }

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
        pin_pages.in.size = page_size / 2;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1)
            THROW_TEST_FAILURE("PIN_PAGES succeeded with size = page_size/2.");
    }
}

void VerifyPinPagesNoUnmapped(const EnumeratedDevice &dev)
{
    auto page_size = getpagesize();

    // Set up so that we have one page mapped followed by one page unmapped.
    void *m = mmap(nullptr, 2 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        throw_system_error("two page anonymous mmap failed.");
    std::unique_ptr<void, Unmapper> mapping(m, Unmapper{2});

    if (mmap(m, page_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED)
        throw_system_error("remap to RW failed.");

    struct tenstorrent_pin_pages pin_pages;

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(mapping.get()) + page_size;
        pin_pages.in.size = page_size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1)
            THROW_TEST_FAILURE("PIN_PAGES succeeded on unmapped page.");
    }

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(mapping.get());
        pin_pages.in.size = 2 * page_size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1)
            THROW_TEST_FAILURE("PIN_PAGES succeeded on mapped + unmapped pages.");
    }
}

void VerifyPinPagesMultipleRanges(const EnumeratedDevice &dev)
{
    unsigned int max_pinned_ranges = 1024;

    auto page_size = getpagesize();

    void *p = std::aligned_alloc(page_size, page_size * max_pinned_ranges);
    std::unique_ptr<void, Freer> pages(p);

    DevFd dev_fd(dev.path);

    struct tenstorrent_pin_pages pin_pages;

    for (unsigned int i = 0; i < max_pinned_ranges; i++)
    {
        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(pages.get()) + page_size * i;
        pin_pages.in.size = page_size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != 0)
            THROW_TEST_FAILURE("PIN_PAGES failed on " + std::to_string(i+1) + " concurrent pin.");
    }
}

void VerifyPinPagesContiguous(const EnumeratedDevice &dev)
{
    // To verify contiguous mappings, we need to use a hugepage.

    static const std::string hugepage_parent_dir = "/sys/kernel/mm/hugepages";
    static const std::regex hugepage_subdir_re("hugepages-([0-9]+)kB");

    auto hugepage_subdirs = list_dir(hugepage_parent_dir);

    bool successful_allocation = false;

    for (const auto &hugepage_subdir : hugepage_subdirs)
    {
        std::smatch m;
        if (std::regex_match(hugepage_subdir, m, hugepage_subdir_re))
        {
            auto hugepage_size_kb = std::stoul(m[1]);
            std::size_t hugepage_size = static_cast<std::size_t>(hugepage_size_kb) * 1024;
            unsigned int huge_size_log2 = std::log2(hugepage_size);

            void *m = mmap(nullptr, hugepage_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (huge_size_log2 << MAP_HUGE_SHIFT),
                           -1, 0);

            if (m != MAP_FAILED)
            {
                successful_allocation = true;

                std::unique_ptr<void, Unmapper> mapping(m, Unmapper{hugepage_size / getpagesize()});

                DevFd dev_fd(dev.path);

                struct tenstorrent_pin_pages pin_pages;
                zero(&pin_pages);
                pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

                pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
                pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(mapping.get());
                pin_pages.in.size = hugepage_size;

                if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != 0)
                    THROW_TEST_FAILURE("Hugepage pin failed.");
            }
        }
    }

    if (!successful_allocation)
    {
        std::cout << "No huge pages could be allocated for VerifyPinPagesContiguous, test skipped.\n";
    }
}

void VerifyPinPagesNotContiguous(const EnumeratedDevice &dev)
{
    // How do we get 2 pages that are not physically contiguous?
    // Create a temporary file large enough for 2 pages, mmap it MAP_SHARED and touch the pages.
    // Create a second mapping of the file with the order of the pages swapped.
    // It's not possible for the pages to be physically contiguous in both mappings.

    auto page_size = getpagesize();

    // Create the 2-page temporary file.
    int temp_fd = make_anonymous_temp();

    if (ftruncate(temp_fd, 2 * page_size))
        throw_system_error("failed to resize temporary file.");

    // First mapping
    void *m = mmap(nullptr, 2 * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, temp_fd, 0);
    if (m == MAP_FAILED)
        throw_system_error("2-page temporary file mapping failed.");

    std::unique_ptr<unsigned char, Unmapper> first_mapping(static_cast<unsigned char*>(m), Unmapper{2});

    first_mapping.get()[0] = 1;
    first_mapping.get()[page_size] = 2;

    // Second mapping
    // reserve 2 pages of VA
    m = mmap(nullptr, 2 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    std::unique_ptr<unsigned char, Unmapper> second_mapping(static_cast<unsigned char*>(m), Unmapper{2});

    // map the pages from the file into that space, but in reverse order
    if (mmap(second_mapping.get(), page_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, temp_fd, page_size) == MAP_FAILED)
        throw_system_error("remapping temporary file (first page) failed.");

    if (mmap(second_mapping.get() + page_size, page_size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, temp_fd, 0) == MAP_FAILED)
        throw_system_error("remapping temporary file (second page) failed.");

    if (second_mapping.get()[0] != 2 || second_mapping.get()[page_size] != 1)
        throw std::logic_error("Reverse mapping was not set up correctly in VerifyPinPagesNotContiguous.");

    unsigned int flags = dev.iommu_translated ? 0 : TENSTORRENT_PIN_PAGES_CONTIGUOUS;

    bool first_pin_succeeded = false;
    bool second_pin_succeeded = false;

    struct tenstorrent_pin_pages pin_pages;

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = flags;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(first_mapping.get());
        pin_pages.in.size = 2 * page_size;

        first_pin_succeeded = (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1);
    }

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = flags;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(second_mapping.get());
        pin_pages.in.size = 2 * page_size;

        second_pin_succeeded = (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1);
    }

    if (dev.iommu_translated)
    {
        // With IOMMU enabled, both pin cases must pass, discontiguous pinnings are allowed.
        if (!first_pin_succeeded && !second_pin_succeeded)
            THROW_TEST_FAILURE("Both PIN_PAGES failed in VerifyPinPagesNotContiguous.");

        if (!first_pin_succeeded)
            THROW_TEST_FAILURE("First PIN_PAGES (presumably contiguous) failed in VerifyPinPagesNotContiguous.");

        if (!second_pin_succeeded)
            THROW_TEST_FAILURE("Second PIN_PAGES (presumably discontiguous) failed in VerifyPinPagesNotContiguous.");
    }
    else
    {
        // With IOMMU disabled, at most one can pass. (Both can fail, the pages might not be contiguous.)
        if (first_pin_succeeded && second_pin_succeeded)
            THROW_TEST_FAILURE("PIN_PAGES passed on discontiguous pages.");
    }
}

}

void TestPinPages(const EnumeratedDevice &dev)
{
    VerifyPinPagesSimple(dev);
    VerifyPinPagesBadFlags(dev);
    VerifyPinPagesBadSize(dev);
    VerifyPinPagesNoUnmapped(dev);
    VerifyPinPagesMultipleRanges(dev);
    VerifyPinPagesContiguous(dev);
    VerifyPinPagesNotContiguous(dev);
}
