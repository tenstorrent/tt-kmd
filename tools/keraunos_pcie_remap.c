// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Dump and decode the Keraunos PCIE_REMAP table.
//
// To compile:
//   gcc -O2 -Wall -o keraunos_pcie_remap tools/keraunos_pcie_remap.c

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/types.h>

#include "keraunos_addrmap.h"

/* Driver definitions mirrored from ioctl.h so this tool is standalone. */
#define TENSTORRENT_IOCTL_MAGIC			0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_NOC_READ		_IO(TENSTORRENT_IOCTL_MAGIC, 18)
#define TENSTORRENT_NOC_FLAG_KLA		(1U << 0)

#define PCI_DEVICE_ID_KERAUNOS			0xFEED

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

static int read32(int fd, uint64_t addr, uint32_t flags, uint32_t *value)
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

static int read_table(int fd, int via_spa, struct raw_entry table[ENTRY_COUNT])
{
	uint64_t addr = 0;
	int rc = read_pcie_remap_table(fd, via_spa, read32, table, &addr);

	if (rc) {
		char text[32];

		format_addr(text, sizeof(text), addr, via_spa ? 10 : 8);
		fprintf(stderr, "NOC_READ at %s failed: %s\n",
			text, strerror(-rc));
		if (rc == -ENODEV)
			fprintf(stderr,
				"The driver may not have acquired its BAR%d window; "
				"see the Keraunos probe warnings.\n",
				via_spa ? 0 : 2);
		return 3;
	}
	return 0;
}

static int interval_cmp(const void *a, const void *b)
{
	const struct interval *ia = a;
	const struct interval *ib = b;

	if (ia->lo < ib->lo)
		return -1;
	if (ia->lo > ib->lo)
		return 1;
	if (ia->hi < ib->hi)
		return -1;
	return ia->hi > ib->hi;
}

static size_t merge_intervals(struct interval *v, size_t n)
{
	size_t i, out;

	if (!n)
		return 0;
	qsort(v, n, sizeof(*v), interval_cmp);
	out = 1;
	for (i = 1; i < n; i++) {
		if (v[i].lo <= v[out - 1].hi) {
			if (v[i].hi > v[out - 1].hi)
				v[out - 1].hi = v[i].hi;
		} else {
			v[out++] = v[i];
		}
	}
	return out;
}

static size_t subtract_interval(struct interval *v, size_t n,
				uint64_t cut_lo, uint64_t cut_hi)
{
	struct interval out[ENTRY_COUNT + 1];
	size_t i, count = 0;

	for (i = 0; i < n; i++) {
		if (cut_hi <= v[i].lo || cut_lo >= v[i].hi) {
			out[count++] = v[i];
			continue;
		}
		if (cut_lo > v[i].lo)
			out[count++] = (struct interval){ v[i].lo, cut_lo };
		if (cut_hi < v[i].hi)
			out[count++] = (struct interval){ cut_hi, v[i].hi };
	}
	memcpy(v, out, count * sizeof(*v));
	return count;
}

static size_t effective_kla_intervals(const struct remap_entry table[ENTRY_COUNT],
				      struct interval *out, size_t capacity)
{
	size_t count = 0;
	int i, j;

	for (i = 0; i < ENTRY_COUNT; i++) {
		struct interval source[ENTRY_COUNT + 1];
		size_t n = 1, k;

		if (!table[i].enabled || table[i].start >= table[i].end)
			continue;
		source[0] = (struct interval){ table[i].start, table[i].end };
		for (j = 0; j < i && n; j++) {
			if (!table[j].enabled || table[j].start >= table[j].end)
				continue;
			n = subtract_interval(source, n, table[j].start, table[j].end);
		}
		for (k = 0; k < n && count < capacity; k++) {
			out[count].lo = table[i].replace +
					(source[k].lo - table[i].start);
			out[count].hi = table[i].replace +
					(source[k].hi - table[i].start);
			count++;
		}
	}
	return merge_intervals(out, count);
}

static void print_raw(const struct remap_entry *e)
{
	printf("      raw start=0x%08X end=0x%08X replace=0x%08X\n",
	       e->raw[0], e->raw[1], e->raw[2]);
}

