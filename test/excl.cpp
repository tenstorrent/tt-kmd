// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Test O_EXCL exclusive-open semantics on the chardev.
//
// While an O_EXCL fd is open: plain open() returns EBUSY, and any other
// open(O_EXCL) blocks (or returns EAGAIN with O_NONBLOCK). The blocking
// variant wakes when open_fds_list becomes empty (i.e. on the last release).

#include <atomic>
#include <chrono>
#include <cerrno>
#include <string>
#include <thread>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "enumeration.h"
#include "test_failure.h"

namespace
{

// Raw open() wrapper that records errno on failure. We can't use DevFd because
// it always opens O_RDWR | O_CLOEXEC and throws -- we need to inspect EBUSY,
// EAGAIN, and EINTR here.
int open_with_flags(const std::string &path, int extra_flags)
{
    return open(path.c_str(), O_RDWR | O_CLOEXEC | extra_flags);
}

void expect_open_fails(const std::string &path, int extra_flags, int expected_errno, const char *what)
{
    int fd = open_with_flags(path, extra_flags);
    if (fd >= 0) {
        close(fd);
        THROW_TEST_FAILURE(std::string(what) + ": open() unexpectedly succeeded");
    }
    if (errno != expected_errno)
        THROW_TEST_FAILURE(std::string(what) + ": expected errno " + std::to_string(expected_errno)
                           + ", got " + std::to_string(errno));
}

// Two plain opens of the same device coexist (baseline -- ensure the common
// case still works).
void VerifyPlainCoexists(const EnumeratedDevice &dev)
{
    int a = open_with_flags(dev.path, 0);
    if (a < 0)
        THROW_TEST_FAILURE("First plain open should succeed");

    int b = open_with_flags(dev.path, 0);
    if (b < 0) {
        close(a);
        THROW_TEST_FAILURE("Second plain open should succeed alongside the first");
    }

    close(a);
    close(b);
}

// O_EXCL on an idle device succeeds.
void VerifyExclOnIdleSucceeds(const EnumeratedDevice &dev)
{
    int fd = open_with_flags(dev.path, O_EXCL);
    if (fd < 0)
        THROW_TEST_FAILURE("O_EXCL open on an idle device should succeed");
    close(fd);
}

// While an O_EXCL fd is held: plain open returns EBUSY, O_EXCL|O_NONBLOCK
// returns EAGAIN.
void VerifyExclBlocksOthers(const EnumeratedDevice &dev)
{
    int excl = open_with_flags(dev.path, O_EXCL);
    if (excl < 0)
        THROW_TEST_FAILURE("Initial O_EXCL open should succeed");

    expect_open_fails(dev.path, 0, EBUSY,
                      "plain open while O_EXCL is held");
    expect_open_fails(dev.path, O_EXCL | O_NONBLOCK, EAGAIN,
                      "O_EXCL|O_NONBLOCK while O_EXCL is held");

    close(excl);
}

// While a plain fd is held: O_EXCL|O_NONBLOCK returns EAGAIN.
void VerifyExclNonblockBlockedByPlain(const EnumeratedDevice &dev)
{
    int plain = open_with_flags(dev.path, 0);
    if (plain < 0)
        THROW_TEST_FAILURE("Plain open should succeed");

    expect_open_fails(dev.path, O_EXCL | O_NONBLOCK, EAGAIN,
                      "O_EXCL|O_NONBLOCK while plain fd is open");

    close(plain);
}

// Releasing the O_EXCL fd reopens the device to ordinary opens.
void VerifyExclReleaseRestoresAccess(const EnumeratedDevice &dev)
{
    int excl = open_with_flags(dev.path, O_EXCL);
    if (excl < 0)
        THROW_TEST_FAILURE("O_EXCL open should succeed");
    close(excl);

    int plain = open_with_flags(dev.path, 0);
    if (plain < 0)
        THROW_TEST_FAILURE("Plain open should succeed after O_EXCL holder closes");
    close(plain);
}

// Blocking O_EXCL waits while another fd is open, then proceeds when that
// fd closes. Mirrors VerifyBlockingLock in lock.cpp.
void VerifyExclBlocksUntilIdle(const EnumeratedDevice &dev)
{
    int plain = open_with_flags(dev.path, 0);
    if (plain < 0)
        THROW_TEST_FAILURE("Plain open should succeed");

    std::atomic<bool> thread_started{false};
    std::atomic<int> blocked_fd{-2};
    std::atomic<int> blocked_err{0};

    std::thread blocker([&]() {
        thread_started = true;
        int fd = open_with_flags(dev.path, O_EXCL);
        blocked_err = (fd < 0) ? errno : 0;
        blocked_fd = fd;
    });

    while (!thread_started)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (blocked_fd.load() != -2) {
        close(plain);
        if (blocked_fd.load() >= 0)
            close(blocked_fd.load());
        blocker.join();
        THROW_TEST_FAILURE("O_EXCL open should block while a plain fd is open");
    }

    close(plain);
    blocker.join();

    int fd = blocked_fd.load();
    if (fd < 0)
        THROW_TEST_FAILURE("O_EXCL open should succeed after plain fd closes, errno="
                           + std::to_string(blocked_err.load()));
    close(fd);
}

// Blocking O_EXCL wakes when the holder dies and the kernel cleans up its fd,
// mirroring VerifyBlockingWakesOnExit in lock.cpp.
void VerifyExclWakesOnHolderExit(const EnumeratedDevice &dev)
{
    pid_t pid = fork();
    if (pid == -1)
        THROW_TEST_FAILURE("fork() failed");

    if (pid == 0) {
        // Child holds the device for a short while, then exits without
        // explicitly closing -- exit() releases the fd.
        int fd = open_with_flags(dev.path, 0);
        if (fd < 0)
            _exit(2);
        usleep(100000);
        _exit(0);
    }

    // Give the child time to open before we try to acquire.
    usleep(10000);

    // Blocks until the child exits and the kernel releases its fd.
    int fd = open_with_flags(dev.path, O_EXCL);
    int err = errno;

    int status;
    if (waitpid(pid, &status, 0) == -1)
        THROW_TEST_FAILURE("waitpid() failed");

    if (fd < 0)
        THROW_TEST_FAILURE("O_EXCL should succeed after holder exits, errno="
                           + std::to_string(err));
    close(fd);
}

// A signal without SA_RESTART interrupts a blocking O_EXCL open and the
// syscall returns EINTR. Mirrors the signal pattern used in lock.cpp's
// VerifySARestart, but with SA_RESTART explicitly cleared so the syscall is
// not transparently restarted.
void sigusr1_noop(int) {}

void VerifyExclInterruptedBySignal(const EnumeratedDevice &dev)
{
    int plain = open_with_flags(dev.path, 0);
    if (plain < 0)
        THROW_TEST_FAILURE("Plain open should succeed");

    struct sigaction sa{};
    sa.sa_handler = sigusr1_noop;
    sa.sa_flags = 0;          // No SA_RESTART: open() returns EINTR.
    sigemptyset(&sa.sa_mask);

    struct sigaction old_sa{};
    if (sigaction(SIGUSR1, &sa, &old_sa) == -1) {
        close(plain);
        THROW_TEST_FAILURE("sigaction() failed");
    }

    std::atomic<bool> thread_started{false};
    std::atomic<int> blocked_fd{-2};
    std::atomic<int> blocked_err{0};

    std::thread blocker([&]() {
        thread_started = true;
        int fd = open_with_flags(dev.path, O_EXCL);
        blocked_err = (fd < 0) ? errno : 0;
        blocked_fd = fd;
    });

    while (!thread_started)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    pthread_kill(blocker.native_handle(), SIGUSR1);
    blocker.join();

    sigaction(SIGUSR1, &old_sa, nullptr);
    close(plain);

    int fd = blocked_fd.load();
    if (fd >= 0) {
        close(fd);
        THROW_TEST_FAILURE("O_EXCL should have returned EINTR, not succeeded");
    }
    if (blocked_err.load() != EINTR)
        THROW_TEST_FAILURE("O_EXCL should have returned EINTR, got errno="
                           + std::to_string(blocked_err.load()));
}

} // namespace

void TestExcl(const EnumeratedDevice &dev)
{
    VerifyPlainCoexists(dev);
    VerifyExclOnIdleSucceeds(dev);
    VerifyExclBlocksOthers(dev);
    VerifyExclNonblockBlockedByPlain(dev);
    VerifyExclReleaseRestoresAccess(dev);
    VerifyExclBlocksUntilIdle(dev);
    VerifyExclWakesOnHolderExit(dev);
    VerifyExclInterruptedBySignal(dev);
}
