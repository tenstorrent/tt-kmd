// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <string>

#include <sys/ioctl.h>

#include "ioctl.h"

#include "util.h"
#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"

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
}