static void print_entry(int index, const struct remap_entry *e, int raw)
{
	char start[32], end[32], replace[32], replace_end[32], what[80];

	format_addr(start, sizeof(start), e->start, 10);
	format_addr(end, sizeof(end), e->end, 10);
	format_addr(replace, sizeof(replace), e->replace, 8);
	if (e->start >= e->end) {
		printf("%2d  %-16s .. %-16s  invalid  -> %-13s  %s\n",
		       index, start, end, replace,
		       e->enabled ? "enabled, matches nothing" : "disabled");
	} else {
		uint64_t size = e->end - e->start;
		uint64_t out_end = e->replace + size;
		const char *description;

		format_addr(replace_end, sizeof(replace_end), out_end, 8);
		description = describe_kla_range(e->replace, out_end,
						 what, sizeof(what));
		printf("%2d  %-16s .. %-16s %4llu MiB  -> %-13s .. %-13s  %s%s\n",
		       index, start, end, (unsigned long long)(size / MIB),
		       replace, replace_end, description,
		       e->enabled ? "" : "  [disabled]");
	}
	if (raw)
		print_raw(e);
}

static void print_disabled_summary(const struct remap_entry table[ENTRY_COUNT])
{
	int i = 0;

	while (i < ENTRY_COUNT) {
		int first, last;

		if (table[i].enabled) {
			i++;
			continue;
		}
		first = i;
		while (i + 1 < ENTRY_COUNT && !table[i + 1].enabled)
			i++;
		last = i++;
		if (first == last)
			printf("%2d disabled\n", first);
		else
			printf("%2d-%d disabled\n", first, last);
	}
}

static void print_spa_hole(uint64_t lo, uint64_t hi)
{
	char start[32], end[32], annotation[64];
	const char *what = describe_spa_range(lo, hi, annotation,
					      sizeof(annotation));

	format_addr(start, sizeof(start), lo, 10);
	format_addr(end, sizeof(end), hi, 10);
	printf("  %-16s .. %-16s %4llu MiB", start, end,
	       (unsigned long long)((hi - lo) / MIB));
	if (strcmp(what, "?"))
		printf("   (would be %s)", what);
	printf("\n");
}

static void print_spa_holes(const struct remap_entry table[ENTRY_COUNT])
{
	struct interval covered[ENTRY_COUNT];
	size_t count = 0, i;
	uint64_t cursor = KERAUNOS_SPA_BASE;
	int printed = 0;

	for (i = 0; i < ENTRY_COUNT; i++) {
		uint64_t lo, hi;

		if (!table[i].enabled || table[i].start >= table[i].end)
			continue;
		lo = table[i].start > KERAUNOS_SPA_BASE ?
			table[i].start : KERAUNOS_SPA_BASE;
		hi = table[i].end < KERAUNOS_SPA_END ?
			table[i].end : KERAUNOS_SPA_END;
		if (lo < hi)
			covered[count++] = (struct interval){ lo, hi };
	}
	count = merge_intervals(covered, count);

	printf("\nNot covered inside the Keraunos SPA window "
	       "(0x12_0000_0000 + 512 MiB), passes through as SPA:\n");
	for (i = 0; i < count; i++) {
		if (covered[i].lo > cursor) {
			print_spa_hole(cursor, covered[i].lo);
			printed = 1;
		}
		if (covered[i].hi > cursor)
			cursor = covered[i].hi;
	}
	if (cursor < KERAUNOS_SPA_END) {
		print_spa_hole(cursor, KERAUNOS_SPA_END);
		printed = 1;
	}
	if (!printed)
		printf("  (none)\n");
}

static void print_kla_hole(uint64_t lo, uint64_t hi)
{
	char start[32], end[32], annotation[64];
	const char *what = describe_kla_range(lo, hi, annotation,
					      sizeof(annotation));

	format_addr(start, sizeof(start), lo, 8);
	format_addr(end, sizeof(end), hi, 8);
	printf("  %-13s .. %-13s %4llu MiB  %s\n",
	       start, end, (unsigned long long)((hi - lo) / MIB), what);
}

