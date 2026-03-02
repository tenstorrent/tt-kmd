// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// PCIe Link Recovery Tool
//
// Attempts to recover degraded (Gen1) links using userspace config space
// writes to the upstream bridge.
//
// Methods:
//   A: Set TLS to max, trigger retrain via RL bit (speed-stepping loop)
//   B: (not yet implemented) PerformEqu + retrain
//   C: (not yet implemented) Secondary Bus Reset
//   D: (not yet implemented) IPMI assert/deassert
//
// Compile: gcc -o recover recover.c
// Run:     sudo ./recover A           # Method A, no functional verify
//          sudo ./recover A --verify  # Method A + NOC/DMA check after

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

// --- Driver UAPI ---

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_PIN_PAGES       _IO(TENSTORRENT_IOCTL_MAGIC, 7)
#define TENSTORRENT_IOCTL_UNPIN_PAGES     _IO(TENSTORRENT_IOCTL_MAGIC, 10)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB    _IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB        _IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB   _IO(TENSTORRENT_IOCTL_MAGIC, 13)
#define TENSTORRENT_PIN_PAGES_NOC_DMA 2

struct tenstorrent_get_device_info {
	struct { __u32 output_size_bytes; } in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id; __u16 device_id;
		__u16 subsystem_vendor_id; __u16 subsystem_id;
		__u16 bus_dev_fn; __u16 max_dma_buf_size_log2;
		__u16 pci_domain; __u16 reserved;
	} out;
};

struct tenstorrent_allocate_tlb {
	struct { __u64 size; __u64 reserved; } in;
	struct { __u32 id; __u32 reserved0; __u64 mmap_offset_uc; __u64 mmap_offset_wc; __u64 reserved1; } out;
};

struct tenstorrent_free_tlb { struct { __u32 id; } in; };

struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end; __u16 y_end; __u16 x_start; __u16 y_start;
	__u8 noc; __u8 mcast; __u8 ordering; __u8 linked; __u8 static_vc;
	__u8 reserved0[3]; __u32 reserved1[2];
};

struct tenstorrent_configure_tlb {
	struct { __u32 id; __u32 reserved; struct tenstorrent_noc_tlb_config config; } in;
	struct { __u64 reserved; } out;
};

struct tenstorrent_pin_pages {
	struct { __u32 output_size_bytes; __u32 flags; __u64 virtual_address; __u64 size; } in;
	struct { __u64 physical_address; __u64 noc_address; } out;
};

struct tenstorrent_unpin_pages {
	struct { __u64 virtual_address; __u64 size; __u64 reserved; } in;
};

// --- PCI config space ---

static int open_config(const char *bdf, int flags)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	return open(path, flags);
}

static int cfg_read16(int fd, int off, uint16_t *val)
{
	return pread(fd, val, 2, off) == 2 ? 0 : -1;
}

static int cfg_read32(int fd, int off, uint32_t *val)
{
	return pread(fd, val, 4, off) == 4 ? 0 : -1;
}

static int cfg_write16(int fd, int off, uint16_t val)
{
	return pwrite(fd, &val, 2, off) == 2 ? 0 : -1;
}

static int cfg_write32(int fd, int off, uint32_t val)
{
	return pwrite(fd, &val, 4, off) == 4 ? 0 : -1;
}

static int find_pcie_cap(int cfg_fd)
{
	uint8_t ptr;
	if (pread(cfg_fd, &ptr, 1, 0x34) != 1)
		return -1;
	while (ptr) {
		uint8_t id;
		if (pread(cfg_fd, &id, 1, ptr) != 1)
			return -1;
		if (id == 0x10)
			return ptr;
		if (pread(cfg_fd, &ptr, 1, ptr + 1) != 1)
			return -1;
	}
	return -1;
}

static int find_ext_cap(int cfg_fd, uint16_t cap_id)
{
	uint32_t header;
	int offset = 0x100;
	while (offset) {
		if (pread(cfg_fd, &header, 4, offset) != 4)
			return -1;
		if (header == 0 || header == 0xFFFFFFFF)
			return -1;
		if ((header & 0xFFFF) == cap_id)
			return offset;
		offset = (header >> 20) & 0xFFC;
	}
	return -1;
}

