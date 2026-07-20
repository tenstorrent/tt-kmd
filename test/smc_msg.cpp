// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>

#include <sys/ioctl.h>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"

namespace
{

// TEST is handled by both WH and BH queue firmware: it echoes word 1 + 1 and
// reports status 0 in word 0.
constexpr uint32_t SMC_MSG_TYPE_TEST = 0x90;

// 0x02 is not a message type on either WH or BH, so the firmware replies with
// a nonzero status and the driver reports -EREMOTEIO.
constexpr uint32_t SMC_MSG_TYPE_UNKNOWN = 0x02;

constexpr auto POLL_TIMEOUT = std::chrono::seconds(5);

// Returns 0 or -errno.
int smc_msg_ioctl(DevFd &dev, tenstorrent_smc_msg &msg)
{
    if (ioctl(dev.get(), TENSTORRENT_IOCTL_SMC_MSG, &msg) != 0)
        return -errno;
    return 0;
}

int post(DevFd &dev, uint32_t type, uint32_t arg)
{
    tenstorrent_smc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_POST;
    msg.message[0] = type;
    msg.message[1] = arg;

    return smc_msg_ioctl(dev, msg);
}

int poll_once(DevFd &dev, uint32_t response[8])
{
    tenstorrent_smc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_POLL;

    int ret = smc_msg_ioctl(dev, msg);
    std::memcpy(response, msg.message, sizeof(msg.message));
    return ret;
}

// Polls until the message completes (0 or -EREMOTEIO) or the timeout expires.
int poll_wait(DevFd &dev, uint32_t response[8])
{
    auto deadline = std::chrono::steady_clock::now() + POLL_TIMEOUT;

    for (;;) {
        int ret = poll_once(dev, response);
        if (ret != -EAGAIN)
            return ret;
        if (std::chrono::steady_clock::now() >= deadline)
            THROW_TEST_FAILURE("Timed out waiting for SMC message response");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int abandon(DevFd &dev)
{
    tenstorrent_smc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_ABANDON;

    return smc_msg_ioctl(dev, msg);
}

// Input validation is checked before the driver touches the hardware queue,
// so these hold even on devices without queue support.
void VerifySmcMsgValidation(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);
    tenstorrent_smc_msg msg{};

    // Bad argsz.
    msg.argsz = sizeof(msg) - 1;
    msg.flags = TENSTORRENT_SMC_MSG_ABANDON;
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("Bad argsz should fail with EINVAL");

    // No operation selected.
    msg = {};
    msg.argsz = sizeof(msg);
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("flags = 0 should fail with EINVAL");

    // POST and POLL may not be combined.
    msg = {};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_POST | TENSTORRENT_SMC_MSG_POLL;
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("POST|POLL should fail with EINVAL");

    // ABANDON is mutually exclusive with POST.
    msg = {};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_POST | TENSTORRENT_SMC_MSG_ABANDON;
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("POST|ABANDON should fail with EINVAL");

    // Undefined flag bits.
    msg = {};
    msg.argsz = sizeof(msg);
    msg.flags = 1u << 31;
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("Unknown flag should fail with EINVAL");

    // Nonzero queue_index is reserved.
    msg = {};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_ABANDON;
    msg.queue_index = 1;
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("Nonzero queue_index should fail with EINVAL");

    // Nonzero reserved0.
    msg = {};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_ABANDON;
    msg.reserved0 = 1;
    if (smc_msg_ioctl(fd, msg) != -EINVAL)
        THROW_TEST_FAILURE("Nonzero reserved0 should fail with EINVAL");

    // POLL with nothing outstanding.
    uint32_t response[8];
    if (poll_once(fd, response) != -ESRCH)
        THROW_TEST_FAILURE("POLL with nothing outstanding should fail with ESRCH");

    // ABANDON with nothing outstanding is a no-op.
    if (abandon(fd) != 0)
        THROW_TEST_FAILURE("ABANDON with nothing outstanding should succeed");
}

void VerifyTestMessage(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);
    uint32_t response[8];

    if (post(fd, SMC_MSG_TYPE_TEST, 42) != 0)
        THROW_TEST_FAILURE("POST of test message failed");

    if (poll_wait(fd, response) != 0)
        THROW_TEST_FAILURE("Test message failed");

    if (response[0] != 0)
        THROW_TEST_FAILURE("Test message response has nonzero status");
    if (response[1] != 43)
        THROW_TEST_FAILURE("Test message response should echo argument + 1");

    // The completed message was consumed; the fd is idle again.
    if (poll_once(fd, response) != -ESRCH)
        THROW_TEST_FAILURE("POLL after completion should fail with ESRCH");
}

void VerifyOneOutstandingPerFd(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);
    uint32_t response[8];

    if (post(fd, SMC_MSG_TYPE_TEST, 1) != 0)
        THROW_TEST_FAILURE("POST failed");

