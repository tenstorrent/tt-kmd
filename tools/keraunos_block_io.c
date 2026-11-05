// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Keraunos test tool - exercise TENSTORRENT_IOCTL_DEV_NOC_{READ,WRITE}_BLOCK.
//
// Targets scratch memory that ttsim (main, mmk/qsr) models and nothing else
// uses with SMC firmware absent:
//   HSIO0 TL1 SRAM  SPA 0x12_0680_0000, 8 MiB (any access width)
//   SMC SPM         SPA 0x12_0206_0000 = KLA 0x0806_0000, 1 MiB
//                   (aligned 1/2/4 or aligned 8 byte accesses only)
// On real silicon with firmware running, SPM holds the bundle staging window:
// don't run this there.
//
// To Compile:
//   gcc -O2 -Wall -Wextra -o keraunos_block_io keraunos_block_io.c
//
// To Run:
//   ./keraunos_block_io <device_id>                          # test suite
//   ./keraunos_block_io <device_id> bench [spa] [len]        # write/read timing
//   ./keraunos_block_io <device_id> dump [-k] <addr> <len>   # hexdump a range

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/types.h>

// --- Driver Definitions (mirror of ioctl.h) ---
#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_NOC_READ		_IO(TENSTORRENT_IOCTL_MAGIC, 18)
#define TENSTORRENT_IOCTL_NOC_WRITE		_IO(TENSTORRENT_IOCTL_MAGIC, 19)
#define TENSTORRENT_IOCTL_DEV_NOC_READ_BLOCK	_IO(TENSTORRENT_IOCTL_MAGIC, 0x81)
#define TENSTORRENT_IOCTL_DEV_NOC_WRITE_BLOCK	_IO(TENSTORRENT_IOCTL_MAGIC, 0x82)
#define TENSTORRENT_NOC_FLAG_KLA	(1 << 0)
#define TENSTORRENT_NOC_BLOCK_IO_MAX_LEN	(UINT64_C(1) << 32)

struct tenstorrent_noc_io {
	__u32 argsz;
	__u32 flags;
	__u16 x;
	__u16 y;
	__u8 noc;
	__u8 width;
	__u8 reserved0[2];
	__u64 addr;
	__u64 value;
};

struct tenstorrent_noc_block_io {
	__u32 argsz;
	__u32 flags;
	__u16 x;
	__u16 y;
	__u8 noc;
	__u8 reserved0[3];
	__u64 addr;
	__u64 data_ptr;
	__u64 data_len;
	__u64 bytes_transferred;
};

// --- Targets ---
#define TL1_SPA		UINT64_C(0x1206800000)
#define TL1_SIZE	(8u << 20)
#define SPM_SPA		UINT64_C(0x1202060000)
#define SPM_KLA		UINT64_C(0x08060000)
#define SPM_SIZE	(1u << 20)

static int fd = -1;
static int failures;
static int passes;

#define CHECK(cond, ...) do {						\
	if (cond) {							\
		passes++;						\
	} else {							\
		failures++;						\
		printf("  FAIL %s:%d: ", __func__, __LINE__);		\
		printf(__VA_ARGS__);					\
		printf("\n");						\
	}								\
} while (0)

static void block_req(struct tenstorrent_noc_block_io *io, uint64_t addr, void *buf, uint64_t len, uint32_t flags)
{
	memset(io, 0, sizeof(*io));
	io->argsz = sizeof(*io);
	io->flags = flags;
	io->addr = addr;
	io->data_ptr = (uintptr_t)buf;
	io->data_len = len;
}

// Returns 0 or -errno; *done (if non-NULL) gets bytes_transferred.
static int block_io(int write, uint64_t addr, void *buf, uint64_t len, uint32_t flags, uint64_t *done)
{
	struct tenstorrent_noc_block_io io;
	int r;

	block_req(&io, addr, buf, len, flags);
	r = ioctl(fd, write ? TENSTORRENT_IOCTL_DEV_NOC_WRITE_BLOCK : TENSTORRENT_IOCTL_DEV_NOC_READ_BLOCK, &io);
	if (done)
		*done = io.bytes_transferred;
	return r ? -errno : 0;
}

