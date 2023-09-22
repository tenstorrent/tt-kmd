#include <iostream>
#include <optional>
#include <stdexcept>
#include <cstddef>
#include <cstdint>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "enumeration.h"
#include "test_failure.h"
#include "util.h"

namespace
{

const unsigned int command_offset = 4;
const std::uint16_t command_memory_space_enable = 2;
const std::uint16_t command_bus_master_enable = 4;

const unsigned int capabilities_pointer_offset = 0x34;
const unsigned int capabilities_pointer_offset_mask = 0xFC;
const unsigned int cap_id_offset = 0;
const unsigned int next_capability_pointer_offset = 1;

const unsigned int msi_cap_id = 5;
const unsigned int msi_message_control_offset = 2;
const std::uint16_t msi_message_control_msi_enable = 1;
const std::uint16_t msi_message_control_64_bit_address_capable = 0x80;
const unsigned int msi_message_address_offset = 4;
const unsigned int msi_message_upper_address_offset = 8;

const unsigned int pcie_cap_id = 0x10;
const unsigned int device_control_offset = 8;
const std::uint16_t device_control_correctable_error_reporting_enable = 1;
const std::uint16_t device_control_non_fatal_error_reporting_enable = 2;
const std::uint16_t device_control_fatal_error_reporting_enable = 4;
const std::uint16_t device_control_unsupported_error_reporting_enable = 8;

struct config_space_read_error : std::runtime_error
{
    config_space_read_error(unsigned int offset, std::size_t len)
    : std::runtime_error("Kernel rejected config space read."),
      offset(offset), len(len)
    {
    }

    unsigned int offset;
    std::size_t len;
};

template <class T>
T read_config(int config_fd, unsigned int offset)
{
    T value;

    ssize_t len_read = pread(config_fd, &value, sizeof(value), offset);

    if (len_read < 0)
        throw_system_error("Error while reading from the config space");
    else if (len_read != sizeof(value))
        throw config_space_read_error(offset, sizeof(value));
    else
        return value;
}

std::optional<unsigned int> find_capability(int config_fd, unsigned int cap_id)
{
    auto cap_offset = read_config<std::uint8_t>(config_fd, capabilities_pointer_offset) & capabilities_pointer_offset_mask;

    while (cap_offset != 0)
    {
        auto this_cap_id = read_config<std::uint8_t>(config_fd, cap_offset + cap_id_offset);
        if (this_cap_id == cap_id)
            return cap_offset;

        cap_offset = read_config<std::uint8_t>(config_fd, cap_offset + next_capability_pointer_offset);
    }

    return {};
}

void VerifyCommand(int config_fd)
{
    // Verify Command.MemorySpaceEnable = 1 and Command.BusMasterEnable = 1.
    std::uint16_t control = read_config<std::uint16_t>(config_fd, command_offset);

    if (!(control & command_memory_space_enable))
        THROW_TEST_FAILURE("Command.MemorySpaceEnable is not set.");

    if (!(control & command_bus_master_enable))
        THROW_TEST_FAILURE("Command.BusMasterEnable is not set.");
}

void VerifyMSI(int config_fd)
{
    // Check that MSI is enabled and has a non-zero address.
    auto maybe_msi_offset = find_capability(config_fd, msi_cap_id);
    if (!maybe_msi_offset)
        THROW_TEST_FAILURE("MSI capability is missing. Config space may be broken.");

    auto msi_offset = *maybe_msi_offset;

    auto message_control = read_config<std::uint16_t>(config_fd, msi_offset + msi_message_control_offset);
    if (!(message_control & msi_message_control_msi_enable))
        THROW_TEST_FAILURE("MSI is not enabled.");

    auto address_lower = read_config<std::uint32_t>(config_fd, msi_offset + msi_message_address_offset);
    auto address_upper = read_config<std::uint32_t>(config_fd, msi_offset + msi_message_upper_address_offset);

    if (address_lower == 0 && (address_upper == 0 || !(message_control & msi_message_control_64_bit_address_capable)))
        THROW_TEST_FAILURE("MSI address is zero.");
}

void VerifyAER(int config_fd)
{
    // Check that AER is enabled.
    auto maybe_pcie_offset = find_capability(config_fd, pcie_cap_id);
    if (!maybe_pcie_offset)
        THROW_TEST_FAILURE("PCIE capability is missing. Config space may be broken.");

    auto pcie_offset = *maybe_pcie_offset;

    auto any_error_reporting_enable = device_control_correctable_error_reporting_enable
        | device_control_non_fatal_error_reporting_enable
        | device_control_fatal_error_reporting_enable
        | device_control_unsupported_error_reporting_enable;

    auto device_control = read_config<std::uint16_t>(config_fd, pcie_offset + device_control_offset);

    if (!(device_control & any_error_reporting_enable))
        THROW_TEST_FAILURE("AER is disabled.");
}

} // namespace

void TestConfigSpace(const EnumeratedDevice &dev)
{
    auto sysfs_dir = sysfs_dir_for_bdf(dev.location);
    int config_fd = open(std::string(sysfs_dir + "/config").c_str(), O_RDONLY);

    VerifyCommand(config_fd);
    try
    {
        VerifyMSI(config_fd);
    }
    catch (const config_space_read_error &)
    {
        std::cout << "Kernel rejects config space reads, skipping MSI test.\n";
    }

    try
    {
        VerifyAER(config_fd);
    }
    catch (const config_space_read_error &)
    {
        std::cout << "Kernel rejects config space reads, skipping AER test.\n";
    }
}
