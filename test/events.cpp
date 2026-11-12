// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Tests for TENSTORRENT_IOCTL_READ_EVENTS and poll() on the device fd.
//
// The end-to-end tests drive the firmware's event ring with
// TT_SMC_MSG_TEST_EVENT_RECORD, and are skipped when the firmware does not
// publish one.  The last one resets the device.

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ioctl.h"

#include "devfd.h"
#include "enumeration.h"
#include "test_failure.h"

namespace
{

constexpr uint32_t TT_SMC_MSG_TEST_EVENT_RECORD = 0x91;
constexpr uint16_t EVENT_TYPE_TEST = 0x0000;	// Firmware type, from device_events.h

// Firmware ring size, and the number of records the driver keeps.
constexpr uint32_t FW_RING_ENTRIES = 64;
constexpr uint32_t KMD_RING_ENTRIES = 256;

// An event must be visible this soon after the message that raised it.
constexpr int EVENT_TIMEOUT_MS = 100;

struct ReadResult
{
    tenstorrent_read_events hdr;
    std::vector<tenstorrent_event> events;
};

int read_events_ioctl(int fd, tenstorrent_read_events &r)
{
    return ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &r) == 0 ? 0 : -errno;
}

ReadResult read_events(int fd, uint32_t capacity = KMD_RING_ENTRIES * 2, uint32_t flags = 0)
{
    ReadResult r{};
    r.events.resize(capacity);
    r.hdr.argsz = sizeof(r.hdr);
    r.hdr.flags = flags;
    r.hdr.capacity = capacity;
    r.hdr.events_ptr = reinterpret_cast<uintptr_t>(r.events.data());

    if (read_events_ioctl(fd, r.hdr) != 0)
        THROW_TEST_FAILURE("READ_EVENTS ioctl failed");

    if (r.hdr.count > capacity)
        THROW_TEST_FAILURE("READ_EVENTS returned more records than fit");

    r.events.resize(r.hdr.count);
    return r;
}

bool fw_attached(int fd)
{
    return read_events(fd, 0).hdr.status & TENSTORRENT_READ_EVENTS_STATUS_FW_ATTACHED;
}

short poll_fd(int fd, int timeout_ms)
{
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN | POLLPRI;

    if (poll(&p, 1, timeout_ms) < 0)
        THROW_TEST_FAILURE("poll failed");

    return p.revents;
}

short poll_now(int fd)
{
    return poll_fd(fd, 0);
}

bool readable_soon(int fd)
{
    return poll_fd(fd, EVENT_TIMEOUT_MS) & POLLIN;
}

// Returns the firmware status byte.
uint32_t smc_msg(int fd, const uint32_t (&request)[8])
{
    tenstorrent_smc_msg msg{};
    msg.argsz = sizeof(msg);
    msg.flags = TENSTORRENT_SMC_MSG_POST;
    std::memcpy(msg.message, request, sizeof(msg.message));

    if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) != 0)
        THROW_TEST_FAILURE("SMC_MSG POST failed");

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        msg = {};
        msg.argsz = sizeof(msg);
        msg.flags = TENSTORRENT_SMC_MSG_POLL;
        if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) == 0)
            break;
        if (errno != EAGAIN)
            THROW_TEST_FAILURE("SMC_MSG POLL failed");
        if (std::chrono::steady_clock::now() > deadline)
            THROW_TEST_FAILURE("Timed out waiting for SMC message response");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return msg.message[0] & 0xFF;
}

struct TestRecord
{
    uint16_t type = EVENT_TYPE_TEST;
    uint16_t source = 0;
    uint16_t repeat = 1;
    uint32_t cause = 0;
    uint32_t data[3] = {};
};

// Sends TT_SMC_MSG_TEST_EVENT_RECORD.  Record i of the batch has data[2] + i.
uint32_t send_record_msg(int fd, const TestRecord &r)
{
    uint32_t request[8] = {};
    request[0] = TT_SMC_MSG_TEST_EVENT_RECORD | (uint32_t(r.type) << 16);
    request[1] = r.source | (uint32_t(r.repeat) << 16);
    request[2] = r.cause;
    request[3] = r.data[0];
    request[4] = r.data[1];
    request[5] = r.data[2];

    return smc_msg(fd, request);
}