static int raw_block_ioctl(int write, struct tenstorrent_noc_block_io *io)
{
	int r = ioctl(fd, write ? TENSTORRENT_IOCTL_DEV_NOC_WRITE_BLOCK : TENSTORRENT_IOCTL_DEV_NOC_READ_BLOCK, io);

	return r ? -errno : 0;
}

static int scalar(int write, uint64_t addr, uint32_t flags, uint32_t *value)
{
	struct tenstorrent_noc_io io = {
		.argsz = sizeof(io), .flags = flags, .width = 4, .addr = addr,
		.value = write ? *value : 0,
	};
	int r = ioctl(fd, write ? TENSTORRENT_IOCTL_NOC_WRITE : TENSTORRENT_IOCTL_NOC_READ, &io);

	if (!r && !write)
		*value = (uint32_t)io.value;
	return r ? -errno : 0;
}

// Deterministic pattern that differs per call and per word.
static void fill(void *buf, size_t len, uint32_t seed)
{
	uint32_t *w = buf;
	uint32_t x = seed * 2654435761u + 1;
	size_t i;

	for (i = 0; i < len / 4; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		w[i] = x;
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Page-aligned allocation with room for an offset and guard bytes.
static uint8_t *alloc_buf(size_t len)
{
	size_t sz = (len + 2 * 4096 + 4095) & ~(size_t)4095;
	void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

#define GUARD 0x5a

// Write a pattern, read it back into a guarded buffer at user offset uoff.
static void round_trip(const char *what, uint64_t addr, uint32_t len, uint32_t flags, size_t uoff, uint32_t seed)
{
	uint8_t *src = alloc_buf(len + uoff);
	uint8_t *dst = alloc_buf(len + uoff + 64);
	uint64_t done = 0;
	size_t i, bad = 0;
	int r;

	fill(src + uoff, len, seed);
	r = block_io(1, addr, src + uoff, len, flags, &done);
	CHECK(r == 0 && done == len, "%s write 0x%" PRIx64 "+%u uoff %zu: r=%d done=%" PRIu64,
	      what, addr, len, uoff, r, done);

	memset(dst, GUARD, len + uoff + 64);
	r = block_io(0, addr, dst + uoff, len, flags, &done);
	CHECK(r == 0 && done == len, "%s read 0x%" PRIx64 "+%u uoff %zu: r=%d done=%" PRIu64,
	      what, addr, len, uoff, r, done);

	for (i = 0; i < len; i++)
		if (dst[uoff + i] != src[uoff + i])
			bad++;
	CHECK(bad == 0, "%s 0x%" PRIx64 "+%u uoff %zu: %zu bytes differ", what, addr, len, uoff, bad);

	for (i = 0; i < uoff; i++)
		if (dst[i] != GUARD)
			break;
	CHECK(i == uoff, "%s: guard before buffer clobbered at %zu", what, i);
	for (i = 0; i < 64; i++)
		if (dst[uoff + len + i] != GUARD)
			break;
	CHECK(i == 64, "%s: guard after buffer clobbered at +%zu", what, i);

	munmap(src, ((len + uoff + 2 * 4096 + 4095) & ~(size_t)4095));
	munmap(dst, ((len + uoff + 64 + 2 * 4096 + 4095) & ~(size_t)4095));
}

static void test_validation(void)
{
	struct tenstorrent_noc_block_io io;
	static uint32_t buf[64];
	int r;

	printf("validation\n");

	block_req(&io, TL1_SPA, buf, sizeof(buf), 0);
	io.argsz = sizeof(io) - 8;
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "short argsz: %d", r);

	block_req(&io, TL1_SPA, buf, sizeof(buf), 0x2);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "unknown flag: %d", r);

	block_req(&io, TL1_SPA, buf, sizeof(buf), 0);
	io.reserved0[1] = 1;
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "reserved0: %d", r);

	block_req(&io, TL1_SPA, buf, sizeof(buf), 0);
	io.bytes_transferred = 4;
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "nonzero bytes_transferred in: %d", r);

	block_req(&io, TL1_SPA, buf, sizeof(buf), 0);
	io.x = 1;
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "x != 0 on Keraunos: %d", r);

	block_req(&io, TL1_SPA + 2, buf, sizeof(buf), 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "unaligned addr: %d", r);

	block_req(&io, TL1_SPA, buf, 6, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "unaligned len: %d", r);

	block_req(&io, TL1_SPA, (uint8_t *)buf + 2, 8, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "unaligned data_ptr: %d", r);

	block_req(&io, TL1_SPA, buf, 0, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "zero len: %d", r);

	block_req(&io, TL1_SPA, buf, TENSTORRENT_NOC_BLOCK_IO_MAX_LEN + 4, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "len > max: %d", r);

	block_req(&io, (UINT64_C(1) << 52) - 4, buf, 8, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "range past 2^52: %d", r);

	block_req(&io, UINT64_MAX - 3, buf, 8, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "addr wrap: %d", r);

	// Buffer overlapping the request struct itself.
	block_req(&io, TL1_SPA, &io, sizeof(io), 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EINVAL, "buffer overlaps request: %d", r);

	block_req(&io, TL1_SPA, NULL, 4096, 0);
	r = raw_block_ioctl(0, &io);
	CHECK(r == -EFAULT && io.bytes_transferred == 0, "NULL buffer: %d done=%llu", r,
	      (unsigned long long)io.bytes_transferred);
}

