// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Dump and trace the Keraunos PCIe inbound and outbound TLBs.
//
// To compile:
//   gcc -O2 -Wall -Wextra -o keraunos_tlb tools/keraunos_tlb.c

#include <errno.h>
#include <ctype.h>
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

#define TLBCFG_BASE		UINT64_C(0x18040000)
#define IOMMU_CXT_ID		(TLBCFG_BASE + UINT64_C(0xfff4))
#define ACCESS_CTRL		(TLBCFG_BASE + UINT64_C(0xfff8))
#define SYSTEM_STATUS		(TLBCFG_BASE + UINT64_C(0xfffc))
#define INBOUND_PA_MASK		UINT64_C(0x000fffffffffffff)
#define INVALID_OUTPUT		UINT64_C(0xf000000000000000)

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

enum table_id {
	T_APPIN0,
	T_APPIN1,
	T_SYSIN0,
	T_APPOUT0,
	T_APPOUT1,
	T_SYSOUT0,
	TABLE_COUNT
};

struct table_desc {
	const char *name;
	const char *bar;
	uint32_t offset;
	unsigned int entries;
	uint64_t window;
	unsigned int index_hi;
	unsigned int index_lo;
	int inbound;
	int app;
};

static const struct table_desc tables[TABLE_COUNT] = {
	[T_APPIN0] = {
		"APPIN0", "BAR0", 0x4000, 256, UINT64_C(0x01000000),
		31, 24, 1, 1
	},
	[T_APPIN1] = {
		"APPIN1", "BAR4", 0x8000, 64, UINT64_C(0x200000000),
		38, 33, 1, 1
	},
	[T_SYSIN0] = {
		"SYSIN0", "BAR2", 0x3000, 64, UINT64_C(0x00004000),
		19, 14, 1, 0
	},
	[T_APPOUT0] = {
		"APPOUT0", NULL, 0x1000, 16, UINT64_C(0x100000000000),
		47, 44, 0, 1
	},
	[T_APPOUT1] = {
		"APPOUT1", NULL, 0x2000, 16, UINT64_C(0x00010000),
		19, 16, 0, 1
	},
	[T_SYSOUT0] = {
		"SYSOUT0", NULL, 0x0000, 16, UINT64_C(0x00010000),
		19, 16, 0, 0
	},
};

struct tlb_entry {
	uint32_t raw[2];
	uint64_t pa;
	uint32_t attr[8];
	int valid;
};

struct table_data {
	struct tlb_entry entry[256];
	uint32_t first[256][2];
	unsigned char changed[256];
};

enum trace_mode {
	TRACE_NONE,
	TRACE_BAR,
	TRACE_FIND,
	TRACE_OUT
};

struct options {
	unsigned int selected;
	int valid_only;
	int attr;
	int raw;
	int no_coalesce;
	int no_remap;
	enum trace_mode trace;
	int bar;
	int out_sys;
	uint64_t address;
};

struct warnings {
	char text[512][240];
	unsigned int count;
};

#ifndef KERAUNOS_TLB_FAKE
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
#endif

#ifdef KERAUNOS_TLB_FAKE
static uint32_t fake_tlb[0x10000 / 4];
static struct raw_entry fake_remap[ENTRY_COUNT];
static int fake_initialized;

static void fake_set_tlb(enum table_id id, unsigned int entry, uint64_t pa,
			 int valid)
{
	size_t word = (tables[id].offset + entry * 0x40) / 4;

	fake_tlb[word] = (uint32_t)pa | (valid ? 1U : 0U);
	fake_tlb[word + 1] = (uint32_t)(pa >> 32);
	if (valid)
		fake_tlb[word + 8] = id == T_SYSOUT0 ? UINT32_MAX :
				   (tables[id].inbound ? 0x00000003 : 0);
}

static void fake_set_remap(int entry, uint64_t start, uint64_t end,
			   uint64_t replace)
{
	fake_remap[entry].reg[0] = (uint32_t)(start >> ADDRESS_SHIFT);
	fake_remap[entry].reg[1] = (uint32_t)(end >> ADDRESS_SHIFT);
	fake_remap[entry].reg[2] = (uint32_t)(replace >> ADDRESS_SHIFT) |
				  ENABLE_MASK;
}

