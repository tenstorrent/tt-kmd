// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Multi-threaded NOC I/O benchmark.
//
// Discovers unharvested Tensix tiles and measures ioctl read32
// throughput with varying thread counts.  With the current
// single-mutex kernel TLB, aggregate throughput should be flat
// regardless of thread count.
//
// To Compile:
//  gcc -O2 -Wall -pthread -o io_mt_bench io_mt_bench.c
//
// To Run:
//  ./io_mt_bench /dev/tenstorrent/0

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

#define NOC_ID_LOGICAL      0xFFB20148ULL
#define NIU_CFG_0_ADDR      0xFFB20100ULL
#define NIU_CFG_0_HARVESTED (1u << 12)

#define MAX_TILES 140
#define MAX_THREADS 32

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

struct tile {
	uint8_t x;
	uint8_t y;
};

static int is_tensix_coord(uint32_t x, uint32_t y)
{
	return (y >= 2 && y <= 11) &&
	       ((x >= 1 && x <= 7) || (x >= 10 && x <= 16));
}

static int discover_tiles(int fd, struct tile *tiles, int max_tiles)
{
	int count = 0;
	uint32_t x, y;

	for (y = 2; y <= 11 && count < max_tiles; y++) {
		for (x = 1; x <= 16 && count < max_tiles; x++) {
			uint32_t node_id, niu_cfg;
			uint32_t nid_x, nid_y;

			if (!is_tensix_coord(x, y))
				continue;

			if (noc_read32(fd, x, y, NOC_ID_LOGICAL, &node_id) < 0)
				continue;

			nid_x = node_id & 0x3f;
			nid_y = (node_id >> 6) & 0x3f;
			if (nid_x != x || nid_y != y)
				continue;

			if (noc_read32(fd, x, y, NIU_CFG_0_ADDR, &niu_cfg) < 0)
				continue;

			if (niu_cfg & NIU_CFG_0_HARVESTED)
				continue;

			tiles[count].x = x;
			tiles[count].y = y;
			count++;
		}
	}

	return count;
}

struct bench_thread_arg {
	int fd;
	uint8_t x;
	uint8_t y;
	uint64_t addr;
	int iterations;
	int do_write;
	atomic_int *barrier;
	uint64_t elapsed_ns;
};

static void *bench_thread(void *arg)
{
	struct bench_thread_arg *a = arg;
	uint32_t dummy;
	uint64_t start;
	int i;

	if (a->do_write) {
		for (i = 0; i < 100; i++)
			noc_write32(a->fd, a->x, a->y, a->addr, 0);
	} else {
		for (i = 0; i < 100; i++)
			noc_read32(a->fd, a->x, a->y, a->addr, &dummy);
	}

	atomic_fetch_sub(a->barrier, 1);
	while (atomic_load(a->barrier) > 0)
		;

	start = now_ns();
	if (a->do_write) {
		for (i = 0; i < a->iterations; i++)
			noc_write32(a->fd, a->x, a->y, a->addr, (uint32_t)i);
	} else {
		for (i = 0; i < a->iterations; i++)
			noc_read32(a->fd, a->x, a->y, a->addr, &dummy);
	}
	a->elapsed_ns = now_ns() - start;

	return NULL;
}

static void run_bench(int fd, const char *label, int nthreads,
		      struct tile *tiles, int ntiles,
		      int same_tile, uint64_t addr, int do_write,
		      int iterations)
{
	pthread_t threads[MAX_THREADS];
	struct bench_thread_arg args[MAX_THREADS];
	atomic_int barrier;
	uint64_t max_elapsed = 0;
	uint64_t sum_elapsed = 0;
	double agg_ops_sec;
	int i;

	if (nthreads > MAX_THREADS)
		nthreads = MAX_THREADS;

	atomic_init(&barrier, nthreads);

	for (i = 0; i < nthreads; i++) {
		int tile_idx = same_tile ? 0 : (i % ntiles);
		args[i].fd = fd;
		args[i].x = tiles[tile_idx].x;
		args[i].y = tiles[tile_idx].y;
		args[i].addr = addr;
		args[i].iterations = iterations;
		args[i].do_write = do_write;
		args[i].barrier = &barrier;
		args[i].elapsed_ns = 0;
	}

	for (i = 0; i < nthreads; i++)
		pthread_create(&threads[i], NULL, bench_thread, &args[i]);

	for (i = 0; i < nthreads; i++)
		pthread_join(threads[i], NULL);

	for (i = 0; i < nthreads; i++) {
		if (args[i].elapsed_ns > max_elapsed)
			max_elapsed = args[i].elapsed_ns;
		sum_elapsed += args[i].elapsed_ns;
	}

	agg_ops_sec = (double)nthreads * iterations / max_elapsed * 1e9;

	printf("  %-14s %2d thr: %7.0f ops/sec  %5.0f ns/op (thread avg)  "
	       "%5.0f ns/op (wall)\n",
	       label, nthreads, agg_ops_sec,
	       (double)sum_elapsed / nthreads / iterations,
	       (double)max_elapsed / iterations);
}

