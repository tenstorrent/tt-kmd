// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Dump and decode the Keraunos fabric firewalls (Arteris FlexNoC filters).
//
// Every host-originated transaction crosses one or more of these on its way
// to a Keraunos target, and they are the only place in the path that carries
// read/write permissions. The TLBs translate; the firewalls decide.
//
//   BAR0/BAR4 (APPIN) -> HSIO<n> "pcie" firewall -> SMN<n> "hsio2smn" -> SMC/SEP inbound
//   BAR2      (SYSIN) -> PCIe mgmt "smn" firewall               -> SMC/SEP inbound
//
// Each firewall is an array of filters; each filter is 0x20 bytes:
//
//   +0x00  FILTER_CONFIG  (64-bit) bit0 read_en, bit1 write_en, bit4 addr_mode,
//                                  bit8 allow_ns, [14:12] data_bus_width (RO),
//                                  [19:16] src_id (0 = any), [23:20] group_id,
//                                  bit24 allow_burst, bit63 locked (write-once)
//   +0x08  START_ADDR     (64-bit)
//   +0x10  END_ADDR       (64-bit)
//
// Reset state is deny-all. Bring-up opens two blankets across the whole
// address space on every firewall, one allowing non-secure and one secure
// only: qsr1_boot uses filters 0 and 15 with END all-ones, the chippy
// validation firmware uses filters 0 and 1 with END 0x00FF_FFFF_FFFF_FFFF.
//
// Semantics that the register model does not pin down and that this tool
// therefore reports without deciding: whether END_ADDR is inclusive (bring-up
// writes all-ones, so it is treated as inclusive here); whether a lower index
// wins or any permitting filter wins when ranges overlap (the tool prints
// both readings under --lookup); and whether addr_mode=0 disables an entry
// (measured on Mimir's instance of the same IP: it does not on the ITN).
//
// Reads go through TENSTORRENT_IOCTL_NOC_READ on the KLA path (BAR2/SYSIN0).
// The firewall registers inside the HSIO fabrics are only reachable once
// that fabric's cold reset has been released; before that expect all-ones.
//
// To compile:
//   gcc -O2 -Wall -Wextra -o keraunos_firewall tools/keraunos_firewall.c

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/types.h>

#include "keraunos_addrmap.h"

#define TENSTORRENT_IOCTL_MAGIC			0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_NOC_READ		_IO(TENSTORRENT_IOCTL_MAGIC, 18)
#define TENSTORRENT_NOC_FLAG_KLA		(1U << 0)
#define PCI_DEVICE_ID_KERAUNOS			0xFEED

#define FILTER_STRIDE		UINT64_C(0x20)
#define WORDS_PER_FILTER	6
#define MAX_FILTERS		16
#define HSIO_TILES		5
#define SMN_TILES		5
#define HSIO_TILE_STRIDE	UINT64_C(0x04000000)
#define SMN_TILE_STRIDE		UINT64_C(0x00020000)

#define HSIO_NOC2AXI_FW_BASE	UINT64_C(0x23020000)
#define HSIO_PCIE_FW_BASE	UINT64_C(0x23021000)
#define SMN_HSIO2HSIO_FW_BASE	UINT64_C(0x10004000)
#define SMN_HSIO2SMN_FW_BASE	UINT64_C(0x10004400)
#define PCIE_MGMT_SMN_FW_BASE	UINT64_C(0x18050000)
#define SMC_INBOUND_FW_BASE	UINT64_C(0x08015000)
#define SMC_OUTBOUND_FW_BASE	UINT64_C(0x08016000)
#define SEP_SYS_IN_FW_BASE	UINT64_C(0x01114000)

#define SMC_SPM_BASE		UINT64_C(0x08060000)
#define SMC_SPM_SIZE		UINT64_C(0x00100000)

#define CFG_READ_EN		(UINT64_C(1) << 0)
#define CFG_WRITE_EN		(UINT64_C(1) << 1)
#define CFG_ADDR_MODE		(UINT64_C(1) << 4)
#define CFG_ALLOW_NS		(UINT64_C(1) << 8)
#define CFG_BUS_WIDTH_SHIFT	12
#define CFG_BUS_WIDTH_MASK	UINT64_C(0x7)
#define CFG_SRC_ID_SHIFT	16
#define CFG_SRC_ID_MASK		UINT64_C(0xf)
#define CFG_GROUP_ID_SHIFT	20
#define CFG_GROUP_ID_MASK	UINT64_C(0xf)
#define CFG_ALLOW_BURST		(UINT64_C(1) << 24)
#define CFG_LOCKED		(UINT64_C(1) << 63)
#define CFG_KNOWN_BITS		(CFG_READ_EN | CFG_WRITE_EN | CFG_ADDR_MODE | \
				 CFG_ALLOW_NS | \
				 (CFG_BUS_WIDTH_MASK << CFG_BUS_WIDTH_SHIFT) | \
				 (CFG_SRC_ID_MASK << CFG_SRC_ID_SHIFT) | \
				 (CFG_GROUP_ID_MASK << CFG_GROUP_ID_SHIFT) | \
				 CFG_ALLOW_BURST | CFG_LOCKED)

