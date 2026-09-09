/* SPDX-FileCopyrightText: © 2026 Tenstorrent Inc. */
/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef KERAUNOS_ADDRMAP_H
#define KERAUNOS_ADDRMAP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ENTRY_COUNT		16
#define REGS_PER_ENTRY		3
#define PCIE_REMAP_KLA_BASE	UINT64_C(0x100c1800)
#define PCIE_REMAP_SPA_BASE	UINT64_C(0x121c0c1800)
#define ENTRY_STRIDE		UINT64_C(0x10)
#define ADDRESS_SHIFT		23
#define ADDRESS_MASK		UINT32_C(0x1fffffff)
#define RESERVED_MASK		UINT32_C(0x60000000)
#define ENABLE_MASK		UINT32_C(0x80000000)

#define KERAUNOS_SPA_BASE	UINT64_C(0x1200000000)
#define KERAUNOS_SPA_SIZE	UINT64_C(0x20000000)
#define KERAUNOS_SPA_END	(KERAUNOS_SPA_BASE + KERAUNOS_SPA_SIZE)
/* Host memory as named inside Keraunos: HSIO0 fabric -> PCIe outbound. */
#define KERAUNOS_HOST_WINDOW	UINT64_C(0x2000000000000)
#define KLA_LIMIT		UINT64_C(0x100000000)
#define MIB			UINT64_C(0x100000)

static const unsigned int reg_offsets[REGS_PER_ENTRY] = { 0x0, 0x8, 0xc };
static const char *const reg_names[REGS_PER_ENTRY] = {
	"REMAP_START", "REMAP_END", "REPLACE_ADDR"
};

struct raw_entry {
	uint32_t reg[REGS_PER_ENTRY];
};

struct remap_entry {
	uint32_t raw[REGS_PER_ENTRY];
	uint64_t start;
	uint64_t end;
	uint64_t replace;
	int enabled;
};

struct interval {
	uint64_t lo;
	uint64_t hi;
};

struct known_range {
	uint64_t lo;
	uint64_t hi;
	const char *name;
};

static const struct known_range kla_ranges[] = {
	{ UINT64_C(0x00000000), UINT64_C(0x02000000), "SEP" },
	{ UINT64_C(0x08000000), UINT64_C(0x0a000000), "SMC" },
	{ UINT64_C(0x10000000), UINT64_C(0x18000000), "SMN tile / D2D cfg" },
	{ UINT64_C(0x18000000), UINT64_C(0x1c000000), "PCIe mgmt" },
	{ UINT64_C(0x20000000), UINT64_C(0x24000000), "HSIO0" },
	{ UINT64_C(0x24000000), UINT64_C(0x28000000), "HSIO1" },
	{ UINT64_C(0x28000000), UINT64_C(0x2c000000), "HSIO2" },
	{ UINT64_C(0x2c000000), UINT64_C(0x30000000), "HSIO3" },
	{ UINT64_C(0x30000000), UINT64_C(0x34000000), "HSIO4" },
};

static const struct known_range package_spa_ranges[] = {
	/* SYS_SRAM_BASE in pcie_init.c: 1 TiB. Bring-up maps 1 GiB of it. */
	{ UINT64_C(0x10000000000), UINT64_C(0x10040000000),
	  "system SRAM (package)" },
	/* PCIE_CFG_M0_GDDR_TILE0_SPA in pcie_config.h: 2^48, 8 GiB per Mimir. */
	{ UINT64_C(0x1000000000000), UINT64_C(0x1008000000000),
	  "GDDR (package)" },
	{ UINT64_C(0x1280000000), UINT64_C(0x1300000000), "Mimir CCE" },
	{ UINT64_C(0x1300000000), UINT64_C(0x1400000000), "Mimir config" },
	/* Keraunos-local name for host memory; HSIO0's fabric routes it to the
	 * PCIe outbound port. The package map calls the same thing 0x6_... and
	 * the other chiplets' ATTs rebase it to 0x2_... on the way in. */
	{ UINT64_C(0x2000000000000), UINT64_C(0x3000000000000),
	  "host window" },
	{ UINT64_C(0x6000000000000), UINT64_C(0x7000000000000),
	  "host window (package name; not routed inside Keraunos)" },
};

