// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Prove that a Keraunos fabric firewall can make a window of SMC SRAM
// read-only for the host, end to end:
//
//   1. inspect the firewall; refuse to run unless it is in the bring-up state
//   2. read a word of SMC SRAM through the path under test; require zero
//   3. write a pattern through that path and read it back (SRAM is writable)
//   4. carve a read-only window around the word out of the blanket filters
//   5. write the inverse pattern through the path under test
//   6. read back through both paths: the first pattern must still be there
//   7. control: write through the other path, which the firewall does not
//      gate; the readback must change (the block was the firewall)
//   8. restore the firewall to its bring-up state, verify by re-reading it
//   9. write through the path under test again; the readback must change
//  10. restore the word to its original value
//
// The two host paths are BAR0/APPIN (SPA, gated by HSIO0's "pcie" firewall)
// and BAR2/SYSIN (KLA, gated by the PCIe mgmt "smn" firewall). Both see the
// post-PCIE_REMAP address, i.e. the KLA: the remapper sits in
// keraunos_pcie_subsystem_struct_wrap.sv ahead of the fabric. The default
// gates the BAR0 path and uses BAR2 for the control write and for the
// firewall registers themselves, so a mistake cannot lock the tool out.
//
// SAFETY (emulation): the SMC runs bring-up firmware out of this SRAM
// (reset vector 0xC006_0000 = KLA 0x0806_0000). Code, data, stacks and heap
// grow up from the bottom; fw_params, the uRP mailbox, H2D staging and the
// log buffer sit in the top ~25 KiB and are polled by the host harness.
// The default test word is at 512 KiB, the middle of the 1 MiB SPM, and the
// tool refuses to write anywhere that does not read back as zero first.
// Sentinel values the harness watches for (0xA111600D, 0xACAFACA1,
// 0xDEADBEEF) are never written. A denied posted write is dropped by the
// fabric; the PCIe controller may log an error for it.
//
// If the tool dies between steps 4 and 8, `--restore-only` rewrites the
// bring-up blankets into filters 0 and 15 and clears the filters this tool
// uses (1, 2, 13, 14).
//
// To compile:
//   gcc -O2 -Wall -Wextra -o keraunos_firewall_ro tools/keraunos_firewall_ro.c

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

#define TENSTORRENT_IOCTL_MAGIC			0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO	_IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_NOC_READ		_IO(TENSTORRENT_IOCTL_MAGIC, 18)
#define TENSTORRENT_IOCTL_NOC_WRITE		_IO(TENSTORRENT_IOCTL_MAGIC, 19)
#define TENSTORRENT_NOC_FLAG_KLA		(1U << 0)
#define PCI_DEVICE_ID_KERAUNOS			0xFEED

/* SMC block: KLA 0x0800_0000 <-> SPA 0x12_0200_0000 (PCIE_REMAP entry 1). */
#define SMC_KLA_BASE		UINT64_C(0x08000000)
#define SMC_SPA_BASE		UINT64_C(0x1202000000)
#define SMC_SIZE		UINT64_C(0x02000000)
#define SMC_SPM_BASE		UINT64_C(0x08060000)
#define SMC_SPM_SIZE		UINT64_C(0x00100000)
#define SMC_SPM_TOP_RESERVED	UINT64_C(0x00010000)	/* fw_params, uRP, log */

#define HSIO0_PCIE_FW_BASE	UINT64_C(0x23021000)
#define PCIE_MGMT_SMN_FW_BASE	UINT64_C(0x18050000)
#define FILTER_STRIDE		UINT64_C(0x20)
#define NUM_FILTERS		16

#define CFG_READ_EN		(UINT64_C(1) << 0)
#define CFG_WRITE_EN		(UINT64_C(1) << 1)
#define CFG_ADDR_MODE		(UINT64_C(1) << 4)
#define CFG_ALLOW_NS		(UINT64_C(1) << 8)
#define CFG_BUS_WIDTH_MASK	(UINT64_C(0x7) << 12)
#define CFG_ALLOW_BURST		(UINT64_C(1) << 24)
#define CFG_LOCKED		(UINT64_C(1) << 63)

/* What bring-up writes (open_filter in qsr1_boot main.c), less the RO width. */
#define CFG_BRINGUP_RW		(CFG_READ_EN | CFG_WRITE_EN | CFG_ADDR_MODE | \
				 CFG_ALLOW_BURST)
#define CFG_BRINGUP_RO		(CFG_READ_EN | CFG_ADDR_MODE | CFG_ALLOW_BURST)

/* Filters this tool owns while the window is in place. */
#define F_NS_BLANKET		0
#define F_NS_WINDOW_RO		1
#define F_NS_ABOVE_RW		2
#define F_S_ABOVE_RW		13
#define F_S_WINDOW_RO		14
#define F_S_BLANKET		15