int main(int argc, char *argv[])
{
	struct tenstorrent_get_device_info info;
	struct tile tiles[MAX_TILES];
	const char *dev_path;
	int fd, ntiles;
	int iterations = 20000;
	int thread_counts[] = { 1, 2, 4, 8 };
	int num_thread_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);
	int i;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s /dev/tenstorrent/N [iterations]\n", argv[0]);
		return 1;
	}
	dev_path = argv[1];

	if (argc == 3)
		iterations = atoi(argv[2]);

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

	if (info.out.device_id != PCI_DEVICE_ID_BLACKHOLE) {
		fprintf(stderr, "This benchmark is Blackhole-only (got device 0x%04x)\n",
			info.out.device_id);
		close(fd);
		return 1;
	}

	printf("Device: %s (PCI %04x:%02x:%02x.%x)\n\n",
	       dev_path,
	       info.out.pci_domain,
	       (info.out.bus_dev_fn >> 8) & 0xff,
	       (info.out.bus_dev_fn >> 3) & 0x1f,
	       info.out.bus_dev_fn & 0x07);

	ntiles = discover_tiles(fd, tiles, MAX_TILES);
	printf("Discovered %d unharvested Tensix tiles\n", ntiles);
	if (ntiles == 0) {
		fprintf(stderr, "No unharvested Tensix tiles found\n");
		close(fd);
		return 1;
	}

	printf("  first: (%u, %u)  last: (%u, %u)\n\n",
	       tiles[0].x, tiles[0].y,
	       tiles[ntiles - 1].x, tiles[ntiles - 1].y);

	printf("Baseline: single-thread read32 (NOC_ID_LOGICAL), %d iterations\n", iterations);
	{
		uint32_t dummy;
		uint64_t start, elapsed;

		for (i = 0; i < 100; i++)
			noc_read32(fd, tiles[0].x, tiles[0].y, NOC_ID_LOGICAL, &dummy);

		start = now_ns();
		for (i = 0; i < iterations; i++)
			noc_read32(fd, tiles[0].x, tiles[0].y, NOC_ID_LOGICAL, &dummy);
		elapsed = now_ns() - start;

		printf("  %.0f ns/op, %.0f ops/sec\n\n",
		       (double)elapsed / iterations,
		       (double)iterations / elapsed * 1e9);
	}

	printf("Same-tile read32: all threads read tile (%u, %u)\n",
	       tiles[0].x, tiles[0].y);
	for (i = 0; i < num_thread_counts; i++)
		run_bench(fd, "same-tile", thread_counts[i],
			  tiles, ntiles, 1, NOC_ID_LOGICAL, 0, iterations);

	printf("\nMulti-tile read32: each thread reads a distinct tile\n");
	for (i = 0; i < num_thread_counts; i++)
		run_bench(fd, "multi-tile", thread_counts[i],
			  tiles, ntiles, 0, NOC_ID_LOGICAL, 0, iterations);

	printf("\nSame-tile write32: all threads write L1 @ 0x4000 on tile (%u, %u)\n",
	       tiles[0].x, tiles[0].y);
	for (i = 0; i < num_thread_counts; i++)
		run_bench(fd, "same-tile", thread_counts[i],
			  tiles, ntiles, 1, 0x4000, 1, iterations);

	printf("\nMulti-tile write32: each thread writes L1 @ 0x4000 on its own tile\n");
	for (i = 0; i < num_thread_counts; i++)
		run_bench(fd, "multi-tile", thread_counts[i],
			  tiles, ntiles, 0, 0x4000, 1, iterations);

	close(fd);
	return 0;
}