static void format_addr(char *buf, size_t bufsz, uint64_t value,
			unsigned int min_digits)
{
	char digits[17];
	char grouped[24];
	size_t len, first, in = 0, out = 0;

	snprintf(digits, sizeof(digits), "%0*llX", (int)min_digits,
		 (unsigned long long)value);
	len = strlen(digits);
	first = len % 4;
	if (!first)
		first = 4;

	while (in < len && out + 1 < sizeof(grouped)) {
		size_t group = in ? 4 : first;
		size_t j;

		if (in && out + 1 < sizeof(grouped))
			grouped[out++] = '_';
		for (j = 0; j < group && in < len && out + 1 < sizeof(grouped); j++)
			grouped[out++] = digits[in++];
	}
	grouped[out] = '\0';
	snprintf(buf, bufsz, "0x%s", grouped);
}

static const char *describe_kla_addr(uint64_t addr, char *buf, size_t bufsz)
{
	size_t i;

	if (addr == UINT64_C(0x18040000))
		return "PCIe mgmt: TLBCFG";
	if (addr == UINT64_C(0x18400000))
		return "PCIe mgmt: DBI";

	for (i = 0; i < sizeof(kla_ranges) / sizeof(kla_ranges[0]); i++) {
		const struct known_range *r = &kla_ranges[i];

		if (addr < r->lo || addr >= r->hi)
			continue;
		if (i >= 4) {
			unsigned int tile = (unsigned int)i - 4;
			uint64_t tl1_lo = r->lo + UINT64_C(0x02800000);
			uint64_t tl1_hi = tl1_lo + UINT64_C(0x00800000);

			if (addr >= tl1_lo && addr < tl1_hi) {
				snprintf(buf, bufsz, "HSIO%u TL1 SRAM", tile);
				return buf;
			}
		}
		return r->name;
	}
	return "?";
}

static __attribute__((unused)) const char *
describe_kla_range(uint64_t lo, uint64_t hi, char *buf, size_t bufsz)
{
	size_t i;

	for (i = 0; i < sizeof(kla_ranges) / sizeof(kla_ranges[0]); i++) {
		const struct known_range *r = &kla_ranges[i];

		if (lo < r->lo || hi > r->hi || lo >= hi)
			continue;
		if (i == 2 && lo == r->lo &&
		    hi == r->lo + UINT64_C(0x01000000))
			return "SMN tile / D2D cfg (first 16 MiB of 128)";
		if (i >= 4) {
			unsigned int tile = (unsigned int)i - 4;
			uint64_t tl1_lo = r->lo + UINT64_C(0x02800000);
			uint64_t tl1_hi = tl1_lo + UINT64_C(0x00800000);

			if (lo == r->lo && hi == r->hi)
				snprintf(buf, bufsz, "HSIO%u (incl. TL1 SRAM)",
					 tile);
			else if (lo == r->lo && hi == tl1_lo)
				snprintf(buf, bufsz, "HSIO%u low", tile);
			else if (lo == tl1_hi && hi == r->hi)
				snprintf(buf, bufsz, "HSIO%u high", tile);
			else if (lo >= tl1_lo && hi <= tl1_hi)
				snprintf(buf, bufsz, "HSIO%u TL1 SRAM", tile);
			else
				snprintf(buf, bufsz, "HSIO%u", tile);
			return buf;
		}
		return r->name;
	}
	return "?";
}

static const char *describe_spa_addr(uint64_t addr, char *buf, size_t bufsz)
{
	uint64_t off;
	unsigned int tile;

	if (addr < KERAUNOS_SPA_BASE || addr >= KERAUNOS_SPA_END)
		return "?";
	off = addr - KERAUNOS_SPA_BASE;
	if (off < UINT64_C(0x02000000))
		return "SEP";
	if (off < UINT64_C(0x04000000))
		return "SMC";
	if (off >= UINT64_C(0x18000000) &&
	    off < UINT64_C(0x1c000000))
		return "PCIe mgmt: TLBCFG, DBI";
	if (off >= UINT64_C(0x1c000000))
		return "SMN tile / D2D cfg";
	if (off >= UINT64_C(0x04000000) &&
	    off < UINT64_C(0x18000000)) {
		tile = (unsigned int)((off - UINT64_C(0x04000000)) /
				     UINT64_C(0x04000000));
		if ((off - UINT64_C(0x04000000)) %
		    UINT64_C(0x04000000) >= UINT64_C(0x02800000) &&
		    (off - UINT64_C(0x04000000)) %
		    UINT64_C(0x04000000) < UINT64_C(0x03000000))
			snprintf(buf, bufsz, "HSIO%u TL1 SRAM", tile);
		else
			snprintf(buf, bufsz, "HSIO%u", tile);
		return buf;
	}
	return "?";
}

