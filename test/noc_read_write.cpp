// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <iostream>

#include <algorithm>
#include <array>
#include <memory>
#include <random>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"
#include "tlbs.h"


namespace {

}

void TestNocReadWrite(const EnumeratedDevice &dev) {
    DevFd dev_fd(dev.path);

    struct tenstorrent_noc_write_byte noc_write_byte;
    struct tenstorrent_noc_read_byte noc_read_byte;

    noc_write_byte.in.x = 1;
    noc_write_byte.in.y = 2;
    noc_write_byte.in.addr = 0x1000;
    noc_write_byte.in.write_value = 0xAB;
    noc_write_byte.in.noc = 0;
    
    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_NOC_WRITE_BYTE, &noc_write_byte) != 0) {
        THROW_TEST_FAILURE("NOC_WRITE_BYTE failed.");
    }

    noc_read_byte.out.read_value = 0;
    noc_read_byte.in.x = 1;
    noc_read_byte.in.y = 2;
    noc_read_byte.in.addr = 0x1000;
    noc_read_byte.in.noc = 0;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_NOC_READ_BYTE, &noc_read_byte) != 0) {
        THROW_TEST_FAILURE("NOC_READ_BYTE failed.");
    }

    if (noc_read_byte.out.read_value != 0xAB) {
        THROW_TEST_FAILURE("NOC_READ_BYTE returned incorrect value.");
    }
}