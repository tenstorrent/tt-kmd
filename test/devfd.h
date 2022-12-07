#pragma once

#include <string>

class DevFd
{
public:
    explicit DevFd(const std::string &dev_name);
    ~DevFd();

    DevFd(DevFd &&that);

    int get() { return fd; }

private:
    int fd;
};
