// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Exercise the NOC_IO ioctl: NOC sanity test and basic read/write.
//
// To Compile:
//  gcc -O2 -Wall -o io_test io_test.c
//
// To Run:
//  ./io_test /dev/tenstorrent/0

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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
	__u64 data;
	__u64 data_ptr;
	__u64 data_len;
};

#define PCI_DEVICE_ID_BLACKHOLE 0xb140
#define PCI_DEVICE_ID_WORMHOLE  0x401e

static int noc_read32(int fd, uint8_t x, uint8_t y, uint64_t addr, uint32_t *value)
{
	struct tenstorrent_noc_io io = {0};

	io.argsz = sizeof(io);
	io.flags = 0;
	io.x = x;
	io.y = y;
	io.addr = addr;

	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_IO, &io) < 0)
		return -errno;

	*value = (uint32_t)io.data;
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
	io.data = value;

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

static int is_tensix_bh(uint32_t x, uint32_t y)
{
	return (y >= 2 && y <= 11) &&
	       ((x >= 1 && x <= 7) || (x >= 10 && x <= 16));
}

static int is_tensix_wh(uint32_t x, uint32_t y)
{
	return ((y != 6) && (y >= 1) && (y <= 11)) &&
	       ((x != 5) && (x >= 1) && (x <= 9));
}

static int blackhole_noc_sanity(int fd)
{
	static const uint64_t NOC_NODE_ID_LOGICAL = 0xffb20148ULL;
	uint32_t x, y;
	int tiles_checked = 0;

	for (x = 0; x < 17; x++) {
		for (y = 0; y < 12; y++) {
			uint32_t node_id = 0, node_id_x, node_id_y;
			int ret;

			if (!is_tensix_bh(x, y))
				continue;

			ret = noc_read32(fd, x, y, NOC_NODE_ID_LOGICAL, &node_id);
			if (ret) {
				fprintf(stderr, "noc_read32(%u, %u) failed: %s\n",
					x, y, strerror(-ret));
				return -1;
			}

			node_id_x = (node_id >> 0) & 0x3f;
			node_id_y = (node_id >> 6) & 0x3f;

			if (node_id_x != x || node_id_y != y) {
				fprintf(stderr, "FAIL: tile (%u, %u) reports "
					"node ID (%u, %u)\n",
					x, y, node_id_x, node_id_y);
				return -1;
			}

			tiles_checked++;
		}
	}

	printf("Blackhole NOC sanity: PASS (%d tiles)\n", tiles_checked);
	return 0;
}

static int wormhole_noc_sanity(int fd)
{
	static const uint64_t ARC_NOC_NODE_ID = 0xFFFB2002CULL;
	static const uint64_t DDR_NOC_NODE_ID = 0x10009002CULL;
	static const uint64_t TENSIX_NOC_NODE_ID = 0xffb2002cULL;
	uint32_t node_id, node_id_x, node_id_y;
	uint32_t x, y;
	int tiles_checked = 0;
	int ret;

	ret = noc_read32(fd, 0, 10, ARC_NOC_NODE_ID, &node_id);
	if (ret) {
		fprintf(stderr, "ARC noc_read32 failed: %s\n", strerror(-ret));
		return -1;
	}
	node_id_x = (node_id >> 0) & 0x3f;
	node_id_y = (node_id >> 6) & 0x3f;
	if (node_id_x != 0 || node_id_y != 10) {
		fprintf(stderr, "FAIL: ARC (0, 10) reports (%u, %u)\n",
			node_id_x, node_id_y);
		return -1;
	}
	tiles_checked++;

	ret = noc_read32(fd, 0, 11, DDR_NOC_NODE_ID, &node_id);
	if (ret) {
		fprintf(stderr, "DDR noc_read32 failed: %s\n", strerror(-ret));
		return -1;
	}
	node_id_x = (node_id >> 0) & 0x3f;
	node_id_y = (node_id >> 6) & 0x3f;
	if (node_id_x != 0 || node_id_y != 11) {
		fprintf(stderr, "FAIL: DDR (0, 11) reports (%u, %u)\n",
			node_id_x, node_id_y);
		return -1;
	}
	tiles_checked++;

	for (x = 0; x < 12; x++) {
		for (y = 0; y < 12; y++) {
			if (!is_tensix_wh(x, y))
				continue;

			ret = noc_read32(fd, x, y, TENSIX_NOC_NODE_ID, &node_id);
			if (ret) {
				fprintf(stderr, "noc_read32(%u, %u) failed: %s\n",
					x, y, strerror(-ret));
				return -1;
			}

			node_id_x = (node_id >> 0) & 0x3f;
			node_id_y = (node_id >> 6) & 0x3f;

			if (node_id_x != x || node_id_y != y) {
				fprintf(stderr, "FAIL: tile (%u, %u) reports "
					"node ID (%u, %u)\n",
					x, y, node_id_x, node_id_y);
				return -1;
			}

			tiles_checked++;
		}
	}

	printf("Wormhole NOC sanity: PASS (%d tiles)\n", tiles_checked);
	return 0;
}

