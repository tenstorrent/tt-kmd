// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include "devfd.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "util.h"

DevFd::DevFd(const std::string &dev_name)
{
    fd = open(dev_name.c_str(), O_RDWR | O_CLOEXEC);
    if (fd == -1)
        throw_system_error("Opening " + dev_name);
}

DevFd::~DevFd()
{
    if (fd != -1)
        close(fd);
}

DevFd::DevFd(DevFd &&that)
{
    fd = that.fd;
    that.fd = -1;
}