struct tenstorrent_get_device_info {
	struct {
		__u32 output_size_bytes;
	} in;
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

struct tenstorrent_noc_read {
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

/*
 * Selection tags. A firewall is selected if any word on the command line
 * equals its name or one of its tags.
 */
#define TAG_MAX 4

struct firewall_desc {
	char name[16];
	uint64_t base;
	unsigned int filters;
	const char *tags[TAG_MAX];
	char what[64];
};

struct filter {
	uint32_t raw[WORDS_PER_FILTER];
	uint64_t config;
	uint64_t start;
	uint64_t end;
	int programmed;
};

struct firewall_data {
	struct filter filter[MAX_FILTERS];
	uint32_t first[MAX_FILTERS][WORDS_PER_FILTER];
	unsigned char changed[MAX_FILTERS];
	int all_ones;
};

struct options {
	int all;
	int raw;
	int once;
	int lookup;
	uint64_t address;
};

struct warnings {
	char text[256][240];
	unsigned int count;
};

#define FIREWALL_MAX (HSIO_TILES * 2 + SMN_TILES * 2 + 4)

static struct firewall_desc firewalls[FIREWALL_MAX];
static unsigned int firewall_count;

static void add_firewall(const char *name, uint64_t base, unsigned int filters,
			 const char *what, const char *tag0, const char *tag1,
			 const char *tag2, const char *tag3)
{
	struct firewall_desc *fw = &firewalls[firewall_count++];

	snprintf(fw->name, sizeof(fw->name), "%s", name);
	fw->base = base;
	fw->filters = filters;
	snprintf(fw->what, sizeof(fw->what), "%s", what);
	fw->tags[0] = tag0;
	fw->tags[1] = tag1;
	fw->tags[2] = tag2;
	fw->tags[3] = tag3;
}

static void build_inventory(void)
{
	static const char *const hsio_tag[HSIO_TILES] = {
		"hsio0", "hsio1", "hsio2", "hsio3", "hsio4"
	};
	static const char *const smn_tag[SMN_TILES] = {
		"smn0", "smn1", "smn2", "smn3", "smn4"
	};
	char name[16], what[64];
	unsigned int n;

	firewall_count = 0;
	for (n = 0; n < HSIO_TILES; n++) {
		snprintf(name, sizeof(name), "hsio%u-pcie", n);
		snprintf(what, sizeof(what),
			 "PCIe APP inbound (BAR0/BAR4) -> HSIO%u fabric", n);
		add_firewall(name, HSIO_PCIE_FW_BASE + n * HSIO_TILE_STRIDE,
			     MAX_FILTERS, what, "hsio", hsio_tag[n], "pcie",
			     n == 0 ? "host" : NULL);
		snprintf(name, sizeof(name), "hsio%u-noc2axi", n);
		snprintf(what, sizeof(what),
			 "NoC (NOC2AXI initiator) -> HSIO%u fabric", n);
		add_firewall(name, HSIO_NOC2AXI_FW_BASE + n * HSIO_TILE_STRIDE,
			     MAX_FILTERS, what, "hsio", hsio_tag[n], "noc2axi",
			     NULL);
	}
	for (n = 0; n < SMN_TILES; n++) {
		snprintf(name, sizeof(name), "smn%u-hsio2smn", n);
		snprintf(what, sizeof(what), "HSIO%u -> SMN", n);
		add_firewall(name, SMN_HSIO2SMN_FW_BASE + n * SMN_TILE_STRIDE,
			     MAX_FILTERS, what, "smn", smn_tag[n], "hsio2smn",
			     n == 0 ? "host" : NULL);
		snprintf(name, sizeof(name), "smn%u-hsio2hsio", n);
		snprintf(what, sizeof(what), "HSIO%u -> other HSIO tiles", n);
		add_firewall(name, SMN_HSIO2HSIO_FW_BASE + n * SMN_TILE_STRIDE,
			     MAX_FILTERS, what, "smn", smn_tag[n], "hsio2hsio",
			     NULL);
	}
	add_firewall("pcie-smn", PCIE_MGMT_SMN_FW_BASE, MAX_FILTERS,
		     "PCIe SYS inbound (BAR2) -> SMN", "pcie", "host", NULL,
		     NULL);
	add_firewall("smc-in", SMC_INBOUND_FW_BASE, MAX_FILTERS,
		     "SMN -> SMC block (regs, ROM, SPM SRAM)", "smc", "host",
		     NULL, NULL);
	add_firewall("smc-out", SMC_OUTBOUND_FW_BASE, 8,
		     "SMC core -> SMN", "smc", NULL, NULL, NULL);
	/*
	 * SEP's sys_in_axi_filter. Only the two entries bring-up programs are
	 * known; the block's true depth is not in the Keraunos register map.
	 */
	add_firewall("sep-in", SEP_SYS_IN_FW_BASE, 2,
		     "SMN -> SEP block (sys_in_axi_filter)", "sep", NULL, NULL,
		     NULL);
}

#ifndef KERAUNOS_FIREWALL_FAKE
static int real_read32(int fd, uint64_t addr, uint32_t flags, uint32_t *value)
{
	struct tenstorrent_noc_read io = { 0 };

	io.argsz = sizeof(io);
	io.flags = flags;
	io.width = 4;
	io.addr = addr;
	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &io) < 0)
		return -errno;
	*value = (uint32_t)io.value;
	return 0;
}