void must_send(int fd, const TestRecord &r)
{
    if (send_record_msg(fd, r) != 0)
        THROW_TEST_FAILURE("Firmware rejected a test event message");
}

// Each test marks its records with a distinct data[0].
TestRecord marked(uint32_t marker, uint16_t repeat = 1)
{
    TestRecord r;
    r.data[0] = marker;
    r.repeat = repeat;
    return r;
}

std::vector<tenstorrent_event> with_marker(const std::vector<tenstorrent_event> &events, uint32_t marker)
{
    std::vector<tenstorrent_event> out;
    for (const auto &e : events) {
        if (e.type == EVENT_TYPE_TEST && e.data[0] == marker)
            out.push_back(e);
    }
    return out;
}

// Reads on fd until count records with marker have arrived.
std::vector<tenstorrent_event> collect(int fd, uint32_t marker, size_t count)
{
    std::vector<tenstorrent_event> got;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (got.size() < count) {
        if (std::chrono::steady_clock::now() > deadline)
            THROW_TEST_FAILURE("Timed out waiting for event records");
        poll_fd(fd, EVENT_TIMEOUT_MS);
        for (const auto &e : with_marker(read_events(fd).events, marker))
            got.push_back(e);
    }
    return got;
}

void check_consecutive(const std::vector<tenstorrent_event> &events, const char *what)
{
    for (size_t i = 1; i < events.size(); ++i) {
        if (events[i].seq != events[i - 1].seq + 1)
            THROW_TEST_FAILURE(std::string(what) + ": sequence numbers are not consecutive");
    }
}

void ExpectRejected(int fd, const tenstorrent_read_events &request, const char *what)
{
    tenstorrent_read_events r = request;
    if (read_events_ioctl(fd, r) != -EINVAL)
        THROW_TEST_FAILURE(std::string("READ_EVENTS did not reject invalid request: ") + what);
}

void VerifyValidation(int fd)
{
    tenstorrent_read_events good{};
    good.argsz = sizeof(good);

    { auto r = good; r.argsz = sizeof(r) - 1; ExpectRejected(fd, r, "short argsz"); }
    { auto r = good; r.argsz = sizeof(r) + 1; ExpectRejected(fd, r, "long argsz"); }
    { auto r = good; r.flags = 2;             ExpectRejected(fd, r, "unknown flags"); }
    { auto r = good; r.reserved = 1;          ExpectRejected(fd, r, "nonzero reserved"); }

    // capacity 0 succeeds without touching events_ptr.
    tenstorrent_read_events r = good;
    r.events_ptr = 1;
    r.flags = TENSTORRENT_READ_EVENTS_FROM_OLDEST;
    if (read_events_ioctl(fd, r) != 0)
        THROW_TEST_FAILURE("READ_EVENTS with capacity 0 failed");
    if (r.count != 0 || r.lost != 0 || r.entry_size != sizeof(tenstorrent_event))
        THROW_TEST_FAILURE("READ_EVENTS with capacity 0 returned wrong results");

    // A bad buffer fails without losing records.
    r = good;
    r.capacity = 1;
    r.events_ptr = 1;
    int ret = read_events_ioctl(fd, r);
    if (ret != 0 && ret != -EFAULT)
        THROW_TEST_FAILURE("READ_EVENTS with a bad buffer failed oddly");
}

// An event can arrive at any time, so poll() readiness is checked only against
// a read after it: readable means there were records.
void VerifyPollMatchesRecords(int fd)
{
    for (int attempt = 0; attempt < 3; ++attempt) {
        short revents = poll_now(fd);

        if (revents & (POLLERR | POLLHUP | POLLNVAL))
            THROW_TEST_FAILURE("poll reported an error on a live fd");

        if (!(revents & POLLIN))
            return;

        if ((revents & POLLPRI) == 0)
            THROW_TEST_FAILURE("poll reported POLLIN without POLLPRI");

        if (read_events(fd).events.empty())
            THROW_TEST_FAILURE("poll reported records but READ_EVENTS found none");
    }

    THROW_TEST_FAILURE("poll stayed readable across repeated READ_EVENTS");
}

