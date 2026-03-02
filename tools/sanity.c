// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Sanity Check Tool for Tenstorrent Blackhole Devices.
//
// Runs NOC coordinate verification (140 Tensix cores) and a 2MB DMA
// loopback test on each device. Reports per-device pass/fail.
//
// With -n <count>, waits until at least <count> devices are visible.
//
// To Compile:
//  gcc -o sanity sanity.c
//
// To Run:
//  ./sanity             # check whatever is present now
//  ./sanity -n 32       # wait for 32 devices, then check all

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/mman.h>
#include <time.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

// --- Driver UAPI Definitions (from ioctl.h) ---

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_PIN_PAGES       _IO(TENSTORRENT_IOCTL_MAGIC, 7)
#define TENSTORRENT_IOCTL_UNPIN_PAGES     _IO(TENSTORRENT_IOCTL_MAGIC, 10)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB    _IO(TENSTORRENT_IOCTL_MAGIC, 11)
#define TENSTORRENT_IOCTL_FREE_TLB        _IO(TENSTORRENT_IOCTL_MAGIC, 12)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB   _IO(TENSTORRENT_IOCTL_MAGIC, 13)

#define TENSTORRENT_PIN_PAGES_NOC_DMA 2
#define PCI_DEVICE_ID_BLACKHOLE   0xb140

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

// --- TLB helpers ---

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

// --- Blackhole sanity checks ---

#define BH_NOC_GRID_X          17
#define BH_NOC_GRID_Y          12
#define BH_NOC_NODE_ID_LOGICAL 0xffb20148ULL
#define BH_PCIE_X              19
#define BH_PCIE_Y              24
#define DMA_TEST_SIZE           (2 * 1024 * 1024) * 32

static bool is_tensix_bh(uint32_t x, uint32_t y)
{
	return (y >= 2 && y <= 11) &&
	       ((x >= 1 && x <= 7) || (x >= 10 && x <= 16));
}

#define NOC_CHECK_DURATION_S 1

static int noc_sanity_check(int fd)
{
	struct tlb_handle tlb;
	if (tlb_alloc(fd, TLB_2M, false, &tlb) < 0)
		return -1;

	uint64_t base = BH_NOC_NODE_ID_LOGICAL & ~(TLB_2M - 1);
	uint64_t off  = BH_NOC_NODE_ID_LOGICAL -  base;
	int bad = 0;
	time_t end = time(NULL) + NOC_CHECK_DURATION_S;

	do {
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
				if (nx != x || ny != y) {
					fprintf(stderr,
						"    NOC (%u,%u): got (%u,%u)\n",
						x, y, nx, ny);
					bad++;
				}
			}
		}
	} while (!bad && time(NULL) < end);

	tlb_free(fd, &tlb);
	return bad;
}

static int dma_loopback_test(int fd)
{
	int ret = -1;
	void *buf = MAP_FAILED;
	uint8_t *pattern = NULL;

	buf = mmap(NULL, DMA_TEST_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (buf == MAP_FAILED)
		buf = mmap(NULL, DMA_TEST_SIZE, PROT_READ | PROT_WRITE,
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

	pattern = malloc(DMA_TEST_SIZE);
	if (!pattern)
		goto out_unpin;

	uint32_t seed = 0xDEADBEEF;
	uint32_t *p32 = (uint32_t *)pattern;
	for (size_t i = 0; i < DMA_TEST_SIZE / 4; i++) {
		seed = seed * 1103515245 + 12345;
		p32[i] = seed;
	}

	struct tlb_handle wc;
	if (tlb_alloc(fd, TLB_2M, true, &wc) < 0)
		goto out_unpin;

	const uint8_t *src = pattern;
	uint64_t addr = noc_addr;
	size_t remaining = DMA_TEST_SIZE;

	while (remaining > 0) {
		uint64_t aligned = addr & ~(TLB_2M - 1);
		uint64_t off = addr - aligned;
		size_t chunk = MIN(remaining, TLB_2M - off);

		if (tlb_map(fd, &wc, BH_PCIE_X, BH_PCIE_Y, aligned) < 0)
			goto out_free_wc;

		volatile uint32_t *dst = (volatile uint32_t *)
			((uint8_t *)wc.mmio + off);
		const uint32_t *s = (const uint32_t *)src;
		for (size_t i = 0; i < chunk / 4; i++)
			dst[i] = s[i];

		src += chunk;
		addr += chunk;
		remaining -= chunk;
	}

	tlb_free(fd, &wc);

	struct tlb_handle uc;
	if (tlb_alloc(fd, TLB_2M, false, &uc) < 0)
		goto out_unpin;

	uint64_t first = noc_addr;
	uint64_t last  = noc_addr + DMA_TEST_SIZE - 4;

	tlb_map(fd, &uc, BH_PCIE_X, BH_PCIE_Y, first & ~(TLB_2M - 1));
	(void)*(volatile uint32_t *)((uint8_t *)uc.mmio + (first & (TLB_2M - 1)));

	if ((last & ~(TLB_2M - 1)) != (first & ~(TLB_2M - 1)))
		tlb_map(fd, &uc, BH_PCIE_X, BH_PCIE_Y, last & ~(TLB_2M - 1));
	(void)*(volatile uint32_t *)((uint8_t *)uc.mmio + (last & (TLB_2M - 1)));

	tlb_free(fd, &uc);

	ret = memcmp(buf, pattern, DMA_TEST_SIZE) == 0 ? 0 : -1;
	goto out_unpin;

out_free_wc:
	tlb_free(fd, &wc);
out_unpin:
	free(pattern);
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
#define WAIT_TIMEOUT_S 30
#define WAIT_POLL_MS 250

struct device_info {
	int dev_id;
	__u16 device_id;
	char bdf[24];
};

static int get_device_info(int dev_id, struct device_info *info)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "/dev/tenstorrent/%d", dev_id);

	int fd = open(path, O_RDWR | O_APPEND);
	if (fd < 0)
		return -1;

	struct tenstorrent_get_device_info di = {0};
	di.in.output_size_bytes = sizeof(di.out);

	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &di) < 0) {
		close(fd);
		return -1;
	}
	close(fd);

	info->dev_id = dev_id;
	info->device_id = di.out.device_id;
	snprintf(info->bdf, sizeof(info->bdf), "%04x:%02x:%02x.%x",
		 di.out.pci_domain,
		 (di.out.bus_dev_fn >> 8) & 0xFF,
		 (di.out.bus_dev_fn >> 3) & 0x1F,
		 di.out.bus_dev_fn & 0x7);
	return 0;
}

