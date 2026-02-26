// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>

#include <sys/ioctl.h>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"

namespace
{

constexpr uint32_t MSG_TYPE_TEST = 0x90;

int send_arc_msg(int fd, tenstorrent_send_arc_msg *msg)
{
    msg->argsz = sizeof(*msg);
    msg->flags = 0;
    return ioctl(fd, TENSTORRENT_IOCTL_SEND_ARC_MSG, msg);
}

void TestEcho(int fd)
{
    uint32_t prev_serial = 0;

    for (uint32_t i = 1; i <= 10; i++)
    {
        tenstorrent_send_arc_msg msg{};
        msg.message[0] = MSG_TYPE_TEST;
        msg.message[1] = i;

        if (send_arc_msg(fd, &msg) != 0)
            THROW_TEST_FAILURE("SEND_ARC_MSG echo failed: " + std::string(std::strerror(errno)));

        if (msg.message[0] != 0)
            THROW_TEST_FAILURE("SEND_ARC_MSG echo: expected header 0, got " + std::to_string(msg.message[0]));

        if (msg.message[1] != i + 1)
            THROW_TEST_FAILURE("SEND_ARC_MSG echo: expected " + std::to_string(i + 1)
                               + ", got " + std::to_string(msg.message[1]));

        // FW returns last_serial+1 in message[2]; verify it advances.
        uint32_t serial = msg.message[2];
        if (i > 1 && serial != prev_serial + 1)
            THROW_TEST_FAILURE("SEND_ARC_MSG echo: serial did not advance (prev="
                               + std::to_string(prev_serial) + " cur=" + std::to_string(serial) + ")");
        prev_serial = serial;

        // FW zeroes the response before populating; unused fields must be 0.
        for (int j = 3; j < 8; j++)
        {
            if (msg.message[j] != 0)
                THROW_TEST_FAILURE("SEND_ARC_MSG echo: expected message[" + std::to_string(j)
                                   + "] == 0, got " + std::to_string(msg.message[j]));
        }
    }
}

void TestUnrecognizedMessage(int fd)
{
    tenstorrent_send_arc_msg msg{};
    msg.message[0] = 0xFF;

    if (send_arc_msg(fd, &msg) == 0)
        THROW_TEST_FAILURE("SEND_ARC_MSG should fail for unrecognized message type");

    if (errno != EREMOTEIO)
        THROW_TEST_FAILURE("SEND_ARC_MSG unrecognized: expected EREMOTEIO, got " + std::string(std::strerror(errno)));

    // Response should be copied back on EREMOTEIO with a nonzero header.
    // BH FW returns 0xFF, WH FW returns 0xFFFFFFFF — both nonzero.
    if (msg.message[0] == 0)
        THROW_TEST_FAILURE("SEND_ARC_MSG unrecognized: expected nonzero response header");
}

void TestBadArgsz(int fd)
{
    tenstorrent_send_arc_msg msg{};
    msg.argsz = 4;  // Too small
    msg.flags = 0;
    msg.message[0] = MSG_TYPE_TEST;

    if (ioctl(fd, TENSTORRENT_IOCTL_SEND_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("SEND_ARC_MSG should fail with bad argsz");

    if (errno != EINVAL)
        THROW_TEST_FAILURE("SEND_ARC_MSG bad argsz: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestBadFlags(int fd)
{
    tenstorrent_send_arc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = 0xFFFFFFFF;
    msg.message[0] = MSG_TYPE_TEST;

    if (ioctl(fd, TENSTORRENT_IOCTL_SEND_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("SEND_ARC_MSG should fail with bad flags");

    if (errno != EINVAL)
        THROW_TEST_FAILURE("SEND_ARC_MSG bad flags: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestRecoveryAfterGarbage(int fd)
{
    // Send an unrecognized message to provoke a FW error.
    tenstorrent_send_arc_msg bad{};
    bad.message[0] = 0xFF;
    send_arc_msg(fd, &bad);

    // Now send a valid echo and verify the queue still works.
    tenstorrent_send_arc_msg msg{};
    msg.message[0] = MSG_TYPE_TEST;
    msg.message[1] = 42;

    if (send_arc_msg(fd, &msg) != 0)
        THROW_TEST_FAILURE("SEND_ARC_MSG failed after garbage: " + std::string(std::strerror(errno)));

    if (msg.message[0] != 0)
        THROW_TEST_FAILURE("SEND_ARC_MSG after garbage: expected header 0, got " + std::to_string(msg.message[0]));

    if (msg.message[1] != 43)
        THROW_TEST_FAILURE("SEND_ARC_MSG after garbage: expected 43, got " + std::to_string(msg.message[1]));
}

using steady_clock = std::chrono::steady_clock;
using dseconds = std::chrono::duration<double>;

constexpr auto THROUGHPUT_DURATION = std::chrono::seconds(1);
constexpr int MIN_EXPECTED_MSG_PER_SEC = 1000;

void TestThroughput(int fd)
{
    int count = 0;
    auto start = steady_clock::now();

    for (;;)
    {
        tenstorrent_send_arc_msg msg{};
        msg.message[0] = MSG_TYPE_TEST;
        msg.message[1] = count;

        if (send_arc_msg(fd, &msg) != 0)
            THROW_TEST_FAILURE("SEND_ARC_MSG throughput test failed: " + std::string(std::strerror(errno)));

        count++;

        if (steady_clock::now() - start >= THROUGHPUT_DURATION)
            break;
    }

    double sec = std::chrono::duration_cast<dseconds>(steady_clock::now() - start).count();
    int rate = static_cast<int>(count / sec);

    std::cout << "  ARC msg throughput: " << rate << " msg/s"
              << " (" << count << " in " << std::fixed << std::setprecision(3) << sec << "s)\n";

    if (rate < MIN_EXPECTED_MSG_PER_SEC)
        THROW_TEST_FAILURE("ARC msg throughput too low: " + std::to_string(rate) + " msg/s");
}

} // namespace

void TestArcMsg(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    // Probe: if the FW doesn't support message queues, skip gracefully.
    {
        tenstorrent_send_arc_msg msg{};
        msg.message[0] = MSG_TYPE_TEST;
        msg.message[1] = 0;

        if (send_arc_msg(dev_fd.get(), &msg) != 0)
        {
            if (errno == EOPNOTSUPP || errno == ETIMEDOUT || errno == EIO)
            {
                std::cout << "ARC message queue not available, skipping test.\n";
                return;
            }
            THROW_TEST_FAILURE("SEND_ARC_MSG probe failed: " + std::string(std::strerror(errno)));
        }
    }

    TestEcho(dev_fd.get());
    TestUnrecognizedMessage(dev_fd.get());
    TestRecoveryAfterGarbage(dev_fd.get());
    TestBadArgsz(dev_fd.get());
    TestBadFlags(dev_fd.get());
    TestThroughput(dev_fd.get());
}
