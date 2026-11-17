// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Exercise device event records (TENSTORRENT_IOCTL_READ_EVENTS) on Blackhole.
//
// To compile:
//  gcc -Wall -o tt-events tt-events.c
//
// Usage:
//  tt-events [-d DEV] send [type=T] [source=S] [cause=C] [data=A,B,C] [repeat=N]
//                                 Send TT_SMC_MSG_TEST_EVENT_RECORD.  type
//                                 defaults to TEST (0); firmware rejects any
//                                 other.  Record i of the batch has data
//                                 C + i.
//  tt-events [-d DEV]... watch [--from-oldest]
//                                 Print records as they arrive.
//  tt-events [-d DEV] selftest [--no-reset]
//                                 Run end-to-end checks against firmware.
//                                 The reset check resets the device.
//
// DEV is a device number or path; the default is /dev/tenstorrent/0.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "../ioctl.h"

#define TT_SMC_MSG_TEST_EVENT_RECORD 0x91

// Firmware event types, from tt-system-firmware's device_events.h
#define EVENT_TYPE_TEST 0x0000
#define EVENT_TYPE_EVENTS_SUPPRESSED 0x0001

#define FW_RING_ENTRIES 64	// Records the firmware ring holds
#define KMD_RING_ENTRIES 256	// Records the driver keeps

#define EVENT_TIMEOUT_MS 100	// An event must be visible this soon after its message
#define SMC_TIMEOUT_MS 5000
#define RESET_TIMEOUT_MS 10000
#define MAX_WATCH_DEVICES 16

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "tt-events: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

static void sleep_ms(unsigned int ms)
{
	usleep(ms * 1000);
}

static void device_path(const char *dev, char *path, size_t len)
{
	char *end;

	strtol(dev, &end, 10);
	if (*dev != '\0' && *end == '\0')
		snprintf(path, len, "/dev/tenstorrent/%s", dev);
	else
		snprintf(path, len, "%s", dev);
}

// O_APPEND opens without changing the device's power state.
static int open_device(const char *path)
{
	int fd = open(path, O_RDWR | O_APPEND | O_CLOEXEC);

	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	return fd;
}

// Returns poll() revents, 0 on timeout.
static short wait_fd(int fd, int timeout_ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN | POLLPRI };
	int r = poll(&p, 1, timeout_ms);

	if (r < 0)
		die("poll: %s", strerror(errno));
	return r ? p.revents : 0;
}

// Sends an SMC message and waits for the response.  Returns the firmware's
// status byte.
static unsigned int smc_msg(int fd, const uint32_t request[8], uint32_t response[8])
{
	struct tenstorrent_smc_msg msg = { .argsz = sizeof(msg), .flags = TENSTORRENT_SMC_MSG_POST };
	uint64_t deadline = now_ms() + SMC_TIMEOUT_MS;

	memcpy(msg.message, request, sizeof(msg.message));
	if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) != 0)
		die("SMC_MSG POST: %s", strerror(errno));

	for (;;) {
		memset(&msg, 0, sizeof(msg));
		msg.argsz = sizeof(msg);
		msg.flags = TENSTORRENT_SMC_MSG_POLL;
		if (ioctl(fd, TENSTORRENT_IOCTL_SMC_MSG, &msg) == 0)
			break;
		if (errno != EAGAIN)
			die("SMC_MSG POLL: %s", strerror(errno));
		if (now_ms() > deadline)
			die("SMC_MSG: no response");
		sleep_ms(1);
	}

	memcpy(response, msg.message, sizeof(msg.message));
	return response[0] & 0xFF;
}

// Returns the firmware status; *count is the firmware's new counter value.

static int bdf_of(int fd, char *bdf, size_t len)
{
	struct stat st;
	char link[PATH_MAX], target[PATH_MAX];
	ssize_t n;
	char *base;

	if (fstat(fd, &st) != 0)
		return -1;

	snprintf(link, sizeof(link), "/sys/dev/char/%u:%u/device", major(st.st_rdev), minor(st.st_rdev));
	n = readlink(link, target, sizeof(target) - 1);
	if (n < 0)
		return -1;
	target[n] = '\0';

	base = strrchr(target, '/');
	if (snprintf(bdf, len, "%s", base ? base + 1 : target) >= (int)len)
		return -1;
	return 0;
}