static int enumerate_devices(struct device_info *devices, int max_count)
{
	DIR *d = opendir("/dev/tenstorrent/");
	if (!d)
		return 0;

	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL && count < max_count) {
		char *endptr;
		long id = strtol(ent->d_name, &endptr, 10);
		if (*endptr != '\0')
			continue;
		if (get_device_info((int)id, &devices[count]) == 0)
			count++;
	}
	closedir(d);
	return count;
}

static int dev_id_cmp(const void *a, const void *b)
{
	return ((const struct device_info *)a)->dev_id -
	       ((const struct device_info *)b)->dev_id;
}

static int count_devices(void)
{
	DIR *d = opendir("/dev/tenstorrent/");
	if (!d)
		return 0;

	int count = 0;
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		char *endptr;
		strtol(ent->d_name, &endptr, 10);
		if (*endptr == '\0')
			count++;
	}
	closedir(d);
	return count;
}

// --- Main ---

int main(int argc, char *argv[])
{
	int expected = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			expected = atoi(argv[++i]);
		} else {
			fprintf(stderr, "Usage: %s [-n expected_count]\n",
				argv[0]);
			return 1;
		}
	}

	if (expected > 0) {
		time_t start = time(NULL);
		while (count_devices() < expected) {
			if (difftime(time(NULL), start) > WAIT_TIMEOUT_S) {
				fprintf(stderr,
					"Timeout: %d/%d devices after %ds\n",
					count_devices(), expected,
					WAIT_TIMEOUT_S);
				return 1;
			}
			usleep(WAIT_POLL_MS * 1000);
		}
	}

	struct device_info devices[MAX_DEVICES];
	int count = enumerate_devices(devices, MAX_DEVICES);

	if (count == 0) {
		fprintf(stderr, "No Tenstorrent devices found.\n");
		return 1;
	}

	if (expected > 0 && count < expected) {
		fprintf(stderr, "Expected %d devices, found %d\n",
			expected, count);
		return 1;
	}

	qsort(devices, count, sizeof(devices[0]), dev_id_cmp);

	int failures = 0;
	for (int i = 0; i < count; i++) {
		bool is_bh = (devices[i].device_id == PCI_DEVICE_ID_BLACKHOLE);
		if (!is_bh)
			continue;

		char path[PATH_MAX];
		snprintf(path, sizeof(path), "/dev/tenstorrent/%d",
			 devices[i].dev_id);

		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "/dev/tenstorrent/%-3d %s: FAIL (open)\n",
				devices[i].dev_id, devices[i].bdf);
			failures++;
			continue;
		}

		int noc_bad = noc_sanity_check(fd);
		int dma_bad = dma_loopback_test(fd);
		close(fd);

		if (noc_bad != 0 || dma_bad != 0) {
			fprintf(stderr, "/dev/tenstorrent/%-3d %s: FAIL "
				"(NOC=%s DMA=%s)\n",
				devices[i].dev_id, devices[i].bdf,
				noc_bad ? "FAIL" : "ok",
				dma_bad ? "FAIL" : "ok");
			failures++;
		}
	}

	if (failures > 0) {
		fprintf(stderr, "%d/%d device(s) failed sanity check.\n",
			failures, count);
		return 1;
	}

	printf("%d device(s) passed (NOC + 2MB DMA loopback).\n", count);
	return 0;
}