#define DEFAULT_ADDR		(SMC_SPM_BASE + SMC_SPM_SIZE / 2)
#define DEFAULT_WINDOW		UINT64_C(0x1000)
#define PATTERN_A		UINT32_C(0x3C5AA5C3)
#define PATTERN_B		UINT32_C(0xC3A55A3C)

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

struct tenstorrent_noc_io {
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

enum path {
	PATH_APP,	/* BAR0/APPIN0, SPA addressing, flags = 0 */
	PATH_SYS	/* BAR2/SYSIN0, KLA addressing, flags = KLA */
};

struct filter {
	uint64_t config;
	uint64_t start;
	uint64_t end;
};

struct options {
	uint64_t addr;		/* KLA of the test word */
	uint64_t window;	/* size of the read-only window */
	enum path gated;	/* path the firewall under test gates */
	int force;
	int dry_run;
	int keep;
	int restore_only;
	int verbose;
};

struct ctx {
	int fd;
	struct options opt;
	uint64_t fw_base;
	const char *fw_name;
	uint64_t win_lo, win_hi;	/* inclusive KLA window */
	struct filter saved[NUM_FILTERS];
	int firewall_programmed;
	int sram_dirty;
	uint32_t sram_original;
	int failures;
};

static const char *path_name(enum path p)
{
	return p == PATH_APP ? "BAR0/APP (SPA)" : "BAR2/SYS (KLA)";
}

static uint64_t path_addr(enum path p, uint64_t kla)
{
	return p == PATH_APP ? SMC_SPA_BASE + (kla - SMC_KLA_BASE) : kla;
}

/* ------------------------------------------------------------------------ */
/* Device access, real or fake.                                             */

#ifndef KERAUNOS_FIREWALL_RO_FAKE
static int dev_read32(int fd, uint64_t addr, uint32_t flags, uint32_t *value)
{
	struct tenstorrent_noc_io io = { 0 };

	io.argsz = sizeof(io);
	io.flags = flags;
	io.width = 4;
	io.addr = addr;
	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_READ, &io) < 0)
		return -errno;
	*value = (uint32_t)io.value;
	return 0;
}

static int dev_write32(int fd, uint64_t addr, uint32_t flags, uint32_t value)
{
	struct tenstorrent_noc_io io = { 0 };

	io.argsz = sizeof(io);
	io.flags = flags;
	io.width = 4;
	io.addr = addr;
	io.value = value;
	if (ioctl(fd, TENSTORRENT_IOCTL_NOC_WRITE, &io) < 0)
		return -errno;
	return 0;
}
#else
/*
 * Fake device: 1 MiB of SPM, two firewalls in bring-up state, lowest-index
 * precedence, allow_ns=0 filters ignore non-secure (PCIe) traffic. The APP
 * path is gated by the HSIO0 firewall, the SYS path by the PCIe mgmt one.
 * FAKE_LEAKY=1 in the environment makes the HSIO0 firewall ignore write_en,
 * to exercise the failure reporting.
 */
static uint32_t fake_spm[SMC_SPM_SIZE / 4];
static struct filter fake_fw[2][NUM_FILTERS];
static int fake_ready;

static void fake_init(void)
{
	int i;

	if (fake_ready)
		return;
	fake_ready = 1;
	for (i = 0; i < 2; i++) {
		fake_fw[i][0].config = CFG_BRINGUP_RW | CFG_ALLOW_NS |
				       (UINT64_C(3) << 12);
		fake_fw[i][0].end = UINT64_MAX;
		fake_fw[i][15].config = CFG_BRINGUP_RW | (UINT64_C(3) << 12);
		fake_fw[i][15].end = UINT64_MAX;
	}
}

static int fake_fw_index(uint64_t kla, unsigned int *idx, unsigned int *word)
{
	int which;

	if (kla >= HSIO0_PCIE_FW_BASE &&
	    kla < HSIO0_PCIE_FW_BASE + NUM_FILTERS * FILTER_STRIDE)
		which = 0;
	else if (kla >= PCIE_MGMT_SMN_FW_BASE &&
		 kla < PCIE_MGMT_SMN_FW_BASE + NUM_FILTERS * FILTER_STRIDE)
		which = 1;
	else
		return -1;
	kla -= which ? PCIE_MGMT_SMN_FW_BASE : HSIO0_PCIE_FW_BASE;
	*idx = (unsigned int)(kla / FILTER_STRIDE);
	*word = (unsigned int)((kla % FILTER_STRIDE) / 4);
	return which;
}

static uint64_t *fake_fw_reg(int which, unsigned int idx, unsigned int word)
{
	struct filter *f = &fake_fw[which][idx];

	switch (word / 2) {
	case 0: return &f->config;
	case 1: return &f->start;
	default: return &f->end;
	}
}

