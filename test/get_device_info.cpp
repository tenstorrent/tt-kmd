#include <string>

#include <sys/ioctl.h>

#include "ioctl.h"

#include "util.h"
#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"

void TestGetDeviceInfo(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    tenstorrent_get_device_info get_device_info;
    zero(&get_device_info);
    get_device_info.in.output_size_bytes = sizeof(get_device_info.out);

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_GET_DEVICE_INFO, &get_device_info) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_GET_DEVICE_INFO failed on " + dev.path);

    // pci_domain has been present since 1.23.
    std::size_t min_get_device_info_out
        = offsetof(tenstorrent_get_device_info_out, pci_domain)
          + sizeof(get_device_info.out.pci_domain);

    if (get_device_info.out.output_size_bytes < min_get_device_info_out)
        THROW_TEST_FAILURE("GET_DEVICE_INFO output is too small.");

    auto sysfs_pci_dir = sysfs_dir_for_bdf(dev.location);
    auto expected_vendor_id = std::stoul(read_file(sysfs_pci_dir + "/vendor"), nullptr, 16);
    auto expected_device_id = std::stoul(read_file(sysfs_pci_dir + "/device"), nullptr, 16);
    auto expected_subsystem_vendor_id = std::stoul(read_file(sysfs_pci_dir + "/subsystem_vendor"), nullptr, 16);
    auto expected_subsystem_device_id = std::stoul(read_file(sysfs_pci_dir + "/subsystem_device"), nullptr, 16);

    if (get_device_info.out.vendor_id != expected_vendor_id)
        THROW_TEST_FAILURE("Wrong vendor id for " + dev.path);

    if (get_device_info.out.device_id != expected_device_id)
        THROW_TEST_FAILURE("Wrong device id for " + dev.path);

    if (get_device_info.out.subsystem_vendor_id != expected_subsystem_vendor_id)
        THROW_TEST_FAILURE("Wrong subsystem vendor id id for " + dev.path);

    if (get_device_info.out.subsystem_id != expected_subsystem_device_id)
        THROW_TEST_FAILURE("Wrong subsystem id for " + dev.path);

    unsigned bus = (get_device_info.out.bus_dev_fn >> 8) & 0xFF;
    unsigned device = (get_device_info.out.bus_dev_fn >> 3) & 0x1F;
    unsigned function = get_device_info.out.bus_dev_fn & 0x7;
    unsigned domain = get_device_info.out.pci_domain;

    if (domain != dev.location.domain || bus != dev.location.bus
        || device != dev.location.device || function != dev.location.function)
        THROW_TEST_FAILURE("Wrong BDF for " + dev.path);

    if (get_device_info.out.max_dma_buf_size_log2 < 12)
        THROW_TEST_FAILURE("max_dma_buf_size_log2 is improbably small for " + dev.path);

    if (get_device_info.out.max_dma_buf_size_log2 > 63)
        THROW_TEST_FAILURE("max_dma_buf_size_log2 is improbably large for " + dev.path);
}
