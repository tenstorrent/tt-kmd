// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Benchmark the NOC_IO ioctl.
//
// To Compile:
//  gcc -O2 -Wall -o io_bench io_bench.c
//
// To Run:
//  ./io_bench /dev/tenstorrent/0

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/types.h>

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_NOC_IO          _IO(TENSTORRENT_IOCTL_MAGIC, 16)

struct tenstorrent_get_device_info {
	struct { __u32 output_size_bytes; } in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
		__u16 bus_dev_fn, max_dma_buf_size_log2, pci_domain;
		__u16 reserved;
	} out;
};

#define TENSTORRENT_NOC_IO_WRITE (1 << 0)
#define TENSTORRENT_NOC_IO_BLOCK (1 << 1)

struct tenstorrent_noc_io {
	__u32 argsz;
	__u32 flags;
	__u8  x;
	__u8  y;
	__u8  reserved0[2];
	__u32 reserved1;
	__u64 addr;
	__u64 data_ptr;
	__u64 data_len;
};

#define PCI_DEVICE_ID_BLACKHOLE 0xb140
#define PCI_DEVICE_ID_WORMHOLE  0x401e

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int noc_read32(int fd, uint8_t x, uint8_t y, uint64_t addr, uint32_t *value)
{
	struct tenstorrent_noc_io io = {0};

	io.argsz = sizeof(io);
	io.x = x;
	io.y = y;
	io.addr = addr;
	io.data_ptr = (uint64_t)(uintptr_t)value;
	io.data_len = 4;

	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io) < 0)
		return -errno;

	return 0;
}

static int noc_write32(int fd, uint8_t x, uint8_t y, uint64_t addr, uint32_t value)
{
	struct tenstorrent_noc_io io = {0};

	io.argsz = sizeof(io);
	io.flags = TENSTORRENT_NOC_IO_WRITE;
	io.x = x;
	io.y = y;
	io.addr = addr;
	io.data_ptr = (uint64_t)(uintptr_t)&value;
	io.data_len = 4;

	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io) < 0)
		return -errno;

	return 0;
}

static int noc_block_write(int fd, uint8_t x, uint8_t y, uint64_t addr, const void *buf, size_t len)
{
	struct tenstorrent_noc_io io = {0};

	io.argsz = sizeof(io);
	io.flags = TENSTORRENT_NOC_IO_WRITE | TENSTORRENT_NOC_IO_BLOCK;
	io.x = x;
	io.y = y;
	io.addr = addr;
	io.data_ptr = (uint64_t)(uintptr_t)buf;
	io.data_len = len;

	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io) < 0)
		return -errno;

	return 0;
}

static int noc_block_read(int fd, uint8_t x, uint8_t y, uint64_t addr, void *buf, size_t len)
{
	struct tenstorrent_noc_io io = {0};

	io.argsz = sizeof(io);
	io.flags = TENSTORRENT_NOC_IO_BLOCK;
	io.x = x;
	io.y = y;
	io.addr = addr;
	io.data_ptr = (uint64_t)(uintptr_t)buf;
	io.data_len = len;

	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io) < 0)
		return -errno;

	return 0;
}

static void bench_read32(int fd, uint8_t x, uint8_t y, uint64_t addr, int iterations)
{
	uint64_t start, elapsed;
	uint32_t dummy;
	int i;

	/* Warmup */
	for (i = 0; i < 100; i++)
		noc_read32(fd, x, y, addr, &dummy);

	start = now_ns();
	for (i = 0; i < iterations; i++)
		noc_read32(fd, x, y, addr, &dummy);
	elapsed = now_ns() - start;

	printf("  read32:  %d ops in %llu us, %.0f ns/op\n",
	       iterations, (unsigned long long)(elapsed / 1000),
	       (double)elapsed / iterations);
}

