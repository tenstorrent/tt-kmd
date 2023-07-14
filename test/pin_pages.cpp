// Verify that pin pages requires TENSTORRENT_PIN_PAGES_CONTIGUOUS.
// Verify that pin pages rejects any other flags.
// Verify that pin pages rejects size == 0 and size not multiple of page size.
// Verify that pin pages rejects an unmapped range, a partially unmapped range.
// Verify that pin pages accepts a single page.
// Verify that pin pages can simultaneously pin many ranges.

#include <iostream>
#include <fstream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#include <cerrno>
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

    {
        DevFd dev_fd(dev.path);

        zero(&pin_pages);
        pin_pages.in.output_size_bytes = sizeof(pin_pages.out);

        pin_pages.in.flags = TENSTORRENT_PIN_PAGES_CONTIGUOUS;
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

        pin_pages.in.flags = 0;
        pin_pages.in.virtual_address = reinterpret_cast<uintptr_t>(page.get());
        pin_pages.in.size = page_size;

        if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_PIN_PAGES, &pin_pages) != -1)
            THROW_TEST_FAILURE("PIN_PAGES succeeded with flags = 0.");
    }

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

}

void TestPinPages(const EnumeratedDevice &dev)
{
    VerifyPinPagesSimple(dev);
    VerifyPinPagesBadFlags(dev);
    VerifyPinPagesBadSize(dev);
    VerifyPinPagesNoUnmapped(dev);
    VerifyPinPagesMultipleRanges(dev);
}
