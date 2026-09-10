// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Keraunos DMA loopback through APPOUT0.
//
// Pin a host buffer, fill it with a pattern, then read it back from the
// device side: NOC_READ at SPA <host window> + <DMA address>. Then the
// reverse: NOC_WRITE through the same path and check host memory changed.
//
// The path, as far as the RTL says: host -> BAR0 -> APPIN0 -> HSIO0 fabric.
// The fabric has no route from its PCIe initiator straight to its PCIe
// target, so the request goes up to D2D router 0, through the ATT, and only
// comes back down into HSIO0 if the ATT sends it there. HSIO0's fabric then
// routes 0x2_0000_0000_0000..0x2_FFFF_FFFF_FFFF to the PCIe outbound port,
// APPOUT0 picks an entry from bits [47:44], and a TLP goes to the host.
//
// So the host window inside Keraunos is 0x2_..., not the package-level
// 0x6_... (Mimir/Quasar ATTs rebase 0x6_ to 0x2_ before it gets here). The
// default below is 0x2_. If the Keraunos ATT has no entry for it, the flit
// goes to a garbage destination: run one word first.
//
// Build:  gcc -O2 -Wall -Wextra -o keraunos_dma_loopback keraunos_dma_loopback.c
// Run:    ./keraunos_dma_loopback <device_id> [-n words] [-r] [-b spa_base]
//           -n N   number of 8-byte words to check (default 8, max 512)
//           -r     read-only: skip the device -> host write test
//           -b B   host window base (default 0x2_0000_0000_0000)
//
// The host address the device sees is what PIN_PAGES returns (IOVA under an
// IOMMU, physical otherwise). Only the low 48 bits fit in the APPOUT0 window
// scheme; if the address is wider the tool refuses rather than guess.

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>

// --- Driver definitions (mirror of ioctl.h) ---------------------------------
#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_PIN_PAGES       _IO(TENSTORRENT_IOCTL_MAGIC, 7)
#define TENSTORRENT_IOCTL_UNPIN_PAGES     _IO(TENSTORRENT_IOCTL_MAGIC, 10)
#define TENSTORRENT_IOCTL_NOC_READ        _IO(TENSTORRENT_IOCTL_MAGIC, 18)
#define TENSTORRENT_IOCTL_NOC_WRITE       _IO(TENSTORRENT_IOCTL_MAGIC, 19)

struct tenstorrent_get_device_info {
	struct { __u32 output_size_bytes; } in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
		__u16 bus_dev_fn, max_dma_buf_size_log2, pci_domain, reserved;
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
		__u64 physical_address;	// or IOVA
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

struct tenstorrent_noc_rw {
	__u32 argsz;
	__u32 flags;
	__u16 x;
	__u16 y;
	__u8 noc;
	__u8 width;
	__u8 reserved0[2];
	__u64 addr;
	__u64 value;
};

// --- Keraunos address map ---------------------------------------------------
// Keraunos-local name for host memory: 0x2_0000_0000_0000 + H (HSIO fabric
// decode, tile 0, -> t_hsio2pcie). The Grendel package map calls the same
// thing 0x6_0000_0000_0000 + H and other chiplets' ATTs rebase it. APPOUT0
// entry = bits [47:44] of H; with identity entries the TLP address is H.
// Same value as KERAUNOS_HOST_WINDOW in keraunos_addrmap.h; this tool stays
// standalone.
#define HOST_WINDOW_SPA   UINT64_C(0x2000000000000)
#define HOST_ADDR_BITS    48

#define MAX_WORDS 512

static int noc_read64(int fd, uint64_t spa, uint64_t *value)
{
	struct tenstorrent_noc_rw io = {0};

	io.argsz = sizeof(io);
	io.width = 8;
	io.addr = spa;
	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &io) < 0)
		return -errno;
	*value = io.value;
	return 0;
}

static int noc_write64(int fd, uint64_t spa, uint64_t value)
{
	struct tenstorrent_noc_rw io = {0};

	io.argsz = sizeof(io);
	io.width = 8;
	io.addr = spa;
	io.value = value;
	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_WRITE, &io) < 0)
		return -errno;
	return 0;
}

static uint64_t pattern(unsigned int i)
{
	// Word index in the top byte, address-like filler below, nothing that
	// looks like all-zeros or all-ones.
	return (UINT64_C(0xA5) << 56) | ((uint64_t)i << 48) |
	       (UINT64_C(0x0123456789AB) ^ ((uint64_t)i * UINT64_C(0x1111111111)));
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <device_id> [-n words] [-r] [-b spa_base]\n", prog);
}

static uint64_t parse_addr(const char *s)
{
	char clean[64];
	size_t j = 0;

	for (; *s && j < sizeof(clean) - 1; s++)
		if (*s != '_')
			clean[j++] = *s;
	clean[j] = 0;
	return strtoull(clean, NULL, 0);
}