#define EXT_CAP_SECPCI  0x0019
#define SECPCI_LNKCTL3  0x04
#define LNKCTL3_PERFORM_EQU 0x01

static void set_perform_equ(int bridge_cfg, bool enable)
{
	int cap = find_ext_cap(bridge_cfg, EXT_CAP_SECPCI);
	if (cap < 0)
		return;
	uint32_t lnkctl3;
	cfg_read32(bridge_cfg, cap + SECPCI_LNKCTL3, &lnkctl3);
	if (enable)
		lnkctl3 |= LNKCTL3_PERFORM_EQU;
	else
		lnkctl3 &= ~LNKCTL3_PERFORM_EQU;
	cfg_write32(bridge_cfg, cap + SECPCI_LNKCTL3, lnkctl3);
}

// PCIe cap register offsets
#define PCIE_LNKCAP   0x0C
#define PCIE_LNKCTL   0x10
#define PCIE_LNKSTA   0x12
#define PCIE_LNKCTL2  0x30
#define PCIE_LNKSTA2  0x32

#define LNKSTA_LBMS   (1 << 12)
#define LNKCTL_RL     (1 << 5)

// --- Method A: TLS + Retrain Link ---

#define METHOD_A_MAX_ATTEMPTS 10
#define METHOD_A_LBMS_TIMEOUT_MS 1000
#define DEVICE_ACCESSIBLE_TIMEOUT_MS 500

struct recover_result {
	int before_speed;
	int after_speed;
	int target_speed;
	int attempts;
	bool timeout;
};

static bool wait_for_lbms(int bridge_cfg, int pcie_cap, int timeout_ms)
{
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		uint16_t lnksta;
		cfg_read16(bridge_cfg, pcie_cap + PCIE_LNKSTA, &lnksta);
		if (lnksta & LNKSTA_LBMS)
			return true;

		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
				  (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed_ms >= timeout_ms)
			return false;

		usleep(1000);
	}
}

static bool wait_for_device(int dev_cfg, int timeout_ms)
{
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);

	for (;;) {
		uint16_t vendor;
		if (pread(dev_cfg, &vendor, 2, 0) == 2 && vendor == 0x1e52)
			return true;

		clock_gettime(CLOCK_MONOTONIC, &now);
		long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
				  (now.tv_nsec - start.tv_nsec) / 1000000;
		if (elapsed_ms >= timeout_ms)
			return false;

		usleep(1000);
	}
}

static void method_a(const char *bridge_bdf, const char *dev_bdf,
		     struct recover_result *res)
{
	memset(res, 0, sizeof(*res));

	int br_fd = open_config(bridge_bdf, O_RDWR);
	int dev_fd = open_config(dev_bdf, O_RDONLY);
	if (br_fd < 0 || dev_fd < 0)
		goto out;

	int br_cap = find_pcie_cap(br_fd);
	int dev_cap = find_pcie_cap(dev_fd);
	if (br_cap < 0 || dev_cap < 0)
		goto out;

	uint32_t br_lnkcap, dev_lnkcap;
	cfg_read32(br_fd, br_cap + PCIE_LNKCAP, &br_lnkcap);
	cfg_read32(dev_fd, dev_cap + PCIE_LNKCAP, &dev_lnkcap);

	int br_max = br_lnkcap & 0xF;
	int dev_max = dev_lnkcap & 0xF;
	res->target_speed = (br_max < dev_max) ? br_max : dev_max;

	uint16_t lnksta;
	cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
	res->before_speed = lnksta & 0xF;

	if (res->before_speed >= res->target_speed) {
		res->after_speed = res->before_speed;
		goto out;
	}

	// Set TLS to target speed
	uint16_t lnkctl2;
	cfg_read16(br_fd, br_cap + PCIE_LNKCTL2, &lnkctl2);
	lnkctl2 = (lnkctl2 & ~0xF) | (res->target_speed & 0xF);
	cfg_write16(br_fd, br_cap + PCIE_LNKCTL2, lnkctl2);