static addrmap_read32_fn read_word = real_read32;
#else
/*
 * Fake device: bring-up state on every firewall, plus a read-only carve-out
 * of the SMC SPM on hsio0-pcie and a locked, changing entry to exercise the
 * warnings. HSIO tile 3 reads as all-ones (fabric still in reset).
 */
struct fake_filter {
	uint64_t config, start, end;
};

static struct fake_filter fake[FIREWALL_MAX][MAX_FILTERS];
static int fake_initialized;
static unsigned int fake_pass_reads;

static void fake_open(unsigned int fw, unsigned int idx, uint64_t start,
		      uint64_t end, uint64_t perm, int ns)
{
	fake[fw][idx].config = perm | CFG_ADDR_MODE | CFG_ALLOW_BURST |
			       (ns ? CFG_ALLOW_NS : 0) |
			       (UINT64_C(3) << CFG_BUS_WIDTH_SHIFT);
	fake[fw][idx].start = start;
	fake[fw][idx].end = end;
}

static void fake_init(void)
{
	unsigned int fw;

	if (fake_initialized)
		return;
	fake_initialized = 1;
	memset(fake, 0, sizeof(fake));
	for (fw = 0; fw < firewall_count; fw++) {
		const struct firewall_desc *d = &firewalls[fw];
		unsigned int i;

		/* Read-only width field, and END[2:0] high on the SMC block. */
		for (i = 0; i < d->filters; i++) {
			fake[fw][i].config = UINT64_C(3) << CFG_BUS_WIDTH_SHIFT;
			if (!strcmp(d->name, "smc-in"))
				fake[fw][i].end = 7;
		}
		if (!strcmp(d->name, "smc-in") || !strcmp(d->name, "sep-in")) {
			fake_open(fw, 0, 0, UINT64_C(0xFFFFFFFFFFFFFF),
				  CFG_READ_EN | CFG_WRITE_EN, 0);
			fake_open(fw, 1, 0, UINT64_C(0xFFFFFFFFFFFFFF),
				  CFG_READ_EN | CFG_WRITE_EN, 1);
		} else if (!strcmp(d->name, "smc-out")) {
			fake_open(fw, 0, 0, UINT64_MAX,
				  CFG_READ_EN | CFG_WRITE_EN, 1);
		} else {
			fake_open(fw, 0, 0, UINT64_MAX,
				  CFG_READ_EN | CFG_WRITE_EN, 1);
			fake_open(fw, 15, 0, UINT64_MAX,
				  CFG_READ_EN | CFG_WRITE_EN, 0);
		}
		if (!strcmp(d->name, "hsio0-pcie")) {
			fake_open(fw, 0, 0, SMC_SPM_BASE - 1,
				  CFG_READ_EN | CFG_WRITE_EN, 1);
			fake_open(fw, 1, SMC_SPM_BASE,
				  SMC_SPM_BASE + SMC_SPM_SIZE - 1, CFG_READ_EN, 1);
			fake[fw][1].config |= CFG_LOCKED;
			fake_open(fw, 2, SMC_SPM_BASE + SMC_SPM_SIZE, UINT64_MAX,
				  CFG_READ_EN | CFG_WRITE_EN, 1);
			fake_open(fw, 3, UINT64_C(0x18000000),
				  UINT64_C(0x1BFFFFFF), 0, 1);
			fake_open(fw, 4, UINT64_C(0x1000), UINT64_C(0x0), CFG_READ_EN, 1);
			fake[fw][4].config |= UINT64_C(1) << 40;
		}
	}
}