static void fake_init(void)
{
	static const uint64_t remap_values[12][3] = {
		{ UINT64_C(0x1200000000), UINT64_C(0x1202000000), 0 },
		{ UINT64_C(0x1202000000), UINT64_C(0x1204000000),
		  UINT64_C(0x08000000) },
		{ UINT64_C(0x121c000000), UINT64_C(0x121d000000),
		  UINT64_C(0x10000000) },
		{ UINT64_C(0x1204000000), UINT64_C(0x1208000000),
		  UINT64_C(0x20000000) },
		{ UINT64_C(0x1208000000), UINT64_C(0x120a800000),
		  UINT64_C(0x24000000) },
		{ UINT64_C(0x120b000000), UINT64_C(0x120c000000),
		  UINT64_C(0x27000000) },
		{ UINT64_C(0x120c000000), UINT64_C(0x120e800000),
		  UINT64_C(0x28000000) },
		{ UINT64_C(0x120f000000), UINT64_C(0x1210000000),
		  UINT64_C(0x2b000000) },
		{ UINT64_C(0x1210000000), UINT64_C(0x1212800000),
		  UINT64_C(0x2c000000) },
		{ UINT64_C(0x1213000000), UINT64_C(0x1214000000),
		  UINT64_C(0x2f000000) },
		{ UINT64_C(0x1214000000), UINT64_C(0x1216800000),
		  UINT64_C(0x30000000) },
		{ UINT64_C(0x1217000000), UINT64_C(0x1218000000),
		  UINT64_C(0x33000000) },
	};
	unsigned int i;

	if (fake_initialized)
		return;
	fake_initialized = 1;
	memset(fake_tlb, 0, sizeof(fake_tlb));
	memset(fake_remap, 0, sizeof(fake_remap));

	for (i = 0; i < 4; i++)
		fake_set_tlb(T_APPIN0, i, UINT64_C(0x10000000000) +
			     (uint64_t)i * tables[T_APPIN0].window, 1);
	for (i = 64; i < 68; i++)
		fake_set_tlb(T_APPIN0, i, UINT64_C(0x1280000000) +
			     (uint64_t)(i - 64) * tables[T_APPIN0].window, 1);
	fake_set_tlb(T_APPIN0, 68, UINT64_C(0x1200000000), 1);
	fake_set_tlb(T_APPIN0, 69, UINT64_C(0x1202000000), 1);
	fake_set_tlb(T_APPIN0, 164, UINT64_C(0x121c000000), 1);

	for (i = 0; i < 4; i++)
		fake_set_tlb(T_APPIN1, i, UINT64_C(0x1000000000000) +
			     (uint64_t)i * tables[T_APPIN1].window, 1);

	fake_set_tlb(T_SYSIN0, 0, UINT64_C(0x18000000), 1);
	fake_set_tlb(T_SYSIN0, 1, UINT64_C(0x18040000), 1);
	fake_set_tlb(T_SYSIN0, 2, UINT64_C(0x18044000), 1);
	fake_set_tlb(T_SYSIN0, 3, UINT64_C(0x18048000), 1);
	fake_set_tlb(T_SYSIN0, 12, UINT64_C(0x1202018000), 1);
	fake_set_tlb(T_SYSIN0, 13, UINT64_C(0x18040000), 1);

	for (i = 0; i < 16; i++)
		fake_set_tlb(T_APPOUT0, i, (uint64_t)i << 44, 1);
	fake_set_tlb(T_SYSOUT0, 0, 0, 1);
	for (i = 0; i < 8; i++)
		fake_tlb[(tables[T_SYSOUT0].offset + 0x20) / 4 + i] =
			UINT32_MAX;

	fake_tlb[(ACCESS_CTRL - TLBCFG_BASE) / 4] = 0x00010001;
	fake_tlb[(SYSTEM_STATUS - TLBCFG_BASE) / 4] = 1;
	fake_tlb[(IOMMU_CXT_ID - TLBCFG_BASE) / 4] = 0;
	for (i = 0; i < 12; i++)
		fake_set_remap(i, remap_values[i][0], remap_values[i][1],
			       remap_values[i][2]);
}

static int fake_read32(int fd, uint64_t addr, uint32_t flags, uint32_t *value)
{
	uint64_t off;
	unsigned int entry, reg;

	(void)fd;
	(void)flags;
	fake_init();
	if (addr >= TLBCFG_BASE && addr < TLBCFG_BASE + UINT64_C(0x10000)) {
		*value = fake_tlb[(addr - TLBCFG_BASE) / 4];
		return 0;
	}
	if (addr >= PCIE_REMAP_KLA_BASE &&
	    addr < PCIE_REMAP_KLA_BASE + UINT64_C(0x100)) {
		off = addr - PCIE_REMAP_KLA_BASE;
		entry = (unsigned int)(off / ENTRY_STRIDE);
		reg = (unsigned int)(off % ENTRY_STRIDE);
		if (reg == 0)
			*value = fake_remap[entry].reg[0];
		else if (reg == 8)
			*value = fake_remap[entry].reg[1];
		else if (reg == 12)
			*value = fake_remap[entry].reg[2];
		else
			return -EINVAL;
		return 0;
	}
	return -EIO;
}

static addrmap_read32_fn read_word = fake_read32;
#else
static addrmap_read32_fn read_word = real_read32;
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
	fprintf(stderr,
		"Usage: %s <device_id> [tables] [options]\n"
		"\n"
		"Tables: in out app sys appin0 appin1 sysin0 "
		"appout0 appout1 sysout0\n"
		"\n"
		"Options:\n"
		"  -v, --valid-only       skip invalid entries\n"
		"  -A, --attr             print eight attr words\n"
		"  -r, --raw              print raw address words\n"
		"  -n, --no-coalesce      print one line per entry\n"
		"  -R, --no-remap         do not read or annotate PCIE_REMAP\n"
		"      --bar <0|2|4> ADDR trace one BAR offset\n"
		"      --find ADDR        find inbound BAR offsets\n"
		"      --out ADDR         trace APP outbound translation\n"
		"      --out --sys ADDR   trace SYS outbound translation\n"
		"  -h, --help             show this help\n",
		prog);
}