static int fake_permits(int which, uint64_t kla, int write)
{
	int i;

	for (i = 0; i < NUM_FILTERS; i++) {
		const struct filter *f = &fake_fw[which][i];

		if (!(f->config & CFG_ADDR_MODE) || kla < f->start ||
		    kla > f->end)
			continue;
		if (!(f->config & CFG_ALLOW_NS))
			continue;	/* PCIe traffic is non-secure */
		if (which == 0 && getenv("FAKE_LEAKY"))
			return 1;
		return write ? !!(f->config & CFG_WRITE_EN) :
			       !!(f->config & CFG_READ_EN);
	}
	return 0;
}

static int fake_access(uint64_t addr, uint32_t flags, uint32_t *value,
		       int write)
{
	int which = flags & TENSTORRENT_NOC_FLAG_KLA ? 1 : 0;
	uint64_t kla;
	unsigned int idx, word;
	int fw;

	fake_init();
	if (which == 0) {
		if (addr < SMC_SPA_BASE || addr >= SMC_SPA_BASE + SMC_SIZE)
			return -EIO;	/* fake models only the SMC block on APP */
		kla = SMC_KLA_BASE + (addr - SMC_SPA_BASE);
	} else {
		kla = addr;
	}
	fw = fake_fw_index(kla, &idx, &word);
	if (fw >= 0) {
		uint64_t *reg = fake_fw_reg(fw, idx, word);
		unsigned int shift = (word & 1) * 32;

		if (write) {
			if (fake_fw[fw][idx].config & CFG_LOCKED)
				return 0;
			*reg = (*reg & ~(UINT64_C(0xFFFFFFFF) << shift)) |
			       ((uint64_t)*value << shift);
		} else {
			*value = (uint32_t)(*reg >> shift);
		}
		return 0;
	}
	if (kla >= SMC_SPM_BASE && kla < SMC_SPM_BASE + SMC_SPM_SIZE) {
		size_t i = (size_t)(kla - SMC_SPM_BASE) / 4;

		if (!fake_permits(which, kla, write)) {
			if (!write)
				*value = UINT32_MAX;
			return 0;	/* dropped write / error completion */
		}
		if (write)
			fake_spm[i] = *value;
		else
			*value = fake_spm[i];
		return 0;
	}
	if (!write)
		*value = 0;
	return 0;
}

static int dev_read32(int fd, uint64_t addr, uint32_t flags, uint32_t *value)
{
	(void)fd;
	return fake_access(addr, flags, value, 0);
}

static int dev_write32(int fd, uint64_t addr, uint32_t flags, uint32_t value)
{
	(void)fd;
	return fake_access(addr, flags, &value, 1);
}
#endif

/* ------------------------------------------------------------------------ */

static void format_addr(char *buf, size_t bufsz, uint64_t value)
{
	char digits[17];
	size_t len, i, out = 0;

	snprintf(digits, sizeof(digits), "%llX", (unsigned long long)value);
	len = strlen(digits);
	if (len < 8) {
		snprintf(digits, sizeof(digits), "%08llX",
			 (unsigned long long)value);
		len = 8;
	}
	buf[out++] = '0';
	buf[out++] = 'x';
	for (i = 0; i < len && out + 2 < bufsz; i++) {
		if (i && (len - i) % 4 == 0)
			buf[out++] = '_';
		buf[out++] = digits[i];
	}
	buf[out] = '\0';
}

static void io_fail(struct ctx *c, const char *what, uint64_t addr, int rc);

static int rd32(struct ctx *c, enum path p, uint64_t addr, uint32_t *value)
{
	uint32_t flags = p == PATH_SYS ? TENSTORRENT_NOC_FLAG_KLA : 0;
	int rc = dev_read32(c->fd, addr, flags, value);
	char text[32];

	if (c->opt.verbose) {
		format_addr(text, sizeof(text), addr);
		if (rc)
			printf("      rd %-14s %-22s -> %s\n",
			       p == PATH_APP ? "SPA" : "KLA", text,
			       strerror(-rc));
		else
			printf("      rd %-14s %-22s -> 0x%08X\n",
			       p == PATH_APP ? "SPA" : "KLA", text, *value);
	}
	if (rc)
		io_fail(c, "NOC_READ", addr, rc);
	return rc;
}

static int wr32(struct ctx *c, enum path p, uint64_t addr, uint32_t value)
{
	uint32_t flags = p == PATH_SYS ? TENSTORRENT_NOC_FLAG_KLA : 0;
	char text[32];
	int rc;

	if (c->opt.verbose) {
		format_addr(text, sizeof(text), addr);
		printf("      wr %-14s %-22s <- 0x%08X%s\n",
		       p == PATH_APP ? "SPA" : "KLA", text, value,
		       c->opt.dry_run ? "  (dry run)" : "");
	}
	if (c->opt.dry_run)
		return 0;
	rc = dev_write32(c->fd, addr, flags, value);
	if (rc)
		io_fail(c, "NOC_WRITE", addr, rc);
	return rc;
}

