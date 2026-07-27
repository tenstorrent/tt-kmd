// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Verify the VFIO-style argsz/flags size-negotiation contract that is common to
// all argsz ioctls. The pahole ABI check freezes the struct layout; this checks
// that the kernel honors argsz at runtime:
//
//   - argsz below the original (minsz) size            -> EINVAL
//   - argsz == sizeof, acceptable body                  -> accepted
//   - argsz larger than the kernel knows, zero tail     -> accepted
//   - argsz larger than the kernel knows, nonzero tail  -> E2BIG
//   - an unknown flag bit                               -> EINVAL
//
// All of this lives in CheckArgsz(), which is templated on the ioctl struct and
// manipulates argsz/flags symbolically through that type. The per-ioctl code
// does nothing but supply an acceptable struct.

#include <optional>
#include <string>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sys/ioctl.h>

#include "devfd.h"
#include "enumeration.h"
#include "ioctl.h"
#include "test_failure.h"

namespace
{

// Run an ioctl on a struct pointer; return 0 on success or the errno.
int ioctl_errno(int fd, unsigned long code, const void *arg)
{
    return ioctl(fd, code, const_cast<void *>(arg)) == 0 ? 0 : errno;
}

std::string errno_name(int e)
{
    return e == 0 ? std::string("success") : std::strerror(e);
}

void expect(int fd, unsigned long code, const std::string &what,
            const void *arg, int expected)
{
    int got = ioctl_errno(fd, code, arg);
    if (got != expected)
        THROW_TEST_FAILURE(what + ": expected " + errno_name(expected) +
                           ", got " + errno_name(got));
}

// Generic argsz/flags negotiation check for one ioctl.
//   minsz        - the frozen original struct size; argsz below this is EINVAL.
//   valid        - a struct the ioctl accepts, or nullopt if it has none. When
//                  given, the success paths are checked and the whole check is
//                  skipped on devices that report EOPNOTSUPP.
//   failing_flag - a flags value that must be rejected, or 0 if none is available.
// T is assumed to have the universal header (argsz and flags members), which is
// set symbolically below; no field is touched by offset.
template <class T>
void CheckArgsz(int fd, unsigned long code, const std::string &name,
                std::uint32_t minsz, std::optional<T> valid, std::uint32_t failing_flag)
{
    const std::uint32_t full = sizeof(T);

    // Body used for every case: the accepted struct if provided, else zeros.
    const T body = valid.value_or(T{});

    // Payload one notch larger than the kernel's struct: the body followed by an
    // 8-byte tail, so a "future" argsz can be claimed and the tail set.
    struct Larger { T body; std::uint8_t tail[8]; };

    // With an acceptable body, probe argsz == sizeof. A device whose class does
    // not support this ioctl reports EOPNOTSUPP before any argsz handling, so
    // skip the whole check there.
    if (valid)
    {
        T s = body;
        s.argsz = full;
        int got = ioctl_errno(fd, code, &s);
        if (got == EOPNOTSUPP)
            return;
        if (got != 0)
            THROW_TEST_FAILURE(name + " baseline (argsz==sizeof): expected success, got " + errno_name(got));
    }

    // argsz below minsz is rejected.
    {
        T s = body;
        s.argsz = minsz - 1;
        expect(fd, code, name + " argsz<minsz", &s, EINVAL);
    }

    // An old caller's smaller-but-valid struct is accepted (only meaningful once
    // the struct has grown past its original size). The kernel reads only the
    // first minsz bytes of the (full-size) struct.
    if (valid && minsz < full)
    {
        T s = body;
        s.argsz = minsz;
        expect(fd, code, name + " argsz==minsz", &s, 0);
    }

    // A larger argsz with a zero tail is accepted (new userspace, old kernel).
    if (valid)
    {
        Larger s{};
        s.body = body;
        s.body.argsz = full + sizeof(s.tail);
        expect(fd, code, name + " argsz>sizeof, zero tail", &s, 0);
    }

    // A larger argsz with a nonzero tail is rejected loudly.
    {
        Larger s{};
        s.body = body;
        s.body.argsz = full + sizeof(s.tail);
        s.tail[4] = 0x01;  // a set byte in the unknown trailing region
        expect(fd, code, name + " argsz>sizeof, nonzero tail", &s, E2BIG);
    }

    // A set, unknown flag bit is rejected.
    if (failing_flag != 0)
    {
        T s = body;
        s.argsz = full;
        s.flags = failing_flag;
        expect(fd, code, name + " unknown flag", &s, EINVAL);
    }
}

bool SupportsSetPowerState(const EnumeratedDevice &dev)
{
    return (dev.type >= Blackhole);
}

}

void TestIoctlArgsz(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    // The only per-ioctl code is supplying an acceptable struct.

    // SET_NOC_CLEANUP: a zeroed struct (enabled == 0) just clears this fd's
    // cleanup action -- benign and acceptable. Skipped on devices whose class
    // lacks noc_write32 (EOPNOTSUPP, handled inside CheckArgsz).
    CheckArgsz<tenstorrent_set_noc_cleanup>(
        fd, TENSTORRENT_IOCTL_SET_NOC_CLEANUP, "SET_NOC_CLEANUP",
        sizeof(tenstorrent_set_noc_cleanup), tenstorrent_set_noc_cleanup{}, 1);

    // SET_POWER_STATE: a zeroed struct is a "no power request" -- safe to issue.
    if (SupportsSetPowerState(dev))
    {
        CheckArgsz<tenstorrent_power_state>(
            fd, TENSTORRENT_IOCTL_SET_POWER_STATE, "SET_POWER_STATE",
            sizeof(tenstorrent_power_state), tenstorrent_power_state{}, 1);
    }
}