    if (post(fd, SMC_MSG_TYPE_TEST, 2) != -EBUSY)
        THROW_TEST_FAILURE("Second POST on same fd should fail with EBUSY");

    if (poll_wait(fd, response) != 0)
        THROW_TEST_FAILURE("Test message failed");
    if (response[1] != 2)
        THROW_TEST_FAILURE("Response should be for the first POST");
}

void VerifyMultipleFds(const EnumeratedDevice &dev)
{
    DevFd fd0(dev.path);
    DevFd fd1(dev.path);
    uint32_t response[8];

    // Both fds have a message outstanding at once; the driver serializes them
    // through the single firmware queue and routes each response to its owner.
    if (post(fd0, SMC_MSG_TYPE_TEST, 100) != 0)
        THROW_TEST_FAILURE("POST on fd0 failed");
    if (post(fd1, SMC_MSG_TYPE_TEST, 200) != 0)
        THROW_TEST_FAILURE("POST on fd1 failed");

    // Collect in reverse order to show completion isn't tied to poll order.
    if (poll_wait(fd1, response) != 0)
        THROW_TEST_FAILURE("Test message on fd1 failed");
    if (response[1] != 201)
        THROW_TEST_FAILURE("fd1 received a response not its own");

    if (poll_wait(fd0, response) != 0)
        THROW_TEST_FAILURE("Test message on fd0 failed");
    if (response[1] != 101)
        THROW_TEST_FAILURE("fd0 received a response not its own");
}

void VerifyAbandon(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);
    uint32_t response[8];

    if (post(fd, SMC_MSG_TYPE_TEST, 3) != 0)
        THROW_TEST_FAILURE("POST failed");

    if (abandon(fd) != 0)
        THROW_TEST_FAILURE("ABANDON of outstanding message failed");

    if (poll_once(fd, response) != -ESRCH)
        THROW_TEST_FAILURE("POLL after ABANDON should fail with ESRCH");

    // The abandoned response is discarded when it arrives; a new message must
    // not receive it.
    if (post(fd, SMC_MSG_TYPE_TEST, 4) != 0)
        THROW_TEST_FAILURE("POST after ABANDON failed");
    if (poll_wait(fd, response) != 0)
        THROW_TEST_FAILURE("Test message after ABANDON failed");
    if (response[1] != 5)
        THROW_TEST_FAILURE("Response after ABANDON should be for the new POST");
}

void VerifyCloseAbandons(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);
    uint32_t response[8];

    // Close an fd with a message outstanding; release implicitly abandons it.
    {
        DevFd doomed(dev.path);
        if (post(doomed, SMC_MSG_TYPE_TEST, 6) != 0)
            THROW_TEST_FAILURE("POST on doomed fd failed");
    }

    // The orphaned response must not disturb a subsequent message.
    if (post(fd, SMC_MSG_TYPE_TEST, 7) != 0)
        THROW_TEST_FAILURE("POST after close failed");
    if (poll_wait(fd, response) != 0)
        THROW_TEST_FAILURE("Test message after close failed");
    if (response[1] != 8)
        THROW_TEST_FAILURE("Response after close should be for the new POST");
}

void VerifyFirmwareError(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);
    uint32_t response[8];

    if (post(fd, SMC_MSG_TYPE_UNKNOWN, 0) != 0)
        THROW_TEST_FAILURE("POST of unknown message failed");

    if (poll_wait(fd, response) != -EREMOTEIO)
        THROW_TEST_FAILURE("Unknown message should fail with EREMOTEIO");
    if (response[0] == 0)
        THROW_TEST_FAILURE("Failed message should have nonzero status");

    // A firmware error completes the message; the fd is idle again.
    if (post(fd, SMC_MSG_TYPE_TEST, 9) != 0)
        THROW_TEST_FAILURE("POST after firmware error failed");
    if (poll_wait(fd, response) != 0)
        THROW_TEST_FAILURE("Test message after firmware error failed");
    if (response[1] != 10)
        THROW_TEST_FAILURE("Response after firmware error should be for the new POST");
}

} // namespace

void TestSmcMsg(const EnumeratedDevice &dev)
{
    VerifySmcMsgValidation(dev);

    // Probe for queue support; old firmware doesn't publish a message queue.
    DevFd probe(dev.path);
    int ret = post(probe, SMC_MSG_TYPE_TEST, 0);
    if (ret == -EOPNOTSUPP) {
        std::cout << "SMC message queue not supported, skipping SMC message tests.\n";
        return;
    }
    if (ret != 0)
        THROW_TEST_FAILURE("POST of test message failed");

    uint32_t response[8];
    if (poll_wait(probe, response) != 0)
        THROW_TEST_FAILURE("Test message failed");

    VerifyTestMessage(dev);
    VerifyOneOutstandingPerFd(dev);
    VerifyMultipleFds(dev);
    VerifyAbandon(dev);
    VerifyCloseAbandons(dev);
    VerifyFirmwareError(dev);
}