static int fake_read32(int fd, uint64_t addr, uint32_t flags, uint32_t *value)
{
	unsigned int fw, idx, word;

	(void)fd;
	(void)flags;
	fake_init();
	for (fw = 0; fw < firewall_count; fw++) {
		const struct firewall_desc *d = &firewalls[fw];
		uint64_t off;
		uint64_t reg;

		if (addr < d->base ||
		    addr >= d->base + d->filters * FILTER_STRIDE)
			continue;
		if (!strcmp(d->name, "hsio3-pcie") ||
		    !strcmp(d->name, "hsio3-noc2axi")) {
			*value = UINT32_MAX;
			return 0;
		}
		off = addr - d->base;
		idx = (unsigned int)(off / FILTER_STRIDE);
		word = (unsigned int)((off % FILTER_STRIDE) / 4);
		if (word == 0)
			reg = fake[fw][idx].config;
		else if (word == 2)
			reg = fake[fw][idx].start;
		else if (word == 4)
			reg = fake[fw][idx].end;
		else if (word == 1)
			reg = fake[fw][idx].config >> 32;
		else if (word == 3)
			reg = fake[fw][idx].start >> 32;
		else if (word == 5)
			reg = fake[fw][idx].end >> 32;
		else
			return -EINVAL;
		*value = (uint32_t)reg;
		/* smn1-hsio2smn filter 15 changes between passes. */
		if (!strcmp(d->name, "smn1-hsio2smn") && idx == 15 && word == 0)
			*value ^= (fake_pass_reads++ & 1) ? 0 : CFG_WRITE_EN;
		return 0;
	}
	return -EIO;
}

static addrmap_read32_fn read_word = fake_read32;
#endif

static void add_warning(struct warnings *warnings, const char *fmt, ...)
{
	va_list ap;
	unsigned int slot = warnings->count;

	if (slot < sizeof(warnings->text) / sizeof(warnings->text[0])) {
		va_start(ap, fmt);
		vsnprintf(warnings->text[slot], sizeof(warnings->text[slot]),
			  fmt, ap);
		va_end(ap);
	}
	warnings->count++;
}

static void print_warnings(const struct warnings *warnings)
{
	unsigned int i, shown = warnings->count;

	if (!warnings->count)
		return;
	if (shown > sizeof(warnings->text) / sizeof(warnings->text[0]))
		shown = sizeof(warnings->text) / sizeof(warnings->text[0]);
	printf("\n");
	for (i = 0; i < shown; i++)
		printf("WARNING: %s.\n", warnings->text[i]);
	if (shown != warnings->count)
		printf("WARNING: %u additional warnings omitted.\n",
		       warnings->count - shown);
}

static int parse_addr(const char *text, uint64_t *value)
{
	char clean[128];
	char *end;
	size_t i, n = 0;
	unsigned long long parsed;

	if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
		return -1;
	for (i = 0; text[i]; i++) {
		if (text[i] == '_')
			continue;
		if (n + 1 >= sizeof(clean))
			return -1;
		clean[n++] = text[i];
	}
	clean[n] = '\0';
	if (n <= 2)
		return -1;
	errno = 0;
	parsed = strtoull(clean, &end, 0);
	if (errno || *end)
		return -1;
	*value = (uint64_t)parsed;
	return 0;
}

static int parse_device_id(const char *text, int *device_id)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(text, &end, 10);
	if (errno || !*text || *end || parsed < 0 || parsed > INT_MAX)
		return -1;
	*device_id = (int)parsed;
	return 0;
}

static void usage(const char *prog)
{
	unsigned int i;

	fprintf(stderr,
		"Usage: %s <device_id> [firewalls] [options]\n"
		"\n"
		"Firewalls (default all): host pcie hsio smn smc sep noc2axi "
		"hsio2smn hsio2hsio\n"
		"  hsio<n> smn<n> (both firewalls of one tile), or by name:\n"
		"  ",
		prog);
	for (i = 0; i < firewall_count; i++)
		fprintf(stderr, "%s%s", i ? " " : "", firewalls[i].name);
	fprintf(stderr,
		"\n"
		"  'host' is the BAR0/BAR2 -> SMC path: hsio0-pcie smn0-hsio2smn "
		"pcie-smn smc-in\n"
		"\n"
		"Options:\n"
		"  -a, --all           print unprogrammed filters in full\n"
		"  -r, --raw           print the six raw 32-bit words per filter\n"
		"  -1, --once          read each firewall once (skip the "
		"consistency check)\n"
		"  -l, --lookup ADDR   show which filters cover ADDR and what "
		"they permit\n"
		"  -h, --help          show this help\n");
}