static int table_word(const char *word)
{
	int i;

	for (i = 0; i < TABLE_COUNT; i++) {
		char lower[16];
		size_t j;

		for (j = 0; tables[i].name[j] && j + 1 < sizeof(lower); j++)
			lower[j] =
				(char)tolower((unsigned char)tables[i].name[j]);
		lower[j] = '\0';
		if (!strcmp(word, lower))
			return i;
	}
	return -1;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	unsigned int exact = 0;
	int have_direction = 0, want_in = 0, want_out = 0;
	int have_path = 0, want_app = 0, want_sys = 0;
	int i;

	memset(options, 0, sizeof(*options));
	for (i = 2; i < argc; i++) {
		const char *arg = argv[i];
		int table = table_word(arg);

		if (table >= 0) {
			exact |= 1U << table;
		} else if (!strcmp(arg, "in")) {
			have_direction = want_in = 1;
		} else if (!strcmp(arg, "out")) {
			have_direction = want_out = 1;
		} else if (!strcmp(arg, "app")) {
			have_path = want_app = 1;
		} else if (!strcmp(arg, "sys")) {
			have_path = want_sys = 1;
		} else if (!strcmp(arg, "-v") ||
			   !strcmp(arg, "--valid-only")) {
			options->valid_only = 1;
		} else if (!strcmp(arg, "-A") || !strcmp(arg, "--attr")) {
			options->attr = 1;
		} else if (!strcmp(arg, "-r") || !strcmp(arg, "--raw")) {
			options->raw = 1;
		} else if (!strcmp(arg, "-n") ||
			   !strcmp(arg, "--no-coalesce")) {
			options->no_coalesce = 1;
		} else if (!strcmp(arg, "-R") ||
			   !strcmp(arg, "--no-remap")) {
			options->no_remap = 1;
		} else if (!strcmp(arg, "--bar")) {
			char *end;
			long bar;

			if (options->trace != TRACE_NONE || i + 2 >= argc)
				return -1;
			errno = 0;
			bar = strtol(argv[++i], &end, 10);
			if (errno || *end || (bar != 0 && bar != 2 && bar != 4) ||
			    parse_addr(argv[++i], &options->address))
				return -1;
			options->bar = (int)bar;
			options->trace = TRACE_BAR;
		} else if (!strcmp(arg, "--find")) {
			if (options->trace != TRACE_NONE || i + 1 >= argc ||
			    parse_addr(argv[++i], &options->address))
				return -1;
			options->trace = TRACE_FIND;
		} else if (!strcmp(arg, "--out")) {
			if (options->trace != TRACE_NONE || i + 1 >= argc)
				return -1;
			if (!strcmp(argv[i + 1], "--sys")) {
				options->out_sys = 1;
				i++;
				if (i + 1 >= argc)
					return -1;
			}
			if (parse_addr(argv[++i], &options->address))
				return -1;
			options->trace = TRACE_OUT;
		} else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			return 1;
		} else {
			return -1;
		}
	}

	if (options->trace != TRACE_NONE) {
		if (exact || have_direction || have_path || options->valid_only ||
		    options->attr || options->raw || options->no_coalesce ||
		    options->no_remap)
			return -1;
		return 0;
	}

	if (!exact && !have_direction && !have_path) {
		options->selected = (1U << TABLE_COUNT) - 1;
		return 0;
	}
	options->selected = exact;
	if (have_direction || have_path) {
		for (i = 0; i < TABLE_COUNT; i++) {
			int direction_match = !have_direction ||
				(tables[i].inbound ? want_in : want_out);
			int path_match = !have_path ||
				(tables[i].app ? want_app : want_sys);

			if (direction_match && path_match)
				options->selected |= 1U << i;
		}
	}
	return options->selected ? 0 : -1;
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

static void decode_tlb_entry(struct tlb_entry *entry)
{
	entry->valid = !!(entry->raw[0] & 1);
	entry->pa = ((uint64_t)entry->raw[1] << 32) |
		    (entry->raw[0] & ~UINT32_C(1));
}

static int read_tlb_table(int fd, enum table_id id, struct table_data *data,
			  int attr)
{
	const struct table_desc *desc = &tables[id];
	unsigned int pass, i, word;
	int rc;

	memset(data, 0, sizeof(*data));
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < desc->entries; i++) {
			for (word = 0; word < 2; word++) {
				uint64_t addr = TLBCFG_BASE + desc->offset +
					(uint64_t)i * 0x40 + word * 4;
				uint32_t value;

				rc = read_one(fd, addr, &value);
				if (rc)
					return rc;
				if (!pass)
					data->first[i][word] = value;
				else
					data->entry[i].raw[word] = value;
			}
		}
	}
	for (i = 0; i < desc->entries; i++) {
		decode_tlb_entry(&data->entry[i]);
		if ((id != T_SYSIN0 || i != 13) &&
		    (id != T_APPIN0 || i != 164) &&
		    (data->first[i][0] != data->entry[i].raw[0] ||
		     data->first[i][1] != data->entry[i].raw[1]))
			data->changed[i] = 1;
	}
	if (!attr)
		return 0;
	for (i = 0; i < desc->entries; i++) {
		if (!data->entry[i].valid)
			continue;
		for (word = 0; word < 8; word++) {
			uint64_t addr = TLBCFG_BASE + desc->offset +
					(uint64_t)i * 0x40 + 0x20 + word * 4;

			rc = read_one(fd, addr, &data->entry[i].attr[word]);
			if (rc)
				return rc;
		}
	}
	return 0;
}

static int read_remap(int fd, struct remap_entry decoded[ENTRY_COUNT])
{
	struct raw_entry raw[ENTRY_COUNT];
	uint64_t failed = 0;
	int rc = read_pcie_remap_table(fd, 0, read_word, raw, &failed);

	if (rc) {
		char text[32];

		format_addr(text, sizeof(text), failed, 8);
		fprintf(stderr, "NOC_READ at KLA %s failed: %s\n",
			text, strerror(-rc));
		if (rc == -ENODEV)
			fprintf(stderr,
				"The driver may not have acquired its BAR2 window; "
				"see the Keraunos probe warnings.\n");
		return 3;
	}
	decode_table(raw, decoded);
	return 0;
}

static uint64_t inbound_base(const struct table_desc *desc,
			     const struct tlb_entry *entry)
{
	return (entry->pa & INBOUND_PA_MASK) & ~(desc->window - 1);
}

static uint64_t outbound_base(const struct table_desc *desc,
			      const struct tlb_entry *entry)
{
	return entry->pa & ~(desc->window - 1);
}

static int is_driver_scratch(enum table_id id, unsigned int entry)
{
	return (id == T_SYSIN0 && entry == 13) ||
	       (id == T_APPIN0 && entry == 164);
}

static const char *tlbcfg_name(uint64_t kla)
{
	if (kla >= TLBCFG_BASE && kla < TLBCFG_BASE + 0x4000)
		return "TLBCFG (SYSOUT0/APPOUT0/APPOUT1/SYSIN0)";
	if (kla >= TLBCFG_BASE + 0x4000 && kla < TLBCFG_BASE + 0x8000)
		return "TLBCFG (APPIN0)";
	if (kla >= TLBCFG_BASE + 0x8000 && kla < TLBCFG_BASE + 0xc000)
		return "TLBCFG (APPIN1)";
	if (kla >= TLBCFG_BASE && kla < TLBCFG_BASE + 0x10000)
		return "TLBCFG controls";
	return NULL;
}

