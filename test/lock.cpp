// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"

namespace
{

// Lock state bits returned by query_lock()
static constexpr uint8_t LOCK_LOCAL  = 0b01;  // This fd holds the lock
static constexpr uint8_t LOCK_GLOBAL = 0b10;  // Some fd holds the lock

// Returns true if lock was acquired, false if already held by someone.
bool acquire_lock(DevFd &dev, uint8_t index)
{
    tenstorrent_lock_ctl ctl{};
    ctl.in.output_size_bytes = sizeof(ctl.out);
    ctl.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
    ctl.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &ctl) != 0)
        THROW_TEST_FAILURE("LOCK_CTL acquire ioctl failed");

    return ctl.out.value != 0;
}

// Returns true if lock was released, false if we didn't hold it.
bool release_lock(DevFd &dev, uint8_t index)
{
    tenstorrent_lock_ctl ctl{};
    ctl.in.output_size_bytes = sizeof(ctl.out);
    ctl.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
    ctl.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &ctl) != 0)
        THROW_TEST_FAILURE("LOCK_CTL release ioctl failed");

    return ctl.out.value != 0;
}

// Returns lock state: LOCK_LOCAL if we hold it, LOCK_GLOBAL if anyone holds it.
uint8_t query_lock(DevFd &dev, uint8_t index)
{
    tenstorrent_lock_ctl ctl{};
    ctl.in.output_size_bytes = sizeof(ctl.out);
    ctl.in.flags = TENSTORRENT_LOCK_CTL_TEST;
    ctl.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &ctl) != 0)
        THROW_TEST_FAILURE("LOCK_CTL query ioctl failed");

    return ctl.out.value;
}

// Blocks until lock is acquired.
void acquire_lock_blocking(DevFd &dev, uint8_t index)
{
    tenstorrent_lock_ctl ctl{};
    ctl.in.output_size_bytes = sizeof(ctl.out);
    ctl.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING;
    ctl.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &ctl) != 0)
        THROW_TEST_FAILURE("LOCK_CTL blocking acquire ioctl failed");
}

// Static state for SA_RESTART signal handler test.
static int sa_restart_fd = -1;
static uint8_t sa_restart_index = 0;

static void sa_restart_handler(int)
{
    tenstorrent_lock_ctl ctl{};
    ctl.in.output_size_bytes = sizeof(ctl.out);
    ctl.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
    ctl.in.index = sa_restart_index;
    ioctl(sa_restart_fd, TENSTORRENT_IOCTL_LOCK_CTL, &ctl);
}