	int cur_speed = res->before_speed;

	for (int i = 0; i < METHOD_A_MAX_ATTEMPTS; i++) {
		res->attempts = i + 1;

		// Clear LBMS (RW1C)
		cfg_write16(br_fd, br_cap + PCIE_LNKSTA, LNKSTA_LBMS);

		// Trigger retrain: set RL bit in LnkCtl
		uint16_t lnkctl;
		cfg_read16(br_fd, br_cap + PCIE_LNKCTL, &lnkctl);
		cfg_write16(br_fd, br_cap + PCIE_LNKCTL, lnkctl | LNKCTL_RL);

		// Wait for LBMS (retrain complete)
		if (!wait_for_lbms(br_fd, br_cap, METHOD_A_LBMS_TIMEOUT_MS)) {
			res->timeout = true;
			break;
		}

		cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
		cur_speed = lnksta & 0xF;

		if (cur_speed >= res->target_speed)
			break;
	}

	// Clear LBMS on bridge and device
	cfg_write16(br_fd, br_cap + PCIE_LNKSTA, LNKSTA_LBMS);
	int dev_pcie_cap = find_pcie_cap(dev_fd);
	if (dev_pcie_cap >= 0) {
		// Need write access to device for clearing LBMS
		int dev_wr = open_config(dev_bdf, O_RDWR);
		if (dev_wr >= 0) {
			cfg_write16(dev_wr, dev_pcie_cap + PCIE_LNKSTA,
				    LNKSTA_LBMS);
			close(dev_wr);
		}
	}

	// Wait for device to be accessible
	wait_for_device(dev_fd, DEVICE_ACCESSIBLE_TIMEOUT_MS);

	// Final speed read
	cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
	res->after_speed = lnksta & 0xF;

out:
	if (br_fd >= 0) close(br_fd);
	if (dev_fd >= 0) close(dev_fd);
}

// --- Method C: TLS + Retrain with settle delay ---

#define METHOD_C_MAX_ATTEMPTS 20
#define METHOD_C_SETTLE_MS 500

static void method_c(const char *bridge_bdf, const char *dev_bdf,
		     struct recover_result *res)
{
	memset(res, 0, sizeof(*res));

	int br_fd = open_config(bridge_bdf, O_RDWR);
	int dev_fd = open_config(dev_bdf, O_RDONLY);
	if (br_fd < 0 || dev_fd < 0)
		goto out;

	int br_cap = find_pcie_cap(br_fd);
	int dev_cap = find_pcie_cap(dev_fd);
	if (br_cap < 0 || dev_cap < 0)
		goto out;

	uint32_t br_lnkcap, dev_lnkcap;
	cfg_read32(br_fd, br_cap + PCIE_LNKCAP, &br_lnkcap);
	cfg_read32(dev_fd, dev_cap + PCIE_LNKCAP, &dev_lnkcap);

	int br_max = br_lnkcap & 0xF;
	int dev_max = dev_lnkcap & 0xF;
	res->target_speed = (br_max < dev_max) ? br_max : dev_max;

	uint16_t lnksta;
	cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
	res->before_speed = lnksta & 0xF;

	if (res->before_speed >= res->target_speed) {
		res->after_speed = res->before_speed;
		goto out;
	}

	uint16_t lnkctl2;
	cfg_read16(br_fd, br_cap + PCIE_LNKCTL2, &lnkctl2);
	lnkctl2 = (lnkctl2 & ~0xF) | (res->target_speed & 0xF);
	cfg_write16(br_fd, br_cap + PCIE_LNKCTL2, lnkctl2);

	int cur_speed = res->before_speed;
	int prev_speed = cur_speed;
	bool settled = false;