/* Firewall registers are always reached over the SYS path (BAR2). */
static int fw_read_filter(struct ctx *c, unsigned int idx, struct filter *f)
{
	uint64_t base = c->fw_base + idx * FILTER_STRIDE;
	uint32_t w[6];
	unsigned int i;

	for (i = 0; i < 6; i++)
		if (rd32(c, PATH_SYS, base + i * 4, &w[i]))
			return -1;
	f->config = ((uint64_t)w[1] << 32) | w[0];
	f->start = ((uint64_t)w[3] << 32) | w[2];
	f->end = ((uint64_t)w[5] << 32) | w[4];
	return 0;
}

static int fw_write64(struct ctx *c, unsigned int idx, unsigned int reg,
		      uint64_t value)
{
	uint64_t addr = c->fw_base + idx * FILTER_STRIDE + reg;

	if (wr32(c, PATH_SYS, addr, (uint32_t)value))
		return -1;
	return wr32(c, PATH_SYS, addr + 4, (uint32_t)(value >> 32));
}

/* Program a whole filter: addresses first, config last so it arms complete. */
static int fw_write_filter(struct ctx *c, unsigned int idx,
			   const struct filter *f)
{
	if (fw_write64(c, idx, 0x08, f->start))
		return -1;
	if (fw_write64(c, idx, 0x10, f->end))
		return -1;
	return fw_write64(c, idx, 0x00, f->config);
}

static int fw_clear_filter(struct ctx *c, unsigned int idx)
{
	/* Config first so it stops matching before the window goes away. */
	if (fw_write64(c, idx, 0x00, 0))
		return -1;
	if (fw_write64(c, idx, 0x08, 0))
		return -1;
	return fw_write64(c, idx, 0x10, 0);
}

static void print_filter(unsigned int idx, const struct filter *f)
{
	char start[32], end[32];

	format_addr(start, sizeof(start), f->start);
	format_addr(end, sizeof(end), f->end);
	printf("      #%-2u cfg=0x%016llX %s .. %s  %s%s%s%s\n", idx,
	       (unsigned long long)f->config, start, end,
	       f->config & CFG_READ_EN ? "R" : "-",
	       f->config & CFG_WRITE_EN ? "W" : "-",
	       f->config & CFG_ALLOW_NS ? " ns" : " S",
	       f->config & CFG_LOCKED ? " LOCKED" : "");
}

static int is_bringup_blanket(const struct filter *f, int ns)
{
	uint64_t cfg = f->config & ~CFG_BUS_WIDTH_MASK;

	return cfg == (CFG_BRINGUP_RW | (ns ? CFG_ALLOW_NS : 0)) &&
	       f->start == 0 && f->end == UINT64_MAX;
}

static int is_unprogrammed(const struct filter *f)
{
	return !(f->config & ~CFG_BUS_WIDTH_MASK) && !f->start && !f->end;
}

/* ------------------------------------------------------------------------ */

static int restore_firewall(struct ctx *c)
{
	static const unsigned int mine[] = {
		F_NS_WINDOW_RO, F_NS_ABOVE_RW, F_S_ABOVE_RW, F_S_WINDOW_RO
	};
	unsigned int i;
	int rc = 0;

	/* Widen the blankets first so every address is permitted again. */
	if (fw_write64(c, F_NS_BLANKET, 0x10, c->saved[F_NS_BLANKET].end))
		rc = -1;
	if (fw_write64(c, F_S_BLANKET, 0x10, c->saved[F_S_BLANKET].end))
		rc = -1;
	for (i = 0; i < sizeof(mine) / sizeof(mine[0]); i++) {
		if (is_unprogrammed(&c->saved[mine[i]])) {
			if (fw_clear_filter(c, mine[i]))
				rc = -1;
		} else if (fw_write_filter(c, mine[i], &c->saved[mine[i]])) {
			rc = -1;
		}
	}
	if (!rc)
		c->firewall_programmed = 0;
	return rc;
}

static int verify_firewall_restored(struct ctx *c)
{
	unsigned int i;
	int mismatches = 0;

	if (c->opt.dry_run)
		return 0;
	for (i = 0; i < NUM_FILTERS; i++) {
		struct filter f;

		if (fw_read_filter(c, i, &f))
			return -1;
		if (memcmp(&f, &c->saved[i], sizeof(f))) {
			printf("      filter %u differs from the saved state:\n", i);
			print_filter(i, &f);
			mismatches++;
		}
	}
	return mismatches ? 1 : 0;
}

static void io_fail(struct ctx *c, const char *what, uint64_t addr, int rc)
{
	char text[32];

	format_addr(text, sizeof(text), addr);
	fprintf(stderr, "%s at %s failed: %s\n", what, text, strerror(-rc));
	if (rc == -ENODEV)
		fprintf(stderr, "The driver may not have acquired its BAR0/BAR2 "
			"windows; see the Keraunos probe warnings.\n");
	if (c->firewall_programmed) {
		fprintf(stderr, "Firewall %s is still carved; attempting to "
			"restore it.\n", c->fw_name);
		c->firewall_programmed = 0;	/* avoid recursion */
		if (restore_firewall(c))
			fprintf(stderr, "Restore failed. Run with "
				"--restore-only once the device responds.\n");
		else
			fprintf(stderr, "Restored.\n");
	}
	exit(3);
}