static void bench_write32(int fd, uint8_t x, uint8_t y, uint64_t addr, int iterations)
{
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++)
		noc_write32(fd, x, y, addr, 0);

	start = now_ns();
	for (i = 0; i < iterations; i++)
		noc_write32(fd, x, y, addr, (uint32_t)i);
	elapsed = now_ns() - start;

	printf("  write32: %d ops in %llu us, %.0f ns/op\n",
	       iterations, (unsigned long long)(elapsed / 1000),
	       (double)elapsed / iterations);
}

static void bench_block_write(int fd, uint8_t x, uint8_t y, uint64_t addr,
			      size_t size, int iterations)
{
	uint64_t start, elapsed, total_bytes;
	double mbps;
	void *buf;
	int i;

	buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		fprintf(stderr, "  block_write %zu: mmap failed\n", size);
		return;
	}
	memset(buf, 0xAA, size);

	for (i = 0; i < 3; i++)
		noc_block_write(fd, x, y, addr, buf, size);

	start = now_ns();
	for (i = 0; i < iterations; i++)
		noc_block_write(fd, x, y, addr, buf, size);
	elapsed = now_ns() - start;

	total_bytes = (uint64_t)size * iterations;
	mbps = (double)total_bytes / elapsed * 1000.0;

	printf("  block_write %7zu: %d ops in %llu us, %.1f MB/s, %.0f us/op\n",
	       size, iterations, (unsigned long long)(elapsed / 1000),
	       mbps, (double)elapsed / iterations / 1000.0);

	munmap(buf, size);
}

static void bench_block_read(int fd, uint8_t x, uint8_t y, uint64_t addr,
			     size_t size, int iterations)
{
	uint64_t start, elapsed, total_bytes;
	double mbps;
	void *buf;
	int i;

	buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) {
		fprintf(stderr, "  block_read %zu: mmap failed\n", size);
		return;
	}

	for (i = 0; i < 3; i++)
		noc_block_read(fd, x, y, addr, buf, size);

	start = now_ns();
	for (i = 0; i < iterations; i++)
		noc_block_read(fd, x, y, addr, buf, size);
	elapsed = now_ns() - start;

	total_bytes = (uint64_t)size * iterations;
	mbps = (double)total_bytes / elapsed * 1000.0;

	printf("  block_read  %7zu: %d ops in %llu us, %.1f MB/s, %.0f us/op\n",
	       size, iterations, (unsigned long long)(elapsed / 1000),
	       mbps, (double)elapsed / iterations / 1000.0);

	munmap(buf, size);
}

int main(int argc, char *argv[])
{
	struct tenstorrent_get_device_info info;
	uint8_t tensix_x, tensix_y;
	const char *dev_path;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s /dev/tenstorrent/N\n", argv[0]);
		return 1;
	}
	dev_path = argv[1];

	fd = open(dev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "GET_DEVICE_INFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	if (info.out.device_id == PCI_DEVICE_ID_BLACKHOLE) {
		tensix_x = 1;
		tensix_y = 2;
	} else if (info.out.device_id == PCI_DEVICE_ID_WORMHOLE) {
		tensix_x = 1;
		tensix_y = 1;
	} else {
		fprintf(stderr, "Unknown device ID 0x%04x\n", info.out.device_id);
		close(fd);
		return 1;
	}

	printf("Device: %s (ID 0x%04x), target tile (%u, %u)\n\n",
	       dev_path, info.out.device_id, tensix_x, tensix_y);

	printf("32-bit operations (L1 @ 0x4000):\n");
	bench_read32(fd, tensix_x, tensix_y, 0x4000, 10000);
	bench_write32(fd, tensix_x, tensix_y, 0x4000, 10000);

	printf("\nBlock write to L1 @ 0x10000:\n");
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 4096, 1000);
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 64 * 1024, 200);
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 256 * 1024, 50);
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 1024 * 1024, 20);

	printf("\nBlock read from L1 @ 0x10000:\n");
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 4096, 1000);
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 64 * 1024, 200);
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 256 * 1024, 50);
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 1024 * 1024, 20);

	close(fd);
	return 0;
}
