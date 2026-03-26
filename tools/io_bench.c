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
#define TENSTORRENT_IOCTL_QUERY_MAPPINGS  _IO(TENSTORRENT_IOCTL_MAGIC, 2)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB    _IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB        _IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB   _IO(TENSTORRENT_IOCTL_MAGIC, 13)
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

struct tenstorrent_allocate_tlb {
	struct { __u64 size; __u64 reserved; } in;
	struct { __u32 id; __u32 reserved0; __u64 mmap_offset_uc; __u64 mmap_offset_wc; __u64 reserved1; } out;
};

struct tenstorrent_free_tlb {
	struct { __u32 id; } in;
	struct { } out;
};

struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end, y_end, x_start, y_start;
	__u8 noc, mcast, ordering, linked, static_vc;
	__u8 reserved0[3];
	__u32 reserved1[2];
};

struct tenstorrent_configure_tlb {
	struct { __u32 id; __u32 reserved; struct tenstorrent_noc_tlb_config config; } in;
	struct { __u64 reserved; } out;
};

#define PCI_DEVICE_ID_BLACKHOLE 0xb140
#define PCI_DEVICE_ID_WORMHOLE  0x401e

#define TLB_2M_SIZE (1 << 21)
#define TLB_2M_MASK (TLB_2M_SIZE - 1)

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

struct mmio_window {
	int fd;
	uint32_t tlb_id;
	volatile uint32_t *base;
	size_t size;
};

static int mmio_window_open(struct mmio_window *w, int fd, uint8_t x, uint8_t y,
			    uint64_t addr, int use_wc)
{
	struct tenstorrent_allocate_tlb alloc = {};
	struct tenstorrent_configure_tlb conf = {};

	alloc.in.size = TLB_2M_SIZE;
	if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) < 0)
		return -errno;

	conf.in.id = alloc.out.id;
	conf.in.config.addr = addr & ~(uint64_t)TLB_2M_MASK;
	conf.in.config.x_end = x;
	conf.in.config.y_end = y;
	conf.in.config.ordering = use_wc ? 0 : 1;
	if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &conf) < 0) {
		struct tenstorrent_free_tlb fr = {};
		fr.in.id = alloc.out.id;
		ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &fr);
		return -errno;
	}

	uint64_t offset = use_wc ? alloc.out.mmap_offset_wc : alloc.out.mmap_offset_uc;
	void *map = mmap(NULL, TLB_2M_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	if (map == MAP_FAILED) {
		struct tenstorrent_free_tlb fr = {};
		fr.in.id = alloc.out.id;
		ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &fr);
		return -errno;
	}

	w->fd = fd;
	w->tlb_id = alloc.out.id;
	w->base = (volatile uint32_t *)map;
	w->size = TLB_2M_SIZE;
	return 0;
}

static void mmio_window_close(struct mmio_window *w)
{
	struct tenstorrent_free_tlb fr = {};

	munmap((void *)w->base, w->size);
	fr.in.id = w->tlb_id;
	ioctl(w->fd, TENSTORRENT_IOCTL_FREE_TLB, &fr);
}

static void bench_mmio_read32(struct mmio_window *w, uint64_t addr, int iterations)
{
	uint32_t offset_words = (addr & TLB_2M_MASK) / sizeof(uint32_t);
	volatile uint32_t *ptr = w->base + offset_words;
	uint32_t dummy;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++)
		dummy = *ptr;
	(void)dummy;

	start = now_ns();
	for (i = 0; i < iterations; i++)
		dummy = *ptr;
	elapsed = now_ns() - start;

	printf("  mmio read32:   %d ops, %.0f ns/op\n",
	       iterations, (double)elapsed / iterations);
}

static void bench_mmio_write32(struct mmio_window *w, uint64_t addr, int iterations)
{
	uint32_t offset_words = (addr & TLB_2M_MASK) / sizeof(uint32_t);
	volatile uint32_t *ptr = w->base + offset_words;
	uint64_t start, elapsed;
	int i;

	for (i = 0; i < 100; i++)
		*ptr = 0;

	start = now_ns();
	for (i = 0; i < iterations; i++)
		*ptr = (uint32_t)i;
	elapsed = now_ns() - start;

	printf("  mmio write32:  %d ops, %.0f ns/op\n",
	       iterations, (double)elapsed / iterations);
}