/* ------------------------------------------------------------------------ */

static void step(unsigned int n, const char *text)
{
	printf("\n[%2u] %s\n", n, text);
}

static void verdict(struct ctx *c, int ok, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

static void verdict(struct ctx *c, int ok, const char *fmt, ...)
{
	va_list ap;

	printf("      ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("  %s\n", ok ? "PASS" : "FAIL");
	if (!ok)
		c->failures++;
}

static int read_both(struct ctx *c, uint32_t *via_app, uint32_t *via_sys)
{
	if (rd32(c, PATH_APP, path_addr(PATH_APP, c->opt.addr), via_app))
		return -1;
	return rd32(c, PATH_SYS, path_addr(PATH_SYS, c->opt.addr), via_sys);
}

static int inspect_firewall(struct ctx *c)
{
	unsigned int i;
	int problems = 0;

	for (i = 0; i < NUM_FILTERS; i++) {
		if (fw_read_filter(c, i, &c->saved[i]))
			return -1;
		if (c->saved[i].config & CFG_LOCKED) {
			printf("      filter %u is locked; nothing can be "
			       "changed on this firewall\n", i);
			problems++;
		}
	}
	print_filter(F_NS_BLANKET, &c->saved[F_NS_BLANKET]);
	print_filter(F_S_BLANKET, &c->saved[F_S_BLANKET]);
	if (!is_bringup_blanket(&c->saved[F_NS_BLANKET], 1)) {
		printf("      filter 0 is not the bring-up non-secure blanket\n");
		problems++;
	}
	if (!is_bringup_blanket(&c->saved[F_S_BLANKET], 0)) {
		printf("      filter 15 is not the bring-up secure-only blanket\n");
		problems++;
	}
	for (i = 1; i < NUM_FILTERS - 1; i++) {
		if (is_unprogrammed(&c->saved[i]))
			continue;
		print_filter(i, &c->saved[i]);
		if (i == F_NS_WINDOW_RO || i == F_NS_ABOVE_RW ||
		    i == F_S_ABOVE_RW || i == F_S_WINDOW_RO) {
			printf("      filter %u is in use; this tool needs it\n", i);
			problems++;
		}
	}
	return problems;
}

static int carve_window(struct ctx *c)
{
	struct filter f;

	/* Additive filters first: read-only window, RW above the window. */
	f.start = c->win_lo;
	f.end = c->win_hi;
	f.config = CFG_BRINGUP_RO | CFG_ALLOW_NS;
	if (fw_write_filter(c, F_NS_WINDOW_RO, &f))
		return -1;
	f.config = CFG_BRINGUP_RO;
	if (fw_write_filter(c, F_S_WINDOW_RO, &f))
		return -1;

	f.start = c->win_hi + 1;
	f.end = UINT64_MAX;
	f.config = CFG_BRINGUP_RW | CFG_ALLOW_NS;
	if (fw_write_filter(c, F_NS_ABOVE_RW, &f))
		return -1;
	f.config = CFG_BRINGUP_RW;
	if (fw_write_filter(c, F_S_ABOVE_RW, &f))
		return -1;

	/* Now the blankets stop below the window. Only END changes. */
	c->firewall_programmed = 1;
	if (fw_write64(c, F_NS_BLANKET, 0x10, c->win_lo - 1))
		return -1;
	return fw_write64(c, F_S_BLANKET, 0x10, c->win_lo - 1);
}

static int dump_firewall(struct ctx *c)
{
	unsigned int i;

	if (c->opt.dry_run)
		return 0;
	for (i = 0; i < NUM_FILTERS; i++) {
		struct filter f;

		if (fw_read_filter(c, i, &f))
			return -1;
		if (!is_unprogrammed(&f))
			print_filter(i, &f);
	}
	return 0;
}

static int run_test(struct ctx *c)
{
	enum path gated = c->opt.gated;
	enum path other = gated == PATH_APP ? PATH_SYS : PATH_APP;
	uint64_t gated_addr = path_addr(gated, c->opt.addr);
	uint64_t other_addr = path_addr(other, c->opt.addr);
	uint32_t via_app, via_sys, lo, hi;
	char text[32], lo_text[32], hi_text[32];
	int rc;

	format_addr(text, sizeof(text), c->opt.addr);
	format_addr(lo_text, sizeof(lo_text), c->win_lo);
	format_addr(hi_text, sizeof(hi_text), c->win_hi);
	printf("Firewall under test: %s @ KLA ", c->fw_name);
	format_addr(text, sizeof(text), c->fw_base);
	printf("%s, gating the %s path\n", text, path_name(gated));
	format_addr(text, sizeof(text), c->opt.addr);
	printf("Test word: KLA %s (SMC SPM + 0x%llX); read-only window %s .. %s\n",
	       text, (unsigned long long)(c->opt.addr - SMC_SPM_BASE),
	       lo_text, hi_text);
	if (c->opt.dry_run)
		printf("DRY RUN: no writes will be issued\n");

	step(1, "inspect the firewall (must be in bring-up state)");
	rc = inspect_firewall(c);
	if (rc < 0)
		return 3;
	if (rc && !c->opt.force) {
		printf("      refusing to continue (use --force to override "
		       "the blanket/unused checks, never the lock)\n");
		return 2;
	}
	if (rc) {
		unsigned int i;

		for (i = 0; i < NUM_FILTERS; i++)
			if (c->saved[i].config & CFG_LOCKED)
				return 2;
	}
	verdict(c, 1, "filters 0 and 15 are blankets, 1/2/13/14 free, nothing locked");

	step(2, "read the test word through both paths (must be zero)");
	if (read_both(c, &via_app, &via_sys))
		return 3;
	if (rd32(c, PATH_SYS, c->win_lo, &lo) ||
	    rd32(c, PATH_SYS, c->win_hi - 3, &hi))
		return 3;
	c->sram_original = via_sys;
	printf("      APP 0x%08X  SYS 0x%08X  window first 0x%08X last 0x%08X\n",
	       via_app, via_sys, lo, hi);
	if (via_app != via_sys) {
		verdict(c, 0, "the two paths disagree; not touching this word");
		return 2;
	}
	if ((via_sys || lo || hi) && !c->opt.force) {
		verdict(c, 0, "not zero; this SRAM may be in use (use --force)");
		return 2;
	}
	verdict(c, 1, "word reads 0x%08X on both paths", via_sys);

	step(3, "write pattern A through the gated path and read it back");
	if (wr32(c, gated, gated_addr, PATTERN_A))
		return 3;
	c->sram_dirty = 1;
	if (read_both(c, &via_app, &via_sys))
		return 3;
	printf("      wrote 0x%08X; APP 0x%08X  SYS 0x%08X\n",
	       PATTERN_A, via_app, via_sys);
	if (!c->opt.dry_run && (via_app != PATTERN_A || via_sys != PATTERN_A)) {
		verdict(c, 0, "SRAM is not writable through %s; nothing to gate",
			path_name(gated));
		goto restore_sram;
	}
	verdict(c, 1, "SRAM is writable through %s", path_name(gated));

	step(4, "carve the read-only window out of the blanket filters");
	if (carve_window(c))
		return 3;
	if (dump_firewall(c))
		return 3;
	verdict(c, 1, "window programmed (filters 0,15 shrunk; 1,2,13,14 added)");

	step(5, "write pattern B through the gated path (should be dropped)");
	if (wr32(c, gated, gated_addr, PATTERN_B))
		return 3;
	printf("      wrote 0x%08X\n", PATTERN_B);

	step(6, "read back through both paths (must still be pattern A)");
	if (read_both(c, &via_app, &via_sys))
		return 3;
	printf("      APP 0x%08X  SYS 0x%08X\n", via_app, via_sys);
	if (!c->opt.dry_run) {
		if (via_sys == PATTERN_A)
			verdict(c, 1, "write through %s was blocked", path_name(gated));
		else if (via_sys == PATTERN_B)
			verdict(c, 0, "write through %s got through; the firewall "
				"did not gate it", path_name(gated));
		else
			verdict(c, 0, "unexpected value 0x%08X", via_sys);
		if (gated == PATH_APP && via_app == UINT32_MAX)
			verdict(c, 0, "read through the gated path returned all-ones; "
				"the read-only filter did not match either");
		else if (via_app != via_sys)
			verdict(c, 0, "paths disagree (APP 0x%08X, SYS 0x%08X)",
				via_app, via_sys);
	}

	step(7, "control: write pattern B through the other path (not gated)");
	if (wr32(c, other, other_addr, PATTERN_B))
		return 3;
	if (read_both(c, &via_app, &via_sys))
		return 3;
	printf("      wrote 0x%08X via %s; APP 0x%08X  SYS 0x%08X\n",
	       PATTERN_B, path_name(other), via_app, via_sys);
	if (!c->opt.dry_run) {
		uint32_t seen = other == PATH_SYS ? via_sys : via_app;

		verdict(c, seen == PATTERN_B,
			"%s still writes the word (so the block was %s)",
			path_name(other), c->fw_name);
	}

	if (c->opt.keep) {
		printf("\n--keep: leaving %s carved. Run with --restore-only to "
		       "undo.\n", c->fw_name);
		return c->failures ? 1 : 0;
	}

	step(8, "restore the firewall and verify it matches the saved state");
	if (restore_firewall(c))
		return 3;
	rc = verify_firewall_restored(c);
	if (rc < 0)
		return 3;
	verdict(c, rc == 0, "firewall %s restored", c->fw_name);

	step(9, "write pattern A through the gated path again (must land)");
	if (wr32(c, gated, gated_addr, PATTERN_A))
		return 3;
	if (read_both(c, &via_app, &via_sys))
		return 3;
	printf("      wrote 0x%08X; APP 0x%08X  SYS 0x%08X\n",
	       PATTERN_A, via_app, via_sys);
	if (!c->opt.dry_run)
		verdict(c, via_sys == PATTERN_A && via_app == PATTERN_A,
			"%s writes again", path_name(gated));

restore_sram:
	step(10, "restore the test word");
	if (wr32(c, PATH_SYS, path_addr(PATH_SYS, c->opt.addr), c->sram_original))
		return 3;
	if (read_both(c, &via_app, &via_sys))
		return 3;
	c->sram_dirty = 0;
	if (!c->opt.dry_run)
		verdict(c, via_sys == c->sram_original && via_app == c->sram_original,
			"word restored to 0x%08X", c->sram_original);

	printf("\n%s\n", c->failures ? "FAIL" : "PASS");
	return c->failures ? 1 : 0;
}

static int run_restore_only(struct ctx *c)
{
	unsigned int i;

	printf("Restoring %s to the bring-up state\n", c->fw_name);
	for (i = 0; i < NUM_FILTERS; i++) {
		if (fw_read_filter(c, i, &c->saved[i]))
			return 3;
		if (!is_unprogrammed(&c->saved[i]))
			print_filter(i, &c->saved[i]);
		if (c->saved[i].config & CFG_LOCKED) {
			printf("filter %u is locked; cannot restore\n", i);
			return 2;
		}
	}
	memset(c->saved, 0, sizeof(c->saved));
	c->saved[F_NS_BLANKET].config = CFG_BRINGUP_RW | CFG_ALLOW_NS;
	c->saved[F_NS_BLANKET].end = UINT64_MAX;
	c->saved[F_S_BLANKET].config = CFG_BRINGUP_RW;
	c->saved[F_S_BLANKET].end = UINT64_MAX;
	if (fw_write_filter(c, F_NS_BLANKET, &c->saved[F_NS_BLANKET]))
		return 3;
	if (fw_write_filter(c, F_S_BLANKET, &c->saved[F_S_BLANKET]))
		return 3;
	if (fw_clear_filter(c, F_NS_WINDOW_RO) || fw_clear_filter(c, F_NS_ABOVE_RW) ||
	    fw_clear_filter(c, F_S_ABOVE_RW) || fw_clear_filter(c, F_S_WINDOW_RO))
		return 3;
	printf("Now:\n");
	if (dump_firewall(c))
		return 3;
	return 0;
}

/* ------------------------------------------------------------------------ */

static int parse_u64(const char *text, uint64_t *value)
{
	char clean[128];
	char *end;
	size_t i, n = 0;
	unsigned long long parsed;

	for (i = 0; text[i]; i++) {
		if (text[i] == '_')
			continue;
		if (n + 1 >= sizeof(clean))
			return -1;
		clean[n++] = text[i];
	}
	clean[n] = '\0';
	if (!n)
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
		"  -a, --addr KLA        test word (default 0x080E_0000, middle of SMC SPM)\n"
		"  -w, --window SIZE     read-only window size, power of two >= 4 (default 0x1000)\n"
		"  -g, --gate app|sys    which path the firewall gates: app = BAR0 via\n"
		"                        hsio0-pcie (default), sys = BAR2 via pcie-smn\n"
		"  -k, --keep            leave the window in place after step 7\n"
		"      --restore-only    rewrite the bring-up blankets and clear 1/2/13/14\n"
		"  -n, --dry-run         read everything, write nothing\n"
		"  -f, --force           allow a non-zero word or a non-bring-up firewall\n"
		"  -v, --verbose         print every register access\n"
		"  -h, --help            show this help\n"
		"\n"
		"Exit: 0 pass, 1 a check failed (firewall restored), 2 refused/usage,\n"
		"      3 ioctl failure (firewall restored if it had been changed)\n",
		prog);
}

static int parse_options(int argc, char **argv, struct options *o)
{
	int i;

	memset(o, 0, sizeof(*o));
	o->addr = DEFAULT_ADDR;
	o->window = DEFAULT_WINDOW;
	o->gated = PATH_APP;
	for (i = 2; i < argc; i++) {
		const char *arg = argv[i];

		if (!strcmp(arg, "-a") || !strcmp(arg, "--addr")) {
			if (i + 1 >= argc || parse_u64(argv[++i], &o->addr))
				return -1;
		} else if (!strcmp(arg, "-w") || !strcmp(arg, "--window")) {
			if (i + 1 >= argc || parse_u64(argv[++i], &o->window))
				return -1;
		} else if (!strcmp(arg, "-g") || !strcmp(arg, "--gate")) {
			if (i + 1 >= argc)
				return -1;
			i++;
			if (!strcmp(argv[i], "app"))
				o->gated = PATH_APP;
			else if (!strcmp(argv[i], "sys"))
				o->gated = PATH_SYS;
			else
				return -1;
		} else if (!strcmp(arg, "-k") || !strcmp(arg, "--keep")) {
			o->keep = 1;
		} else if (!strcmp(arg, "--restore-only")) {
			o->restore_only = 1;
		} else if (!strcmp(arg, "-n") || !strcmp(arg, "--dry-run")) {
			o->dry_run = 1;
		} else if (!strcmp(arg, "-f") || !strcmp(arg, "--force")) {
			o->force = 1;
		} else if (!strcmp(arg, "-v") || !strcmp(arg, "--verbose")) {
			o->verbose = 1;
		} else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			return 1;
		} else {
			return -1;
		}
	}
	return 0;
}