void VerifyLockSemantics(const EnumeratedDevice &dev)
{
    DevFd fd0(dev.path);
    DevFd fd1(dev.path);

    // 1. Acquire and release works.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");
    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");

    // 2. Can't release an unheld lock.
    if (release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 released lock 0 but didn't hold it");

    // 3. Can't release another fd's lock.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");
    if (release_lock(fd1, 0))
        THROW_TEST_FAILURE("fd1 released lock 0 held by fd0");
    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");

    // 4. Not re-entrant: same fd can't acquire twice.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");
    if (acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 acquired lock 0 twice (should not be re-entrant)");
    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");

    // 5. Exclusive: different fd can't acquire held lock.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");
    if (acquire_lock(fd1, 0))
        THROW_TEST_FAILURE("fd1 acquired lock 0 held by fd0");
    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");

    // 6. Query shows local vs global state correctly.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");

    uint8_t state0 = query_lock(fd0, 0);
    if (state0 != (LOCK_LOCAL | LOCK_GLOBAL))
        THROW_TEST_FAILURE("fd0 should see local+global for lock it holds");

    uint8_t state1 = query_lock(fd1, 0);
    if (state1 != LOCK_GLOBAL)
        THROW_TEST_FAILURE("fd1 should see only global for lock held by fd0");

    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");

    // 7. Lock indices are independent.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");
    if (!acquire_lock(fd1, 1))
        THROW_TEST_FAILURE("fd1 should acquire lock 1 (independent of lock 0)");
    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");
    if (!release_lock(fd1, 1))
        THROW_TEST_FAILURE("fd1 should release lock 1");

    // 8. Close auto-releases locks.
    {
        DevFd fd2(dev.path);
        if (!acquire_lock(fd2, 0))
            THROW_TEST_FAILURE("fd2 should acquire lock 0");

        uint8_t state = query_lock(fd2, 0);
        if (state != (LOCK_LOCAL | LOCK_GLOBAL))
            THROW_TEST_FAILURE("fd2 should see local+global");

        // fd1 can't steal the lock while fd2 holds it.
        if (release_lock(fd1, 0))
            THROW_TEST_FAILURE("fd1 released lock 0 held by fd2");
    }
    // fd2 closed here, lock should be auto-released.

    uint8_t state_after = query_lock(fd1, 0);
    if (state_after != 0)
        THROW_TEST_FAILURE("Lock 0 should be free after fd2 closed");

    if (!acquire_lock(fd1, 0))
        THROW_TEST_FAILURE("fd1 should acquire lock 0 after fd2 closed");
    if (!release_lock(fd1, 0))
        THROW_TEST_FAILURE("fd1 should release lock 0");
}

void VerifyLockBounds(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);

    // Index at the limit should fail.
    tenstorrent_lock_ctl ctl{};
    ctl.in.output_size_bytes = sizeof(ctl.out);
    ctl.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
    ctl.in.index = TENSTORRENT_RESOURCE_LOCK_COUNT;

    if (ioctl(fd.get(), TENSTORRENT_IOCTL_LOCK_CTL, &ctl) != -1)
        THROW_TEST_FAILURE("Acquire with out-of-bounds index should fail");
    if (errno != EINVAL)
        THROW_TEST_FAILURE("Acquire with out-of-bounds index should fail with EINVAL");

    // Max valid index should work.
    uint8_t max_index = TENSTORRENT_RESOURCE_LOCK_COUNT - 1;
    if (!acquire_lock(fd, max_index))
        THROW_TEST_FAILURE("Should acquire max index lock");
    if (!release_lock(fd, max_index))
        THROW_TEST_FAILURE("Should release max index lock");
}

void VerifyAllLocks(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);

    // Acquire all 64 locks.
    for (uint8_t i = 0; i < TENSTORRENT_RESOURCE_LOCK_COUNT; i++) {
        if (!acquire_lock(fd, i))
            THROW_TEST_FAILURE("Should acquire all locks");
    }

    // Verify all are held.
    for (uint8_t i = 0; i < TENSTORRENT_RESOURCE_LOCK_COUNT; i++) {
        uint8_t state = query_lock(fd, i);
        if (state != (LOCK_LOCAL | LOCK_GLOBAL))
            THROW_TEST_FAILURE("All locks should show local+global");
    }

    // Release all.
    for (uint8_t i = 0; i < TENSTORRENT_RESOURCE_LOCK_COUNT; i++) {
        if (!release_lock(fd, i))
            THROW_TEST_FAILURE("Should release all locks");
    }
}

