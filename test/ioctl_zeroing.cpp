// Some ioctls have an output_size_bytes input value. When the actual output
// data is smaller than output_size_bytes, the remainder must be zero-filled.

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <sys/ioctl.h>

#include "aligned_allocator.h"
#include "devfd.h"
#include "enumeration.h"
#include "ioctl.h"
#include "test_failure.h"
#include "util.h"

namespace
{

#define CHECK_IOCTL_ZEROING(fd, ioctl_name, ioctl_data) CheckIoctlZeroing(fd, ioctl_name, #ioctl_name, ioctl_data)

template <class IoctlData>
void CheckIoctlZeroing(int fd, unsigned long ioctl_code, const char *ioctl_name, const IoctlData &ioctl_data)
{
    std::vector<unsigned char, AlignedAllocator<unsigned char, alignof(IoctlData)>> buf;
    buf.resize(offsetof(IoctlData, out) + page_size(), 0xFF);

    IoctlData *ioctl_param = new (buf.data()) IoctlData(ioctl_data);

    if (ioctl(fd, ioctl_code, ioctl_param) != 0)
        THROW_TEST_FAILURE(std::string(ioctl_name) + " ioctl errored in zeroing test.");

    if (std::find_if(buf.begin()+sizeof(IoctlData), buf.end(),
                     [] (unsigned char c) { return c != 0; }) != buf.end())
        THROW_TEST_FAILURE(std::string(ioctl_name) + " did not zero the entire output range.");
}

void TestGetDeviceInfoZeroing(int fd)
{
    tenstorrent_get_device_info get_device_info{};
    get_device_info.in.output_size_bytes = page_size();

    CHECK_IOCTL_ZEROING(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, get_device_info);
}

void TestGetDriverInfoZeroing(int fd)
{
    tenstorrent_get_driver_info get_driver_info{};
    get_driver_info.in.output_size_bytes = page_size();

    CHECK_IOCTL_ZEROING(fd, TENSTORRENT_IOCTL_GET_DRIVER_INFO, get_driver_info);
}

void TestResetDeviceZeroing(int fd)
{
    tenstorrent_reset_device reset_device{};
    reset_device.in.output_size_bytes = page_size();
    reset_device.in.flags = TENSTORRENT_RESET_DEVICE_RESTORE_STATE;

    CHECK_IOCTL_ZEROING(fd, TENSTORRENT_IOCTL_RESET_DEVICE, reset_device);
}

void TestPinPagesZeroing(int fd)
{
    std::unique_ptr<void, Freer> page(std::aligned_alloc(page_size(), page_size()));

    tenstorrent_pin_pages pin_pages{};
    pin_pages.in.output_size_bytes = page_size();
    pin_pages.in.virtual_address = reinterpret_cast<std::uintptr_t>(page.get());
    pin_pages.in.size = page_size();

    CHECK_IOCTL_ZEROING(fd, TENSTORRENT_IOCTL_PIN_PAGES, pin_pages);
}

void TestLockCtlZeroing(int fd)
{
    tenstorrent_lock_ctl lock_ctl{};
    lock_ctl.in.output_size_bytes = page_size();
    lock_ctl.in.flags = TENSTORRENT_LOCK_CTL_TEST;
    lock_ctl.in.index = 0;

    CHECK_IOCTL_ZEROING(fd, TENSTORRENT_IOCTL_LOCK_CTL, lock_ctl);
}

}

void TestIoctlZeroing(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    TestGetDeviceInfoZeroing(dev_fd.get());
    // TENSTORRENT_IOCTL_GET_HARVESTING simply fails.
    // TENSTORRENT_IOCTL_QUERY_MAPPINGS is complicated, has its own test.
    // TENSTORRENT_IOCTL_ALLOCATE_DMA_BUF does not zero.
    // TENSTORRENT_IOCTL_FREE_DMA_BUF does not zero.
    TestGetDriverInfoZeroing(dev_fd.get());
    TestResetDeviceZeroing(dev_fd.get());
    TestPinPagesZeroing(dev_fd.get());
    TestLockCtlZeroing(dev_fd.get());
    // TENSTORRENT_IOCTL_MAP_PEER_BAR does not zero.
}
