// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <cerrno>
#include <cstddef>
#include <cstring>

#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "util.h"

void throw_system_error(const std::string &msg)
{
    throw std::system_error(errno, std::system_category(), msg);
}

std::string read_file(std::string filename)
{
    std::ifstream file(filename);
    if (!file)
        throw_system_error("Can't open file " + filename);

    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Return a list of all names except . and .. in the directory dir_name.
std::vector<std::string> list_dir(const std::string &dir_name)
{
    std::vector<std::string> names;

    DIR *dir = opendir(dir_name.c_str());
    if (dir == nullptr)
        throw_system_error("Can't open directory " + dir_name);

    try
    {
        struct dirent *d;
        errno = 0;  // detect readdir failures
        while ((d = readdir(dir)) != nullptr)
        {
            if (std::strcmp(d->d_name, ".") == 0 || std::strcmp(d->d_name, "..") == 0)
                continue;

            names.push_back(d->d_name);

            errno = 0;
        }

        if (errno != 0)
            throw_system_error("Error while listing " + dir_name);

    } catch (...)
    {
        closedir(dir);
        throw;
    }
    closedir(dir);

    return names;
}

std::vector<std::string> list_dir_full_path(const std::string &dir_name)
{
    const char *separator = (dir_name.empty() || dir_name.back() == '/') ? "" : "/";

    std::vector<std::string> full_paths;
    for (const std::string &name : list_dir(dir_name))
    {
        full_paths.push_back(dir_name + separator + name);
    }

    return full_paths;
}

// Returns the final component of the path in filename.
std::string basename(const std::string &filename)
{
    auto last_before_slash = filename.find_last_not_of('/');
    if (last_before_slash == std::string::npos)
    {
        // e.g. "///" return "", also handles empty filename
        return std::string();
    }

    // e.g. a/b/cd//
    // last_before_slash has index of d
    auto prior_slash = filename.find_last_of('/', last_before_slash);
    if (prior_slash == std::string::npos)
    {
        return filename.substr(0, last_before_slash+1);
    }
    else
    {
        return filename.substr(prior_slash+1, last_before_slash - prior_slash);
    }
}

std::string readlink_str(const std::string &link_name)
{
    std::vector<char> buf(PATH_MAX);
    while (true)
    {
        ssize_t bytes_out = readlink(link_name.c_str(), buf.data(), buf.size());
        if (bytes_out < 0)
            throw_system_error("Could not read symbolic link target for " + link_name);

        if (static_cast<std::size_t>(bytes_out) < buf.size())
            return std::string(buf.data(), buf.data() + bytes_out);

        if (buf.size() > std::numeric_limits<std::vector<char>::size_type>::max() / 2)
            throw std::runtime_error("Could not read symbolic link target for " + link_name + ", it's too long.");

        buf.resize(buf.size() * 2);
    }
}

std::string sysfs_dir_for_bdf(PciBusDeviceFunction bdf)
{
    static const char format[] = "/sys/bus/pci/devices/%04x:%02x:%02x.%u";

    char buf[sizeof(format)];

    snprintf(buf, sizeof(buf), format, bdf.domain, bdf.bus, bdf.device, bdf.function);

    return buf;
}

unsigned page_size()
{
    return static_cast<unsigned>(sysconf(_SC_PAGE_SIZE));
}

std::string PciBusDeviceFunction::format() const
{
    static const char format[] = "%04x:%02x:%02x.%u";

    char buf[sizeof(format)];

    snprintf(buf, sizeof(buf), format, domain, bus, device, function);

    return buf;
}