static int select_word(const char *word, unsigned int *selected)
{
	unsigned int i, t, hit = 0;

	for (i = 0; i < firewall_count; i++) {
		const struct firewall_desc *fw = &firewalls[i];
		int match = !strcmp(word, fw->name);

		for (t = 0; t < TAG_MAX && !match; t++)
			if (fw->tags[t] && !strcmp(word, fw->tags[t]))
				match = 1;
		if (match) {
			*selected |= 1U << i;
			hit = 1;
		}
	}
	return hit ? 0 : -1;
}

static int parse_options(int argc, char **argv, struct options *options,
			 unsigned int *selected)
{
	int i;

	memset(options, 0, sizeof(*options));
	*selected = 0;
	for (i = 2; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "-a") || !strcmp(arg, "--all")) {
			options->all = 1;
		} else if (!strcmp(arg, "-r") || !strcmp(arg, "--raw")) {
			options->raw = 1;
		} else if (!strcmp(arg, "-1") || !strcmp(arg, "--once")) {
			options->once = 1;
		} else if (!strcmp(arg, "-l") || !strcmp(arg, "--lookup")) {
			if (options->lookup || i + 1 >= argc ||
			    parse_addr(argv[++i], &options->address))
				return -1;
			options->lookup = 1;
		} else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			return 1;
		} else if (!strcmp(arg, "all")) {
			*selected = (1U << firewall_count) - 1;
		} else if (select_word(arg, selected)) {
			return -1;
		}
	}
	if (!*selected)
		*selected = (1U << firewall_count) - 1;
	return 0;
}

static int read_one(int fd, uint64_t addr, uint32_t *value)
{
	int rc = read_word(fd, addr, TENSTORRENT_NOC_FLAG_KLA, value);

	if (rc) {
		char text[32];

		format_addr(text, sizeof(text), addr, 8);
		fprintf(stderr, "NOC_READ at KLA %s failed: %s\n",
			text, strerror(-rc));
		if (rc == -ENODEV)
			fprintf(stderr,
				"The driver may not have acquired its BAR2 window; "
				"see the Keraunos probe warnings.\n");
		return 3;
	}
	return 0;
}

/*
 * A filter nobody has written still reads back non-zero: data_bus_width is
 * a read-only hardware field (0x3000 or 0x4000), and blocks with 8-byte
 * granularity read END's low three bits as ones. Neither means anything.
 */
#define CFG_HW_BITS		(CFG_BUS_WIDTH_MASK << CFG_BUS_WIDTH_SHIFT)
#define END_GRANULE_BITS	UINT64_C(0x7)

static void decode_filter(struct filter *f)
{
	f->config = ((uint64_t)f->raw[1] << 32) | f->raw[0];
	f->start = ((uint64_t)f->raw[3] << 32) | f->raw[2];
	f->end = ((uint64_t)f->raw[5] << 32) | f->raw[4];
	f->programmed = (f->config & ~CFG_HW_BITS) || f->start ||
			(f->end & ~END_GRANULE_BITS);
}

static int read_firewall(int fd, const struct firewall_desc *desc,
			 struct firewall_data *data, int once)
{
	unsigned int pass, i, word, passes = once ? 1 : 2;
	int rc;

	memset(data, 0, sizeof(*data));
	data->all_ones = 1;
	for (pass = 0; pass < passes; pass++) {
		for (i = 0; i < desc->filters; i++) {
			for (word = 0; word < WORDS_PER_FILTER; word++) {
				uint64_t addr = desc->base +
					(uint64_t)i * FILTER_STRIDE + word * 4;
				uint32_t value;

				rc = read_one(fd, addr, &value);
				if (rc)
					return rc;
				if (pass + 1 < passes)
					data->first[i][word] = value;
				else
					data->filter[i].raw[word] = value;
				if (value != UINT32_MAX)
					data->all_ones = 0;
			}
		}
	}
	for (i = 0; i < desc->filters; i++) {
		decode_filter(&data->filter[i]);
		if (!once && memcmp(data->first[i], data->filter[i].raw,
				    sizeof(data->first[i])))
			data->changed[i] = 1;
	}
	return 0;
}

static const char *perm_text(uint64_t config)
{
	switch (config & (CFG_READ_EN | CFG_WRITE_EN)) {
	case CFG_READ_EN | CFG_WRITE_EN:
		return "RW";
	case CFG_READ_EN:
		return "R-";
	case CFG_WRITE_EN:
		return "-W";
	default:
		return "--";
	}
}

/* END_ADDR is treated as inclusive; returns the first address past the window. */
static uint64_t window_limit(uint64_t end)
{
	return end == UINT64_MAX ? UINT64_MAX : end + 1;
}

