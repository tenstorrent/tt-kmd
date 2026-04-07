// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Test the VFIO-style argsz protocol for SET_NOC_CLEANUP and SET_POWER_STATE.
// Verifies size negotiation (undersized, exact, oversized), trailing byte
// rejection (check_zeroed_user), reserved field validation, unknown flag
// rejection, and overrun protection.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <cerrno>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "devfd.h"
#include "enumeration.h"
#include "ioctl.h"
#include "test_failure.h"
#include "util.h"

namespace
{

void expect_errno(int fd, unsigned long code, const char *name, void *arg, int expected_errno, const char *scenario)
{
	if (ioctl(fd, code, arg) == 0)
		THROW_TEST_FAILURE(std::string(name) + ": " + scenario + " was not rejected");
	if (errno != expected_errno)
		THROW_TEST_FAILURE(std::string(name) + ": " + scenario + " expected errno " + std::to_string(expected_errno) +
				   ", got " + std::to_string(errno));
}

void expect_ok(int fd, unsigned long code, const char *name, void *arg,
	       const char *scenario)
{
	if (ioctl(fd, code, arg) != 0)
		THROW_TEST_FAILURE(std::string(name) + ": " + scenario + " failed with errno " + std::to_string(errno));
}

// Generic argsz protocol tests are templated over struct type.
template <class T>
void TestArgszZero(int fd, unsigned long code, const char *name, T valid)
{
	valid.argsz = 0;
	expect_errno(fd, code, name, &valid, EINVAL, "argsz=0");
}

template <class T>
void TestArgszTooSmall(int fd, unsigned long code, const char *name, T valid)
{
	valid.argsz = sizeof(T) - 1;
	expect_errno(fd, code, name, &valid, EINVAL, "argsz too small");
}

template <class T>
void TestArgszExact(int fd, unsigned long code, const char *name, T valid)
{
	expect_ok(fd, code, name, &valid, "argsz exact");
}

template <class T>
void TestArgszOversizedZeroTail(int fd, unsigned long code, const char *name, T valid)
{
	const size_t extra = 64;
	std::vector<unsigned char> buf(sizeof(T) + extra, 0);

	std::memcpy(buf.data(), &valid, sizeof(T));
	__u32 argsz = static_cast<__u32>(buf.size());
	std::memcpy(buf.data(), &argsz, sizeof(argsz));

	expect_ok(fd, code, name, buf.data(), "oversized argsz with zero tail");
}

template <class T>
void TestArgszOversizedNonzeroTail(int fd, unsigned long code, const char *name, T valid)
{
	const size_t extra = 64;
	std::vector<unsigned char> buf(sizeof(T) + extra, 0);

	std::memcpy(buf.data(), &valid, sizeof(T));
	__u32 argsz = static_cast<__u32>(buf.size());
	std::memcpy(buf.data(), &argsz, sizeof(argsz));
	buf.back() = 0xFF;

	expect_errno(fd, code, name, buf.data(), E2BIG, "oversized argsz with nonzero tail");
}

template <class T>
void TestArgszOverrun(int fd, unsigned long code, const char *name, T valid)
{
	size_t ps = page_size();
	size_t alloc = round_up(sizeof(T), ps) + ps;

	void *mapping = mmap(nullptr, alloc, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (mapping == MAP_FAILED)
		throw_system_error(std::string(name) + " overrun test: mmap");

	void *guard = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(mapping) + alloc - ps);
	if (mprotect(guard, ps, PROT_NONE) != 0) {
		munmap(mapping, alloc);
		throw_system_error(std::string(name) + " overrun test: mprotect");
	}

	void *p = reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(guard) - sizeof(T));
	T *s = new (p) T(valid);

	int result = ioctl(fd, code, s);
	int saved_errno = errno;

	munmap(mapping, alloc);

	if (result != 0 && saved_errno == EFAULT)
		THROW_TEST_FAILURE(std::string(name) + " overrun: kernel accessed past argsz");
}

template <class T>
void TestArgszUnknownFlags(int fd, unsigned long code, const char *name, T valid)
{
	valid.flags = 0xFFFFFFFF;
	expect_errno(fd, code, name, &valid, EINVAL, "unknown flags");
}

void TestSetNocCleanupArgsz(int fd)
{
	tenstorrent_set_noc_cleanup s = {};
	s.argsz = sizeof(s);

	const auto code = TENSTORRENT_IOCTL_SET_NOC_CLEANUP;
	const char *name = "SET_NOC_CLEANUP";

	if (ioctl(fd, code, &s) != 0 && errno == EOPNOTSUPP)
		return;

	TestArgszZero(fd, code, name, s);
	TestArgszTooSmall(fd, code, name, s);
	TestArgszExact(fd, code, name, s);
	TestArgszOversizedZeroTail(fd, code, name, s);
	TestArgszOversizedNonzeroTail(fd, code, name, s);
	TestArgszOverrun(fd, code, name, s);
	TestArgszUnknownFlags(fd, code, name, s);

	tenstorrent_set_noc_cleanup bad_reserved = s;
	bad_reserved.reserved0 = 1;
	expect_errno(fd, code, name, &bad_reserved, EINVAL, "nonzero reserved0");
}

void TestSetPowerStateArgsz(int fd)
{
	tenstorrent_power_state s = {};
	s.argsz = sizeof(s);

	const auto code = TENSTORRENT_IOCTL_SET_POWER_STATE;
	const char *name = "SET_POWER_STATE";

	// These fail argsz/flags/reserved validation before reaching firmware.
	TestArgszZero(fd, code, name, s);
	TestArgszTooSmall(fd, code, name, s);
	TestArgszOversizedNonzeroTail(fd, code, name, s);
	TestArgszOverrun(fd, code, name, s);
	TestArgszUnknownFlags(fd, code, name, s);

	tenstorrent_power_state bad_reserved = s;
	bad_reserved.reserved0 = 1;
	expect_errno(fd, code, name, &bad_reserved, EINVAL, "nonzero reserved0");

	// These need firmware to accept the power state. On WH without
	// queue-capable firmware, send_arc_message fails and the ioctl
	// returns -EINVAL even though argsz validation passed.
	if (ioctl(fd, code, &s) == 0) {
		TestArgszExact(fd, code, name, s);
		TestArgszOversizedZeroTail(fd, code, name, s);
	}
}

} // namespace

void TestIoctlArgsz(const EnumeratedDevice &dev)
{
	DevFd dev_fd(dev.path);

	TestSetNocCleanupArgsz(dev_fd.get());
	TestSetPowerStateArgsz(dev_fd.get());
}
