// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <string>
#include <regex>

#include <sys/ioctl.h>

#include "ioctl.h"

#include "util.h"
#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"

namespace {

void parse_driver_version(const std::string &version_str, int &major, int &minor, int &patch)
{
    // From semver.org:
    std::regex version_regex(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$)");
    std::smatch match;

    major = minor = patch = 0;

    if (std::regex_match(version_str, match, version_regex)) {
        major = std::stoi(match[1].str());
        minor = std::stoi(match[2].str());
        patch = std::stoi(match[3].str());
    }
}

} // namespace

void TestGetDriverInfo(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    tenstorrent_get_driver_info get_driver_info{};
    get_driver_info.in.output_size_bytes = sizeof(get_driver_info.out);

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_GET_DRIVER_INFO, &get_driver_info) != 0)
        THROW_TEST_FAILURE("TENSTORRENT_IOCTL_GET_DRIVER_INFO failed on " + dev.path);

    std::size_t min_get_driver_info_out
        = offsetof(tenstorrent_get_driver_info_out, driver_version)
          + sizeof(get_driver_info.out.driver_version);

    if (get_driver_info.out.output_size_bytes < min_get_driver_info_out)
        THROW_TEST_FAILURE("GET_DRIVER_INFO output is too small.");

    if (get_driver_info.out.output_size_bytes > sizeof(get_driver_info.out))
        THROW_TEST_FAILURE("GET_DRIVER_INFO output is too large. (Test may be out of date.)");

    if (get_driver_info.out.driver_version != TENSTORRENT_DRIVER_VERSION)
        THROW_TEST_FAILURE("GET_DRIVER_INFO reports an unexpected driver version.");

    std::string sysfs_version = read_file("/sys/module/tenstorrent/version");
    int major, minor, patch;
    sysfs_version.pop_back(); // drop the '\n' at the end
    parse_driver_version(sysfs_version, major, minor, patch);

    if (get_driver_info.out.driver_version_major != major
        || get_driver_info.out.driver_version_minor != minor
        || get_driver_info.out.driver_version_patch != patch) {
        THROW_TEST_FAILURE("GET_DRIVER_INFO reports an unexpected driver version: "
                           + std::to_string(get_driver_info.out.driver_version_major) + "."
                           + std::to_string(get_driver_info.out.driver_version_minor) + "."
                           + std::to_string(get_driver_info.out.driver_version_patch));
    }
}