void VerifyRoundTrip(int fd)
{
    TestRecord sent;
    sent.source = 0x1234;
    sent.cause = 0xCAFEBABE;
    sent.data[0] = 0x0100;
    sent.data[1] = 0xDEADBEEF;
    sent.data[2] = 0x5A5A5A5A;
    must_send(fd, sent);

    if (poll_fd(fd, EVENT_TIMEOUT_MS) != (POLLIN | POLLPRI))
        THROW_TEST_FAILURE("poll did not report the event as POLLIN | POLLPRI in time");

    auto events = with_marker(read_events(fd).events, sent.data[0]);
    if (events.size() != 1)
        THROW_TEST_FAILURE("Expected exactly one record");

    const auto &e = events[0];
    if (e.source != sent.source || e.cause != sent.cause || e.data[1] != sent.data[1] ||
        e.data[2] != sent.data[2] || e.data[3] != 0)
        THROW_TEST_FAILURE("Record fields did not round-trip");

    if (e.device_time == 0 || e.host_time_ns == 0)
        THROW_TEST_FAILURE("Record has no timestamp");

    if (poll_now(fd) != 0)
        THROW_TEST_FAILURE("poll stayed readable after READ_EVENTS");
}

void VerifyEmptyDoorbell(int fd)
{
    // A batch of 0 records sends only the doorbell.
    TestRecord r;
    r.repeat = 0;
    if (send_record_msg(fd, r) != 0)
        THROW_TEST_FAILURE("Firmware rejected the doorbell message");

    if (poll_fd(fd, EVENT_TIMEOUT_MS) != 0)
        THROW_TEST_FAILURE("An MSI without a record made poll readable");
}

void VerifyTwoFds(const std::string &path, int fd)
{
    DevFd other(path);

    must_send(fd, marked(0x0200));

    if (!readable_soon(fd) || !readable_soon(other.get()))
        THROW_TEST_FAILURE("Both fds must see the record");

    if (with_marker(read_events(fd).events, 0x0200).size() != 1)
        THROW_TEST_FAILURE("First fd did not read the record");

    if (!(poll_now(other.get()) & POLLIN))
        THROW_TEST_FAILURE("Reading on one fd consumed the record for another");

    if (with_marker(read_events(other.get()).events, 0x0200).size() != 1)
        THROW_TEST_FAILURE("Second fd did not read the record");
}

void VerifyOpenAfterEvent(const std::string &path, int fd)
{
    must_send(fd, marked(0x0300));

    if (!readable_soon(fd))
        THROW_TEST_FAILURE("Record not delivered");

    DevFd late(path);
    if (poll_now(late.get()) != 0 || !read_events(late.get()).events.empty())
        THROW_TEST_FAILURE("An fd opened after a record could read it");

    read_events(fd);
}

void VerifyNonTestRejected(int fd)
{
    TestRecord r = marked(0x0400);
    r.type = 1;

    if (send_record_msg(fd, r) == 0)
        THROW_TEST_FAILURE("Firmware accepted a non-TEST type");

    if (poll_fd(fd, EVENT_TIMEOUT_MS) != 0)
        THROW_TEST_FAILURE("A rejected message delivered a record");
}

void VerifyBatch(int fd)
{
    constexpr uint16_t k = 10;
    TestRecord r = marked(0x0500, k);
    r.data[2] = 1000;
    must_send(fd, r);

    auto events = collect(fd, 0x0500, k);
    if (events.size() != k)
        THROW_TEST_FAILURE("Batch delivered the wrong number of records");

    for (size_t i = 0; i < k; ++i) {
        if (events[i].data[2] != 1000 + i)
            THROW_TEST_FAILURE("Batch records out of order");
    }
    check_consecutive(events, "Batch");
}