static void print_kla_holes(const struct remap_entry table[ENTRY_COUNT])
{
	struct interval covered[ENTRY_COUNT * (ENTRY_COUNT + 1)];
	size_t count = effective_kla_intervals(table, covered,
					       sizeof(covered) / sizeof(covered[0]));
	size_t r, i;
	int printed = 0;

	printf("\nKLA not reachable from SPA:\n");
	for (r = 0; r < sizeof(kla_ranges) / sizeof(kla_ranges[0]); r++) {
		uint64_t cursor = kla_ranges[r].lo;

		for (i = 0; i < count && cursor < kla_ranges[r].hi; i++) {
			uint64_t lo, hi;

			if (covered[i].hi <= kla_ranges[r].lo ||
			    covered[i].lo >= kla_ranges[r].hi)
				continue;
			lo = covered[i].lo > kla_ranges[r].lo ?
				covered[i].lo : kla_ranges[r].lo;
			hi = covered[i].hi < kla_ranges[r].hi ?
				covered[i].hi : kla_ranges[r].hi;
			if (lo > cursor) {
				print_kla_hole(cursor, lo);
				printed = 1;
			}
			if (hi > cursor)
				cursor = hi;
		}
		if (cursor < kla_ranges[r].hi) {
			print_kla_hole(cursor, kla_ranges[r].hi);
			printed = 1;
		}
	}
	if (!printed)
		printf("  (none)\n");
}

static void print_dump(const struct remap_entry table[ENTRY_COUNT],
		       int via_spa, int raw, int all)
{
	int i;

	printf("PCIE_REMAP @ KLA 0x100C_1800 (read via %s)\n\n",
	       via_spa ? "BAR0/APPIN0" : "BAR2/SYSIN0");
	printf(" #  SPA range                                  size   -> "
	       "KLA range                   what\n");
	for (i = 0; i < ENTRY_COUNT; i++) {
		if (table[i].enabled || all || raw)
			print_entry(i, &table[i], raw);
	}
	if (!all && !raw)
		print_disabled_summary(table);
	print_spa_holes(table);
	print_kla_holes(table);
}

static void print_lookup(const struct remap_entry table[ENTRY_COUNT],
			 uint64_t spa)
{
	char addr[32], start[32], end[32], replace[32], result[32], what[80];
	int index = first_matching_entry(table, spa);

	format_addr(addr, sizeof(addr), spa, 10);
	printf("SPA %s\n", addr);
	if (index < 0) {
		printf("  no match; passes through unchanged as SPA\n");
		if (spa >= KERAUNOS_SPA_BASE && spa < KERAUNOS_SPA_END) {
			const char *description =
				describe_spa_addr(spa, what, sizeof(what));

			printf("  (inside the Keraunos window; would be %s. "
			       "On the APPIN path\n"
			       "  this leaves tile 0 for the NoC. On the SYSIN path "
			       "the SMN rejects it.)\n", description);
		}
		return;
	}

	format_addr(start, sizeof(start), table[index].start, 10);
	format_addr(end, sizeof(end), table[index].end, 10);
	format_addr(replace, sizeof(replace), table[index].replace, 8);
	format_addr(result, sizeof(result),
		    table[index].replace + spa - table[index].start, 8);
	printf("  matches entry %d (%s .. %s -> %s)\n",
	       index, start, end, replace);
	printf("  -> KLA %s  (%s)\n", result,
	       describe_kla_addr(table[index].replace + spa -
				 table[index].start, what, sizeof(what)));
}

static void print_rlookup(const struct remap_entry table[ENTRY_COUNT],
			  uint64_t kla)
{
	char addr[32], spa_text[32], what[80];
	int i, found = 0;

	format_addr(addr, sizeof(addr), kla, 8);
	printf("KLA %s  (%s)\n", addr,
	       describe_kla_addr(kla, what, sizeof(what)));
	for (i = 0; i < ENTRY_COUNT; i++) {
		uint64_t size, spa;

		if (!table[i].enabled || table[i].start >= table[i].end)
			continue;
		size = table[i].end - table[i].start;
		if (kla < table[i].replace || kla - table[i].replace >= size)
			continue;
		spa = table[i].start + kla - table[i].replace;
		if (first_matching_entry(table, spa) != i)
			continue;
		format_addr(spa_text, sizeof(spa_text), spa, 10);
		printf("  reachable as SPA %s via entry %d\n", spa_text, i);
		found = 1;
	}
	if (!found)
		printf("  not reachable from SPA; BAR2/SYSIN0 only\n");
}