static int noc_sanity(int fd, uint16_t device_id)
{
	if (device_id == PCI_DEVICE_ID_BLACKHOLE)
		return blackhole_noc_sanity(fd);
	else if (device_id == PCI_DEVICE_ID_WORMHOLE)
		return wormhole_noc_sanity(fd);

	fprintf(stderr, "Unknown device ID 0x%04x\n", device_id);
	return -1;
}

/*
 * L1 write/readback test.
 *
 * Write a pattern to Tensix L1 memory, read it back, verify. Uses a
 * single tile that was already validated by the NOC sanity test. The
 * L1 offset is chosen to avoid the bottom of the address space where
 * firmware-reserved structures may live.
 *
 * Assumes NOC coordinate translation is enabled (normal production
 * configuration). The translated coordinate (1, 2) maps to a Tensix
 * tile present on all Blackhole SKUs (p100 and p150).
 */
#define L1_TEST_OFFSET  0x4000
#define L1_TEST_WORDS   256
#define L1_TEST_SEED    0xCAFEBABE

static int l1_write_readback(int fd, uint8_t x, uint8_t y)
{
	uint32_t written[L1_TEST_WORDS];
	uint32_t readback;
	uint32_t i;
	int ret;

	for (i = 0; i < L1_TEST_WORDS; i++)
		written[i] = L1_TEST_SEED ^ i;

	for (i = 0; i < L1_TEST_WORDS; i++) {
		uint64_t addr = L1_TEST_OFFSET + i * sizeof(uint32_t);

		ret = noc_write32(fd, x, y, addr, written[i]);
		if (ret) {
			fprintf(stderr, "L1 write failed at offset 0x%lx: %s\n",
				(unsigned long)addr, strerror(-ret));
			return -1;
		}
	}

	for (i = 0; i < L1_TEST_WORDS; i++) {
		uint64_t addr = L1_TEST_OFFSET + i * sizeof(uint32_t);

		ret = noc_read32(fd, x, y, addr, &readback);
		if (ret) {
			fprintf(stderr, "L1 read failed at offset 0x%lx: %s\n",
				(unsigned long)addr, strerror(-ret));
			return -1;
		}

		if (readback != written[i]) {
			fprintf(stderr, "FAIL: L1[0x%lx] = 0x%08x, "
				"expected 0x%08x\n",
				(unsigned long)addr, readback, written[i]);
			return -1;
		}
	}

	printf("L1 write/readback (%u, %u): PASS (%u words at 0x%x)\n",
	       x, y, L1_TEST_WORDS, L1_TEST_OFFSET);
	return 0;
}

static int l1_test(int fd, uint16_t device_id)
{
	if (device_id == PCI_DEVICE_ID_BLACKHOLE)
		return l1_write_readback(fd, 1, 2);
	else if (device_id == PCI_DEVICE_ID_WORMHOLE)
		return l1_write_readback(fd, 1, 1);

	fprintf(stderr, "Unknown device ID 0x%04x for L1 test\n", device_id);
	return -1;
}

