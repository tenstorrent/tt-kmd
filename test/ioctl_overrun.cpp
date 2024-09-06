// Try to catch ioctls that read or write the wrong amount of data.
//
// When an ioctl input has output_size_bytes, we align the input to the end of the page
// and set output_size_bytes = 0. This should result in no output being written and no error.
// This catches read and write overruns.
//
// When an ioctl input doesn't have output_size_bytes, we align the entire structure to the
// end of the page. This catches write overruns.
// If hardware had support for PROT_WRITE without PROT_READ we could also check for read overruns.

#include <memory>
#include <string>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include "devfd.h"
#include "enumeration.h"
#include "ioctl.h"
#include "test_failure.h"
#include "util.h"

namespace
{

// Allocate data aligned to the end of a page, guaranteeing that the next page is unmapped.
template <class T>
class EndOfPage
{
public:
    EndOfPage(const T& init = {});
    ~EndOfPage();

    EndOfPage(const EndOfPage<T>&) = delete;
    void operator = (const EndOfPage<T>&) = delete;

    T *get();

private:
    void *mapping = nullptr;
    T *value = nullptr;

    static std::size_t mapping_size();
};

template <class T>
std::size_t EndOfPage<T>::mapping_size()
{
    return round_up(sizeof(T), page_size()) + page_size();
}

template <class T>
EndOfPage<T>::EndOfPage(const T& init)
{
    auto size = mapping_size();

    mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mapping == MAP_FAILED)
        throw_system_error("end-of-page mapping allocation failed");

    void *final_page = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(mapping) + size - page_size());

    if (mprotect(final_page, page_size(), PROT_NONE) != 0)
        throw_system_error("failed to disable access to overrun detection page");

    void *p = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(final_page) - sizeof(T));
    value = new (p) T(init);
}

template <class T>
EndOfPage<T>::~EndOfPage()
{
    value->~T();
    munmap(mapping, mapping_size());
}

template <class T>
T *EndOfPage<T>::get()
{
    return value;
}

// The assumption is that the ioctl_data is aligned to the end of the page and no EFAULT should occur.
#define CHECK_IOCTL_OVERRUN(fd, ioctl_name, ioctl_data) CheckIoctlOverrun(fd, ioctl_name, #ioctl_name, ioctl_data)
#define CHECK_IOCTL_OVERRUN_ERROR(fd, ioctl_name, ioctl_data, expected_error) CheckIoctlOverrun(fd, ioctl_name, #ioctl_name, ioctl_data, expected_error)

template <class IoctlData>
void CheckIoctlOverrun(int fd, unsigned long ioctl_code, const char *ioctl_name, const IoctlData& ioctl_data, int expected_error = 0)
{
    EndOfPage<IoctlData> aligned_ioctl_data(ioctl_data);

    int result = ioctl(fd, ioctl_code, aligned_ioctl_data.get());

    if (result != 0)
    {
        if (errno == EFAULT)
            THROW_TEST_FAILURE(std::string(ioctl_name) + " failed overrun check.");
        else if (errno != expected_error)
            THROW_TEST_FAILURE(std::string(ioctl_name) + " overrun check failed other than EFAULT.");
    }
}

void TestGetDeviceInfoOverrun(int fd)
{
    tenstorrent_get_device_info_in in{};
    in.output_size_bytes = 0;

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, in);
}

void TestQueryMappingsOverrun(int fd)
{
    tenstorrent_query_mappings_in in{};
    in.output_mapping_count = 0;

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_QUERY_MAPPINGS, in);
}

void TestAllocateDmaBufOverrun(int fd)
{
    tenstorrent_allocate_dma_buf alloc_buf{};

    alloc_buf.in.requested_size = page_size();
    alloc_buf.in.buf_index = 0;

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF, alloc_buf);
}

void TestFreeDmaBufOverrun(int fd)
{
    tenstorrent_free_dma_buf free_buf{};

    CHECK_IOCTL_OVERRUN_ERROR(fd, TENSTORRENT_IOCTL_FREE_DMA_BUF, free_buf, EINVAL);
}

void TestGetDriverInfoOverrun(int fd)
{
    tenstorrent_get_driver_info_in in{};
    in.output_size_bytes = 0;

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_GET_DRIVER_INFO, in);
}

void TestResetDeviceOverrun(int fd)
{
    tenstorrent_reset_device_in in{};
    in.output_size_bytes = 0;
    in.flags = TENSTORRENT_RESET_DEVICE_RESTORE_STATE;

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_RESET_DEVICE, in);
}

void TestPinPagesOverrun(int fd)
{
    std::unique_ptr<void, Freer> page(std::aligned_alloc(page_size(), page_size()));

    tenstorrent_pin_pages_in in{};
    in.output_size_bytes = 0;
    in.virtual_address = reinterpret_cast<std::uintptr_t>(page.get());
    in.size = page_size();

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_PIN_PAGES, in);
}

void TestLockCtlOverrun(int fd)
{
    tenstorrent_lock_ctl_in in{};
    in.output_size_bytes = 0;
    in.flags = TENSTORRENT_LOCK_CTL_TEST;
    in.index = 0;

    CHECK_IOCTL_OVERRUN(fd, TENSTORRENT_IOCTL_LOCK_CTL, in);
}

void TestMapPeerBarOverrun(int fd)
{
    // TENSTORRENT_IOCTL_MAP_PEER_BAR requires 2 devices and doesn't have output_size_bytes
    // so we can only test that it rejects the input without EFAULT.

    tenstorrent_map_peer_bar_in in{};

    in.peer_fd = fd;
    in.peer_bar_index = 0;
    in.peer_bar_offset = 0;
    in.peer_bar_length = page_size();
    in.flags = 0;

    CHECK_IOCTL_OVERRUN_ERROR(fd, TENSTORRENT_IOCTL_MAP_PEER_BAR, in, EINVAL);
}

}

void TestIoctlOverrun(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    TestGetDeviceInfoOverrun(dev_fd.get());
    // TENSTORRENT_IOCTL_GET_HARVESTING simply fails.
    TestQueryMappingsOverrun(dev_fd.get());
    TestAllocateDmaBufOverrun(dev_fd.get());
    TestFreeDmaBufOverrun(dev_fd.get());
    TestGetDriverInfoOverrun(dev_fd.get());
    TestResetDeviceOverrun(dev_fd.get());
    TestPinPagesOverrun(dev_fd.get());
    TestLockCtlOverrun(dev_fd.get());
    TestMapPeerBarOverrun(dev_fd.get());
}