static bool reset_ioctl(int fd, unsigned int flags)
{
	struct tenstorrent_reset_device r = { 0 };

	r.in.output_size_bytes = sizeof(r.out);
	r.in.flags = flags;
	return ioctl(fd, TENSTORRENT_IOCTL_RESET_DEVICE, &r) == 0 && r.out.result == 0;
}

// Resets the device behind fd as tools/reset.c does, and returns a new fd for
// it.  Returns -1 on failure.
static int reset_device(int fd)
{
	char bdf[NAME_MAX + 1], config[PATH_MAX], by_bdf[PATH_MAX];
	uint64_t deadline;
	int new_fd = -1;

	if (bdf_of(fd, bdf, sizeof(bdf)) != 0)
		return -1;

	if (!reset_ioctl(fd, TENSTORRENT_RESET_DEVICE_ASIC_RESET))
		return -1;

	// In-place reset is complete once firmware clears the reset marker, bit 6
	// of the PCI command register.  Hotplug removes and re-adds the device.
	snprintf(config, sizeof(config), "/sys/bus/pci/devices/%s/config", bdf);
	deadline = now_ms() + RESET_TIMEOUT_MS;
	for (;;) {
		int cfd = open(config, O_RDONLY);
		unsigned char cmd;
		bool done = false;

		if (cfd >= 0) {
			done = pread(cfd, &cmd, 1, 4) == 1 && !(cmd & (1 << 6));
			close(cfd);
		}
		if (done)
			break;
		if (now_ms() > deadline)
			return -1;
		sleep_ms(100);
	}

	snprintf(by_bdf, sizeof(by_bdf), "/dev/tenstorrent/by-bdf/%s", bdf);
	deadline = now_ms() + RESET_TIMEOUT_MS;
	while ((new_fd = open(by_bdf, O_RDWR | O_APPEND | O_CLOEXEC)) < 0) {
		if (now_ms() > deadline)
			return -1;
		sleep_ms(100);
	}

	if (!reset_ioctl(new_fd, TENSTORRENT_RESET_DEVICE_POST_RESET)) {
		close(new_fd);
		return -1;
	}

	return new_fd;
}

struct test_record {
	uint16_t type;
	uint16_t source;
	uint16_t repeat;
	uint32_t cause;
	uint32_t data[3];
};

// Returns the firmware status.
static unsigned int send_record_msg(int fd, const struct test_record *r)
{
	uint32_t request[8] = { 0 };
	uint32_t response[8];

	request[0] = TT_SMC_MSG_TEST_EVENT_RECORD | ((uint32_t)r->type << 16);
	request[1] = r->source | ((uint32_t)r->repeat << 16);
	request[2] = r->cause;
	request[3] = r->data[0];
	request[4] = r->data[1];
	request[5] = r->data[2];
	return smc_msg(fd, request, response);
}

// Reads up to capacity records into events (which may be NULL if capacity is 0).
static struct tenstorrent_read_events read_events(int fd, struct tenstorrent_event *events, uint32_t capacity,
						  uint32_t flags)
{
	struct tenstorrent_read_events r = {
		.argsz = sizeof(r),
		.flags = flags,
		.capacity = capacity,
		.events_ptr = (uintptr_t)events,
	};

	if (ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &r) != 0)
		die("READ_EVENTS: %s", strerror(errno));
	return r;
}

static bool fw_attached(int fd)
{
	return read_events(fd, NULL, 0, 0).status & TENSTORRENT_READ_EVENTS_STATUS_FW_ATTACHED;
}

static const char *type_name(uint16_t type)
{
	switch (type) {
	case EVENT_TYPE_TEST: return "TEST";
	case EVENT_TYPE_EVENTS_SUPPRESSED: return "EVENTS_SUPPRESSED";
	case TENSTORRENT_EVENT_TYPE_FW_EVENTS_LOST: return "FW_EVENTS_LOST";
	default: return "?";
	}
}