static const char *kla_name(uint64_t kla, char *buf, size_t bufsz)
{
	const char *name = tlbcfg_name(kla);

	if (name)
		return name;
	if (kla == UINT64_C(0x18000000))
		return "MSI relay";
	if (kla == UINT64_C(0x08018000))
		return "SMC mailbox";
	return describe_kla_addr(kla, buf, bufsz);
}

static void print_attr(const struct tlb_entry *entry)
{
	unsigned int i;

	printf("      attr");
	for (i = 0; i < 8; i++)
		printf(" [%u]=0x%08X", i, entry->attr[i]);
	printf("\n");
}

static void print_raw(const struct tlb_entry *entry)
{
	printf("  raw +0x00=0x%08X +0x04=0x%08X",
	       entry->raw[0], entry->raw[1]);
}

/* Entry number or "first-last", padded to a fixed column. */
static void print_entry_label(unsigned int first, unsigned int last)
{
	char label[16];

	if (first == last)
		snprintf(label, sizeof(label), "%u", first);
	else
		snprintf(label, sizeof(label), "%u-%u", first, last);
	printf("  %-8s", label);
}

static void print_invalid_run(unsigned int first, unsigned int last)
{
	print_entry_label(first, last);
	printf("invalid\n");
}

static int same_valid_run(const struct table_desc *desc,
			  const struct tlb_entry *a,
			  const struct tlb_entry *b)
{
	uint64_t first = desc->inbound ? inbound_base(desc, a) :
					 outbound_base(desc, a);
	uint64_t second = desc->inbound ? inbound_base(desc, b) :
					  outbound_base(desc, b);

	return a->valid && b->valid && second == first + desc->window;
}

static int sysin_path(uint64_t value,
		      const struct remap_entry remap[ENTRY_COUNT],
		      int no_remap, uint64_t *kla);

static int same_sysin_annotation(const struct table_desc *desc,
				 const struct tlb_entry *a,
				 const struct tlb_entry *b,
				 const struct remap_entry remap[ENTRY_COUNT],
				 int no_remap)
{
	char first_buf[80], second_buf[80];
	const char *first_name, *second_name;
	uint64_t first_kla, second_kla;
	int first_path, second_path;

	if (no_remap)
		return 1;
	first_path = sysin_path(inbound_base(desc, a), remap, 0, &first_kla);
	second_path = sysin_path(inbound_base(desc, b), remap, 0, &second_kla);
	if (first_path != second_path)
		return 0;
	if (first_path != 1 && first_path != 2)
		return 1;
	first_name = kla_name(first_kla, first_buf, sizeof(first_buf));
	second_name = kla_name(second_kla, second_buf, sizeof(second_buf));
	return !strcmp(first_name, second_name);
}

static void print_appin_line(enum table_id id, const struct table_desc *desc,
			     const struct tlb_entry *entry, unsigned int first,
			     unsigned int last,
			     const struct remap_entry remap[ENTRY_COUNT],
			     const struct options *options)
{
	char bar_lo[32], bar_hi[32], value[32], kla[32], annotation[80];
	uint64_t bar_start = (uint64_t)first * desc->window;
	uint64_t bar_end = ((uint64_t)last + 1) * desc->window - 1;
	uint64_t base = inbound_base(desc, entry);
	int remap_index = options->no_remap ? -1 :
			  first_matching_entry(remap, base);

	format_addr(bar_lo, sizeof(bar_lo), bar_start, id == T_APPIN0 ? 8 : 10);
	format_addr(bar_hi, sizeof(bar_hi), bar_end, id == T_APPIN0 ? 8 : 10);
	format_addr(value, sizeof(value), base, 10);
	print_entry_label(first, last);
	printf("%-16s .. %-16s -> SPA %-18s%s",
	       bar_lo, bar_hi, value, first == last ? "" : " + off");
	if (options->no_remap) {
		printf("\n");
	} else if (remap_index >= 0) {
		uint64_t result = remap[remap_index].replace +
				  base - remap[remap_index].start;

		format_addr(kla, sizeof(kla), result, 8);
		printf(" -> KLA %-13s  %s", kla,
		       kla_name(result, annotation, sizeof(annotation)));
		if (is_driver_scratch(id, first))
			printf("  (driver scratch)");
		printf("\n");
	} else if (base >= KERAUNOS_SPA_BASE && base < KERAUNOS_SPA_END) {
		printf(" pass-through as SPA (NoC)  %s",
		       describe_spa_addr(base, annotation, sizeof(annotation)));
		if (is_driver_scratch(id, first))
			printf("  (driver scratch)");
		printf("\n");
	} else {
		printf(" %s", describe_package_spa(base));
		if (first != last)
			printf(", %llu MiB contiguous",
			       (unsigned long long)((last - first + 1) *
						    desc->window / MIB));
		if (is_driver_scratch(id, first))
			printf("  (driver scratch)");
		printf("\n");
	}
	if (options->raw)
		print_raw(entry), printf("\n");
	if (options->attr)
		print_attr(entry);
}

static int sysin_path(uint64_t value,
		      const struct remap_entry remap[ENTRY_COUNT],
		      int no_remap, uint64_t *kla)
{
	int index;

	if (no_remap) {
		*kla = value;
		return 0;
	}
	index = first_matching_entry(remap, value);
	if (index >= 0) {
		*kla = remap[index].replace + value - remap[index].start;
		return 2;
	}
	*kla = value;
	if (value < UINT64_C(0x100000000))
		return 1;
	if (value >= UINT64_C(0x1000000000) &&
	    value < UINT64_C(0x2000000000))
		return 3;
	return 4;
}