	for (int i = 0; i < METHOD_C_MAX_ATTEMPTS; i++) {
		res->attempts = i + 1;

		cfg_write16(br_fd, br_cap + PCIE_LNKSTA, LNKSTA_LBMS);

		uint16_t lnkctl;
		cfg_read16(br_fd, br_cap + PCIE_LNKCTL, &lnkctl);
		cfg_write16(br_fd, br_cap + PCIE_LNKCTL, lnkctl | LNKCTL_RL);

		if (!wait_for_lbms(br_fd, br_cap, METHOD_A_LBMS_TIMEOUT_MS)) {
			res->timeout = true;
			break;
		}

		cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
		cur_speed = lnksta & 0xF;

		if (cur_speed >= res->target_speed)
			break;

		if (cur_speed == prev_speed && cur_speed > 1 && !settled) {
			usleep(METHOD_C_SETTLE_MS * 1000);
			settled = true;
		}
		prev_speed = cur_speed;
	}

	cfg_write16(br_fd, br_cap + PCIE_LNKSTA, LNKSTA_LBMS);
	{
		int dev_wr = open_config(dev_bdf, O_RDWR);
		if (dev_wr >= 0) {
			int dc = find_pcie_cap(dev_wr);
			if (dc >= 0)
				cfg_write16(dev_wr, dc + PCIE_LNKSTA, LNKSTA_LBMS);
			close(dev_wr);
		}
	}

	wait_for_device(dev_fd, DEVICE_ACCESSIBLE_TIMEOUT_MS);

	cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
	res->after_speed = lnksta & 0xF;

out:
	if (br_fd >= 0) close(br_fd);
	if (dev_fd >= 0) close(dev_fd);
}

// --- Method B: PerformEqu + TLS + Retrain Link ---

static void method_b(const char *bridge_bdf, const char *dev_bdf,
		     struct recover_result *res)
{
	memset(res, 0, sizeof(*res));

	int br_fd = open_config(bridge_bdf, O_RDWR);
	int dev_fd = open_config(dev_bdf, O_RDONLY);
	if (br_fd < 0 || dev_fd < 0)
		goto out;

	int br_cap = find_pcie_cap(br_fd);
	int dev_cap = find_pcie_cap(dev_fd);
	if (br_cap < 0 || dev_cap < 0)
		goto out;

	uint32_t br_lnkcap, dev_lnkcap;
	cfg_read32(br_fd, br_cap + PCIE_LNKCAP, &br_lnkcap);
	cfg_read32(dev_fd, dev_cap + PCIE_LNKCAP, &dev_lnkcap);

	int br_max = br_lnkcap & 0xF;
	int dev_max = dev_lnkcap & 0xF;
	res->target_speed = (br_max < dev_max) ? br_max : dev_max;

	uint16_t lnksta;
	cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
	res->before_speed = lnksta & 0xF;

	if (res->before_speed >= res->target_speed) {
		res->after_speed = res->before_speed;
		goto out;
	}

	uint16_t lnkctl2;
	cfg_read16(br_fd, br_cap + PCIE_LNKCTL2, &lnkctl2);
	lnkctl2 = (lnkctl2 & ~0xF) | (res->target_speed & 0xF);
	cfg_write16(br_fd, br_cap + PCIE_LNKCTL2, lnkctl2);

	set_perform_equ(br_fd, true);

	int cur_speed = res->before_speed;

	for (int i = 0; i < METHOD_A_MAX_ATTEMPTS; i++) {
		res->attempts = i + 1;

		cfg_write16(br_fd, br_cap + PCIE_LNKSTA, LNKSTA_LBMS);

		uint16_t lnkctl;
		cfg_read16(br_fd, br_cap + PCIE_LNKCTL, &lnkctl);
		cfg_write16(br_fd, br_cap + PCIE_LNKCTL, lnkctl | LNKCTL_RL);

		if (!wait_for_lbms(br_fd, br_cap, METHOD_A_LBMS_TIMEOUT_MS)) {
			res->timeout = true;
			break;
		}

		cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
		cur_speed = lnksta & 0xF;

		if (cur_speed >= res->target_speed)
			break;
	}

	set_perform_equ(br_fd, false);