// A batch bigger than the firmware ring fills it, and the rest is dropped.
void VerifyFwRingOverflow(int fd)
{
    constexpr uint16_t k = FW_RING_ENTRIES + 36;
    must_send(fd, marked(0x0600, k));

    std::vector<tenstorrent_event> all;
    uint64_t lost = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    // Survivors and lost records together account for the whole batch.
    for (;;) {
        poll_fd(fd, EVENT_TIMEOUT_MS);
        for (const auto &e : read_events(fd).events) {
            all.push_back(e);
            if (e.type == TENSTORRENT_EVENT_TYPE_FW_EVENTS_LOST)
                lost += e.data[0];
        }
        if (with_marker(all, 0x0600).size() + lost >= k)
            break;
        if (std::chrono::steady_clock::now() > deadline)
            THROW_TEST_FAILURE("Timed out waiting for an overflowing batch");
    }

    auto survivors = with_marker(all, 0x0600);
    if (survivors.size() + lost != k)
        THROW_TEST_FAILURE("Lost and delivered records don't add up to the batch");

    // A drain in the middle of the batch (another MSI) frees room, so more
    // than a ringful may survive, but always the oldest first and in order.
    if (survivors.size() < FW_RING_ENTRIES || survivors.front().data[2] != 0)
        THROW_TEST_FAILURE("Overflow did not keep the oldest records");

    for (size_t i = 1; i < survivors.size(); ++i) {
        if (survivors[i].data[2] <= survivors[i - 1].data[2])
            THROW_TEST_FAILURE("Records after an overflow are out of order");
    }

    // With nothing else draining in between, the result is exactly the first
    // FW_RING_ENTRIES records followed by one FW_EVENTS_LOST.
    if (all.size() == FW_RING_ENTRIES + 1) {
        if (all.back().type != TENSTORRENT_EVENT_TYPE_FW_EVENTS_LOST || all.back().data[0] != k - FW_RING_ENTRIES)
            THROW_TEST_FAILURE("FW_EVENTS_LOST record missing or wrong");
        check_consecutive(all, "Overflow");
    }
}

// The driver keeps KMD_RING_ENTRIES; an fd that falls further behind is told
// how many it lost.
void VerifyKmdRingOverflow(const std::string &path, int fd)
{
    constexpr uint16_t per_msg = 32;
    constexpr uint32_t messages = 10;
    constexpr uint32_t total = per_msg * messages;
    DevFd observer(path);

    for (uint32_t m = 0; m < messages; ++m) {
        TestRecord r = marked(0x0700, per_msg);
        r.data[2] = m * per_msg;
        must_send(fd, r);

        // Wait until the driver has the batch, so the firmware ring never
        // overflows.  Reading on another fd leaves fd's cursor alone.
        collect(observer.get(), 0x0700, per_msg);
    }

    ReadResult behind = read_events(fd);
    if (behind.hdr.lost != total - KMD_RING_ENTRIES)
        THROW_TEST_FAILURE("READ_EVENTS reported the wrong number lost: " + std::to_string(behind.hdr.lost));

    if (behind.events.size() != KMD_RING_ENTRIES)
        THROW_TEST_FAILURE("READ_EVENTS did not return every record kept");

    check_consecutive(behind.events, "KMD overflow");
    if (behind.events.back().data[2] != total - 1)
        THROW_TEST_FAILURE("READ_EVENTS did not return the newest records");

    if (read_events(fd).hdr.lost != 0)
        THROW_TEST_FAILURE("lost was reported twice");

    // A new fd sees nothing, unless it asks for everything kept.
    DevFd fresh(path);
    if (!read_events(fresh.get()).events.empty())
        THROW_TEST_FAILURE("A new fd read old records");

    ReadResult oldest = read_events(fresh.get(), KMD_RING_ENTRIES * 2, TENSTORRENT_READ_EVENTS_FROM_OLDEST);
    if (oldest.hdr.lost != 0 || oldest.events.size() != KMD_RING_ENTRIES ||
        oldest.events.front().seq != behind.events.front().seq)
        THROW_TEST_FAILURE("FROM_OLDEST did not return every record kept");

    // FROM_OLDEST with a small buffer reads from the oldest, then continues.
    ReadResult two = read_events(fresh.get(), 2, TENSTORRENT_READ_EVENTS_FROM_OLDEST);
    ReadResult next = read_events(fresh.get(), 1);
    if (two.events.size() != 2 || two.events[0].seq != oldest.events[0].seq ||
        next.events.size() != 1 || next.events[0].seq != two.events[1].seq + 1)
        THROW_TEST_FAILURE("FROM_OLDEST did not continue from where it stopped");
}

