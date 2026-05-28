// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// dmabuf_loopback - exercise TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF without a NIC.
//
// Proves that a CPU store made through a *dma-buf* mapping of a TLB window
// reaches the BAR aperture and routes onto the NOC, by reusing the same
// PCIe-tile loopback that device_read_file/device_write_readonly rely on:
//
//   host buf (pinned, NOC_DMA) <- iATU <- PCIe tile <- NOC <- TLB window
//                                                              ^ dma-buf mmap
//
// Flow:
//   - Open Blackhole.
//   - PIN_PAGES a writable host buffer with NOC_DMA -> noc_address.
//   - ALLOCATE_TLB (2M), EXPORT_TLB_DMABUF -> dma-buf fd.
//   - mmap() the dma-buf fd (the path under test).
//   - CONFIGURE_TLB to aim the window at the PCIe tile at noc_address.
//   - Store a pattern through the dma-buf mapping; read it back through the
//     same mapping (UC read fences the write) and also directly via the host
//     buffer's VA. Both must match the pattern.
//
// This needs an IOMMU-translated Blackhole and no NIC.
//
// To compile:
//   make            # builds via tools/rdma/Makefile
//
// To run:
//   sudo ./dmabuf_loopback            # /dev/tenstorrent/0
//   sudo ./dmabuf_loopback /dev/tenstorrent/1

#define _GNU_SOURCE
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

/* ===== ioctl definitions (subset of tt-kmd ioctl.h) ===== */

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_PIN_PAGES		_IO(TENSTORRENT_IOCTL_MAGIC, 7)
#define TENSTORRENT_IOCTL_UNPIN_PAGES		_IO(TENSTORRENT_IOCTL_MAGIC, 10)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB		_IO(TENSTORRENT_IOCTL_MAGIC, 13)
#define TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF	_IO(TENSTORRENT_IOCTL_MAGIC, 16)

#define TENSTORRENT_PIN_PAGES_NOC_DMA 2

#define BLACKHOLE_PCI_DEVICE_ID 0xb140

struct tenstorrent_get_device_info {
	struct {
		__u32 output_size_bytes;
	} in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
		__u16 bus_dev_fn, max_dma_buf_size_log2, pci_domain;
		__u16 reserved;
	} out;
};

struct tenstorrent_pin_pages {
	struct {
		__u32 output_size_bytes;
		__u32 flags;
		__u64 virtual_address;
		__u64 size;
	} in;
	struct {
		__u64 physical_address;
		__u64 noc_address;
	} out;
};

struct tenstorrent_unpin_pages {
	struct {
		__u64 virtual_address;
		__u64 size;
		__u64 reserved;
	} in;
	struct {
	} out;
};

struct tenstorrent_allocate_tlb {
	struct {
		__u64 size;
		__u64 reserved;
	} in;
	struct {
		__u32 id;
		__u32 reserved0;
		__u64 mmap_offset_uc;
		__u64 mmap_offset_wc;
		__u64 reserved1;
	} out;
};

struct tenstorrent_free_tlb {
	struct {
		__u32 id;
	} in;
	struct {
	} out;
};

struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end, y_end, x_start, y_start;
	__u8 noc, mcast, ordering, linked, static_vc;
	__u8 reserved0[3];
	__u32 reserved1[2];
};

struct tenstorrent_configure_tlb {
	struct {
		__u32 id;
		__u32 reserved;
		struct tenstorrent_noc_tlb_config config;
	} in;
	struct {
		__u64 reserved;
	} out;
};

struct tenstorrent_export_tlb_dmabuf {
	__u32 argsz;
	__u32 flags;
	__u32 tlb_id;
	__s32 fd;
	__u64 offset;
	__u64 size;
};

/* ===== Blackhole constants ===== */

#define TLB_SIZE_2M (2ULL << 20)
#define BH_PCIE_X 19
#define BH_PCIE_Y 24

#define VERIFY_BYTES 4096

/* ===== Helpers ===== */

#define DIE(fmt, ...)                                               \
	do {                                                        \
		fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__); \
		exit(1);                                            \
	} while (0)

static int open_blackhole(const char *path)
{
	struct tenstorrent_get_device_info info;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		DIE("open %s: %s", path, strerror(errno));

	memset(&info, 0, sizeof(info));
	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0)
		DIE("GET_DEVICE_INFO: %s", strerror(errno));

	if (info.out.device_id != BLACKHOLE_PCI_DEVICE_ID)
		DIE("%s is not Blackhole (device_id=0x%04x)", path, info.out.device_id);

	return fd;
}