	cfg_write16(br_fd, br_cap + PCIE_LNKSTA, LNKSTA_LBMS);
	{
		int dev_wr = open_config(dev_bdf, O_RDWR);
		if (dev_wr >= 0) {
			int dc = find_pcie_cap(dev_wr);
			if (dc >= 0)
				cfg_write16(dev_wr, dc + PCIE_LNKSTA, LNKSTA_LBMS);
			close(dev_wr);
		}
	}

	wait_for_device(dev_fd, DEVICE_ACCESSIBLE_TIMEOUT_MS);

	cfg_read16(br_fd, br_cap + PCIE_LNKSTA, &lnksta);
	res->after_speed = lnksta & 0xF;

out:
	if (br_fd >= 0) close(br_fd);
	if (dev_fd >= 0) close(dev_fd);
}

// --- TLB helpers (from sanity.c) ---

#define TLB_2M (1ULL << 21)

struct tlb_handle { uint32_t id; size_t size; void *mmio; };

static int tlb_alloc(int fd, size_t size, bool wc, struct tlb_handle *out)
{
	struct tenstorrent_allocate_tlb cmd = {0};
	cmd.in.size = size;
	if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &cmd) < 0) return -1;
	off_t offset = wc ? cmd.out.mmap_offset_wc : cmd.out.mmap_offset_uc;
	void *mmio = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
	if (mmio == MAP_FAILED) {
		struct tenstorrent_free_tlb f = { .in.id = cmd.out.id };
		ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &f);
		return -1;
	}
	out->id = cmd.out.id; out->size = size; out->mmio = mmio;
	return 0;
}

static void tlb_free(int fd, struct tlb_handle *tlb)
{
	munmap(tlb->mmio, tlb->size);
	struct tenstorrent_free_tlb f = { .in.id = tlb->id };
	ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &f);
}

static int tlb_map(int fd, struct tlb_handle *tlb, uint8_t x, uint8_t y, uint64_t addr)
{
	struct tenstorrent_configure_tlb cmd = {0};
	cmd.in.id = tlb->id;
	cmd.in.config.addr = addr;
	cmd.in.config.x_end = x;
	cmd.in.config.y_end = y;
	return ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cmd) < 0 ? -1 : 0;
}

// --- Quick functional tests ---

#define BH_NOC_GRID_X 17
#define BH_NOC_GRID_Y 12
#define BH_NOC_NODE_ID_LOGICAL 0xffb20148ULL
#define BH_PCIE_X 19
#define BH_PCIE_Y 24
#define DMA_TEST_SIZE 4096

static bool is_tensix_bh(uint32_t x, uint32_t y)
{
	return (y >= 2 && y <= 11) && ((x >= 1 && x <= 7) || (x >= 10 && x <= 16));
}

static int noc_sanity_quick(int fd)
{
	struct tlb_handle tlb;
	if (tlb_alloc(fd, TLB_2M, false, &tlb) < 0) return -1;

	uint64_t base = BH_NOC_NODE_ID_LOGICAL & ~(TLB_2M - 1);
	uint64_t off  = BH_NOC_NODE_ID_LOGICAL - base;
	int bad = 0;

	for (uint32_t x = 0; x < BH_NOC_GRID_X && !bad; x++)
		for (uint32_t y = 0; y < BH_NOC_GRID_Y && !bad; y++) {
			if (!is_tensix_bh(x, y)) continue;
			if (tlb_map(fd, &tlb, x, y, base) < 0) { bad++; continue; }
			uint32_t nid = *(volatile uint32_t *)((uint8_t *)tlb.mmio + off);
			if (((nid >> 0) & 0x3f) != x || ((nid >> 6) & 0x3f) != y) bad++;
		}

	tlb_free(fd, &tlb);
	return bad;
}