static const char *describe_window(uint64_t start, uint64_t end, char *buf,
				   size_t bufsz)
{
	uint64_t hi = window_limit(end);
	const char *name;
	char a[48], b[48];

	if (start > end)
		return "matches nothing";
	if (start == 0 && end >= (UINT64_C(1) << 52) - 1)
		return "everything";
	if (start >= SMC_SPM_BASE && hi <= SMC_SPM_BASE + SMC_SPM_SIZE)
		return start == SMC_SPM_BASE && hi == SMC_SPM_BASE + SMC_SPM_SIZE ?
			"SMC SRAM (SPM)" : "part of SMC SRAM (SPM)";
	if (hi <= KLA_LIMIT) {
		name = describe_kla_range(start, hi, buf, bufsz);
		if (strcmp(name, "?"))
			return name;
		snprintf(a, sizeof(a), "%s", describe_kla_addr(start, buf, bufsz));
		snprintf(b, sizeof(b), "%s", describe_kla_addr(end, buf, bufsz));
		if (!strcmp(a, b))
			snprintf(buf, bufsz, "within %s", a);
		else
			snprintf(buf, bufsz, "%s .. %s", a, b);
		return buf;
	}
	if (start >= KERAUNOS_SPA_BASE && hi <= KERAUNOS_SPA_END) {
		name = describe_spa_range(start, hi, buf, bufsz);
		if (strcmp(name, "?"))
			return name;
		snprintf(a, sizeof(a), "%s", describe_spa_addr(start, buf, bufsz));
		snprintf(b, sizeof(b), "%s", describe_spa_addr(end, buf, bufsz));
		snprintf(buf, bufsz, "SPA %s .. %s", a, b);
		return buf;
	}
	name = describe_package_spa(start);
	if (strcmp(name, "?")) {
		snprintf(buf, bufsz, "from %s", name);
		return buf;
	}
	if (start < KLA_LIMIT) {
		snprintf(buf, bufsz, "from %s upward",
			 describe_kla_addr(start, a, sizeof(a)));
		return buf;
	}
	return "?";
}

static void print_filter(unsigned int index, const struct filter *f,
			 const struct options *options)
{
	char start[32], end[32], src[8], what[128];

	format_addr(start, sizeof(start), f->start, 16);
	format_addr(end, sizeof(end), f->end, 16);
	if ((f->config >> CFG_SRC_ID_SHIFT) & CFG_SRC_ID_MASK)
		snprintf(src, sizeof(src), "%llu",
			 (unsigned long long)((f->config >> CFG_SRC_ID_SHIFT) &
					      CFG_SRC_ID_MASK));
	else
		snprintf(src, sizeof(src), "*");
	printf("  %2u  %s .. %s  %s  %-3s %-3s %llu   %-5s %-3s %-4s  %s\n",
	       index, start, end, perm_text(f->config),
	       f->config & CFG_ALLOW_NS ? "ns" : "S",
	       src,
	       (unsigned long long)((f->config >> CFG_GROUP_ID_SHIFT) &
				    CFG_GROUP_ID_MASK),
	       f->config & CFG_ALLOW_BURST ? "burst" : "-",
	       f->config & CFG_ADDR_MODE ? "en" : "am0",
	       f->config & CFG_LOCKED ? "lock" : "-",
	       f->programmed ? describe_window(f->start, f->end, what,
					       sizeof(what)) :
			       "unprogrammed");
	if (options->raw)
		printf("      raw cfg=%08X_%08X start=%08X_%08X end=%08X_%08X\n",
		       f->raw[1], f->raw[0], f->raw[3], f->raw[2],
		       f->raw[5], f->raw[4]);
}

static void print_firewall(const struct firewall_desc *desc,
			   const struct firewall_data *data,
			   const struct options *options)
{
	char base[32];
	unsigned int i;

	format_addr(base, sizeof(base), desc->base, 8);
	printf("\n%-14s @ KLA %s  %s  (%u filters)\n",
	       desc->name, base, desc->what, desc->filters);
	if (data->all_ones) {
		printf("  every word read as 0xFFFF_FFFF; block unreachable "
		       "(error completion) or its fabric is in reset\n");
		return;
	}
	printf("   #  start                  .. end                     "
	       "perm ns  src grp burst mode lock  what\n");
	i = 0;
	while (i < desc->filters) {
		unsigned int first, last;

		if (data->filter[i].programmed || options->all ||
		    options->raw) {
			print_filter(i, &data->filter[i], options);
			i++;
			continue;
		}
		first = i;
		while (i + 1 < desc->filters && !data->filter[i + 1].programmed)
			i++;
		last = i++;
		if (first == last)
			printf("  %2u  unprogrammed\n", first);
		else
			printf("  %2u-%-2u unprogrammed\n", first, last);
	}
}