bool reset_ioctl(int fd, uint32_t flags)
{
    tenstorrent_reset_device r{};
    r.in.output_size_bytes = sizeof(r.out);
    r.in.flags = flags;

    return ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &r) == 0 && r.out.result == 0;
}

// Resets the device as tools/reset.c does.  Returns the device's path after
// reset, which differs from the old one if the reset was a hotplug.
std::string ResetDevice(const EnumeratedDevice &dev, int fd)
{
    std::string bdf = dev.location.format();

    if (!reset_ioctl(fd, TENSTORRENT_RESET_DEVICE_ASIC_RESET))
        THROW_TEST_FAILURE("ASIC reset failed");

    // Done once firmware clears the reset marker, bit 6 of the PCI command
    // register.
    std::string config = "/sys/bus/pci/devices/" + bdf + "/config";
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        int cfd = open(config.c_str(), O_RDONLY);
        unsigned char cmd = 0;
        bool done = false;

        if (cfd >= 0) {
            done = pread(cfd, &cmd, 1, 4) == 1 && !(cmd & (1 << 6));
            close(cfd);
        }
        if (done)
            break;
        if (std::chrono::steady_clock::now() > deadline)
            THROW_TEST_FAILURE("Timed out waiting for reset");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string path = "/dev/tenstorrent/by-bdf/" + bdf;
    for (;;) {
        int nfd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (nfd >= 0) {
            bool ok = reset_ioctl(nfd, TENSTORRENT_RESET_DEVICE_POST_RESET);
            close(nfd);
            if (!ok)
                THROW_TEST_FAILURE("POST_RESET failed");
            return path;
        }
        if (std::chrono::steady_clock::now() > deadline)
            THROW_TEST_FAILURE("Device did not come back after reset");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void VerifyReset(const EnumeratedDevice &dev)
{
    DevFd old_fd(dev.path);
    std::string path;

    must_send(old_fd.get(), marked(0x0800));
    if (!readable_soon(old_fd.get()))
        THROW_TEST_FAILURE("Record before reset not delivered");

    {
        DevFd resetter(dev.path);
        path = ResetDevice(dev, resetter.get());
    }

    short revents = poll_now(old_fd.get());
    if ((revents & (POLLERR | POLLHUP)) != (POLLERR | POLLHUP))
        THROW_TEST_FAILURE("A pre-reset fd did not report POLLERR | POLLHUP");

    tenstorrent_read_events r{};
    r.argsz = sizeof(r);
    if (read_events_ioctl(old_fd.get(), r) != -ENODEV)
        THROW_TEST_FAILURE("READ_EVENTS on a pre-reset fd did not fail with ENODEV");

    DevFd fd(path);
    if (!fw_attached(fd.get()))
        THROW_TEST_FAILURE("Firmware events not reattached after reset");

    // Reset discards every record, so there is nothing to rewind to.
    if (!read_events(fd.get(), KMD_RING_ENTRIES, TENSTORRENT_READ_EVENTS_FROM_OLDEST).events.empty())
        THROW_TEST_FAILURE("Records from before the reset are still readable");

    must_send(fd.get(), marked(0x0900));
    if (collect(fd.get(), 0x0900, 1).size() != 1)
        THROW_TEST_FAILURE("Record not delivered after reset");
}

} // namespace

void TestEvents(const EnumeratedDevice &dev)
{
    {
        DevFd dev_fd(dev.path);
        int fd = dev_fd.get();

        VerifyValidation(fd);
        VerifyPollMatchesRecords(fd);

        if (dev.type != Blackhole || !fw_attached(fd)) {
            std::cout << "Skipping event delivery tests: firmware does not publish an event ring\n";
            return;
        }

        poll_fd(fd, EVENT_TIMEOUT_MS);
        read_events(fd);

        VerifyRoundTrip(fd);
        VerifyEmptyDoorbell(fd);
        VerifyTwoFds(dev.path, fd);
        VerifyOpenAfterEvent(dev.path, fd);
        VerifyNonTestRejected(fd);
        VerifyBatch(fd);
        VerifyFwRingOverflow(fd);
        VerifyKmdRingOverflow(dev.path, fd);
    }

    VerifyReset(dev);
}
