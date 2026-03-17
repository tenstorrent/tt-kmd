// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"

namespace
{

constexpr uint32_t MSG_TYPE_TEST = 0x90;

// Helper: call ARC_MSG ioctl with the given flags and message content.
int arc_msg_ioctl(int fd, uint32_t flags, uint32_t *message)
{
    tenstorrent_arc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = flags;
    msg.queue_index = 0;

    if (message)
        memcpy(msg.message, message, sizeof(msg.message));

    int ret = ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg);

    if (message)
        memcpy(message, msg.message, sizeof(msg.message));

    return ret;
}

// Helper: POST|POLL a message and return the ioctl result.
int post_poll(int fd, uint32_t *message)
{
    return arc_msg_ioctl(fd, TENSTORRENT_ARC_MSG_POST | TENSTORRENT_ARC_MSG_POLL, message);
}

// Helper: standalone POLL.
int poll_msg(int fd, uint32_t *message)
{
    return arc_msg_ioctl(fd, TENSTORRENT_ARC_MSG_POLL, message);
}

// Helper: standalone POST.
int post_msg(int fd, uint32_t *message)
{
    return arc_msg_ioctl(fd, TENSTORRENT_ARC_MSG_POST, message);
}

// Helper: ABANDON.
int abandon_msg(int fd)
{
    return arc_msg_ioctl(fd, TENSTORRENT_ARC_MSG_ABANDON, nullptr);
}

// --- Ioctl contract tests ---

