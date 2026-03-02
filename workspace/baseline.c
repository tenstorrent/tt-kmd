// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Baseline Characterization Tool
//
// Reads PCIe register state from all devices and their upstream bridges,
// then runs quick NOC sanity + DMA loopback on degraded (Gen1) chips.
//
// Compile: gcc -o baseline baseline.c
// Run:     sudo ./baseline

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
		__u16 vendor_id;
		__u16 device_id;
		__u16 subsystem_vendor_id;
		__u16 subsystem_id;
		__u16 bus_dev_fn;
		__u16 max_dma_buf_size_log2;
		__u16 pci_domain;
		__u16 reserved;
	} out;
};

struct tenstorrent_allocate_tlb {
	struct { __u64 size; __u64 reserved; } in;
	struct { __u32 id; __u32 reserved0; __u64 mmap_offset_uc; __u64 mmap_offset_wc; __u64 reserved1; } out;
};

struct tenstorrent_free_tlb {
	struct { __u32 id; } in;
};

struct tenstorrent_noc_tlb_config {
	__u64 addr;
	__u16 x_end; __u16 y_end; __u16 x_start; __u16 y_start;
	__u8 noc; __u8 mcast; __u8 ordering; __u8 linked; __u8 static_vc;
	__u8 reserved0[3];
	__u32 reserved1[2];
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

// --- PCI config space access ---

static int open_config(const char *bdf)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	return open(path, O_RDONLY);
}

static int cfg_read16(int fd, int offset, uint16_t *val)
{
	return pread(fd, val, 2, offset) == 2 ? 0 : -1;
}

static int cfg_read32(int fd, int offset, uint32_t *val)
{
	return pread(fd, val, 4, offset) == 4 ? 0 : -1;
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

// PCIe Capability register offsets (relative to cap base)
#define PCIE_LNKCAP   0x0C
#define PCIE_LNKCTL   0x10
#define PCIE_LNKSTA   0x12
#define PCIE_LNKCTL2  0x30
#define PCIE_LNKSTA2  0x32

// Secondary PCIe Extended Capability
#define EXT_CAP_SECPCI 0x0019
#define SECPCI_LNKCTL3 0x04

struct pcie_regs {
	uint32_t lnkcap;
	uint16_t lnksta;
	uint16_t lnkctl2;
	uint16_t lnksta2;
	uint32_t lnkctl3;
	bool has_secpci;
};

static int read_pcie_regs(const char *bdf, struct pcie_regs *r)
{
	memset(r, 0, sizeof(*r));

	int fd = open_config(bdf);
	if (fd < 0)
		return -1;

	int cap = find_pcie_cap(fd);
	if (cap < 0) {
		close(fd);
		return -1;
	}

	cfg_read32(fd, cap + PCIE_LNKCAP, &r->lnkcap);
	cfg_read16(fd, cap + PCIE_LNKSTA, &r->lnksta);
	cfg_read16(fd, cap + PCIE_LNKCTL2, &r->lnkctl2);
	cfg_read16(fd, cap + PCIE_LNKSTA2, &r->lnksta2);

	int secpci = find_ext_cap(fd, EXT_CAP_SECPCI);
	if (secpci >= 0) {
		cfg_read32(fd, secpci + SECPCI_LNKCTL3, &r->lnkctl3);
		r->has_secpci = true;
	}

	close(fd);
	return 0;
}

// --- TLB helpers (from sanity.c) ---

#define TLB_2M (1ULL << 21)

struct tlb_handle {
	uint32_t id;
	size_t size;
	void *mmio;
};

static int tlb_alloc(int fd, size_t size, bool wc, struct tlb_handle *out)
{
	struct tenstorrent_allocate_tlb cmd = {0};
	cmd.in.size = size;

	if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &cmd) < 0)
		return -1;

	off_t offset = wc ? cmd.out.mmap_offset_wc : cmd.out.mmap_offset_uc;
	void *mmio = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  fd, offset);
	if (mmio == MAP_FAILED) {
		struct tenstorrent_free_tlb f = { .in.id = cmd.out.id };
		ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &f);
		return -1;
	}

	out->id = cmd.out.id;
	out->size = size;
	out->mmio = mmio;
	return 0;
}

