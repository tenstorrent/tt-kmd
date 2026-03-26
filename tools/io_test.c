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

	close(fd);
	return ret;
}