void TestBadArgsz(int fd)
{
    tenstorrent_arc_msg msg{};
    msg.argsz = 4;  // Too small
    msg.flags = TENSTORRENT_ARC_MSG_POST | TENSTORRENT_ARC_MSG_POLL;
    msg.message[0] = MSG_TYPE_TEST;

    if (ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("ARC_MSG should fail with bad argsz");

    if (errno != EINVAL)
        THROW_TEST_FAILURE("ARC_MSG bad argsz: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestBadFlags(int fd)
{
    tenstorrent_arc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = 0xFF;
    msg.message[0] = MSG_TYPE_TEST;

    if (ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("ARC_MSG should fail with unknown flags");

    if (errno != EINVAL)
        THROW_TEST_FAILURE("ARC_MSG bad flags: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestZeroFlags(int fd)
{
    tenstorrent_arc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = 0;

    if (ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("ARC_MSG should fail with flags == 0");

    if (errno != EINVAL)
        THROW_TEST_FAILURE("ARC_MSG zero flags: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestInvalidFlagCombinations(int fd)
{
    tenstorrent_arc_msg msg{};
    msg.argsz = sizeof(msg);

    // ABANDON | POST
    msg.flags = TENSTORRENT_ARC_MSG_ABANDON | TENSTORRENT_ARC_MSG_POST;
    if (ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("ARC_MSG should fail with ABANDON|POST");
    if (errno != EINVAL)
        THROW_TEST_FAILURE("ARC_MSG ABANDON|POST: expected EINVAL, got " + std::string(std::strerror(errno)));

    // ABANDON | POLL
    msg.flags = TENSTORRENT_ARC_MSG_ABANDON | TENSTORRENT_ARC_MSG_POLL;
    if (ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("ARC_MSG should fail with ABANDON|POLL");
    if (errno != EINVAL)
        THROW_TEST_FAILURE("ARC_MSG ABANDON|POLL: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestBadQueueIndex(int fd)
{
    tenstorrent_arc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_ARC_MSG_POST | TENSTORRENT_ARC_MSG_POLL;
    msg.queue_index = 1;
    msg.message[0] = MSG_TYPE_TEST;

    if (ioctl(fd, TENSTORRENT_IOCTL_ARC_MSG, &msg) == 0)
        THROW_TEST_FAILURE("ARC_MSG should fail with queue_index != 0");

    if (errno != EINVAL)
        THROW_TEST_FAILURE("ARC_MSG bad queue_index: expected EINVAL, got " + std::string(std::strerror(errno)));
}

void TestPollWithNoMessage(int fd)
{
    uint32_t message[8] = {};
    int ret = poll_msg(fd, message);

    if (ret == 0)
        THROW_TEST_FAILURE("POLL with no message should not return success");

    if (errno != ESRCH)
        THROW_TEST_FAILURE("POLL with no message: expected ESRCH, got " + std::string(std::strerror(errno)));
}

void TestAbandonWithNoMessage(int fd)
{
    if (abandon_msg(fd) != 0)
        THROW_TEST_FAILURE("ABANDON with no message should succeed, got " + std::string(std::strerror(errno)));
}

void TestDoublePOST(int fd)
{
    uint32_t message[8] = {};
    message[0] = MSG_TYPE_TEST;

    // First POST should succeed.
    if (post_msg(fd, message) != 0)
        THROW_TEST_FAILURE("First POST failed: " + std::string(std::strerror(errno)));

    // Second POST should fail with EBUSY.
    message[0] = MSG_TYPE_TEST;
    if (post_msg(fd, message) == 0)
        THROW_TEST_FAILURE("Second POST should fail with EBUSY");

    if (errno != EBUSY)
        THROW_TEST_FAILURE("Double POST: expected EBUSY, got " + std::string(std::strerror(errno)));

    // Clean up: poll until complete, then abandon as fallback.
    for (int i = 0; i < 1000; i++) {
        if (poll_msg(fd, message) == 0)
            return;
        usleep(100);
    }
    abandon_msg(fd);
}

// --- Hardware integration tests ---

void TestPostPollEcho(int fd)
{
    for (uint32_t i = 1; i <= 10; i++) {
        uint32_t message[8] = {};
        message[0] = MSG_TYPE_TEST;
        message[1] = i;

        if (post_poll(fd, message) != 0)
            THROW_TEST_FAILURE("POST|POLL echo failed: " + std::string(std::strerror(errno)));

        if (message[0] != 0)
            THROW_TEST_FAILURE("POST|POLL echo: expected header 0, got " + std::to_string(message[0]));

        if (message[1] != i + 1)
            THROW_TEST_FAILURE("POST|POLL echo: expected " + std::to_string(i + 1) + ", got " + std::to_string(message[1]));
    }
}

void TestPostThenPoll(int fd)
{
    uint32_t message[8] = {};
    message[0] = MSG_TYPE_TEST;
    message[1] = 42;

    if (post_msg(fd, message) != 0)
        THROW_TEST_FAILURE("POST failed: " + std::string(std::strerror(errno)));

    // Poll until complete.
    for (int i = 0; i < 10000; i++) {
        int ret = poll_msg(fd, message);
        if (ret == 0) {
            if (message[0] != 0)
                THROW_TEST_FAILURE("POST then POLL: expected header 0, got " + std::to_string(message[0]));
            if (message[1] != 43)
                THROW_TEST_FAILURE("POST then POLL: expected 43, got " + std::to_string(message[1]));
            return;
        }
        if (errno != EAGAIN)
            THROW_TEST_FAILURE("POLL unexpected error: " + std::string(std::strerror(errno)));
        usleep(100);
    }

    THROW_TEST_FAILURE("POST then POLL: timed out waiting for response");
}

void TestPollDrainsResponse(int fd)
{
    uint32_t message[8] = {};
    message[0] = MSG_TYPE_TEST;

    if (post_poll(fd, message) != 0)
        THROW_TEST_FAILURE("POST|POLL failed: " + std::string(std::strerror(errno)));

    // fd should now be IDLE.  POLL should return ESRCH.
    if (poll_msg(fd, message) == 0)
        THROW_TEST_FAILURE("POLL after drain should not succeed");
    if (errno != ESRCH)
        THROW_TEST_FAILURE("POLL after drain: expected ESRCH, got " + std::string(std::strerror(errno)));

    // A new POST should succeed.
    message[0] = MSG_TYPE_TEST;
    if (post_poll(fd, message) != 0)
        THROW_TEST_FAILURE("POST after drain failed: " + std::string(std::strerror(errno)));
}

void TestPostAbandonPost(int fd)
{
    uint32_t message[8] = {};
    message[0] = MSG_TYPE_TEST;
    message[1] = 100;

    if (post_msg(fd, message) != 0)
        THROW_TEST_FAILURE("First POST failed: " + std::string(std::strerror(errno)));

    if (abandon_msg(fd) != 0)
        THROW_TEST_FAILURE("ABANDON failed: " + std::string(std::strerror(errno)));

    // Second POST should succeed now that the fd is back to IDLE.
    message[0] = MSG_TYPE_TEST;
    message[1] = 200;

    if (post_poll(fd, message) != 0)
        THROW_TEST_FAILURE("POST after ABANDON failed: " + std::string(std::strerror(errno)));

    if (message[0] != 0)
        THROW_TEST_FAILURE("POST after ABANDON: expected header 0, got " + std::to_string(message[0]));
    if (message[1] != 201)
        THROW_TEST_FAILURE("POST after ABANDON: expected 201, got " + std::to_string(message[1]));
}

void TestCloseAbandonsInflight(const EnumeratedDevice &dev)
{
    pid_t pid = fork();
    if (pid == -1)
        THROW_TEST_FAILURE("fork() failed");

    if (pid == 0) {
        // Child: open fd, POST a message, exit without polling or abandoning.
        DevFd child_fd(dev.path);
        uint32_t message[8] = {};
        message[0] = MSG_TYPE_TEST;
        post_msg(child_fd.get(), message);
        _exit(0);
    }

    int status;
    if (waitpid(pid, &status, 0) == -1)
        THROW_TEST_FAILURE("waitpid() failed");

    // Queue should be healthy.  Verify by sending a message on a new fd.
    DevFd fd(dev.path);
    uint32_t message[8] = {};
    message[0] = MSG_TYPE_TEST;
    message[1] = 99;

    if (post_poll(fd.get(), message) != 0)
        THROW_TEST_FAILURE("POST|POLL after child exit failed: " + std::string(std::strerror(errno)));

    if (message[1] != 100)
        THROW_TEST_FAILURE("POST|POLL after child exit: expected 100, got " + std::to_string(message[1]));
}

void TestUnrecognizedMessage(int fd)
{
    uint32_t message[8] = {};
    message[0] = 0xFF;

    if (post_poll(fd, message) == 0)
        THROW_TEST_FAILURE("POST|POLL should fail for unrecognized message type");

    if (errno != EREMOTEIO)
        THROW_TEST_FAILURE("Unrecognized message: expected EREMOTEIO, got " + std::string(std::strerror(errno)));

    // Response should be copied back with a nonzero header.
    if (message[0] == 0)
        THROW_TEST_FAILURE("Unrecognized message: expected nonzero response header");
}

void TestRecoveryAfterError(int fd)
{
    // Send an unrecognized message to provoke a FW error.
    uint32_t message[8] = {};
    message[0] = 0xFF;
    post_poll(fd, message);

    // Now send a valid echo and verify the queue still works.
    memset(message, 0, sizeof(message));
    message[0] = MSG_TYPE_TEST;
    message[1] = 42;

    if (post_poll(fd, message) != 0)
        THROW_TEST_FAILURE("POST|POLL after error failed: " + std::string(std::strerror(errno)));

    if (message[0] != 0)
        THROW_TEST_FAILURE("Recovery: expected header 0, got " + std::to_string(message[0]));
    if (message[1] != 43)
        THROW_TEST_FAILURE("Recovery: expected 43, got " + std::to_string(message[1]));
}

void TestThroughput(int fd)
{
    int count = 0;
    auto start = std::chrono::steady_clock::now();
    auto end = start + std::chrono::seconds(1);

    while (std::chrono::steady_clock::now() < end) {
        uint32_t message[8] = {};
        message[0] = MSG_TYPE_TEST;
        message[1] = count;

        if (post_poll(fd, message) != 0)
            THROW_TEST_FAILURE("Throughput POST|POLL failed: " + std::string(std::strerror(errno)));

        count++;
    }

    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    int rate = static_cast<int>(count / sec);

    std::cout << "  ARC msg throughput: " << rate << " msg/s"
              << " (" << count << " in " << std::fixed << std::setprecision(3) << sec << "s)\n";

    if (rate < 1000)
        THROW_TEST_FAILURE("ARC msg throughput too low: " + std::to_string(rate) + " msg/s");
}

void TestAbandonBeforeSubmit(const EnumeratedDevice &dev)
{
    // Use two fds: fd2 occupies the inflight slot so fd1's message stays
    // QUEUED in the SW queue.  Abandon fd1's message while still QUEUED.
    DevFd fd1(dev.path);
    DevFd fd2(dev.path);

    for (int i = 0; i < 100; i++) {
        // fd2 posts to occupy the inflight slot.
        uint32_t msg2[8] = {};
        msg2[0] = MSG_TYPE_TEST;
        if (post_msg(fd2.get(), msg2) != 0)
            THROW_TEST_FAILURE("fd2 POST failed: " + std::string(std::strerror(errno)));

        // fd1 posts — should stay QUEUED behind fd2's message.
        uint32_t msg1[8] = {};
        msg1[0] = MSG_TYPE_TEST;
        if (post_msg(fd1.get(), msg1) != 0) {
            // Clean up fd2.
            uint32_t drain[8] = {};
            post_poll(fd2.get(), drain);
            THROW_TEST_FAILURE("fd1 POST failed: " + std::string(std::strerror(errno)));
        }

        // Abandon fd1's message while it's still in the SW queue.
        if (abandon_msg(fd1.get()) != 0)
            THROW_TEST_FAILURE("ABANDON of QUEUED message failed: " + std::string(std::strerror(errno)));

        // Drain fd2's message.
        uint32_t drain[8] = {};
        for (int j = 0; j < 10000; j++) {
            if (poll_msg(fd2.get(), drain) == 0)
                break;
            if (errno != EAGAIN)
                THROW_TEST_FAILURE("fd2 POLL error: " + std::string(std::strerror(errno)));
            usleep(100);
        }
    }
}

void TestRapidPostAbandon(int fd)
{
    for (int i = 0; i < 10000; i++) {
        uint32_t message[8] = {};
        message[0] = MSG_TYPE_TEST;
        message[1] = i;

        if (post_msg(fd, message) != 0)
            THROW_TEST_FAILURE("Rapid POST failed at " + std::to_string(i) + ": " + std::string(std::strerror(errno)));

        if (abandon_msg(fd) != 0)
            THROW_TEST_FAILURE("Rapid ABANDON failed at " + std::to_string(i) + ": " + std::string(std::strerror(errno)));
    }

    // Verify the fd is still healthy after the storm.
    uint32_t message[8] = {};
    message[0] = MSG_TYPE_TEST;
    message[1] = 0xDEAD;

    if (post_poll(fd, message) != 0)
        THROW_TEST_FAILURE("POST|POLL after rapid abandon storm failed: " + std::string(std::strerror(errno)));

    if (message[1] != 0xDEAE)
        THROW_TEST_FAILURE("Echo mismatch after storm: expected 0xDEAE, got " + std::to_string(message[1]));
}

} // namespace

void TestArcMsg(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);

    // Ioctl contract tests — these exercise the validation layer and work
    // regardless of whether the FW supports message queues.
    TestBadArgsz(dev_fd.get());
    TestBadFlags(dev_fd.get());
    TestZeroFlags(dev_fd.get());
    TestInvalidFlagCombinations(dev_fd.get());
    TestBadQueueIndex(dev_fd.get());

    // Probe: if the FW doesn't support message queues, skip the rest.
    {
        uint32_t message[8] = {};
        message[0] = MSG_TYPE_TEST;

        if (post_poll(dev_fd.get(), message) != 0) {
            if (errno == EOPNOTSUPP) {
                std::cout << "  ARC message queue not available, skipping HW tests.\n";
                return;
            }
            THROW_TEST_FAILURE("ARC_MSG probe failed: " + std::string(std::strerror(errno)));
        }
    }

    TestPollWithNoMessage(dev_fd.get());
    TestAbandonWithNoMessage(dev_fd.get());
    TestPostPollEcho(dev_fd.get());
    TestPostThenPoll(dev_fd.get());
    TestPollDrainsResponse(dev_fd.get());
    TestDoublePOST(dev_fd.get());
    TestPostAbandonPost(dev_fd.get());
    TestUnrecognizedMessage(dev_fd.get());
    TestRecoveryAfterError(dev_fd.get());
    TestCloseAbandonsInflight(dev);
    TestThroughput(dev_fd.get());
    TestAbandonBeforeSubmit(dev);
    TestRapidPostAbandon(dev_fd.get());
}