static int dma_loopback_quick(int fd)
{
	int ret = -1;
	void *buf = mmap(NULL, DMA_TEST_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED) return -1;
	memset(buf, 0, DMA_TEST_SIZE);

	struct tenstorrent_pin_pages pin = {0};
	pin.in.output_size_bytes = sizeof(pin.out);
	pin.in.flags = TENSTORRENT_PIN_PAGES_NOC_DMA;
	pin.in.virtual_address = (__u64)buf;
	pin.in.size = DMA_TEST_SIZE;
	if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) < 0) goto out_unmap;

	uint8_t pattern[DMA_TEST_SIZE];
	uint32_t seed = 0xDEADBEEF;
	for (size_t i = 0; i < DMA_TEST_SIZE / 4; i++) {
		seed = seed * 1103515245 + 12345;
		((uint32_t *)pattern)[i] = seed;
	}

	struct tlb_handle wc;
	if (tlb_alloc(fd, TLB_2M, true, &wc) < 0) goto out_unpin;

	uint64_t aligned = pin.out.noc_address & ~(TLB_2M - 1);
	uint64_t off = pin.out.noc_address - aligned;
	if (tlb_map(fd, &wc, BH_PCIE_X, BH_PCIE_Y, aligned) < 0) goto out_free_wc;

	volatile uint32_t *dst = (volatile uint32_t *)((uint8_t *)wc.mmio + off);
	for (size_t i = 0; i < DMA_TEST_SIZE / 4; i++) dst[i] = ((uint32_t *)pattern)[i];
	tlb_free(fd, &wc);

	struct tlb_handle uc;
	if (tlb_alloc(fd, TLB_2M, false, &uc) < 0) goto out_unpin;
	if (tlb_map(fd, &uc, BH_PCIE_X, BH_PCIE_Y, aligned) < 0) { tlb_free(fd, &uc); goto out_unpin; }
	(void)*(volatile uint32_t *)((uint8_t *)uc.mmio + off);
	tlb_free(fd, &uc);

	ret = memcmp(buf, pattern, DMA_TEST_SIZE) == 0 ? 0 : -1;
	goto out_unpin;

out_free_wc:
	tlb_free(fd, &wc);
out_unpin:
	{ struct tenstorrent_unpin_pages u = {0}; u.in.virtual_address = (__u64)buf; u.in.size = DMA_TEST_SIZE;
	  ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &u); }
out_unmap:
	munmap(buf, DMA_TEST_SIZE);
	return ret;
}

// --- Device enumeration ---

#define MAX_DEVICES 64

struct device_entry {
	int dev_id;
	uint8_t bus;
	char bdf[24];
	char bridge_bdf[24];
};

static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n <= 0) return -1;
	if (buf[n - 1] == '\n') n--;
	buf[n] = '\0';
	return 0;
}

static int find_bridge_bdf(const char *device_bdf, char *out, size_t len)
{
	char link[PATH_MAX], real[PATH_MAX];
	snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s", device_bdf);
	if (!realpath(link, real)) return -1;
	char *slash = strrchr(real, '/');
	if (!slash) return -1;
	*slash = '\0';
	char *parent = strrchr(real, '/');
	if (!parent) return -1;
	parent++;
	if (strlen(parent) >= 10 && parent[4] == ':')
		snprintf(out, len, "%s", parent);
	else
		snprintf(out, len, "(root)");
	return 0;
}

static int entry_cmp(const void *a, const void *b)
{
	return (int)((const struct device_entry *)a)->bus -
	       (int)((const struct device_entry *)b)->bus;
}