static int print_table_warnings(const struct remap_entry table[ENTRY_COUNT])
{
	int i, j, warnings = 0;

	for (i = 0; i < ENTRY_COUNT; i++) {
		for (j = 0; j < REGS_PER_ENTRY; j++) {
			if (table[i].raw[j] & RESERVED_MASK) {
				printf("WARNING: entry %d %s has reserved bits [30:29] "
				       "set (0x%08X).\n",
				       i, reg_names[j],
				       table[i].raw[j] & RESERVED_MASK);
				warnings++;
			}
		}
		if (!table[i].enabled)
			continue;
		if (table[i].start >= table[i].end) {
			printf("WARNING: enabled entry %d has start >= end and "
			       "matches nothing.\n", i);
			warnings++;
			continue;
		}
		if (table[i].replace + (table[i].end - table[i].start) >
		    KLA_LIMIT) {
			printf("WARNING: entry %d output crosses 0x1_0000_0000 "
			       "and is not entirely KLA.\n", i);
			warnings++;
		}
		if (table[i].start < KERAUNOS_SPA_BASE ||
		    table[i].end > KERAUNOS_SPA_END) {
			printf("WARNING: entry %d SPA range lies outside the "
			       "Keraunos SPA window.\n", i);
			warnings++;
		}
	}

	for (i = 0; i < ENTRY_COUNT; i++) {
		if (!table[i].enabled || table[i].start >= table[i].end)
			continue;
		for (j = i + 1; j < ENTRY_COUNT; j++) {
			char lo[32], hi[32];
			uint64_t overlap_lo, overlap_hi;

			if (!table[j].enabled || table[j].start >= table[j].end)
				continue;
			overlap_lo = table[i].start > table[j].start ?
				table[i].start : table[j].start;
			overlap_hi = table[i].end < table[j].end ?
				table[i].end : table[j].end;
			if (overlap_lo >= overlap_hi)
				continue;
			format_addr(lo, sizeof(lo), overlap_lo, 10);
			format_addr(hi, sizeof(hi), overlap_hi, 10);
			printf("WARNING: entries %d and %d overlap at %s .. %s; "
			       "entry %d wins.\n", i, j, lo, hi, i);
			warnings++;
		}
	}
	return warnings;
}

static void print_changed_entries(const char *path,
				  const struct raw_entry first[ENTRY_COUNT],
				  const struct raw_entry second[ENTRY_COUNT])
{
	int i, first_index = 1;

	printf("WARNING: PCIE_REMAP changed between the two %s reads (entries ",
	       path);
	for (i = 0; i < ENTRY_COUNT; i++) {
		if (!memcmp(&first[i], &second[i], sizeof(first[i])))
			continue;
		printf("%s%d", first_index ? "" : ", ", i);
		first_index = 0;
	}
	printf("); showing the second read.\n");
}

static void print_mismatched_entries(const struct raw_entry kla[ENTRY_COUNT],
				     const struct raw_entry spa[ENTRY_COUNT])
{
	int i, first_index = 1;

	printf("WARNING: SPA-path table does not match the KLA-path table "
	       "(entries ");
	for (i = 0; i < ENTRY_COUNT; i++) {
		if (!memcmp(&kla[i], &spa[i], sizeof(kla[i])))
			continue;
		printf("%s%d", first_index ? "" : ", ", i);
		first_index = 0;
	}
	printf(").\n");
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
		"Usage: %s <device_id> [options]\n"
		"\n"
		"Options:\n"
		"  -r, --raw           print raw register values\n"
		"  -a, --all           print disabled entries in full\n"
		"      --via-spa       read via BAR0/APPIN0 and compare with KLA\n"
		"  -l, --lookup ADDR   translate SPA to KLA\n"
		"  -k, --rlookup ADDR  find every SPA mapping to KLA\n"
		"  -h, --help          show this help\n",
		prog);
}