static void print_sysin_line(const struct table_desc *desc,
			     const struct tlb_entry *entry, unsigned int first,
			     unsigned int last,
			     const struct remap_entry remap[ENTRY_COUNT],
			     const struct options *options)
{
	static const char *const paths[] = {
		"", "KLA (remap miss)", "SPA, folded", "GLOBAL_CFG", "DEAD"
	};
	char bar_lo[32], bar_hi[32], value_text[32], kla_text[32], what[80];
	uint64_t bar_start = (uint64_t)first * desc->window;
	uint64_t bar_end = ((uint64_t)last + 1) * desc->window - 1;
	uint64_t value = inbound_base(desc, entry);
	uint64_t kla;
	int path = sysin_path(value, remap, options->no_remap, &kla);

	format_addr(bar_lo, sizeof(bar_lo), bar_start, 5);
	format_addr(bar_hi, sizeof(bar_hi), bar_end, 5);
	format_addr(value_text, sizeof(value_text), value, 8);
	print_entry_label(first, last);
	printf("%-13s .. %-13s -> %-16s", bar_lo, bar_hi, value_text);
	if (options->no_remap) {
		printf("\n");
	} else if (path == 2) {
		format_addr(kla_text, sizeof(kla_text), kla, 8);
		printf(" %-17s -> KLA %-13s  %s", paths[path], kla_text,
		       kla_name(kla, what, sizeof(what)));
		if (is_driver_scratch(T_SYSIN0, first))
			printf("  (driver scratch)");
		printf("\n");
	} else {
		const char *name;

		if (path == 3 && value >> 32 == 0x12)
			name = "Keraunos GLOBAL_CFG";
		else if (path == 3 && value >> 32 == 0x13)
			name = "Mimir GLOBAL_CFG";
		else if (path == 3 || path == 4)
			name = "?";
		else
			name = kla_name(kla, what, sizeof(what));
		printf(" %-17s %s", paths[path], name);
		if (is_driver_scratch(T_SYSIN0, first))
			printf("  (driver scratch)");
		printf("\n");
	}
	if (options->raw)
		print_raw(entry), printf("\n");
	if (options->attr)
		print_attr(entry);
}

static int appout0_identity(const struct table_data *data)
{
	unsigned int i;

	for (i = 0; i < 16; i++) {
		if (!data->entry[i].valid ||
		    outbound_base(&tables[T_APPOUT0], &data->entry[i]) !=
		    (uint64_t)i << 44)
			return 0;
	}
	return 1;
}

static int attr_all_ones(const struct tlb_entry *entry)
{
	unsigned int i;

	for (i = 0; i < 8; i++) {
		if (entry->attr[i] != UINT32_MAX)
			return 0;
	}
	return 1;
}

static void print_outbound_line(enum table_id id,
				const struct table_desc *desc,
				const struct tlb_entry *entry,
				unsigned int first, unsigned int last,
				const struct options *options)
{
	char input_lo[32], input_hi[32], target[32];
	uint64_t incoming, incoming_end, base = outbound_base(desc, entry);

	if (id == T_APPOUT0) {
		incoming = KERAUNOS_HOST_WINDOW +
			   (uint64_t)first * desc->window;
		incoming_end = KERAUNOS_HOST_WINDOW +
			       ((uint64_t)last + 1) * desc->window - 1;
	} else if (id == T_SYSOUT0) {
		incoming = UINT64_C(0x18400000) +
			   (uint64_t)first * desc->window;
		incoming_end = UINT64_C(0x18400000) +
			       ((uint64_t)last + 1) * desc->window - 1;
	} else {
		incoming = (uint64_t)first * desc->window;
		incoming_end = ((uint64_t)last + 1) * desc->window - 1;
	}
	format_addr(input_lo, sizeof(input_lo), incoming, id == T_APPOUT0 ? 13 : 8);
	format_addr(input_hi, sizeof(input_hi), incoming_end,
		    id == T_APPOUT0 ? 13 : 8);
	format_addr(target, sizeof(target), base, id == T_APPOUT0 ? 13 : 8);
	print_entry_label(first, last);
	printf("%-21s .. %-21s -> %s + off", input_lo, input_hi, target);
	if (id == T_SYSOUT0 && attr_all_ones(entry))
		printf("  (attr dbi set)");
	printf("\n");
	if (options->raw)
		print_raw(entry), printf("\n");
	if (options->attr)
		print_attr(entry);
}

static void print_table(enum table_id id, const struct table_data *data,
			const struct remap_entry remap[ENTRY_COUNT],
			const struct options *options)
{
	const struct table_desc *desc = &tables[id];
	unsigned int i = 0;
	int coalesce = !options->no_coalesce && !options->raw && !options->attr;

	printf("\n");
	if (id == T_APPIN0)
		printf("APPIN0  BAR0, 256 x 16 MiB\n"
		       "  #       BAR0 offset                      -> value / what\n");
	else if (id == T_APPIN1)
		printf("APPIN1  BAR4, 64 x 8 GiB\n"
		       "  #       BAR4 offset                      -> value / what\n");
	else if (id == T_SYSIN0)
		printf("SYSIN0  BAR2, 64 x 16 KiB\n"
		       "  #       BAR2 offset                   -> value            path / what\n");
	else if (id == T_APPOUT0)
		printf("APPOUT0  16 x 16 TiB, selected when SPA[63:48] != 0, "
		       "index SPA[47:44]\n"
		       "  #       device SPA                       -> host address\n");
	else if (id == T_APPOUT1)
		printf("APPOUT1  16 x 64 KiB, selected when SPA[63:48] == 0, "
		       "index SPA[19:16]\n"
		       "  #       device SPA                       -> host address\n");
	else
		printf("SYSOUT0  16 x 64 KiB, SMN 0x1840_0000 + n<<16, "
		       "index [19:16]\n"
		       "  #       SMN address                      -> target\n");

	if (id == T_APPOUT0 && coalesce && appout0_identity(data)) {
		printf("  0-15    0x2_0000_0000_0000 + n<<44 -> n<<44 + off  "
		       "identity: HPA = SPA[47:0]\n");
		return;
	}

	while (i < desc->entries) {
		unsigned int first = i, last = i;

		if (!data->entry[i].valid) {
			if (options->valid_only) {
				i++;
				continue;
			}
			if (!options->no_coalesce)
				while (last + 1 < desc->entries &&
				       !data->entry[last + 1].valid)
					last++;
			print_invalid_run(first, last);
			i = last + 1;
			continue;
		}
		if (coalesce && !is_driver_scratch(id, first))
			while (last + 1 < desc->entries &&
			       same_valid_run(desc, &data->entry[last],
					      &data->entry[last + 1]) &&
			       (id != T_SYSIN0 ||
				same_sysin_annotation(
					desc, &data->entry[last],
					&data->entry[last + 1], remap,
					options->no_remap)) &&
			       !is_driver_scratch(id, last + 1))
				last++;
		if (id == T_APPIN0 || id == T_APPIN1)
			print_appin_line(id, desc, &data->entry[first], first,
					 last, remap, options);
		else if (id == T_SYSIN0)
			print_sysin_line(desc, &data->entry[first], first, last,
					remap, options);
		else
			print_outbound_line(id, desc, &data->entry[first],
					    first, last, options);
		i = last + 1;
	}
}