static int check_placement(const struct options *o, uint64_t *lo, uint64_t *hi)
{
	uint64_t spm_end = SMC_SPM_BASE + SMC_SPM_SIZE;

	if (o->window < 4 || (o->window & (o->window - 1))) {
		fprintf(stderr, "window must be a power of two >= 4\n");
		return -1;
	}
	if (o->addr & 3) {
		fprintf(stderr, "address must be 4-byte aligned\n");
		return -1;
	}
	*lo = o->addr & ~(o->window - 1);
	*hi = *lo + o->window - 1;
	if (*lo < SMC_SPM_BASE || *hi >= spm_end) {
		fprintf(stderr, "window 0x%llX..0x%llX is not inside SMC SPM "
			"(0x%llX..0x%llX)\n",
			(unsigned long long)*lo, (unsigned long long)*hi,
			(unsigned long long)SMC_SPM_BASE,
			(unsigned long long)(spm_end - 1));
		return -1;
	}
	if (!o->force && *hi >= spm_end - SMC_SPM_TOP_RESERVED) {
		fprintf(stderr, "window reaches into the top 64 KiB of SPM "
			"(fw_params / uRP mailbox / log, host-monitored); "
			"use --force if you mean it\n");
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct ctx c;
	char device_path[PATH_MAX];
	int device_id, rc;

	memset(&c, 0, sizeof(c));
	c.fd = -1;

	if (argc == 2 &&
	    (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return 0;
	}
	if (argc < 2 || parse_device_id(argv[1], &device_id)) {
		usage(argv[0]);
		return 2;
	}
	rc = parse_options(argc, argv, &c.opt);
	if (rc) {
		if (rc < 0)
			fprintf(stderr, "%s: invalid arguments\n", argv[0]);
		usage(argv[0]);
		return rc > 0 ? 0 : 2;
	}
	if (check_placement(&c.opt, &c.win_lo, &c.win_hi))
		return 2;

	if (c.opt.gated == PATH_APP) {
		c.fw_base = HSIO0_PCIE_FW_BASE;
		c.fw_name = "hsio0-pcie";
	} else {
		c.fw_base = PCIE_MGMT_SMN_FW_BASE;
		c.fw_name = "pcie-smn";
	}

#ifdef KERAUNOS_FIREWALL_RO_FAKE
	snprintf(device_path, sizeof(device_path), "fake:/dev/tenstorrent/%d",
		 device_id);
	c.fd = 0;
#else
	{
		struct tenstorrent_get_device_info info = { 0 };

		snprintf(device_path, sizeof(device_path), "/dev/tenstorrent/%d",
			 device_id);
		c.fd = open(device_path, O_RDWR);
		if (c.fd < 0) {
			fprintf(stderr, "Failed to open %s: %s\n", device_path,
				strerror(errno));
			return 3;
		}
		info.in.output_size_bytes = sizeof(info.out);
		if (ioctl(c.fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
			fprintf(stderr, "GET_DEVICE_INFO failed for %s: %s\n",
				device_path, strerror(errno));
			close(c.fd);
			return 3;
		}
		if (info.out.device_id != PCI_DEVICE_ID_KERAUNOS) {
			fprintf(stderr, "%s is PCI device 0x%04X, not Keraunos "
				"(0x%04X).\n", device_path, info.out.device_id,
				PCI_DEVICE_ID_KERAUNOS);
			close(c.fd);
			return 2;
		}
	}
#endif

	if (c.opt.restore_only)
		rc = run_restore_only(&c);
	else
		rc = run_test(&c);

	if (c.firewall_programmed && !c.opt.keep) {
		fprintf(stderr, "Restoring %s after an early exit\n", c.fw_name);
		if (restore_firewall(&c))
			fprintf(stderr, "Restore failed; run --restore-only\n");
	}
	if (c.sram_dirty && !c.opt.dry_run) {
		fprintf(stderr, "Restoring the test word\n");
		wr32(&c, PATH_SYS, path_addr(PATH_SYS, c.opt.addr), c.sram_original);
	}
#ifndef KERAUNOS_FIREWALL_RO_FAKE
	close(c.fd);
#endif
	return rc;
}
