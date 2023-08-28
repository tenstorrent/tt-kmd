// Verify that the contents of /dev/tenstorrent are sensible and complete.

// /dev/tenstorrent/* must be a (symlink to) character device, where its MAJOR:MINOR
// must be a Tenstorrent device. This gives us a list of MAJOR:MINORs.

// If we enumerate all devices with PCI VID 1E52 (/sys/bus/pci/devices/*), they must
// each have /sys/bus/pci/devices/0000\:01\:00.0/tenstorrent/tenstorrent\!*/dev which
// contains a MAJOR:MINOR.

#include "enumeration.h"

#include <regex>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "test_failure.h"
#include "util.h"

bool IsTenstorrentDeviceNode(dev_t device_node)
{
    std::string sys_link = "/sys/dev/char/" + std::to_string(major(device_node)) + ':'
                           + std::to_string(minor(device_node)) + "/subsystem";

    return basename(readlink_str(sys_link)) == "tenstorrent";
}

// For each device in /dev/tenstorrent, returns a pair of absolute /dev/tenstorrent path
// and associated dev_t.
std::map<dev_t, std::string> EnumerateDriverDevices(void)
{
    static const char DEVICE_PATH[] = "/dev/tenstorrent";

    std::map<dev_t, std::string> driver_nodes;

    for (const auto &dev_name : list_dir_full_path(DEVICE_PATH))
    {
        struct stat statbuf;

        if (stat(dev_name.c_str(), &statbuf) == -1)
            throw_system_error("Could not stat " + dev_name);

        if ((statbuf.st_mode & S_IFMT) != S_IFCHR)
            THROW_TEST_FAILURE("Expected " + dev_name + " to be a char dev, but it's not.");

        if (!IsTenstorrentDeviceNode(statbuf.st_rdev))
            THROW_TEST_FAILURE(dev_name + " is not connected to the Tenstorrent driver.");

        driver_nodes[statbuf.st_rdev] = dev_name;
    }

    return driver_nodes;
}

PciBusDeviceFunction ParseBdfFromSysfsPath(const std::string &device_path)
{
    static const std::regex bdf_parse_re("([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2}).([0-7])");

    std::smatch m;
    auto base = basename(device_path);
    if (!std::regex_match(base, m, bdf_parse_re))
        THROW_TEST_FAILURE("PCI device " + base + " has an unparseable bdf in name.");

    return PciBusDeviceFunction{ std::stoul(m[1], nullptr, 16), std::stoul(m[2], nullptr, 16),
                                 std::stoul(m[3], nullptr, 16), std::stoul(m[4]) };
}

// For each tenstorrent device, return pair of PCI BDF and dev_t.
std::map<dev_t, PciBusDeviceFunction> EnumeratePciDevices()
{
    static const char SYS_BUS_PCI_DEVICES[] = "/sys/bus/pci/devices";
    static const unsigned int TT_VENDOR_ID = 0x1E52;

    std::map<dev_t, PciBusDeviceFunction> devices;

    for (const std::string &device_path : list_dir_full_path(SYS_BUS_PCI_DEVICES))
    {
        unsigned long vendor_id = std::stoul(read_file(device_path + "/vendor"), nullptr, 16);

        if (vendor_id != TT_VENDOR_ID)
            continue;

        auto device_node_names = list_dir_full_path(device_path + "/tenstorrent");
        if (device_node_names.empty())
            THROW_TEST_FAILURE("PCI device " + basename(device_path) + " has Tenstorrent vendor ID but no tenstorrent device node.");

        if (device_node_names.size() > 1)
            THROW_TEST_FAILURE("PCI device " + basename(device_path) + " has more than one device node associated with it.");

        std::string device_node_text = read_file(device_node_names[0] + "/dev");
        // device_node_text should be of the form MAJOR:MINOR.

        static const std::regex dev_major_minor_re("(\\d+):(\\d+)\n");

        std::smatch m;
        if (!std::regex_match(device_node_text, m, dev_major_minor_re))
            THROW_TEST_FAILURE("PCI device " + basename(device_path) + " has an unparseable string in dev.");

        devices[makedev(std::stoul(m[1]), std::stoul(m[2]))] = ParseBdfFromSysfsPath(device_path);
    }

    return devices;
}

std::vector<EnumeratedDevice> EnumerateDevices(void)
{
    auto driver_devices = EnumerateDriverDevices();
    auto pci_devices = EnumeratePciDevices();

    std::set<dev_t> driver_nodes;
    for (const auto &driver_dev : driver_devices)
        driver_nodes.insert(driver_dev.first);

    std::set<dev_t> pci_nodes;
    for (const auto &pci_dev : pci_devices)
        pci_nodes.insert(pci_dev.first);

    if (driver_nodes != pci_nodes)
        THROW_TEST_FAILURE("PCI devices and driver-reported devices do not match.");

    // We know they have the same keys.
    std::vector<EnumeratedDevice> devices;
    for (const auto &driver_dev : driver_devices)
    {
        dev_t dev = driver_dev.first;
        devices.push_back({driver_dev.second, pci_devices[dev], dev});
    }

    return devices;
}