int main(int argc, char **argv)
{
	const char *dev_path = argc > 1 ? argv[1] : "/dev/tenstorrent/0";
	struct tenstorrent_pin_pages pin = { 0 };
	struct tenstorrent_unpin_pages unpin = { 0 };
	struct tenstorrent_allocate_tlb alloc = { 0 };
	struct tenstorrent_configure_tlb cfg = { 0 };
	struct tenstorrent_free_tlb ft = { 0 };
	struct tenstorrent_export_tlb_dmabuf exp = { 0 };
	long page_size = sysconf(_SC_PAGESIZE);
	size_t host_size;
	uint8_t *host;
	void *mmio;
	volatile uint32_t *window;
	uint32_t pattern[VERIFY_BYTES / 4];
	uint32_t readback[VERIFY_BYTES / 4];
	uint64_t aligned, offset;
	int dev_fd, dmabuf_fd;
	size_t i;

	host_size = (VERIFY_BYTES + page_size - 1) & ~(page_size - 1);

	dev_fd = open_blackhole(dev_path);

	// Writable host buffer, pinned for NOC DMA. The returned noc_address
	// routes back here through the outbound iATU.
	host = mmap(NULL, host_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (host == MAP_FAILED)
		DIE("mmap host buffer: %s", strerror(errno));
	memset(host, 0, host_size);

	pin.in.output_size_bytes = sizeof(pin.out);
	pin.in.flags = TENSTORRENT_PIN_PAGES_NOC_DMA;
	pin.in.virtual_address = (uintptr_t)host;
	pin.in.size = host_size;
	if (ioctl(dev_fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) != 0)
		DIE("PIN_PAGES (NOC_DMA): %s", strerror(errno));

	printf("Device:      %s\n", dev_path);
	printf("Host buffer: %p (%zu bytes), noc_address=0x%016llx\n",
	       (void *)host, host_size, (unsigned long long)pin.out.noc_address);

	// Allocate a 2M window and export it as a dma-buf.
	alloc.in.size = TLB_SIZE_2M;
	if (ioctl(dev_fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
		DIE("ALLOCATE_TLB: %s", strerror(errno));

	exp.argsz = sizeof(exp);
	exp.tlb_id = alloc.out.id;
	exp.offset = 0;
	exp.size = 0; // whole window
	if (ioctl(dev_fd, TENSTORRENT_IOCTL_EXPORT_TLB_DMABUF, &exp) != 0)
		DIE("EXPORT_TLB_DMABUF: %s", strerror(errno));
	dmabuf_fd = exp.fd;
	printf("Exported:    tlb_id=%u -> dma-buf fd=%d\n", alloc.out.id, dmabuf_fd);

	// mmap the dma-buf fd -- this is the path under test.
	mmio = mmap(NULL, TLB_SIZE_2M, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
	if (mmio == MAP_FAILED)
		DIE("mmap dma-buf fd: %s", strerror(errno));

	// Aim the window at the PCIe tile at the pinned buffer's noc_address.
	aligned = pin.out.noc_address & ~(TLB_SIZE_2M - 1);
	cfg.in.id = alloc.out.id;
	cfg.in.config.addr = aligned;
	cfg.in.config.x_end = BH_PCIE_X;
	cfg.in.config.y_end = BH_PCIE_Y;
	if (ioctl(dev_fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cfg) != 0)
		DIE("CONFIGURE_TLB: %s", strerror(errno));

	offset = pin.out.noc_address & (TLB_SIZE_2M - 1);
	window = (volatile uint32_t *)((uint8_t *)mmio + offset);

	for (i = 0; i < VERIFY_BYTES / 4; i++)
		pattern[i] = 0xA5A50000u + (uint32_t)i;

	// Store through the dma-buf mapping: each store is a NOC write to the
	// PCIe tile, forwarded by the iATU into the pinned host buffer.
	for (i = 0; i < VERIFY_BYTES / 4; i++)
		window[i] = pattern[i];

	// Read back through the same mapping. UC reads are non-posted, so this
	// also fences the writes above before we inspect the host buffer.
	for (i = 0; i < VERIFY_BYTES / 4; i++)
		readback[i] = window[i];

	if (memcmp(readback, pattern, VERIFY_BYTES) != 0)
		DIE("FAIL: read-back through dma-buf mapping differs from pattern");
	printf("OK: %d bytes round-tripped through the dma-buf mapping\n", VERIFY_BYTES);

	if (memcmp(host, pattern, VERIFY_BYTES) != 0)
		DIE("FAIL: pattern did not land in the pinned host buffer");
	printf("OK: pattern observed directly in the pinned host buffer\n");

	printf("\nThe dma-buf mmap reaches the BAR aperture and routes onto the NOC.\n");

	munmap(mmio, TLB_SIZE_2M);
	close(dmabuf_fd);

	ft.in.id = alloc.out.id;
	ioctl(dev_fd, TENSTORRENT_IOCTL_FREE_TLB, &ft);

	unpin.in.virtual_address = (uintptr_t)host;
	unpin.in.size = host_size;
	ioctl(dev_fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin);

	munmap(host, host_size);
	close(dev_fd);
	return 0;
}
