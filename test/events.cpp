// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Tests for TENSTORRENT_IOCTL_GET_EVENTS and poll() on the device fd.

#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <string>

#include "ioctl.h"

#include "devfd.h"
#include "enumeration.h"
#include "test_failure.h"

namespace
{

tenstorrent_get_events get_events(int fd)
{
    tenstorrent_get_events e{};
    e.argsz = sizeof(e);

    if (ioctl(fd, TENSTORRENT_IOCTL_GET_EVENTS, &e) != 0)
        THROW_TEST_FAILURE("GET_EVENTS ioctl failed");

    return e;
}

short poll_now(int fd)
{
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN | POLLPRI;

    if (poll(&p, 1, 0) < 0)
        THROW_TEST_FAILURE("poll failed");

    return p.revents;
}

void ExpectRejected(int fd, const tenstorrent_get_events &request, const char *what)
{
    tenstorrent_get_events e = request;
    if (ioctl(fd, TENSTORRENT_IOCTL_GET_EVENTS, &e) == 0 || errno != EINVAL)
        THROW_TEST_FAILURE(std::string("GET_EVENTS did not reject invalid request: ") + what);
}

void VerifyValidation(int fd)
{
    tenstorrent_get_events good{};
    good.argsz = sizeof(good);

    { auto e = good; e.argsz = sizeof(e) - 1; ExpectRejected(fd, e, "short argsz"); }
    { auto e = good; e.argsz = sizeof(e) + 1; ExpectRejected(fd, e, "long argsz"); }
    { auto e = good; e.flags = 1;             ExpectRejected(fd, e, "nonzero flags"); }
}

void VerifyUnusedCountsZero(int fd)
{
    tenstorrent_get_events e = get_events(fd);

    for (unsigned i = TENSTORRENT_EVENT_DEVICE_ERROR + 1; i < TENSTORRENT_MAX_EVENTS; ++i) {
        if (e.counts[i] != 0)
            THROW_TEST_FAILURE("GET_EVENTS reported a count for an undefined event");
    }
}

// An event can arrive at any time, so poll() readiness is checked only against
// counts read on either side of it: readable means the counts moved.
void VerifyPollMatchesCounts(int fd)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        tenstorrent_get_events before = get_events(fd);
        short revents = poll_now(fd);

        if (revents & (POLLERR | POLLHUP | POLLNVAL))
            THROW_TEST_FAILURE("poll reported an error on a live fd");

        if (!(revents & POLLIN))
            return;

        if ((revents & POLLPRI) == 0)
            THROW_TEST_FAILURE("poll reported POLLIN without POLLPRI");

        tenstorrent_get_events after = get_events(fd);
        if (std::equal(std::begin(before.counts), std::end(before.counts), std::begin(after.counts)))
            THROW_TEST_FAILURE("poll reported events but the counts did not change");
    }

    THROW_TEST_FAILURE("poll stayed readable across repeated GET_EVENTS");
}

} // namespace

void TestEvents(const EnumeratedDevice &dev)
{
    DevFd dev_fd(dev.path);
    int fd = dev_fd.get();

    VerifyValidation(fd);
    VerifyUnusedCountsZero(fd);
    VerifyPollMatchesCounts(fd);
}
