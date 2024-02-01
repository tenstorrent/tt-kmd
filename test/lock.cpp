// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <optional>
#include <string>
#include <cstdint>

#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ioctl.h"

#include "enumeration.h"
#include "devfd.h"
#include "test_failure.h"

std::optional<bool> lock(DevFd &dev, uint32_t index) {
    struct tenstorrent_lock_ctl lock;

    zero(&lock);
    lock.in.output_size_bytes = sizeof(lock.out);

    lock.in.flags = TENSTORRENT_LOCK_CTL_ACQUIRE;
    lock.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &lock) != 0)
        return std::nullopt;
    return lock.out.value != 0;
}

std::optional<bool> unlock(DevFd &dev, uint32_t index) {
    struct tenstorrent_lock_ctl lock;

    zero(&lock);
    lock.in.output_size_bytes = sizeof(lock.out);

    lock.in.flags = TENSTORRENT_LOCK_CTL_RELEASE;
    lock.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &lock) != 0)
        return std::nullopt;
    return lock.out.value != 0;
}

std::optional<uint8_t> test(DevFd &dev, uint32_t index) {
    struct tenstorrent_lock_ctl lock;

    zero(&lock);
    lock.in.output_size_bytes = sizeof(lock.out);

    lock.in.flags = TENSTORRENT_LOCK_CTL_TEST;
    lock.in.index = index;

    if (ioctl(dev.get(), TENSTORRENT_IOCTL_LOCK_CTL, &lock) != 0)
        return std::nullopt;
    return lock.out.value;
}

void test_test(std::string test_name, DevFd &dev, uint32_t index, uint8_t expected_test = 0) {
    auto result = test(dev, index);
    if (!result.has_value())
        THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK failed to test bit " + std::to_string(index) + ".");

    if (result.value() != expected_test)
        THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK test showed that bit " + std::to_string(index) + " was not released after release.");
}

void lock_test(std::string test_name, DevFd &dev, uint32_t index, bool expect_failure, uint8_t expected_test = 0b11) {
    auto result = lock(dev, index);
    if (!result.has_value())
        THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK lock failed to lock single bit " + std::to_string(index) + ".");

    if (expect_failure) {
        if (result.value())
            THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK lock failed to lock single bit " + std::to_string(index) + ".");
    } else {
        if (!result.value())
            THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK lock failed to lock single bit " + std::to_string(index) + ".");

        test_test(test_name, dev, index, expected_test);
    }
}

void unlock_test(std::string test_name, DevFd &dev, uint32_t index, bool expect_failure, uint8_t expected_test = 0) {
    auto result = unlock(dev, index);
    if (!result.has_value())
        THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK release failed to lock bit " + std::to_string(index) + ".");

    if (expect_failure) {
        if (result.value())
            THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK release unexpectedly succeeded in releasing the lock on bit " + std::to_string(index) + ".");
    } else {
        if (!result.value())
            THROW_TEST_FAILURE(test_name + std::string(": ") + "LOCK release failed to release the lock on bit " + std::to_string(index) + ".");

        test_test(test_name, dev, index, expected_test);
    }
}

void VerifyLockSimple(const EnumeratedDevice &dev)
{
    struct tenstorrent_lock_ctl lock;

    DevFd dev_fd(dev.path);

    zero(&lock);
    lock.in.output_size_bytes = sizeof(lock.out);

    lock.in.flags = TENSTORRENT_LOCK_CTL_TEST;
    lock.in.index = 0;

    if (ioctl(dev_fd.get(), TENSTORRENT_IOCTL_LOCK_CTL, &lock) != 0)
        THROW_TEST_FAILURE("simple_test_1: LOCK failed to test single lock bit.");
}

void VerifyLockUnlock(const EnumeratedDevice &dev)
{
    DevFd dev_fd_0(dev.path);
    DevFd dev_fd_1(dev.path);

    // fd 0 can lock and unlock.
    lock_test("simple_lock_unlock", dev_fd_0, 0, false);
    unlock_test("simple_lock_unlock", dev_fd_0, 0, false);

    // fd 0 can't unlock an unlocked resource.
    unlock_test("double_unlock", dev_fd_0, 0, true);

    // fd 1 can't unlock a lock made by fd 0.
    lock_test("unlock_locked", dev_fd_0, 0, false);
    unlock_test("unlock_locked", dev_fd_1, 0, true);
    unlock_test("unlock_locked", dev_fd_0, 0, false);

    // fd 0 can't lock a lock made by fd 0.
    lock_test("double_lock", dev_fd_0, 0, false);
    lock_test("double_lock", dev_fd_0, 0, true);
    unlock_test("double_lock", dev_fd_0, 0, false);

    // fd 1 can't lock a lock made by fd 0.
    lock_test("double_lock", dev_fd_0, 0, false);
    lock_test("double_lock", dev_fd_1, 0, true);
    unlock_test("double_lock", dev_fd_0, 0, false);

    // Once locked a test on the bit will show a global lock on
    // fd 1 and local lock on fd 0.
    lock_test("global_lock", dev_fd_0, 0, false);
    test_test("global_lock", dev_fd_0, 0, 0b11);
    test_test("global_lock", dev_fd_1, 0, 0b10);
    unlock_test("global_lock", dev_fd_0, 0, false);

    // If bit 0 is locked, other bits remain unlocked
    lock_test("isolated_lock", dev_fd_0, 0, false);
    lock_test("isolated_lock", dev_fd_1, 1, false);
    unlock_test("isolated_lock", dev_fd_0, 0, false);
    unlock_test("isolated_lock", dev_fd_1, 1, false);

    // locking with fd 3, then closing releases the resource
    {
        DevFd dev_fd_3(dev.path);
        lock_test("close_release_lock", dev_fd_3, 0, false);
        test_test("close_release_lock", dev_fd_3, 0, 0b11);
        unlock_test("close_release_lock", dev_fd_1, 0, true);
    } // dev_fd_3 goes out of scope and the destructor is called (which closes the file handle).

    test_test("close_release_lock", dev_fd_1, 0, 0b00);
    lock_test("close_release_lock", dev_fd_1, 0, false);
    unlock_test("close_release_lock", dev_fd_1, 0, false);
}

void TestLock(const EnumeratedDevice &dev)
{
    VerifyLockSimple(dev);
    VerifyLockUnlock(dev);
}