static int filter_covers(const struct filter *f, uint64_t addr)
{
	return f->programmed && f->start <= f->end &&
	       addr >= f->start && addr <= f->end;
}

static void print_lookup(const struct firewall_desc *desc,
			 const struct firewall_data *data, uint64_t addr)
{
	unsigned int i, matches = 0, first = 0;
	uint64_t union_cfg = 0;
	int any_locked = 0;

	printf("  %-14s ", desc->name);
	if (data->all_ones) {
		printf("unreadable (all-ones)\n");
		return;
	}
	for (i = 0; i < desc->filters; i++) {
		const struct filter *f = &data->filter[i];

		if (!filter_covers(f, addr))
			continue;
		if (!matches)
			first = i;
		matches++;
		union_cfg |= f->config & (CFG_READ_EN | CFG_WRITE_EN |
					  CFG_ALLOW_NS);
		if (f->config & CFG_LOCKED)
			any_locked = 1;
	}
	if (!matches) {
		printf("no filter covers it -> denied (default deny)\n");
		return;
	}
	printf("lowest index: #%-2u %s %-2s | any-permits: %s %-2s | %u matching:",
	       first, perm_text(data->filter[first].config),
	       data->filter[first].config & CFG_ALLOW_NS ? "ns" : "S",
	       perm_text(union_cfg), union_cfg & CFG_ALLOW_NS ? "ns" : "S",
	       matches);
	for (i = 0; i < desc->filters; i++)
		if (filter_covers(&data->filter[i], addr))
			printf(" %u", i);
	if (any_locked)
		printf(" (locked)");
	printf("\n");
}

static void collect_warnings(const struct firewall_desc *desc,
			     const struct firewall_data *data,
			     struct warnings *warnings)
{
	unsigned int i, j, permissive = 0;

	if (data->all_ones) {
		add_warning(warnings, "%s read as all-ones; its registers are "
			    "unreachable or the fabric is in reset", desc->name);
		return;
	}
	for (i = 0; i < desc->filters; i++) {
		const struct filter *f = &data->filter[i];

		if (data->changed[i])
			add_warning(warnings, "%s filter %u changed between "
				    "the two reads; showing the second",
				    desc->name, i);
		if (!f->programmed)
			continue;
		if (f->config & ~CFG_KNOWN_BITS)
			add_warning(warnings, "%s filter %u has undefined "
				    "FILTER_CONFIG bits set (0x%016llX)",
				    desc->name, i,
				    (unsigned long long)(f->config &
							 ~CFG_KNOWN_BITS));
		if (f->start > f->end) {
			add_warning(warnings, "%s filter %u has start > end and "
				    "matches nothing", desc->name, i);
			continue;
		}
		if (!(f->config & (CFG_READ_EN | CFG_WRITE_EN)))
			add_warning(warnings, "%s filter %u covers a range "
				    "with neither read_en nor write_en "
				    "(explicit deny)", desc->name, i);
		else
			permissive++;
		if (!(f->config & CFG_ADDR_MODE))
			add_warning(warnings, "%s filter %u has addr_mode=0; "
				    "the RDL calls that disabled but the ITN "
				    "instance of this IP still matched",
				    desc->name, i);
	}
	if (!permissive)
		add_warning(warnings, "%s permits nothing: every access "
			    "through it is denied", desc->name);

	for (i = 0; i < desc->filters; i++) {
		const struct filter *a = &data->filter[i];

		if (!a->programmed || a->start > a->end)
			continue;
		for (j = i + 1; j < desc->filters; j++) {
			const struct filter *b = &data->filter[j];
			uint64_t lo, hi;
			uint64_t a_perm, b_perm;
			char lo_text[32], hi_text[32];

			if (!b->programmed || b->start > b->end)
				continue;
			lo = a->start > b->start ? a->start : b->start;
			hi = a->end < b->end ? a->end : b->end;
			if (lo > hi)
				continue;
			a_perm = a->config & (CFG_READ_EN | CFG_WRITE_EN |
					      CFG_ALLOW_NS);
			b_perm = b->config & (CFG_READ_EN | CFG_WRITE_EN |
					      CFG_ALLOW_NS);
			if (a_perm == b_perm)
				continue;
			/* The bring-up pair (ns + secure-only, same R/W) is expected. */
			if ((a_perm ^ b_perm) == CFG_ALLOW_NS)
				continue;
			format_addr(lo_text, sizeof(lo_text), lo, 8);
			format_addr(hi_text, sizeof(hi_text), hi, 8);
			add_warning(warnings, "%s filters %u (%s) and %u (%s) "
				    "overlap at %s .. %s with different "
				    "permissions; which wins is not pinned",
				    desc->name, i, perm_text(a->config), j,
				    perm_text(b->config), lo_text, hi_text);
		}
	}
}