static void tlb_free(int fd, struct tlb_handle *tlb)
{
	munmap(tlb->mmio, tlb->size);
	struct tenstorrent_free_tlb f = { .in.id = tlb->id };
	ioctl(fd, TENSTORRENT_IOCTL_FREE_TLB, &f);
}

static int tlb_map(int fd, struct tlb_handle *tlb,
		   uint8_t x, uint8_t y, uint64_t addr)
{
	struct tenstorrent_configure_tlb cmd = {0};
	cmd.in.id = tlb->id;
	cmd.in.config.addr = addr;
	cmd.in.config.x_end = x;
	cmd.in.config.y_end = y;
	return ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cmd) < 0 ? -1 : 0;
}

// --- NOC sanity (single pass, from sanity.c) ---

#define BH_NOC_GRID_X          17
#define BH_NOC_GRID_Y          12
#define BH_NOC_NODE_ID_LOGICAL 0xffb20148ULL
#define BH_PCIE_X              19
#define BH_PCIE_Y              24

static bool is_tensix_bh(uint32_t x, uint32_t y)
{
	return (y >= 2 && y <= 11) &&
	       ((x >= 1 && x <= 7) || (x >= 10 && x <= 16));
}

static int noc_sanity_quick(int fd)
{
	struct tlb_handle tlb;
	if (tlb_alloc(fd, TLB_2M, false, &tlb) < 0)
		return -1;

	uint64_t base = BH_NOC_NODE_ID_LOGICAL & ~(TLB_2M - 1);
	uint64_t off  = BH_NOC_NODE_ID_LOGICAL -  base;
	int bad = 0;

	for (uint32_t x = 0; x < BH_NOC_GRID_X && !bad; x++) {
		for (uint32_t y = 0; y < BH_NOC_GRID_Y && !bad; y++) {
			if (!is_tensix_bh(x, y))
				continue;
			if (tlb_map(fd, &tlb, x, y, base) < 0) {
				bad++;
				continue;
			}
			uint32_t nid = *(volatile uint32_t *)
				((uint8_t *)tlb.mmio + off);
			uint32_t nx = (nid >> 0) & 0x3f;
			uint32_t ny = (nid >> 6) & 0x3f;
			if (nx != x || ny != y)
				bad++;
		}
	}

	tlb_free(fd, &tlb);
	return bad;
}

// --- DMA loopback (single 4KB page) ---

#define DMA_TEST_SIZE 4096

static int dma_loopback_quick(int fd)
{
	int ret = -1;

	void *buf = mmap(NULL, DMA_TEST_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED)
		return -1;

	memset(buf, 0, DMA_TEST_SIZE);

	struct tenstorrent_pin_pages pin = {0};
	pin.in.output_size_bytes = sizeof(pin.out);
	pin.in.flags = TENSTORRENT_PIN_PAGES_NOC_DMA;
	pin.in.virtual_address = (__u64)buf;
	pin.in.size = DMA_TEST_SIZE;

	if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) < 0)
		goto out_unmap;

	uint64_t noc_addr = pin.out.noc_address;

	uint8_t pattern[DMA_TEST_SIZE];
	uint32_t seed = 0xDEADBEEF;
	uint32_t *p32 = (uint32_t *)pattern;
	for (size_t i = 0; i < DMA_TEST_SIZE / 4; i++) {
		seed = seed * 1103515245 + 12345;
		p32[i] = seed;
	}

	struct tlb_handle wc;
	if (tlb_alloc(fd, TLB_2M, true, &wc) < 0)
		goto out_unpin;

	uint64_t aligned = noc_addr & ~(TLB_2M - 1);
	uint64_t off = noc_addr - aligned;

	if (tlb_map(fd, &wc, BH_PCIE_X, BH_PCIE_Y, aligned) < 0)
		goto out_free_wc;

	volatile uint32_t *dst = (volatile uint32_t *)
		((uint8_t *)wc.mmio + off);
	const uint32_t *s = (const uint32_t *)pattern;
	for (size_t i = 0; i < DMA_TEST_SIZE / 4; i++)
		dst[i] = s[i];

	tlb_free(fd, &wc);

	struct tlb_handle uc;
	if (tlb_alloc(fd, TLB_2M, false, &uc) < 0)
		goto out_unpin;

	if (tlb_map(fd, &uc, BH_PCIE_X, BH_PCIE_Y, aligned) < 0) {
		tlb_free(fd, &uc);
		goto out_unpin;
	}
	(void)*(volatile uint32_t *)((uint8_t *)uc.mmio + off);
	tlb_free(fd, &uc);

	ret = memcmp(buf, pattern, DMA_TEST_SIZE) == 0 ? 0 : -1;
	goto out_unpin;

