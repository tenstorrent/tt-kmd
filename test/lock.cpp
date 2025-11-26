// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <cerrno>
#include <cstdint>

#include <sys/ioctl.h>

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

} // namespace

void TestLock(const EnumeratedDevice &dev)
{
    VerifyLockSemantics(dev);
    VerifyLockBounds(dev);
    VerifyAllLocks(dev);
}