// --- Main ---

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <method> [--verify]\n", argv[0]);
		fprintf(stderr, "  A: TLS+retrain  B: PerformEqu+retrain  C: TLS+retrain+settle\n");
		return 1;
	}

	char method = argv[1][0];
	bool verify = false;
	for (int i = 2; i < argc; i++)
		if (strcmp(argv[i], "--verify") == 0)
			verify = true;

	if (method >= 'a' && method <= 'e') method -= 32;
	if (method != 'A' && method != 'B' && method != 'C') {
		fprintf(stderr, "Methods: A, B, C\n");
		return 1;
	}

	DIR *d = opendir("/dev/tenstorrent/");
	if (!d) { fprintf(stderr, "Cannot open /dev/tenstorrent/\n"); return 1; }

	struct device_entry devices[MAX_DEVICES];
	int count = 0;
	struct dirent *ent;

	while ((ent = readdir(d)) != NULL && count < MAX_DEVICES) {
		char *endptr;
		long dev_id = strtol(ent->d_name, &endptr, 10);
		if (*endptr != '\0') continue;

		char dev_path[PATH_MAX];
		snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%ld", dev_id);
		int fd = open(dev_path, O_RDWR | O_APPEND);
		if (fd < 0) continue;

		struct tenstorrent_get_device_info info = {0};
		info.in.output_size_bytes = sizeof(info.out);
		if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) { close(fd); continue; }
		close(fd);

		struct device_entry *e = &devices[count];
		e->dev_id = (int)dev_id;
		e->bus = (info.out.bus_dev_fn >> 8) & 0xFF;
		snprintf(e->bdf, sizeof(e->bdf), "%04x:%02x:%02x.%x",
			 info.out.pci_domain, e->bus,
			 (info.out.bus_dev_fn >> 3) & 0x1F,
			 info.out.bus_dev_fn & 0x7);
		find_bridge_bdf(e->bdf, e->bridge_bdf, sizeof(e->bridge_bdf));
		count++;
	}
	closedir(d);

	qsort(devices, count, sizeof(devices[0]), entry_cmp);

	printf("Method %c: recovering degraded links\n\n", method);
	printf("%-14s %-14s  %s -> %s  %s  %s",
	       "BDF", "Bridge", "Before", "After", "Att", "Result");
	if (verify)
		printf("  %-4s %-4s", "NOC", "DMA");
	printf("\n");
	printf("%-14s %-14s  %s    %s  %s  %s",
	       "--------------", "--------------", "------", "-----",
	       "---", "------");
	if (verify)
		printf("  %-4s %-4s", "----", "----");
	printf("\n");

	int tried = 0, recovered = 0, failed = 0, skipped = 0;

	for (int i = 0; i < count; i++) {
		struct device_entry *e = &devices[i];

		// Quick check: is this device degraded?
		int br_fd = open_config(e->bridge_bdf, O_RDONLY);
		if (br_fd < 0) continue;
		int cap = find_pcie_cap(br_fd);
		if (cap < 0) { close(br_fd); continue; }

		uint32_t lnkcap;
		uint16_t lnksta;
		cfg_read32(br_fd, cap + PCIE_LNKCAP, &lnkcap);
		cfg_read16(br_fd, cap + PCIE_LNKSTA, &lnksta);
		close(br_fd);

		int cur = lnksta & 0xF;
		int max = lnkcap & 0xF;
		if (cur >= max) {
			skipped++;
			continue;
		}

		tried++;
		struct recover_result res;
		if (method == 'A')
			method_a(e->bridge_bdf, e->bdf, &res);
		else if (method == 'B')
			method_b(e->bridge_bdf, e->bdf, &res);
		else
			method_c(e->bridge_bdf, e->bdf, &res);

		bool ok = (res.after_speed >= res.target_speed);
		if (ok) recovered++;
		else failed++;

		const char *result_str = ok ? "OK" :
					 res.timeout ? "TIMEOUT" : "STUCK";

		printf("%-14s %-14s  Gen%-2d -> Gen%-2d %3d  %-7s",
		       e->bdf, e->bridge_bdf,
		       res.before_speed, res.after_speed,
		       res.attempts, result_str);

		if (verify) {
			char dev_path[PATH_MAX];
			snprintf(dev_path, sizeof(dev_path),
				 "/dev/tenstorrent/%d", e->dev_id);
			int fd = open(dev_path, O_RDWR | O_CLOEXEC);
			if (fd >= 0) {
				int noc = noc_sanity_quick(fd);
				int dma = dma_loopback_quick(fd);
				close(fd);
				printf("  %-4s %-4s",
				       noc == 0 ? "PASS" : "FAIL",
				       dma == 0 ? "PASS" : "FAIL");
			} else {
				printf("  %-4s %-4s", "ERR", "ERR");
			}
		}

		printf("\n");
	}

	printf("\n--- Summary: %d tried, %d recovered, %d failed, %d skipped (healthy) ---\n",
	       tried, recovered, failed, skipped);

	return failed > 0 ? 1 : 0;
}