#define L1_BLOCK_TEST_OFFSET 0x10000
#define L1_BLOCK_TEST_SIZE   (64 * 1024)
#define L1_BLOCK_TEST_SEED   0xDEAD0000

static int l1_block_write_readback(int fd, uint8_t x, uint8_t y)
{
	size_t nwords = L1_BLOCK_TEST_SIZE / sizeof(uint32_t);
	uint32_t *write_buf = NULL;
	uint32_t *read_buf = NULL;
	size_t i;
	int ret;

	write_buf = mmap(NULL, L1_BLOCK_TEST_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	read_buf = mmap(NULL, L1_BLOCK_TEST_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (write_buf == MAP_FAILED || read_buf == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %s\n", strerror(errno));
		ret = -1;
		goto out;
	}

	for (i = 0; i < nwords; i++)
		write_buf[i] = L1_BLOCK_TEST_SEED ^ (uint32_t)i;

	ret = noc_block_write(fd, x, y, L1_BLOCK_TEST_OFFSET, write_buf, L1_BLOCK_TEST_SIZE);
	if (ret) {
		fprintf(stderr, "L1 block write failed: %s\n", strerror(-ret));
		goto out;
	}

	memset(read_buf, 0, L1_BLOCK_TEST_SIZE);

	ret = noc_block_read(fd, x, y, L1_BLOCK_TEST_OFFSET, read_buf, L1_BLOCK_TEST_SIZE);
	if (ret) {
		fprintf(stderr, "L1 block read failed: %s\n", strerror(-ret));
		goto out;
	}

	for (i = 0; i < nwords; i++) {
		if (read_buf[i] != write_buf[i]) {
			fprintf(stderr, "FAIL: L1 block [0x%lx] = 0x%08x, expected 0x%08x\n",
				(unsigned long)(L1_BLOCK_TEST_OFFSET + i * sizeof(uint32_t)),
				read_buf[i], write_buf[i]);
			ret = -1;
			goto out;
		}
	}

	printf("L1 block write/readback (%u, %u): PASS (%u KiB at 0x%x)\n",
	       x, y, L1_BLOCK_TEST_SIZE / 1024, L1_BLOCK_TEST_OFFSET);
	ret = 0;

out:
	if (write_buf != MAP_FAILED)
		munmap(write_buf, L1_BLOCK_TEST_SIZE);
	if (read_buf != MAP_FAILED)
		munmap(read_buf, L1_BLOCK_TEST_SIZE);
	return ret;
}

static int l1_block_test(int fd, uint16_t device_id)
{
	if (device_id == PCI_DEVICE_ID_BLACKHOLE)
		return l1_block_write_readback(fd, 1, 2);
	else if (device_id == PCI_DEVICE_ID_WORMHOLE)
		return l1_block_write_readback(fd, 1, 1);

	fprintf(stderr, "Unknown device ID 0x%04x for L1 block test\n", device_id);
	return -1;
}

int main(int argc, char *argv[])
{
	struct tenstorrent_get_device_info info;
	const char *dev_path;
	int fd, ret = 0;

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

	printf("Device: %s (PCI %04x:%02x:%02x.%x, ID 0x%04x)\n",
	       dev_path,
	       info.out.pci_domain,
	       (info.out.bus_dev_fn >> 8) & 0xff,
	       (info.out.bus_dev_fn >> 3) & 0x1f,
	       info.out.bus_dev_fn & 0x07,
	       info.out.device_id);

	if (noc_sanity(fd, info.out.device_id) != 0)
		ret = 1;

	if (ret == 0 && l1_test(fd, info.out.device_id) != 0)
		ret = 1;

	if (ret == 0 && l1_block_test(fd, info.out.device_id) != 0)
		ret = 1;

	close(fd);
	return ret;
}