static __attribute__((unused)) const char *
describe_spa_range(uint64_t lo, uint64_t hi, char *buf, size_t bufsz)
{
	uint64_t off_lo, off_hi;
	unsigned int tile;

	if (lo < KERAUNOS_SPA_BASE || hi > KERAUNOS_SPA_END || lo >= hi)
		return "?";
	off_lo = lo - KERAUNOS_SPA_BASE;
	off_hi = hi - KERAUNOS_SPA_BASE;
	if (off_hi <= UINT64_C(0x02000000))
		return "SEP";
	if (off_lo >= UINT64_C(0x02000000) &&
	    off_hi <= UINT64_C(0x04000000))
		return "SMC";
	if (off_lo >= UINT64_C(0x18000000) &&
	    off_hi <= UINT64_C(0x1c000000))
		return "PCIe mgmt: TLBCFG, DBI";
	if (off_lo >= UINT64_C(0x1c000000))
		return "SMN tile / D2D cfg";
	if (off_lo < UINT64_C(0x04000000) ||
	    off_hi > UINT64_C(0x18000000))
		return "?";

	tile = (unsigned int)((off_lo - UINT64_C(0x04000000)) /
			     UINT64_C(0x04000000));
	if (off_hi > UINT64_C(0x04000000) +
	    (uint64_t)(tile + 1) * UINT64_C(0x04000000))
		return "?";
	if (off_lo >= UINT64_C(0x04000000) +
	    (uint64_t)tile * UINT64_C(0x04000000) +
	    UINT64_C(0x02800000) &&
	    off_hi <= UINT64_C(0x04000000) +
	    (uint64_t)tile * UINT64_C(0x04000000) +
	    UINT64_C(0x03000000))
		snprintf(buf, bufsz, "HSIO%u TL1 SRAM", tile);
	else
		snprintf(buf, bufsz, "HSIO%u", tile);
	return buf;
}

static __attribute__((unused)) const char *
describe_package_spa(uint64_t addr)
{
	size_t i;

	for (i = 0;
	     i < sizeof(package_spa_ranges) / sizeof(package_spa_ranges[0]);
	     i++) {
		if (addr >= package_spa_ranges[i].lo &&
		    addr < package_spa_ranges[i].hi)
			return package_spa_ranges[i].name;
	}
	return "?";
}

typedef int (*addrmap_read32_fn)(int fd, uint64_t addr, uint32_t flags,
				 uint32_t *value);

static int read_pcie_remap_table(int fd, int via_spa,
				 addrmap_read32_fn read_word,
				 struct raw_entry table[ENTRY_COUNT],
				 uint64_t *failed_addr)
{
	uint64_t base = via_spa ? PCIE_REMAP_SPA_BASE : PCIE_REMAP_KLA_BASE;
	uint32_t flags = via_spa ? 0 : 1U;
	int i, r;

	for (i = 0; i < ENTRY_COUNT; i++) {
		for (r = 0; r < REGS_PER_ENTRY; r++) {
			uint64_t addr = base + (uint64_t)i * ENTRY_STRIDE +
					reg_offsets[r];
			int rc = read_word(fd, addr, flags, &table[i].reg[r]);

			if (rc) {
				if (failed_addr)
					*failed_addr = addr;
				return rc;
			}
		}
	}
	return 0;
}

static void decode_table(const struct raw_entry raw[ENTRY_COUNT],
			 struct remap_entry table[ENTRY_COUNT])
{
	int i;

	for (i = 0; i < ENTRY_COUNT; i++) {
		memcpy(table[i].raw, raw[i].reg, sizeof(table[i].raw));
		table[i].start =
			(uint64_t)(raw[i].reg[0] & ADDRESS_MASK) << ADDRESS_SHIFT;
		table[i].end =
			(uint64_t)(raw[i].reg[1] & ADDRESS_MASK) << ADDRESS_SHIFT;
		table[i].replace =
			(uint64_t)(raw[i].reg[2] & ADDRESS_MASK) << ADDRESS_SHIFT;
		table[i].enabled = !!(raw[i].reg[2] & ENABLE_MASK);
	}
}

static int first_matching_entry(const struct remap_entry table[ENTRY_COUNT],
				uint64_t spa)
{
	int i;

	for (i = 0; i < ENTRY_COUNT; i++) {
		if (table[i].enabled && table[i].start < table[i].end &&
		    spa >= table[i].start && spa < table[i].end)
			return i;
	}
	return -1;
}

#endif