static void bench_mmio_block_write_u32(struct mmio_window *w, uint64_t addr,
				       size_t size, int iterations, const char *label)
{
	uint32_t offset = addr & TLB_2M_MASK;
	volatile uint32_t *dst = (volatile uint32_t *)((volatile char *)w->base + offset);
	size_t nwords = size / sizeof(uint32_t);
	uint32_t *buf;
	uint64_t start, elapsed, total_bytes;
	double mbps;
	int i;
	size_t j;

	if (offset + size > w->size) {
		printf("  mmio %s %7zu: SKIP (crosses TLB boundary)\n", label, size);
		return;
	}

	buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED)
		return;
	for (j = 0; j < nwords; j++)
		buf[j] = 0xAA;

	for (i = 0; i < 3; i++)
		for (j = 0; j < nwords; j++)
			dst[j] = buf[j];

	start = now_ns();
	for (i = 0; i < iterations; i++)
		for (j = 0; j < nwords; j++)
			dst[j] = buf[j];
	elapsed = now_ns() - start;

	total_bytes = (uint64_t)size * iterations;
	mbps = (double)total_bytes / elapsed * 1000.0;

	printf("  mmio %s %7zu: %d ops, %.1f MB/s, %.0f us/op\n",
	       label, size, iterations, mbps,
	       (double)elapsed / iterations / 1000.0);

	munmap(buf, size);
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

	uint8_t dram_x, dram_y;

	if (info.out.device_id == PCI_DEVICE_ID_BLACKHOLE) {
		dram_x = 17; dram_y = 12;
	} else {
		dram_x = 0; dram_y = 0;
	}

	printf("Device: %s (ID 0x%04x), tensix (%u, %u), dram (%u, %u)\n\n",
	       dev_path, info.out.device_id, tensix_x, tensix_y, dram_x, dram_y);

	{
		struct mmio_window uc_win = {}, wc_win = {};
		int have_uc = mmio_window_open(&uc_win, fd, tensix_x, tensix_y, 0x0, 0) == 0;
		int have_wc = mmio_window_open(&wc_win, fd, tensix_x, tensix_y, 0x0, 1) == 0;

		if (have_uc) {
			printf("Direct MMIO baseline (UC, L1 @ 0x4000):\n");
			bench_mmio_read32(&uc_win, 0x4000, 10000);
			bench_mmio_write32(&uc_win, 0x4000, 10000);
			mmio_window_close(&uc_win);
		} else {
			printf("Direct MMIO baseline: SKIP (TLB alloc failed)\n");
		}

		if (have_uc) {
			have_uc = mmio_window_open(&uc_win, fd, tensix_x, tensix_y, 0x0, 0) == 0;
		}
		if (have_uc) {
			#if 0
			printf("\nDirect MMIO block write — UC memcpy (L1 @ 0x10000):\n");
			bench_mmio_block_write(&uc_win, 0x10000, 4096, 1000, "uc_memcpy");
			bench_mmio_block_write(&uc_win, 0x10000, 65536, 200, "uc_memcpy");
			bench_mmio_block_write(&uc_win, 0x10000, 262144, 50, "uc_memcpy");
			bench_mmio_block_write(&uc_win, 0x10000, 1048576, 20, "uc_memcpy");

			printf("\nDirect MMIO block write — UC u32 loop (L1 @ 0x10000):\n");
			bench_mmio_block_write_u32(&uc_win, 0x10000, 4096, 1000, "uc_u32  ");
			bench_mmio_block_write_u32(&uc_win, 0x10000, 65536, 200, "uc_u32  ");
			bench_mmio_block_write_u32(&uc_win, 0x10000, 262144, 50, "uc_u32  ");
			bench_mmio_block_write_u32(&uc_win, 0x10000, 1048576, 20, "uc_u32  ");
			mmio_window_close(&uc_win);
			#endif
		}

		if (have_wc) {
			printf("\nDirect MMIO block write — WC u32 loop (L1 @ 0x10000):\n");
			bench_mmio_block_write_u32(&wc_win, 0x10000, 4096, 1000, "wc_u32  ");
			bench_mmio_block_write_u32(&wc_win, 0x10000, 65536, 200, "wc_u32  ");
			bench_mmio_block_write_u32(&wc_win, 0x10000, 262144, 50, "wc_u32  ");
			bench_mmio_block_write_u32(&wc_win, 0x10000, 1048576, 20, "wc_u32  ");
			mmio_window_close(&wc_win);
		} else {
			printf("\nDirect MMIO block write (WC): SKIP (TLB alloc failed)\n");
		}
	}

	{
		struct mmio_window dram_wc = {};
		int have_dram_wc = mmio_window_open(&dram_wc, fd, dram_x, dram_y, 0x0, 1) == 0;

		if (have_dram_wc) {
			printf("\nDirect MMIO block write — WC u32 loop (DRAM @ 0x0):\n");
			bench_mmio_block_write_u32(&dram_wc, 0x0, 4096, 1000, "wc_u32  ");
			bench_mmio_block_write_u32(&dram_wc, 0x0, 65536, 200, "wc_u32  ");
			bench_mmio_block_write_u32(&dram_wc, 0x0, 262144, 50, "wc_u32  ");
			bench_mmio_block_write_u32(&dram_wc, 0x0, 1048576, 20, "wc_u32  ");
			mmio_window_close(&dram_wc);
		}
	}

	printf("\nioctl 32-bit operations (L1 @ 0x4000):\n");
	bench_read32(fd, tensix_x, tensix_y, 0x4000, 10000);
	bench_write32(fd, tensix_x, tensix_y, 0x4000, 10000);

	printf("\nioctl block write to L1 @ 0x10000:\n");
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 4096, 1000);
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 64 * 1024, 200);
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 256 * 1024, 50);
	bench_block_write(fd, tensix_x, tensix_y, 0x10000, 1024 * 1024, 20);

	printf("\nioctl block write to DRAM @ 0x0:\n");
	bench_block_write(fd, dram_x, dram_y, 0x0, 4096, 1000);
	bench_block_write(fd, dram_x, dram_y, 0x0, 64 * 1024, 200);
	bench_block_write(fd, dram_x, dram_y, 0x0, 256 * 1024, 50);
	bench_block_write(fd, dram_x, dram_y, 0x0, 1024 * 1024, 20);

	printf("\nioctl block read from L1 @ 0x10000:\n");
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 4096, 1000);
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 64 * 1024, 200);
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 256 * 1024, 50);
	bench_block_read(fd, tensix_x, tensix_y, 0x10000, 1024 * 1024, 20);

	close(fd);
	return 0;
}