static void collect_warnings(enum table_id id, const struct table_data *data,
			     const struct remap_entry remap[ENTRY_COUNT],
			     const struct options *options,
			     struct warnings *warnings)
{
	const struct table_desc *desc = &tables[id];
	unsigned int i;

	for (i = 0; i < desc->entries; i++) {
		const struct tlb_entry *entry = &data->entry[i];
		uint64_t base, kla;
		int path;

		if (data->changed[i])
			add_warning(warnings,
				    "%s entry %u changed between reads; showing "
				    "the second read", desc->name, i);
		if (!entry->valid)
			continue;
		if (desc->inbound && entry->pa & ~INBOUND_PA_MASK)
			add_warning(warnings,
				    "%s entry %u has pa[63:52] set (0x%llX); "
				    "hardware ignores those bits", desc->name, i,
				    (unsigned long long)(entry->pa >>
							 52));
		if (options->no_remap)
			continue;
		base = desc->inbound ? inbound_base(desc, entry) :
				      outbound_base(desc, entry);
		if (id == T_APPIN0 || id == T_APPIN1) {
			uint64_t end = base + desc->window - 1;

			if (base >= KERAUNOS_SPA_BASE &&
			    base < KERAUNOS_SPA_END &&
			    (first_matching_entry(remap, base) < 0 ||
			     first_matching_entry(remap, end) < 0))
				add_warning(warnings,
					    "%s entry %u points into a "
					    "PCIE_REMAP hole; traffic passes "
					    "through as SPA (NoC)",
					    desc->name, i);
		} else if (id == T_SYSIN0) {
			path = sysin_path(base, remap, 0, &kla);
			if (path == 4)
				add_warning(warnings,
					    "SYSIN0 entry %u has DEAD value "
					    "0x%llX; the SMN will reject it",
					    i, (unsigned long long)base);
		}
	}
	if (id == T_APPOUT0 && !appout0_identity(data))
		add_warning(warnings,
			    "APPOUT0 is not identity; tt-kmd assumes identity "
			    "host mappings");
}