static void test_round_trips(void)
{
	static const uint32_t lens[] = { 4, 8, 12, 60, 4096, 4100, 65536 + 4, 256 * 1024 + 12, 1 << 20 };
	static const size_t uoffs[] = { 0, 4, 8, 4092 };
	unsigned i, j;

	printf("TL1 round trips (SPA)\n");
	for (i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
		for (j = 0; j < sizeof(uoffs) / sizeof(uoffs[0]); j++)
			round_trip("tl1", TL1_SPA + 4 * (i + j) + 0x1000 * i, lens[i], 0, uoffs[j], i * 16 + j);

	// 4-byte-aligned but not 8-byte-aligned device address: 32-bit path.
	round_trip("tl1 odd", TL1_SPA + 0x100004, 4096, 0, 0, 99);

	printf("TL1 whole 8 MiB\n");
	round_trip("tl1 all", TL1_SPA, TL1_SIZE, 0, 0, 1234);

	printf("SPM round trips (SPA and KLA)\n");
	round_trip("spm spa", SPM_SPA + 0x40000, 4096, 0, 4, 7);
	round_trip("spm spa odd", SPM_SPA + 0x40004, 1000, 0, 0, 8);
	// KLA through the 4 x 16KB WC SysIn0 window: cross 16 KiB and 64 KiB
	// boundaries so the window is retargeted mid-transfer.
	round_trip("spm kla", SPM_KLA + 0x3ffc, 100 * 1024, TENSTORRENT_NOC_FLAG_KLA, 8, 9);
	round_trip("spm kla small", SPM_KLA + 0x80000, 16, TENSTORRENT_NOC_FLAG_KLA, 0, 10);
}

// The block and scalar paths, and the SPA and KLA views of SPM, must agree.
static void test_coherence(void)
{
	uint32_t buf[256], v;
	unsigned i, bad;
	int r;

	printf("block vs scalar, SPA vs KLA\n");

	fill(buf, sizeof(buf), 42);
	r = block_io(1, TL1_SPA + 0x200000, buf, sizeof(buf), 0, NULL);
	CHECK(r == 0, "block write: %d", r);
	for (i = 0, bad = 0; i < 256; i += 37) {
		r = scalar(0, TL1_SPA + 0x200000 + 4 * i, 0, &v);
		if (r || v != buf[i])
			bad++;
	}
	CHECK(bad == 0, "scalar reads disagree with block write (%u)", bad);

	v = 0xfeedf00d;
	r = scalar(1, TL1_SPA + 0x200000 + 4 * 100, 0, &v);
	CHECK(r == 0, "scalar write: %d", r);
	r = block_io(0, TL1_SPA + 0x200000, buf, sizeof(buf), 0, NULL);
	CHECK(r == 0 && buf[100] == 0xfeedf00d, "block read after scalar write: r=%d v=0x%x", r, buf[100]);

	fill(buf, sizeof(buf), 43);
	r = block_io(1, SPM_KLA + 0x10000, buf, sizeof(buf), TENSTORRENT_NOC_FLAG_KLA, NULL);
	CHECK(r == 0, "kla write: %d", r);
	{
		uint32_t back[256];

		r = block_io(0, SPM_SPA + 0x10000, back, sizeof(back), 0, NULL);
		CHECK(r == 0 && !memcmp(buf, back, sizeof(buf)), "SPM via SPA disagrees with KLA write: r=%d", r);
	}

	// Ping-pong between two 16 MiB windows so entry 255 is retargeted
	// between transfers, then verify both.
	{
		uint32_t a[64], b[64], ra[64], rb[64];

		for (i = 0; i < 8; i++) {
			fill(a, sizeof(a), 100 + i);
			fill(b, sizeof(b), 200 + i);
			block_io(1, TL1_SPA + 0x300000, a, sizeof(a), 0, NULL);
			block_io(1, SPM_SPA + 0x20000, b, sizeof(b), 0, NULL);
		}
		block_io(0, TL1_SPA + 0x300000, ra, sizeof(ra), 0, NULL);
		block_io(0, SPM_SPA + 0x20000, rb, sizeof(rb), 0, NULL);
		CHECK(!memcmp(a, ra, sizeof(a)) && !memcmp(b, rb, sizeof(b)), "window ping-pong mismatch");
	}
}

// A buffer whose second page is inaccessible: the first page's worth moves,
// the call fails with EFAULT, and bytes_transferred says exactly how much.
static void test_partial(void)
{
	uint8_t *p = mmap(NULL, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	uint64_t done = 0;
	int r;

	printf("partial progress\n");
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	mprotect(p + 4096, 4096, PROT_NONE);

	r = block_io(0, TL1_SPA, p + 1024, 8192, 0, &done);
	CHECK(r == -EFAULT && done == 4096 - 1024, "read into half-bad buffer: r=%d done=%" PRIu64, r, done);

	r = block_io(1, TL1_SPA, p + 1024, 8192, 0, &done);
	CHECK(r == -EFAULT && done == 4096 - 1024, "write from half-bad buffer: r=%d done=%" PRIu64, r, done);

	munmap(p, 2 * 4096);
}

static void bench(uint64_t addr, uint32_t len)
{
	uint8_t *buf = alloc_buf(len);
	double t;
	int r;

	fill(buf, len, 1);
	t = now();
	r = block_io(1, addr, buf, len, 0, NULL);
	t = now() - t;
	printf("write %u bytes: r=%d %.3f s %.2f MB/s\n", len, r, t, len / t / 1e6);

	t = now();
	r = block_io(0, addr, buf, len, 0, NULL);
	t = now() - t;
	printf("read  %u bytes: r=%d %.3f s %.2f MB/s\n", len, r, t, len / t / 1e6);
}

static void dump(uint64_t addr, uint32_t len, uint32_t flags)
{
	uint32_t *buf = calloc(1, len);
	uint32_t i;
	int r = block_io(0, addr, buf, len, flags, NULL);

	if (r) {
		printf("read failed: %s\n", strerror(-r));
		return;
	}
	for (i = 0; i < len / 4; i++)
		printf("%s%08x%s", i % 8 ? " " : "", buf[i], i % 8 == 7 ? "\n" : "");
	if (len / 4 % 8)
		printf("\n");
}

int main(int argc, char **argv)
{
	char path[64];

	if (argc < 2) {
		fprintf(stderr, "usage: %s <device_id> [bench [spa] [len] | dump [-k] <addr> <len>]\n", argv[0]);
		return 2;
	}
	snprintf(path, sizeof(path), "/dev/tenstorrent/%s", argv[1]);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 1;
	}

	if (argc >= 3 && !strcmp(argv[2], "bench")) {
		uint64_t addr = argc > 3 ? strtoull(argv[3], NULL, 0) : TL1_SPA;
		uint32_t len = argc > 4 ? strtoul(argv[4], NULL, 0) : TL1_SIZE;

		bench(addr, len);
		return 0;
	}
	if (argc >= 3 && !strcmp(argv[2], "dump")) {
		int k = argc > 3 && !strcmp(argv[3], "-k");

		if (argc < 5 + k) {
			fprintf(stderr, "dump [-k] <addr> <len>\n");
			return 2;
		}
		dump(strtoull(argv[3 + k], NULL, 0), strtoul(argv[4 + k], NULL, 0),
		     k ? TENSTORRENT_NOC_FLAG_KLA : 0);
		return 0;
	}

	test_validation();
	test_round_trips();
	test_coherence();
	test_partial();

	printf("%d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