out_free_wc:
	tlb_free(fd, &wc);
out_unpin:
	{
		struct tenstorrent_unpin_pages unpin = {0};
		unpin.in.virtual_address = (__u64)buf;
		unpin.in.size = DMA_TEST_SIZE;
		ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin);
	}
out_unmap:
	munmap(buf, DMA_TEST_SIZE);
	return ret;
}

// --- Device enumeration ---

#define MAX_DEVICES 64

struct device_entry {
	int dev_id;
	uint16_t domain;
	uint8_t bus;
	char bdf[24];
	char bridge_bdf[24];
	int cur_width;
	struct pcie_regs bridge_regs;
	struct pcie_regs dev_regs;
	int noc_result;   // 0 = pass, -1 = skip, >0 = bad tiles
	int dma_result;   // 0 = pass, -1 = fail/skip
};

static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	if (buf[n - 1] == '\n')
		n--;
	buf[n] = '\0';
	return 0;
}

static int find_bridge_bdf(const char *device_bdf, char *out, size_t len)
{
	char link[PATH_MAX], real[PATH_MAX];
	snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s", device_bdf);

	if (!realpath(link, real))
		return -1;

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
	const struct device_entry *ea = a, *eb = b;
	return (int)ea->bus - (int)eb->bus;
}

// --- Output helpers ---

static int lnksta_speed(uint16_t lnksta) { return lnksta & 0xF; }
static int lnksta_width(uint16_t lnksta) { return (lnksta >> 4) & 0x3F; }
static int lnksta_lbms(uint16_t lnksta) { return (lnksta >> 12) & 1; }
static int lnksta_dllla(uint16_t lnksta) { return (lnksta >> 13) & 1; }
static int lnkcap_maxspeed(uint32_t lnkcap) { return lnkcap & 0xF; }
static int lnkctl2_tls(uint16_t lnkctl2) { return lnkctl2 & 0xF; }
static int lnksta2_eq_complete(uint16_t s2) { return (s2 >> 1) & 1; }
static int lnksta2_ph1(uint16_t s2) { return (s2 >> 2) & 1; }
static int lnksta2_ph2(uint16_t s2) { return (s2 >> 3) & 1; }
static int lnksta2_ph3(uint16_t s2) { return (s2 >> 4) & 1; }
static int lnkctl3_pequ(uint32_t c3) { return c3 & 1; }