static void print_event(const char *path, const struct tenstorrent_event *e)
{
	printf("%s: seq %" PRIu64 " %s (0x%04x) source 0x%04x cause 0x%08x data 0x%08x 0x%08x 0x%08x"
	       " device_time %" PRIu64 " host_time_ns %" PRIu64 "\n",
	       path, (uint64_t)e->seq, type_name(e->type), e->type, e->source, e->cause,
	       e->data[0], e->data[1], e->data[2], (uint64_t)e->device_time, (uint64_t)e->host_time_ns);
}

// Parses up to three comma-separated values.
static void parse_data(const char *val, uint32_t data[3])
{
	char *end;
	int i;

	for (i = 0; i < 3; i++) {
		data[i] = strtoul(val, &end, 0);
		if (*end != ',')
			break;
		val = end + 1;
	}
	if (*end != '\0')
		die("bad data %s", val);
}

static int cmd_send(const char *path, int argc, char **argv)
{
	struct test_record r = { .repeat = 1 };
	unsigned int status;
	int i, fd;

	for (i = 0; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		const char *val;

		if (!eq)
			die("bad argument %s", argv[i]);
		*eq = '\0';
		val = eq + 1;

		if (strcmp(argv[i], "type") == 0)
			r.type = strtoul(val, NULL, 0);
		else if (strcmp(argv[i], "source") == 0)
			r.source = strtoul(val, NULL, 0);
		else if (strcmp(argv[i], "cause") == 0)
			r.cause = strtoul(val, NULL, 0);
		else if (strcmp(argv[i], "repeat") == 0)
			r.repeat = strtoul(val, NULL, 0);
		else if (strcmp(argv[i], "data") == 0)
			parse_data(val, r.data);
		else
			die("bad argument %s", argv[i]);
	}

	fd = open_device(path);
	status = send_record_msg(fd, &r);
	close(fd);

	if (status != 0) {
		printf("firmware rejected the message: status %u\n", status);
		return 1;
	}

	printf("firmware appended %u record(s)\n", r.repeat);
	return 0;
}

static int cmd_watch(char paths[][PATH_MAX], int ndev, bool from_oldest)
{
	static struct tenstorrent_event events[KMD_RING_ENTRIES];
	struct pollfd pfd[MAX_WATCH_DEVICES];
	int i, open_fds = ndev;

	for (i = 0; i < ndev; i++) {
		pfd[i].fd = open_device(paths[i]);
		pfd[i].events = POLLIN | POLLPRI;
		printf("%s: firmware events %s\n", paths[i], fw_attached(pfd[i].fd) ? "attached" : "NOT attached");

		// A new fd has nothing unread, so read what is kept now rather than
		// waiting for poll().
		if (from_oldest) {
			struct tenstorrent_read_events r;
			uint32_t j;

			r = read_events(pfd[i].fd, events, KMD_RING_ENTRIES, TENSTORRENT_READ_EVENTS_FROM_OLDEST);
			for (j = 0; j < r.count; j++)
				print_event(paths[i], &events[j]);
		}
	}
	fflush(stdout);

	while (open_fds > 0) {
		if (poll(pfd, ndev, -1) < 0)
			die("poll: %s", strerror(errno));

		for (i = 0; i < ndev; i++) {
			struct tenstorrent_read_events r;
			uint32_t j;

			if (pfd[i].fd < 0 || !pfd[i].revents)
				continue;

			if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				printf("%s: device reset or removed; reopen to continue\n", paths[i]);
				close(pfd[i].fd);
				pfd[i].fd = -1;
				open_fds--;
				continue;
			}

			r = read_events(pfd[i].fd, events, KMD_RING_ENTRIES, 0);
			if (r.lost)
				printf("%s: missed %u record(s)\n", paths[i], r.lost);
			for (j = 0; j < r.count; j++)
				print_event(paths[i], &events[j]);
		}
		fflush(stdout);
	}

	return 0;
}