int main(int argc, char *argv[])
{
	char dev_path[PATH_MAX];
	struct tenstorrent_get_device_info info = {0};
	struct tenstorrent_pin_pages pin = {0};
	struct tenstorrent_unpin_pages unpin = {0};
	const char *prog = argv[0];
	int dev_id = -1, words = 8, read_only = 0;
	int fd, rc, failures = 0, ret = 1;
	long page = sysconf(_SC_PAGESIZE);
	volatile uint64_t *buf = NULL;
	uint64_t dma_addr, base_spa, window = HOST_WINDOW_SPA;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			words = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
			window = parse_addr(argv[++i]);
		} else if (!strcmp(argv[i], "-r")) {
			read_only = 1;
		} else if (dev_id < 0) {
			dev_id = atoi(argv[i]);
		} else {
			usage(prog);
			return 2;
		}
	}
	if (dev_id < 0 || words < 1 || words > MAX_WORDS) {
		usage(prog);
		return 2;
	}

	snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);
	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "GET_DEVICE_INFO: %s\n", strerror(errno));
		goto out_close;
	}
	if (info.out.device_id != 0xFEED) {
		fprintf(stderr, "device id 0x%04x is not Keraunos (0xFEED)\n",
			info.out.device_id);
		ret = 2;
		goto out_close;
	}

	// One page, page-aligned, locked in memory so PIN_PAGES has something
	// to pin.
	buf = mmap(NULL, page, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (buf == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		buf = NULL;
		goto out_close;
	}
	memset((void *)buf, 0, page);

	pin.in.output_size_bytes = sizeof(pin.out);
	pin.in.flags = 0;
	pin.in.virtual_address = (uint64_t)(uintptr_t)buf;
	pin.in.size = page;
	if (ioctl(fd, TENSTORRENT_IOCTL_PIN_PAGES, &pin) < 0) {
		fprintf(stderr, "PIN_PAGES: %s\n", strerror(errno));
		goto out_unmap;
	}
	dma_addr = pin.out.physical_address;

	printf("device      %s\n", dev_path);
	printf("host buffer VA 0x%" PRIx64 ", %ld bytes\n",
	       (uint64_t)(uintptr_t)buf, page);
	printf("DMA address 0x%" PRIx64 "\n", dma_addr);

	if (dma_addr >> HOST_ADDR_BITS) {
		fprintf(stderr,
			"DMA address is wider than %d bits; APPOUT0 cannot express it. "
			"Nothing to test.\n", HOST_ADDR_BITS);
		ret = 2;
		goto out_unpin;
	}
	base_spa = window + dma_addr;
	printf("host window 0x%" PRIx64 "\n", window);
	printf("device SPA  0x%" PRIx64 "  (APPOUT0 entry %" PRIu64 ")\n\n",
	       base_spa, (dma_addr >> 44) & 0xF);

	// --- device reads host ------------------------------------------------
	for (i = 0; i < words; i++)
		buf[i] = pattern(i);
	__sync_synchronize();

	printf("=== device -> host read (NOC_READ through APPOUT0) ===\n");
	for (i = 0; i < words; i++) {
		uint64_t spa = base_spa + (uint64_t)i * 8;
		uint64_t got = 0;

		rc = noc_read64(fd, spa, &got);
		if (rc) {
			printf("  [%3d] SPA 0x%" PRIx64 "  ERROR %s\n", i, spa,
			       strerror(-rc));
			failures++;
			if (rc == -ENODEV || rc == -EINVAL)
				break;	// not going to get better
			continue;
		}
		if (got == pattern(i)) {
			printf("  [%3d] 0x%016" PRIx64 "  ok\n", i, got);
		} else {
			printf("  [%3d] 0x%016" PRIx64 "  expected 0x%016" PRIx64 "  MISMATCH\n",
			       i, got, pattern(i));
			failures++;
		}
	}

	// --- device writes host -----------------------------------------------
	if (!read_only) {
		printf("\n=== device -> host write (NOC_WRITE through APPOUT0) ===\n");
		for (i = 0; i < words; i++) {
			uint64_t spa = base_spa + (uint64_t)i * 8;
			uint64_t want = ~pattern(i);

			rc = noc_write64(fd, spa, want);
			if (rc) {
				printf("  [%3d] SPA 0x%" PRIx64 "  ERROR %s\n", i, spa,
				       strerror(-rc));
				failures++;
				continue;
			}
		}
		// A posted write may still be in flight; a read through the same
		// path is the only fence we have.
		{
			uint64_t dummy;
			(void)noc_read64(fd, base_spa, &dummy);
		}
		__sync_synchronize();
		for (i = 0; i < words; i++) {
			uint64_t got = buf[i];
			uint64_t want = ~pattern(i);

			if (got == want) {
				printf("  [%3d] 0x%016" PRIx64 "  ok\n", i, got);
			} else {
				printf("  [%3d] 0x%016" PRIx64 "  expected 0x%016" PRIx64 "  MISMATCH\n",
				       i, got, want);
				failures++;
			}
		}
	}

	printf("\n%s\n", failures ? "FAIL" : "PASS");
	ret = failures ? 1 : 0;

out_unpin:
	unpin.in.virtual_address = (uint64_t)(uintptr_t)buf;
	unpin.in.size = page;
	if (ioctl(fd, TENSTORRENT_IOCTL_UNPIN_PAGES, &unpin) < 0)
		fprintf(stderr, "UNPIN_PAGES: %s\n", strerror(errno));
out_unmap:
	munmap((void *)buf, page);
out_close:
	close(fd);
	return ret;
}