void VerifyBlockingLock(const EnumeratedDevice &dev)
{
    DevFd fd0(dev.path);
    DevFd fd1(dev.path);

    // fd0 holds the lock.
    if (!acquire_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should acquire lock 0");

    std::atomic<bool> thread_acquired{false};
    std::atomic<bool> thread_started{false};

    // Thread blocks waiting for the lock.
    std::thread blocker([&]() {
        thread_started = true;
        acquire_lock_blocking(fd1, 0);
        thread_acquired = true;
    });

    // Wait for thread to start and enter the blocking call.
    while (!thread_started)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Thread should still be blocked.
    if (thread_acquired)
        THROW_TEST_FAILURE("Thread acquired lock while it should be blocked");

    // Release wakes the blocked thread.
    if (!release_lock(fd0, 0))
        THROW_TEST_FAILURE("fd0 should release lock 0");

    blocker.join();

    if (!thread_acquired)
        THROW_TEST_FAILURE("Thread should have acquired lock after release");

    // fd1 now holds the lock.
    uint8_t state = query_lock(fd1, 0);
    if (state != (LOCK_LOCAL | LOCK_GLOBAL))
        THROW_TEST_FAILURE("fd1 should hold lock after blocking acquire");

    if (!release_lock(fd1, 0))
        THROW_TEST_FAILURE("fd1 should release lock 0");
}

// C++ Lockable implementation for use with std::unique_lock.
class DeviceLock
{
public:
    DeviceLock(DevFd &dev, uint8_t index) : dev_(dev), index_(index) {}

    void lock() { acquire_lock_blocking(dev_, index_); }
    bool try_lock() { return acquire_lock(dev_, index_); }

    // unlock() shouldn't throw per BasicLockable, but for test code a clear
    // failure beats silent misbehavior.
    void unlock()
    {
        if (!release_lock(dev_, index_)) {
            if (std::uncaught_exceptions() == 0)
                THROW_TEST_FAILURE("DeviceLock::unlock() failed");
            // Unwinding already; don't throw to avoid std::terminate().
        }
    }

private:
    DevFd &dev_;
    uint8_t index_;
};

void VerifyLockable(const EnumeratedDevice &dev)
{
    DevFd fd0(dev.path);
    DevFd fd1(dev.path);
    DeviceLock lock0(fd0, 0);
    DeviceLock lock1(fd1, 0);

    // std::unique_lock with try_lock.
    {
        std::unique_lock<DeviceLock> guard(lock0);

        uint8_t state = query_lock(fd0, 0);
        if (state != (LOCK_LOCAL | LOCK_GLOBAL))
            THROW_TEST_FAILURE("unique_lock should hold lock");

        // try_lock fails from another fd.
        std::unique_lock<DeviceLock> guard2(lock1, std::try_to_lock);
        if (guard2.owns_lock())
            THROW_TEST_FAILURE("try_lock should fail when lock is held");
    }

    // Lock released after scope exit.
    uint8_t state = query_lock(fd0, 0);
    if (state != 0)
        THROW_TEST_FAILURE("Lock should be free after unique_lock destructor");

    // Blocking acquisition from another thread via std::unique_lock.
    std::atomic<bool> thread_acquired{false};
    {
        std::unique_lock<DeviceLock> guard(lock0);

        std::thread blocker([&]() {
            std::unique_lock<DeviceLock> guard2(lock1);
            thread_acquired = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (thread_acquired)
            THROW_TEST_FAILURE("Thread should be blocked");

        guard.unlock();
        blocker.join();
    }

    if (!thread_acquired)
        THROW_TEST_FAILURE("Thread should have acquired lock");

    // Thread's unique_lock already released when thread exited.
    state = query_lock(fd0, 0);
    if (state != 0)
        THROW_TEST_FAILURE("Lock should be free after thread exit");
}

// Verify that process exit releases locks even without explicit unlock.
// The child opens its own fd (not inherited) so it has a separate lock context.
void VerifyExitReleasesLock(const EnumeratedDevice &dev)
{
    pid_t pid = fork();
    if (pid == -1)
        THROW_TEST_FAILURE("fork() failed");

    if (pid == 0) {
        // Child: open our own fd, acquire lock, then exit without releasing.
        DevFd child_fd(dev.path);
        if (!acquire_lock(child_fd, 0))
            _exit(2);
        _exit(1);
    }

    // Parent: wait for child to terminate.
    int status;
    if (waitpid(pid, &status, 0) == -1)
        THROW_TEST_FAILURE("waitpid() failed");

    // Lock should now be available since the child's fd was closed on exit.
    DevFd parent_fd(dev.path);
    if (!acquire_lock(parent_fd, 0))
        THROW_TEST_FAILURE("Should acquire lock after child exit");
    if (!release_lock(parent_fd, 0))
        THROW_TEST_FAILURE("Should release lock");
}

// Verify that a blocking acquire wakes when the holder exits unexpectedly.
// This tests that wake_up_interruptible is called during fd cleanup.
void VerifyBlockingWakesOnExit(const EnumeratedDevice &dev)
{
    DevFd parent_fd(dev.path);

    pid_t pid = fork();
    if (pid == -1)
        THROW_TEST_FAILURE("fork() failed");

    if (pid == 0) {
        // Child: acquire lock, hold it briefly, then exit.
        DevFd child_fd(dev.path);
        if (!acquire_lock(child_fd, 0))
            _exit(2);
        usleep(100000);  // Hold for 100ms.
        _exit(1);
    }

    // Give child time to acquire the lock.
    usleep(10000);

    // This blocks until child exits and the kernel releases the lock.
    acquire_lock_blocking(parent_fd, 0);

    // Reap the child.
    int status;
    if (waitpid(pid, &status, 0) == -1)
        THROW_TEST_FAILURE("waitpid() failed");

    // Verify we actually hold the lock now.
    uint8_t state = query_lock(parent_fd, 0);
    if (state != (LOCK_LOCAL | LOCK_GLOBAL))
        THROW_TEST_FAILURE("Should hold lock after blocking acquire");

    if (!release_lock(parent_fd, 0))
        THROW_TEST_FAILURE("Should release lock");
}

// Verify that a blocking acquire will restart if it is interrupted by a signal
// with the SA_RESTART flag.
void VerifySARestart(const EnumeratedDevice &dev)
{
    DevFd fd(dev.path);

    // Step 1: Acquire the lock (we will block on ourselves).
    if (!acquire_lock(fd, 0))
        THROW_TEST_FAILURE("Should acquire lock");

    // Step 2: Set up signal handler with SA_RESTART.
    sa_restart_fd = fd.get();
    sa_restart_index = 0;

    struct sigaction sa{};
    sa.sa_handler = sa_restart_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);

    struct sigaction old_sa{};
    if (sigaction(SIGALRM, &sa, &old_sa) == -1)
        THROW_TEST_FAILURE("sigaction() failed");

    // Step 3: Arm timer to fire SIGALRM in 50ms.
    struct itimerval timer{};
    timer.it_value.tv_usec = 50000;
    if (setitimer(ITIMER_REAL, &timer, nullptr) == -1) {
        sigaction(SIGALRM, &old_sa, nullptr);
        THROW_TEST_FAILURE("setitimer() failed");
    }

    // Step 4: Call blocking acquire on the lock we already hold.
    // This will block in wait_event_interruptible.
    // When SIGALRM fires:
    //   - Kernel returns -ERESTARTSYS internally
    //   - Because SA_RESTART, kernel runs handler then restarts the ioctl
    //   - Handler releases the lock
    //   - Restarted ioctl sees lock is free, acquires it, returns success
    acquire_lock_blocking(fd, 0);

    // Restore old signal handler.
    sigaction(SIGALRM, &old_sa, nullptr);

    // We should hold the lock now.
    uint8_t state = query_lock(fd, 0);
    if (state != (LOCK_LOCAL | LOCK_GLOBAL))
        THROW_TEST_FAILURE("Should hold lock after SA_RESTART");

    if (!release_lock(fd, 0))
        THROW_TEST_FAILURE("Should release lock");
}

} // namespace

void TestLock(const EnumeratedDevice &dev)
{
    VerifyLockSemantics(dev);
    VerifyLockBounds(dev);
    VerifyAllLocks(dev);
    VerifyBlockingLock(dev);
    VerifyLockable(dev);
    VerifyExitReleasesLock(dev);
    VerifyBlockingWakesOnExit(dev);
    VerifySARestart(dev);
}