// --- selftest ---

static int failures;

#define CHECK(cond, ...)						\
	do {								\
		if (cond) {						\
			printf("  ok    ");				\
		} else {						\
			printf("  FAIL  ");				\
			failures++;					\
		}							\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	} while (0)

static struct tenstorrent_event buf[KMD_RING_ENTRIES * 2];

static void must_send(int fd, uint32_t marker, uint16_t repeat, uint32_t base)
{
	struct test_record r = { .repeat = repeat, .data = { marker, 0, base } };

	if (send_record_msg(fd, &r) != 0)
		die("firmware rejected a test event message");
}

// Reads everything unread on fd into buf.  Returns the number read.
static uint32_t drain(int fd, uint32_t *lost)
{
	struct tenstorrent_read_events r = read_events(fd, buf, KMD_RING_ENTRIES * 2, 0);

	if (lost)
		*lost = r.lost;
	return r.count;
}

// Counts TEST records in buf[0..n) carrying marker.
static uint32_t count_marked(uint32_t n, uint32_t marker)
{
	uint32_t i, c = 0;

	for (i = 0; i < n; i++)
		c += buf[i].type == EVENT_TYPE_TEST && buf[i].data[0] == marker;
	return c;
}

// Waits on fd until want records carrying marker have been read.
static bool wait_marked(int fd, uint32_t marker, uint32_t want)
{
	uint64_t deadline = now_ms() + 2000;
	uint32_t got = 0;

	while (got < want && now_ms() < deadline) {
		wait_fd(fd, EVENT_TIMEOUT_MS);
		got += count_marked(drain(fd, NULL), marker);
	}
	return got == want;
}

static void test_round_trip(int fd)
{
	struct test_record r = { .repeat = 1, .source = 0x1234, .cause = 0xCAFEBABE,
				 .data = { 0x100, 0xDEADBEEF, 0x5A5A5A5A } };
	short revents;
	uint32_t n;

	printf("A record round-trips every field\n");
	if (send_record_msg(fd, &r) != 0)
		die("firmware rejected a test event message");
	revents = wait_fd(fd, EVENT_TIMEOUT_MS);
	CHECK(revents == (POLLIN | POLLPRI), "poll readable within %d ms (revents 0x%x)", EVENT_TIMEOUT_MS, revents);
	n = drain(fd, NULL);
	CHECK(n == 1, "one record (%u)", n);
	if (n == 1) {
		print_event("   ", &buf[0]);
		CHECK(buf[0].type == r.type && buf[0].source == r.source && buf[0].cause == r.cause &&
		      buf[0].data[0] == r.data[0] && buf[0].data[1] == r.data[1] && buf[0].data[2] == r.data[2] &&
		      buf[0].data[3] == 0, "fields match");
		CHECK(buf[0].device_time != 0, "device_time is set");
	}
	CHECK(wait_fd(fd, 0) == 0, "poll not readable after READ_EVENTS");
}

static void test_empty_doorbell(int fd)
{
	struct test_record r = { .repeat = 0 };	// A batch of 0 sends only the doorbell

	printf("A doorbell without a record wakes nobody\n");
	if (send_record_msg(fd, &r) != 0)
		die("firmware rejected the doorbell message");
	CHECK(wait_fd(fd, EVENT_TIMEOUT_MS) == 0, "poll not readable");
}

static void test_two_fds(const char *path, int fd)
{
	int fd2 = open_device(path);

	printf("Two fds both see a record, independently\n");
	must_send(fd, 0x200, 1, 0);
	CHECK(wait_fd(fd, EVENT_TIMEOUT_MS) & POLLIN, "first fd readable");
	CHECK(wait_fd(fd2, EVENT_TIMEOUT_MS) & POLLIN, "second fd readable");
	CHECK(count_marked(drain(fd, NULL), 0x200) == 1, "first fd reads it");
	CHECK(wait_fd(fd2, 0) & POLLIN, "second fd still readable after the first read");
	CHECK(count_marked(drain(fd2, NULL), 0x200) == 1, "second fd reads it too");
	close(fd2);
}