static void collect_access_warnings(uint32_t access_ctrl,
				    unsigned int selected,
				    struct warnings *warnings)
{
	if ((selected & ((1U << T_APPIN0) | (1U << T_APPIN1))) &&
	    !(access_ctrl & (1U << 16)))
		add_warning(warnings,
			    "inbound APP is disabled in access_ctrl; BAR0/BAR4 "
			    "traffic is blocked regardless of the tables");
	if ((selected & ((1U << T_APPOUT0) | (1U << T_APPOUT1))) &&
	    !(access_ctrl & 1U))
		add_warning(warnings,
			    "outbound APP is disabled in access_ctrl; "
			    "device-to-host DMA is blocked regardless of "
			    "the tables");
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

static int bar_table(int bar)
{
	if (bar == 0)
		return T_APPIN0;
	if (bar == 2)
		return T_SYSIN0;
	return T_APPIN1;
}

static int trace_bar(const struct options *options,
		     const struct table_data data[TABLE_COUNT],
		     const struct remap_entry remap[ENTRY_COUNT])
{
	enum table_id id = (enum table_id)bar_table(options->bar);
	const struct table_desc *desc = &tables[id];
	uint64_t offset = options->address;
	unsigned int index;
	const struct tlb_entry *entry;
	uint64_t base, result, low;
	char input[32], value[32], output[32], low_text[32];
	char kla[32], start[32], end[32];
	char what[80];
	int remap_index;

	if (offset >= (uint64_t)desc->entries * desc->window) {
		fprintf(stderr, "%s offset is outside the %s aperture\n",
			desc->bar, desc->name);
		return 2;
	}
	index = (unsigned int)(offset / desc->window);
	entry = &data[id].entry[index];
	format_addr(input, sizeof(input), offset, id == T_SYSIN0 ? 5 :
		    (id == T_APPIN0 ? 8 : 10));
	printf("%s offset %s\n", desc->bar, input);
	if (id == T_SYSIN0)
		printf("  %s entry %u (offset bits [%u:%u] = %u), %s\n",
		       desc->name, index, desc->index_hi, desc->index_lo, index,
		       entry->valid ? "valid" : "invalid");
	else
		printf("  %s entry %u (offset bits [%u:%u] = 0x%X), %s\n",
		       desc->name, index, desc->index_hi, desc->index_lo, index,
		       entry->valid ? "valid" : "invalid");
	if (!entry->valid) {
		format_addr(output, sizeof(output), INVALID_OUTPUT, 16);
		printf("  invalid entry -> hardware emits %s\n", output);
		return 0;
	}
	base = inbound_base(desc, entry);
	low = offset & (desc->window - 1);
	result = base + low;
	format_addr(value, sizeof(value), base, id == T_SYSIN0 ? 8 : 10);
	format_addr(output, sizeof(output), result, id == T_SYSIN0 ? 8 : 10);
	format_addr(low_text, sizeof(low_text), low,
		    id == T_SYSIN0 ? 4 : (id == T_APPIN0 ? 8 : 9));
	printf("  value  %s%s, in-window offset %s\n",
	       id == T_SYSIN0 ? "" : "SPA ", value, low_text);
	if (id == T_SYSIN0)
		printf("  -> %s%s", output,
		       result >= UINT64_C(0x100000000) ? ", above 4 GiB" : "");
	else
		printf("  -> SPA %s", output);
	remap_index = first_matching_entry(remap, result);
	if (remap_index < 0) {
		printf(", PCIE_REMAP miss\n");
		if (id == T_SYSIN0 && result < UINT64_C(0x100000000))
			printf("  -> KLA %s  (%s)\n", output,
			       kla_name(result, what, sizeof(what)));
		else if (result >= UINT64_C(0x1000000000) &&
			 result < UINT64_C(0x2000000000))
			printf("  -> GLOBAL_CFG sideband\n");
		else
			printf("  -> pass-through as SPA%s\n",
			       id == T_SYSIN0 ? "; SMN rejects it" : " (NoC)");
		return 0;
	}
	format_addr(start, sizeof(start), remap[remap_index].start, 10);
	format_addr(end, sizeof(end), remap[remap_index].end, 10);
	format_addr(value, sizeof(value), remap[remap_index].replace, 8);
	if (id == T_SYSIN0) {
		printf(", PCIE_REMAP entry %d folds it\n", remap_index);
	} else {
		printf("\n  PCIE_REMAP entry %d (%s .. %s -> %s)\n",
		       remap_index, start, end, value);
	}
	result = remap[remap_index].replace +
		 result - remap[remap_index].start;
	format_addr(kla, sizeof(kla), result, 8);
	printf("  -> KLA %s  (%s)\n", kla,
	       kla_name(result, what, sizeof(what)));
	return 0;
}

static void print_find_match(enum table_id id, unsigned int index,
			     uint64_t offset, const char *how)
{
	char text[32];

	format_addr(text, sizeof(text), offset, id == T_SYSIN0 ? 5 :
		    (id == T_APPIN0 ? 8 : 10));
	printf("  %-4s offset %-16s %s entry %u (%s%s)\n",
	       tables[id].bar, text, tables[id].name, index, how,
	       is_driver_scratch(id, index) ? ", driver scratch" : "");
}

static int find_in_table(enum table_id id, const struct table_data *data,
			 uint64_t target, const char *how)
{
	const struct table_desc *desc = &tables[id];
	unsigned int i;
	int found = 0;

	for (i = 0; i < desc->entries; i++) {
		uint64_t base;

		if (!data->entry[i].valid)
			continue;
		base = inbound_base(desc, &data->entry[i]);
		if (target < base || target - base >= desc->window)
			continue;
		print_find_match(id, i, (uint64_t)i * desc->window +
				 target - base, how);
		found++;
	}
	return found;
}

static int trace_find(const struct options *options,
		      const struct table_data data[TABLE_COUNT],
		      const struct remap_entry remap[ENTRY_COUNT])
{
	char address[32], what[80], spa_text[32], how[96];
	uint64_t target = options->address;
	int found = 0, app_found = 0, i;

	format_addr(address, sizeof(address), target,
		    target < UINT64_C(0x100000000) ? 8 : 10);
	if (target == TLBCFG_BASE)
		printf("KLA %s  (TLBCFG)\n", address);
	else if (target < UINT64_C(0x100000000))
		printf("KLA %s  (%s)\n", address,
		       kla_name(target, what, sizeof(what)));
	else
		printf("SPA %s  (%s)\n", address,
		       target >= KERAUNOS_SPA_BASE && target < KERAUNOS_SPA_END ?
		       describe_spa_addr(target, what, sizeof(what)) :
		       describe_package_spa(target));

	if (target >= UINT64_C(0x100000000)) {
		int n;

		n = find_in_table(T_APPIN0, &data[T_APPIN0], target, "as SPA");
		found += n;
		app_found += n;
		n = find_in_table(T_APPIN1, &data[T_APPIN1], target, "as SPA");
		found += n;
		app_found += n;
	}
	for (i = 0; i < ENTRY_COUNT; i++) {
		uint64_t size, spa;
		int n;

		if (!remap[i].enabled || remap[i].start >= remap[i].end)
			continue;
		size = remap[i].end - remap[i].start;
		if (target < remap[i].replace ||
		    target - remap[i].replace >= size)
			continue;
		spa = remap[i].start + target - remap[i].replace;
		if (first_matching_entry(remap, spa) != i)
			continue;
		format_addr(spa_text, sizeof(spa_text), spa, 10);
		snprintf(how, sizeof(how), "as SPA %s, folded", spa_text);
		n = find_in_table(T_SYSIN0, &data[T_SYSIN0], spa, how);
		found += n;
		n = find_in_table(T_APPIN0, &data[T_APPIN0], spa, how);
		found += n;
		app_found += n;
		n = find_in_table(T_APPIN1, &data[T_APPIN1], spa, how);
		found += n;
		app_found += n;
	}
	if (target < UINT64_C(0x100000000))
		found += find_in_table(T_SYSIN0, &data[T_SYSIN0], target,
				       "raw KLA");
	if (!app_found && target < UINT64_C(0x100000000))
		printf("  not reachable through BAR0/BAR4: no PCIE_REMAP "
		       "entry covers PCIe mgmt\n");
	if (!found)
		printf("  not reachable through any inbound table\n");
	return 0;
}

static int trace_out(const struct options *options,
		     const struct table_data data[TABLE_COUNT])
{
	enum table_id id;
	const struct table_desc *desc;
	const struct tlb_entry *entry;
	unsigned int index;
	uint64_t base, result, low;
	char input[32], value[32], output[32];

	if (options->out_sys)
		id = T_SYSOUT0;
	else if (options->address >> 48)
		id = T_APPOUT0;
	else
		id = T_APPOUT1;
	desc = &tables[id];
	index = (unsigned int)((options->address >> desc->index_lo) &
			      (desc->entries - 1));
	entry = &data[id].entry[index];
	format_addr(input, sizeof(input), options->address,
		    id == T_APPOUT0 ? 13 : 8);
	printf("%s %s\n", options->out_sys ? "SMN address" : "device SPA",
	       input);
	if (id == T_APPOUT0)
		printf("  bits [63:48] = 0x%04llX != 0 -> APPOUT0, entry %u "
		       "(bits [47:44])\n",
		       (unsigned long long)(options->address >> 48), index);
	else
		printf("  %s, entry %u (bits [19:16])\n", desc->name, index);
	if (!entry->valid) {
		format_addr(output, sizeof(output), INVALID_OUTPUT, 16);
		printf("  invalid -> hardware emits %s\n", output);
		return 0;
	}
	base = outbound_base(desc, entry);
	low = options->address & (desc->window - 1);
	result = base + low;
	format_addr(value, sizeof(value), base, id == T_APPOUT0 ? 16 : 8);
	format_addr(output, sizeof(output), result, id == T_APPOUT0 ? 12 : 8);
	printf("  value %s, valid\n", value);
	printf("  -> %s %s", options->out_sys ? "target" : "host", output);
	if (!options->out_sys && id == T_APPOUT0 &&
	    result == (options->address & UINT64_C(0x0000ffffffffffff)))
		printf("  (identity)");
	printf("\n");
	return 0;
}

static int open_device(int device_id, char *path, size_t path_size)
{
#ifdef KERAUNOS_TLB_FAKE
	(void)device_id;
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
#ifdef KERAUNOS_TLB_FAKE
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

int main(int argc, char **argv)
{
	struct options options;
	struct table_data data[TABLE_COUNT];
	struct remap_entry remap[ENTRY_COUNT] = { 0 };
	struct warnings warnings = { { { 0 } }, 0 };
	char device_path[PATH_MAX];
	uint32_t access_ctrl = 0, status = 0, context = 0;
	unsigned int selected;
	int device_id, fd, rc, id, need_remap;

	if (argc == 2 &&
	    (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return 0;
	}
	if (argc < 2 || parse_device_id(argv[1], &device_id)) {
		usage(argv[0]);
		return 2;
	}
	rc = parse_options(argc, argv, &options);
	if (rc) {
		if (rc < 0)
			fprintf(stderr, "%s: invalid or conflicting arguments\n",
				argv[0]);
		usage(argv[0]);
		return rc > 0 ? 0 : 2;
	}

	if (options.trace == TRACE_BAR)
		selected = 1U << bar_table(options.bar);
	else if (options.trace == TRACE_FIND)
		selected = (1U << T_APPIN0) | (1U << T_APPIN1) |
			   (1U << T_SYSIN0);
	else if (options.trace == TRACE_OUT) {
		if (options.out_sys)
			selected = 1U << T_SYSOUT0;
		else if (options.address >> 48)
			selected = 1U << T_APPOUT0;
		else
			selected = 1U << T_APPOUT1;
	} else {
		selected = options.selected;
	}
	need_remap = options.trace == TRACE_BAR ||
		     options.trace == TRACE_FIND ||
		     (!options.no_remap &&
		      (selected & ((1U << T_APPIN0) | (1U << T_APPIN1) |
				   (1U << T_SYSIN0))));

	fd = open_device(device_id, device_path, sizeof(device_path));
	if (fd < 0)
		return 3;
	rc = check_device(fd, device_path);
	if (rc) {
#ifndef KERAUNOS_TLB_FAKE
		close(fd);
#endif
		return rc;
	}

	if (need_remap) {
		rc = read_remap(fd, remap);
		if (rc)
			goto out;
	}
	for (id = 0; id < TABLE_COUNT; id++) {
		if (!(selected & (1U << id)))
			continue;
		rc = read_tlb_table(fd, (enum table_id)id, &data[id],
				    options.trace == TRACE_NONE && options.attr);
		if (rc)
			goto out;
	}
	if (options.trace == TRACE_NONE) {
		rc = read_one(fd, ACCESS_CTRL, &access_ctrl);
		if (!rc)
			rc = read_one(fd, SYSTEM_STATUS, &status);
		if (!rc)
			rc = read_one(fd, IOMMU_CXT_ID, &context);
		if (rc)
			goto out;
	}

#ifndef KERAUNOS_TLB_FAKE
	close(fd);
#endif
	fd = -1;
	rc = 0;

	if (options.trace == TRACE_BAR) {
		rc = trace_bar(&options, data, remap);
	} else if (options.trace == TRACE_FIND) {
		rc = trace_find(&options, data, remap);
	} else if (options.trace == TRACE_OUT) {
		rc = trace_out(&options, data);
	} else {
		printf("TLBCFG @ KLA 0x1804_0000 "
		       "(read via BAR2/SYSIN0 entry 13)\n");
		printf("access_ctrl   = 0x%04X_%04X  inbound APP %s, "
		       "outbound APP %s\n",
		       access_ctrl >> 16, access_ctrl & 0xffff,
		       access_ctrl & (1U << 16) ? "enabled" : "disabled",
		       access_ctrl & 1U ? "enabled" : "disabled");
		printf("system_status = 0x%04X_%04X  system %s\n",
		       status >> 16, status & 0xffff,
		       status & 1U ? "ready" : "not ready");
		printf("iommu_cxt_id  = 0x%04X_%04X  (dead register)\n",
		       context >> 16, context & 0xffff);
		for (id = 0; id < TABLE_COUNT; id++) {
			if (!(selected & (1U << id)))
				continue;
			print_table((enum table_id)id, &data[id], remap,
				    &options);
		}
		collect_access_warnings(access_ctrl, selected, &warnings);
	}
	for (id = 0; id < TABLE_COUNT; id++) {
		if (!(selected & (1U << id)))
			continue;
		collect_warnings((enum table_id)id, &data[id], remap,
				 &options, &warnings);
	}
	print_warnings(&warnings);
	if (!rc && warnings.count)
		rc = 1;
	return rc;

out:
#ifndef KERAUNOS_TLB_FAKE
	if (fd >= 0)
		close(fd);
#endif
	return rc;
}