static int open_device(int device_id, char *path, size_t path_size)
{
#ifdef KERAUNOS_FIREWALL_FAKE
	snprintf(path, path_size, "fake:/dev/tenstorrent/%d", device_id);
	return 0;
#else
	int fd;

	snprintf(path, path_size, "/dev/tenstorrent/%d", device_id);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
		return -1;
	}
	return fd;
#endif
}

static int check_device(int fd, const char *path)
{
#ifdef KERAUNOS_FIREWALL_FAKE
	(void)fd;
	(void)path;
	return 0;
#else
	struct tenstorrent_get_device_info info = { 0 };

	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "GET_DEVICE_INFO failed for %s: %s\n",
			path, strerror(errno));
		return 3;
	}
	if (info.out.device_id != PCI_DEVICE_ID_KERAUNOS) {
		fprintf(stderr, "%s is PCI device 0x%04X, not Keraunos "
			"(0x%04X).\n", path, info.out.device_id,
			PCI_DEVICE_ID_KERAUNOS);
		return 2;
	}
	return 0;
#endif
}

static void close_device(int fd)
{
#ifdef KERAUNOS_FIREWALL_FAKE
	(void)fd;
#else
	if (fd >= 0)
		close(fd);
#endif
}

int main(int argc, char **argv)
{
	static struct firewall_data data[FIREWALL_MAX];
	struct options options;
	struct warnings warnings = { { { 0 } }, 0 };
	char device_path[PATH_MAX];
	unsigned int selected, i;
	int device_id, fd, rc;

	build_inventory();

	if (argc == 2 &&
	    (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return 0;
	}
	if (argc < 2 || parse_device_id(argv[1], &device_id)) {
		usage(argv[0]);
		return 2;
	}
	rc = parse_options(argc, argv, &options, &selected);
	if (rc) {
		if (rc < 0)
			fprintf(stderr, "%s: invalid or conflicting arguments\n",
				argv[0]);
		usage(argv[0]);
		return rc > 0 ? 0 : 2;
	}

	fd = open_device(device_id, device_path, sizeof(device_path));
	if (fd < 0)
		return 3;
	rc = check_device(fd, device_path);
	if (rc) {
		close_device(fd);
		return rc;
	}

	for (i = 0; i < firewall_count; i++) {
		if (!(selected & (1U << i)))
			continue;
		rc = read_firewall(fd, &firewalls[i], &data[i], options.once);
		if (rc) {
			close_device(fd);
			return rc;
		}
	}
	close_device(fd);

	if (options.lookup) {
		char addr[32], what[128];

		format_addr(addr, sizeof(addr), options.address,
			    options.address < KLA_LIMIT ? 8 : 16);
		if (options.address >= SMC_SPM_BASE &&
		    options.address < SMC_SPM_BASE + SMC_SPM_SIZE)
			snprintf(what, sizeof(what), "SMC SRAM (SPM)");
		else if (options.address < KLA_LIMIT) {
			char scratch[64];

			snprintf(what, sizeof(what), "%s",
				 describe_kla_addr(options.address, scratch,
						   sizeof(scratch)));
		} else
			snprintf(what, sizeof(what), "%s",
				 describe_package_spa(options.address));
		printf("%s %s  (%s)\n",
		       options.address < KLA_LIMIT ? "KLA" : "address", addr,
		       what);
		printf("  perm is R/W; ns = non-secure allowed, S = secure only\n");
		for (i = 0; i < firewall_count; i++) {
			if (selected & (1U << i))
				print_lookup(&firewalls[i], &data[i],
					     options.address);
		}
	} else {
		printf("Keraunos fabric firewalls (read via BAR2/SYSIN0 entry 13)\n"
		       "perm: R/W permitted; ns: non-secure allowed, S: secure "
		       "only; src: src_id (* = any);\n"
		       "mode: addr_mode (am0 = 0); end is shown as programmed "
		       "(treated as inclusive)\n");
		for (i = 0; i < firewall_count; i++) {
			if (selected & (1U << i))
				print_firewall(&firewalls[i], &data[i],
					       &options);
		}
	}
	for (i = 0; i < firewall_count; i++) {
		if (selected & (1U << i))
			collect_warnings(&firewalls[i], &data[i], &warnings);
	}
	print_warnings(&warnings);
	return warnings.count ? 1 : 0;
}