static void test_open_after(const char *path, int fd)
{
	int fd2;

	printf("An fd opened after a record does not see it\n");
	must_send(fd, 0x300, 1, 0);
	CHECK(wait_fd(fd, EVENT_TIMEOUT_MS) & POLLIN, "existing fd readable");
	fd2 = open_device(path);
	CHECK(wait_fd(fd2, 0) == 0, "new fd not readable");
	CHECK(drain(fd2, NULL) == 0, "new fd reads nothing");
	close(fd2);
	drain(fd, NULL);
}

static void test_rejected(int fd)
{
	struct test_record r = { .type = 1, .repeat = 1 };
	unsigned int status;

	printf("Firmware rejects a non-TEST type\n");
	status = send_record_msg(fd, &r);
	CHECK(status != 0, "nonzero status (%u)", status);
	CHECK(wait_fd(fd, EVENT_TIMEOUT_MS) == 0, "nothing delivered");
}

static void test_batch(int fd)
{
	const uint32_t k = 10;
	uint32_t n, i;
	bool ordered = true;

	printf("A batch of %u arrives in order with consecutive seq\n", k);
	must_send(fd, 0x500, k, 1000);
	wait_fd(fd, EVENT_TIMEOUT_MS);
	sleep_ms(EVENT_TIMEOUT_MS);
	n = drain(fd, NULL);
	CHECK(n == k && count_marked(n, 0x500) == k, "%u records (%u)", k, n);
	for (i = 0; i < n; i++)
		ordered &= buf[i].data[2] == 1000 + i && (i == 0 || buf[i].seq == buf[i - 1].seq + 1);
	CHECK(ordered, "in order, consecutive seq");
}

static void test_fw_overflow(int fd)
{
	const uint32_t k = FW_RING_ENTRIES + 36;
	uint32_t n, i;
	bool consecutive = true;

	printf("A batch of %u overflows the firmware ring\n", k);
	must_send(fd, 0x600, k, 0);
	wait_fd(fd, EVENT_TIMEOUT_MS);
	sleep_ms(EVENT_TIMEOUT_MS);
	n = drain(fd, NULL);
	CHECK(n == FW_RING_ENTRIES + 1, "%u records (%u)", FW_RING_ENTRIES + 1, n);
	if (n == 0)
		return;
	for (i = 0; i + 1 < n; i++)
		consecutive &= buf[i].data[2] == i && (i == 0 || buf[i].seq == buf[i - 1].seq + 1);
	CHECK(consecutive, "the first %u records, in order", FW_RING_ENTRIES);
	CHECK(buf[n - 1].type == TENSTORRENT_EVENT_TYPE_FW_EVENTS_LOST && buf[n - 1].data[0] == k - FW_RING_ENTRIES,
	      "then FW_EVENTS_LOST with count %u (%s, %u)", k - FW_RING_ENTRIES, type_name(buf[n - 1].type),
	      buf[n - 1].data[0]);
}

static void test_kmd_overflow(const char *path, int fd)
{
	const uint32_t per_msg = 32, messages = 10, total = per_msg * messages;
	int observer = open_device(path), fresh;
	struct tenstorrent_read_events r;
	uint32_t m, n, lost;

	printf("%u records without reading overflow the driver's %u\n", total, KMD_RING_ENTRIES);
	for (m = 0; m < messages; m++) {
		must_send(fd, 0x700, per_msg, m * per_msg);
		// Keep the firmware ring from overflowing: wait for the driver to
		// have each batch, reading on another fd.
		if (!wait_marked(observer, 0x700, per_msg))
			die("batch %u not delivered", m);
	}
	close(observer);

	n = drain(fd, &lost);
	CHECK(lost == total - KMD_RING_ENTRIES, "lost %u (%u)", total - KMD_RING_ENTRIES, lost);
	CHECK(n == KMD_RING_ENTRIES, "read %u (%u)", KMD_RING_ENTRIES, n);
	CHECK(n && buf[n - 1].data[2] == total - 1, "newest record is last");
	drain(fd, &lost);
	CHECK(lost == 0, "lost reported once");

	fresh = open_device(path);
	r = read_events(fresh, buf, KMD_RING_ENTRIES * 2, TENSTORRENT_READ_EVENTS_FROM_OLDEST);
	CHECK(r.count == KMD_RING_ENTRIES && r.lost == 0, "FROM_OLDEST on a new fd reads all %u kept (%u)",
	      KMD_RING_ENTRIES, r.count);
	close(fresh);
}