int main(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "raw", no_argument, NULL, 'r' },
		{ "all", no_argument, NULL, 'a' },
		{ "via-spa", no_argument, NULL, 1000 },
		{ "lookup", required_argument, NULL, 'l' },
		{ "rlookup", required_argument, NULL, 'k' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	struct tenstorrent_get_device_info info = { 0 };
	struct raw_entry kla_first[ENTRY_COUNT], kla_second[ENTRY_COUNT];
	struct raw_entry spa_first[ENTRY_COUNT], spa_second[ENTRY_COUNT];
	const struct raw_entry *selected;
	struct remap_entry table[ENTRY_COUNT];
	char device_path[PATH_MAX];
	uint64_t lookup_addr = 0;
	int raw = 0, all = 0, via_spa = 0;
	int lookup = 0, rlookup = 0;
	int kla_changed = 0, spa_changed = 0, paths_mismatch = 0;
	int device_id, fd, opt, rc, warnings = 0;

	opterr = 0;
	while ((opt = getopt_long(argc, argv, "ral:k:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'r':
			raw = 1;
			break;
		case 'a':
			all = 1;
			break;
		case 'l':
			if (lookup || rlookup || parse_addr(optarg, &lookup_addr)) {
				fprintf(stderr, "%s: invalid or conflicting --lookup\n",
					argv[0]);
				return 2;
			}
			lookup = 1;
			break;
		case 'k':
			if (lookup || rlookup || parse_addr(optarg, &lookup_addr)) {
				fprintf(stderr, "%s: invalid or conflicting --rlookup\n",
					argv[0]);
				return 2;
			}
			rlookup = 1;
			break;
		case 1000:
			via_spa = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (optind + 1 != argc || parse_device_id(argv[optind], &device_id)) {
		usage(argv[0]);
		return 2;
	}
	snprintf(device_path, sizeof(device_path), "/dev/tenstorrent/%d", device_id);
	fd = open(device_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n",
			device_path, strerror(errno));
		return 3;
	}

	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "GET_DEVICE_INFO failed for %s: %s\n",
			device_path, strerror(errno));
		close(fd);
		return 3;
	}
	if (info.out.device_id != PCI_DEVICE_ID_KERAUNOS) {
		fprintf(stderr, "%s is PCI device 0x%04X, not Keraunos (0x%04X).\n",
			device_path, info.out.device_id, PCI_DEVICE_ID_KERAUNOS);
		close(fd);
		return 2;
	}

	rc = read_table(fd, 0, kla_first);
	if (!rc)
		rc = read_table(fd, 0, kla_second);
	if (rc) {
		close(fd);
		return rc;
	}
	kla_changed = memcmp(kla_first, kla_second, sizeof(kla_first)) != 0;
	selected = kla_second;

	if (via_spa) {
		rc = read_table(fd, 1, spa_first);
		if (!rc)
			rc = read_table(fd, 1, spa_second);
		if (rc) {
			close(fd);
			return rc;
		}
		spa_changed = memcmp(spa_first, spa_second,
				     sizeof(spa_first)) != 0;
		paths_mismatch = memcmp(kla_second, spa_second,
					sizeof(kla_second)) != 0;
		selected = spa_second;
	}
	close(fd);

	decode_table(selected, table);
	if (lookup)
		print_lookup(table, lookup_addr);
	else if (rlookup)
		print_rlookup(table, lookup_addr);
	else
		print_dump(table, via_spa, raw, all);

	if (kla_changed) {
		print_changed_entries("KLA-path", kla_first, kla_second);
		warnings++;
	}
	if (spa_changed) {
		print_changed_entries("SPA-path", spa_first, spa_second);
		warnings++;
	}
	if (paths_mismatch) {
		print_mismatched_entries(kla_second, spa_second);
		warnings++;
	}
	rc = print_table_warnings(table);
	if (rc)
		warnings += rc;
	return warnings ? 1 : 0;
}
