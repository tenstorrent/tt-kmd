// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Test the procfs pids file that shows which processes have the device open.
// This file shows one PID per line, with one entry per open file descriptor.

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <charconv>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_failure.h"
#include "enumeration.h"
#include "devfd.h"

namespace
{

std::string get_procfs_pids_path(const EnumeratedDevice &dev)
{
    // Extract ordinal from device path like "/dev/tenstorrent/0"
    std::string path = dev.path;
    size_t last_slash = path.find_last_of('/');
    if (last_slash == std::string::npos)
        THROW_TEST_FAILURE("Could not parse device path: " + path);

    std::string ordinal = path.substr(last_slash + 1);
    return "/proc/driver/tenstorrent/" + ordinal + "/pids";
}

bool is_file_readable(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;

    return access(path.c_str(), R_OK) == 0;
}

// Parse PIDs from procfs pids file content
// Returns a vector containing all PIDs found (one per line)
// Fails the test if parsing encounters an error
std::vector<pid_t> parse_pids_from_content(const std::string &content)
{
    std::vector<pid_t> pids;
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip empty lines
        if (line.empty())
            continue;

        // Try to parse the line as a PID
        long parsed_pid;
        auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), parsed_pid);

        // Check for parsing errors
        if (ec == std::errc::invalid_argument) {
            THROW_TEST_FAILURE("Invalid PID format: '" + line + "'");
        }
        if (ec == std::errc::result_out_of_range) {
            THROW_TEST_FAILURE("PID overflow: '" + line + "'");
        }
        if (ptr != line.data() + line.size()) {
            THROW_TEST_FAILURE("Trailing characters in PID: '" + line + "'");
        }

        if (parsed_pid <= 0) {
            THROW_TEST_FAILURE("Invalid PID value: " + std::to_string(parsed_pid));
        }

        pids.push_back(static_cast<pid_t>(parsed_pid));
    }

    return pids;
}

void VerifyProcfsPids(const EnumeratedDevice &dev, const std::string &procfs_pids_path)
{
    pid_t my_pid = getpid();
    std::string pid_str = std::to_string(my_pid);

    std::vector<pid_t> pids_with_fd;
    {
        // Open a device file descriptor
        DevFd dev_fd(dev.path);

        // Read the pids file with the device open
        std::string content_with_fd = read_file(procfs_pids_path);
        pids_with_fd = parse_pids_from_content(content_with_fd);

        // Check that our PID appears
        bool found = false;
        for (pid_t pid : pids_with_fd) {
            if (pid == my_pid) {
                found = true;
                break;
            }
        }
        if (!found)
            THROW_TEST_FAILURE("PID " + pid_str + " not found in procfs pids file");

        // dev_fd destructor closes the file descriptor when it goes out of scope
    }

    // Read the pids file after closing
    std::string content_after_close = read_file(procfs_pids_path);
    std::vector<pid_t> pids_after_close = parse_pids_from_content(content_after_close);

    // Check that our PID no longer appears
    for (pid_t pid : pids_after_close) {
        if (pid == my_pid)
            THROW_TEST_FAILURE("PID " + pid_str + " still in procfs pids file after close");
    }
}

void VerifyProcfsPidsMultipleFds(const EnumeratedDevice &dev, const std::string &procfs_pids_path)
{
    pid_t my_pid = getpid();
    std::string pid_str = std::to_string(my_pid);

    // Open the device multiple times
    DevFd dev_fd1(dev.path);
    DevFd dev_fd2(dev.path);
    DevFd dev_fd3(dev.path);

    // Read the pids file and parse all PIDs
    std::string content = read_file(procfs_pids_path);
    std::vector<pid_t> pids = parse_pids_from_content(content);

    // Count how many times our PID appears (should be 3, once per FD)
    size_t count = 0;
    for (pid_t pid : pids) {
        if (pid == my_pid) {
            count++;
        }
    }

    if (count != 3)
        THROW_TEST_FAILURE("Expected PID to appear 3 times (once per FD), found " + std::to_string(count));
}

} // anonymous namespace

void TestProcfsPids(const EnumeratedDevice &dev)
{
    std::string procfs_pids_path = get_procfs_pids_path(dev);

    // Check if procfs pids file is accessible
    if (!is_file_readable(procfs_pids_path)) {
        std::cout << "Procfs pids file not accessible, skipping test.\n";
        return;
    }

    VerifyProcfsPids(dev, procfs_pids_path);
    VerifyProcfsPidsMultipleFds(dev, procfs_pids_path);
}