// Returns a fresh fd for the device.
static int test_reset(const char *path, int fd)
{
	struct tenstorrent_read_events r = { .argsz = sizeof(r) };
	short revents;
	int resetter, new_fd;

	printf("Reset: old fd is dead, records are discarded, delivery resumes\n");
	must_send(fd, 0x800, 1, 0);
	wait_fd(fd, EVENT_TIMEOUT_MS);

	// The fd that resets the device stays usable, so reset through another.
	resetter = open_device(path);
	new_fd = reset_device(resetter);
	close(resetter);
	if (new_fd < 0) {
		CHECK(false, "device reset");
		return fd;
	}

	revents = wait_fd(fd, 0);
	CHECK((revents & (POLLERR | POLLHUP)) == (POLLERR | POLLHUP), "old fd reports POLLERR | POLLHUP (0x%x)",
	      revents);
	CHECK(ioctl(fd, TENSTORRENT_IOCTL_READ_EVENTS, &r) != 0 && errno == ENODEV,
	      "READ_EVENTS on old fd fails with ENODEV");
	close(fd);

	CHECK(fw_attached(new_fd), "firmware reattached");

	r = read_events(new_fd, buf, KMD_RING_ENTRIES * 2, TENSTORRENT_READ_EVENTS_FROM_OLDEST);
	CHECK(r.count == 0, "no records from before the reset (%u)", r.count);

	must_send(new_fd, 0x900, 1, 0);
	CHECK(wait_marked(new_fd, 0x900, 1), "records delivered after reset");
	return new_fd;
}

static int cmd_selftest(const char *path, bool do_reset)
{
	int fd = open_device(path);

	if (!fw_attached(fd)) {
		printf("SKIP: firmware does not publish an event ring\n");
		close(fd);
		return 0;
	}

	wait_fd(fd, EVENT_TIMEOUT_MS);
	drain(fd, NULL);

	test_round_trip(fd);
	test_empty_doorbell(fd);
	test_two_fds(path, fd);
	test_open_after(path, fd);
	test_rejected(fd);
	test_batch(fd);
	test_fw_overflow(fd);
	test_kmd_overflow(path, fd);
	if (do_reset)
		fd = test_reset(path, fd);
	close(fd);

	printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	static char paths[MAX_WATCH_DEVICES][PATH_MAX];
	int ndev = 0, i = 1;

	while (i < argc && strcmp(argv[i], "-d") == 0) {
		if (i + 1 >= argc || ndev == MAX_WATCH_DEVICES)
			die("bad -d");
		device_path(argv[i + 1], paths[ndev++], PATH_MAX);
		i += 2;
	}
	if (ndev == 0)
		device_path("0", paths[ndev++], PATH_MAX);

	if (i >= argc)
		die("usage: tt-events [-d DEV]... send|watch|selftest ...");

	if (strcmp(argv[i], "send") == 0)
		return cmd_send(paths[0], argc - i - 1, argv + i + 1);
	if (strcmp(argv[i], "watch") == 0)
		return cmd_watch(paths, ndev, i + 1 < argc && strcmp(argv[i + 1], "--from-oldest") == 0);
	if (strcmp(argv[i], "selftest") == 0)
		return cmd_selftest(paths[0], !(i + 1 < argc && strcmp(argv[i + 1], "--no-reset") == 0));

	die("unknown command %s", argv[i]);
	return 1;
}
