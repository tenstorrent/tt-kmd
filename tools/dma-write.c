// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Host-to-device DMA on Blackhole: the smallest complete use of
// TENSTORRENT_URING_CMD_DMA_WRITE.
//
// It pins a buffer, submits one DMA_WRITE through io_uring, waits for the
// CQE, and reads the data back from the device with NOC_READ.
//
// Needs Linux 6.6 or newer, and firmware that leaves HDMA channel 1 to the
// host; otherwise the CQE says EOPNOTSUPP.
//
// To Compile (liburing 2.2 or newer):
//  gcc -Wall -o dma-write dma-write.c -luring
//
// To Run:
//  ./dma-write <device> <x> <y> <address> [size]
//
// For example, to write 4 KiB to DRAM at (17, 12), as a Blackhole with NOC
// translation names it:
//  ./dma-write /dev/tenstorrent/0 17 12 0x10000000 4096
//
// [address, address + size) must not cross a 2 MiB boundary: the driver
// doesn't split requests. size must be a multiple of 4, for the read-back.

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <liburing.h>

#include "../ioctl.h"

#define FATAL(fmt, ...) do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); exit(1); } while (0)

static void submit_dma_write(struct io_uring *ring, int fd, const struct tenstorrent_dma_desc *descs, uint32_t count)
{
	struct tenstorrent_dma_write args = {
		.descs = (uintptr_t)descs,
		.count = count,
		.flags = 0,
	};
	uint32_t cmd_op = TENSTORRENT_URING_CMD_DMA_WRITE;
	struct io_uring_sqe *sqe = io_uring_get_sqe(ring);

	if (!sqe)
		FATAL("io_uring_get_sqe: submission queue full");

	// An IORING_OP_URING_CMD SQE carries the command in cmd_op and its
	// payload in the command area. liburing 2.2 doesn't name those fields:
	// cmd_op is the first four bytes of off, and the command area starts
	// at addr3.
	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, NULL, 0, 0);
	memcpy((uint8_t *)sqe + offsetof(struct io_uring_sqe, off), &cmd_op, sizeof(cmd_op));
	memcpy((uint8_t *)sqe + offsetof(struct io_uring_sqe, addr3), &args, sizeof(args));

	if (io_uring_submit(ring) != 1)
		FATAL("io_uring_submit failed");
}

static uint32_t noc_read32(int fd, uint16_t x, uint16_t y, uint64_t addr)
{
	struct tenstorrent_noc_io io = {
		.argsz = sizeof(io),
		.x = x,
		.y = y,
		.width = 4,
		.addr = addr,
	};

	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &io) != 0)
		FATAL("NOC_READ at 0x%llx: %s", (unsigned long long)addr, strerror(errno));

	return (uint32_t)io.value;
}

int main(int argc, char *argv[])
{
	struct tenstorrent_pin_pages pin = { 0 };
	struct tenstorrent_unpin_pages unpin = { 0 };
	struct tenstorrent_dma_desc desc = { 0 };
	struct io_uring ring;
	struct io_uring_cqe *cqe;
	uint16_t x, y;
	uint64_t addr;
	uint32_t size = 4096;
	size_t page = sysconf(_SC_PAGESIZE);
	size_t alloc_size;
	uint32_t *buf;
	uint32_t i;
	int fd;
	int r;

	if (argc < 5 || argc > 6)
		FATAL("Usage: %s <device> <x> <y> <address> [size]", argv[0]);

	x = strtoul(argv[2], NULL, 0);
	y = strtoul(argv[3], NULL, 0);
	addr = strtoull(argv[4], NULL, 0);
	if (argc == 6)
		size = strtoul(argv[5], NULL, 0);

	if (size == 0 || size % 4 != 0)
		FATAL("size must be a non-zero multiple of 4");

	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0)
		FATAL("open %s: %s", argv[1], strerror(errno));

	// The source: pinned host memory, which the device reaches by IOVA.
	alloc_size = (size + page - 1) / page * page;
	buf = aligned_alloc(page, alloc_size);
	if (!buf)
		FATAL("aligned_alloc failed");

	for (i = 0; i < size / 4; i++)
		buf[i] = 0xD0A00000 ^ (i * 2654435761u);

	pin.in.output_size_bytes = sizeof(pin.out);
	pin.in.virtual_address = (uintptr_t)buf;
	pin.in.size = alloc_size;
	if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0)
		FATAL("PIN_PAGES: %s", strerror(errno));

	r = io_uring_queue_init(8, &ring, 0);
	if (r < 0)
		FATAL("io_uring_queue_init: %s", strerror(-r));

	// One descriptor: all of the buffer to (x, y, addr) on NOC 0. The
	// descriptor must stay put until the CQE arrives.
	desc.src = pin.out.physical_address;
	desc.addr = addr;
	desc.size = size;
	desc.x = x;
	desc.y = y;
	desc.noc = 0;

	submit_dma_write(&ring, fd, &desc, 1);

	r = io_uring_wait_cqe(&ring, &cqe);
	if (r < 0)
		FATAL("io_uring_wait_cqe: %s", strerror(-r));

	r = cqe->res;
	io_uring_cqe_seen(&ring, cqe);
	if (r < 0)
		FATAL("DMA_WRITE: %s", strerror(-r));

	// A successful CQE means the engine has issued every write. Read them
	// back.
	for (i = 0; i < size / 4; i++) {
		uint32_t got = noc_read32(fd, x, y, addr + i * 4);

		if (got != buf[i])
			FATAL("Mismatch at 0x%llx: wrote 0x%08x, read 0x%08x",
			      (unsigned long long)(addr + i * 4), buf[i], got);
	}

	printf("Wrote %u bytes to (%u, %u) 0x%llx by DMA and read them back.\n",
	       size, x, y, (unsigned long long)addr);

	io_uring_queue_exit(&ring);

	// The pinning would also go with the fd.
	unpin.in.virtual_address = (uintptr_t)buf;
	unpin.in.size = alloc_size;
	if (ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin) != 0)
		FATAL("UNPIN_PAGES: %s", strerror(errno));

	close(fd);
	free(buf);
	return 0;
}