int main(void)
{
	DIR *d = opendir("/dev/tenstorrent/");
	if (!d) {
		fprintf(stderr, "Cannot open /dev/tenstorrent/\n");
		return 1;
	}

	struct device_entry devices[MAX_DEVICES];
	int count = 0;
	struct dirent *ent;

	while ((ent = readdir(d)) != NULL && count < MAX_DEVICES) {
		char *endptr;
		long dev_id = strtol(ent->d_name, &endptr, 10);
		if (*endptr != '\0')
			continue;

		char dev_path[PATH_MAX];
		snprintf(dev_path, sizeof(dev_path),
			 "/dev/tenstorrent/%ld", dev_id);

		int fd = open(dev_path, O_RDWR | O_APPEND);
		if (fd < 0)
			continue;

		struct tenstorrent_get_device_info info = {0};
		info.in.output_size_bytes = sizeof(info.out);

		if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
			close(fd);
			continue;
		}
		close(fd);

		struct device_entry *e = &devices[count];
		e->dev_id = (int)dev_id;
		e->domain = info.out.pci_domain;
		e->bus = (info.out.bus_dev_fn >> 8) & 0xFF;

		snprintf(e->bdf, sizeof(e->bdf), "%04x:%02x:%02x.%x",
			 e->domain, e->bus,
			 (info.out.bus_dev_fn >> 3) & 0x1F,
			 info.out.bus_dev_fn & 0x7);

		find_bridge_bdf(e->bdf, e->bridge_bdf, sizeof(e->bridge_bdf));

		char sysfs_buf[64], sysfs_path[PATH_MAX];
		snprintf(sysfs_path, sizeof(sysfs_path),
			 "/sys/bus/pci/devices/%s/current_link_width", e->bdf);
		e->cur_width = (read_sysfs(sysfs_path, sysfs_buf, sizeof(sysfs_buf)) == 0)
			     ? atoi(sysfs_buf) : 0;

		read_pcie_regs(e->bridge_bdf, &e->bridge_regs);
		read_pcie_regs(e->bdf, &e->dev_regs);

		e->noc_result = -1;
		e->dma_result = -1;
		count++;
	}
	closedir(d);

	if (count == 0) {
		fprintf(stderr, "No devices found.\n");
		return 1;
	}

	qsort(devices, count, sizeof(devices[0]), entry_cmp);

	// Run functional tests on degraded devices
	int degraded_count = 0;
	for (int i = 0; i < count; i++) {
		struct device_entry *e = &devices[i];
		int cur = lnksta_speed(e->bridge_regs.lnksta);
		int max = lnkcap_maxspeed(e->bridge_regs.lnkcap);

		if (cur >= max)
			continue;

		degraded_count++;

		char dev_path[PATH_MAX];
		snprintf(dev_path, sizeof(dev_path),
			 "/dev/tenstorrent/%d", e->dev_id);
		int fd = open(dev_path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		e->noc_result = noc_sanity_quick(fd);
		e->dma_result = dma_loopback_quick(fd);
		close(fd);
	}

	// Print register table
	printf("=== PCIe Register State (bridge) ===\n\n");
	printf("%-14s %-14s %3s %4s %3s %4s %4s %5s %3s %3s %3s %4s  %s\n",
	       "BDF", "Bridge", "Wdt", "Gen", "Max", "TLS", "LBMS", "DLLLA",
	       "EqC", "Ph1", "Ph2", "Ph3", "PEqu");
	printf("%-14s %-14s %3s %4s %3s %4s %4s %5s %3s %3s %3s %4s  %s\n",
	       "--------------", "--------------", "---", "----", "---",
	       "----", "----", "-----", "---", "---", "---", "----", "----");

	for (int i = 0; i < count; i++) {
		struct device_entry *e = &devices[i];
		struct pcie_regs *br = &e->bridge_regs;
		int cur = lnksta_speed(br->lnksta);
		int max = lnkcap_maxspeed(br->lnkcap);
		bool degraded = (cur < max);

		printf("%-14s %-14s x%-2d %4d %3d %4d %4d %5d %3d %3d %3d %4d  %4d  %s\n",
		       e->bdf, e->bridge_bdf,
		       e->cur_width,
		       cur, max,
		       lnkctl2_tls(br->lnkctl2),
		       lnksta_lbms(br->lnksta),
		       lnksta_dllla(br->lnksta),
		       lnksta2_eq_complete(br->lnksta2),
		       lnksta2_ph1(br->lnksta2),
		       lnksta2_ph2(br->lnksta2),
		       lnksta2_ph3(br->lnksta2),
		       br->has_secpci ? lnkctl3_pequ(br->lnkctl3) : -1,
		       degraded ? "<<< DEGRADED" : "");
	}

	// Print functional test results for degraded devices
	if (degraded_count > 0) {
		printf("\n=== Functional Tests (degraded chips only) ===\n\n");
		printf("%-14s %-14s  %3s  %-4s  %-4s\n",
		       "BDF", "Bridge", "Gen", "NOC", "DMA");
		printf("%-14s %-14s  %3s  %-4s  %-4s\n",
		       "--------------", "--------------", "---", "----", "----");

		for (int i = 0; i < count; i++) {
			struct device_entry *e = &devices[i];
			int cur = lnksta_speed(e->bridge_regs.lnksta);
			int max = lnkcap_maxspeed(e->bridge_regs.lnkcap);
			if (cur >= max)
				continue;

			const char *noc_str = (e->noc_result == 0) ? "PASS" :
					      (e->noc_result == -1) ? "SKIP" : "FAIL";
			const char *dma_str = (e->dma_result == 0) ? "PASS" :
					      (e->dma_result == -1) ? "FAIL" : "FAIL";

			printf("%-14s %-14s  %3d  %-4s  %-4s\n",
			       e->bdf, e->bridge_bdf, cur, noc_str, dma_str);
		}
	}

	printf("\n--- Summary: %d devices, %d degraded, %d healthy ---\n",
	       count, degraded_count, count - degraded_count);

	return 0;
}
