// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <string>
#include <vector>
#include <cstring>

struct PciBusDeviceFunction
{
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int function;

    std::string format() const;
};

template <class T>
static inline void zero(T* p)
{
    std::memset(p, 0, sizeof(*p));
}

[[noreturn]] void throw_system_error(const std::string &msg);

std::string read_file(std::string filename);

// Return a list of all names except . and .. in the directory dir_name.
std::vector<std::string> list_dir(const std::string &dir_name);

// Return a list of all files except . and .. in the directory dir_name, with paths including dir_name.
std::vector<std::string> list_dir_full_path(const std::string &dir_name);

// Returns the final component of the path in filename.
std::string basename(const std::string &filename);

// Returns the target for a symlink.
std::string readlink_str(const std::string &link_name);

std::string sysfs_dir_for_bdf(PciBusDeviceFunction bdf);

unsigned page_size();
