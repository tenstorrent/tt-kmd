// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Keraunos test tool - exercise TENSTORRENT_IOCTL_KER_READ32 / _KER_WRITE32.
//
// [R&D / UNRELEASED] Grendel/Keraunos only. The kernel reserves and programs a
// TLB entry to reach an arbitrary 52-bit system physical address (SPA), so
// userspace just hands it an address (and value, for writes). No mmap, no
// BAR/TLB math here.
//
// To Compile:
//   gcc -O2 -o keraunos_read32 keraunos_read32.c
//
// To Run:
//   ./keraunos_read32 [-a] [-f] <device_id>                 # test suite
//   ./keraunos_read32 [-f] <device_id> <addr> [expected]    # single read
//   ./keraunos_read32 [-f] <device_id> <addr> -w <value>    # single write
//     -a / --all    : in suite mode, also dump full SMC scratch banks
//     -w / --write V : write V to <addr> (then read back to verify)
//     -f / --force  : permit an access outside the safe allowlist / to a
//                     write-protected register
//
// SAFETY (emulation): each MMIO is real seconds over the ZeBu transactor, and
// accesses to unmodeled or special-semantics regions can *hang the emulator*
// (observed: SEP SRAM 0x1201800000 wedged it; SMC mailbox 0x1202018000 returns
// 0xffffffff; writing cpu_ctrl SCRATCH_1 -- the SIM POST-code register -- also
// hung it). Reads touch only addresses confirmed to respond cleanly (SMC scratch
// + a couple of plain SMC CPU-ctrl regs). WRITES are restricted to the SMC cold
// scratch bank (plain RW storage, restored afterward); anything else needs -f.
//
// Examples:
//   ./keraunos_read32 0                  # run read + write-RMW test suite
//   ./keraunos_read32 -a 0               # suite + full scratch-bank dump
//   ./keraunos_read32 0 0x1202010108     # read SMC cpu_ctrl SCRATCH_1
//   ./keraunos_read32 0 0x1202002810 -w 0xdeadbeef   # write a cold scratch reg
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/types.h>

// --- Driver Definitions (mirror of ioctl.h) ---
// NOTE: KER_READ32/KER_WRITE32 are R&D/unreleased, Grendel/Keraunos only.
#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)
#define TENSTORRENT_IOCTL_KER_READ32      _IO(TENSTORRENT_IOCTL_MAGIC, 16)
#define TENSTORRENT_IOCTL_KER_WRITE32     _IO(TENSTORRENT_IOCTL_MAGIC, 17)

struct tenstorrent_get_device_info {
	struct { __u32 output_size_bytes; } in;
	struct {
		__u32 output_size_bytes;
		__u16 vendor_id, device_id, subsystem_vendor_id, subsystem_id;
		__u16 bus_dev_fn, max_dma_buf_size_log2, pci_domain, reserved;
	} out;
};

struct tenstorrent_ker_read32 {
	__u32 argsz;
	__u32 flags;
	__u64 addr;
	__u32 value;
	__u32 reserved;
};

struct tenstorrent_ker_write32 {
	__u32 argsz;
	__u32 flags;
	__u64 addr;
	__u32 value;
	__u32 reserved;
};

// --- Known firmware values --------------------------------------------------
// Firmware "init complete" sentinel, in SMC cpu_ctrl SCRATCH_0.
#define INIT_COMPLETE_SENTINEL 0xa111600d
#define TEST_PASS_CODE         0xacafaca1	// SMC_SCRATCHPAD_SIM_PASS_CODE
#define TEST_FAIL_CODE         0xdeadbeef

// --- SMC register map (KLA -> SPA via Keraunos remap: SPA = 0x1202000000 +
// (KLA - 0x08000000)). Addresses from keraunos_soc_reg.py. -------------------
// SMC cpu_ctrl SCRATCH bank: 16 regs, 8-byte spacing (SCRATCH_i @ base + i*8).
#define SMC_CPUCTRL_SCRATCH_BASE   0x1202010100ULL
#define SMC_CPUCTRL_SCRATCH_STRIDE 0x8
#define SMC_CPUCTRL_SCRATCH_COUNT  16
#define SMC_CPUCTRL_SCRATCH(i)     (SMC_CPUCTRL_SCRATCH_BASE + (uint64_t)(i) * SMC_CPUCTRL_SCRATCH_STRIDE)
// SMC cold SCRATCH bank: 8 regs, 4-byte spacing (SCRATCH_i @ base + i*4).
#define SMC_COLD_SCRATCH_BASE      0x1202002800ULL
#define SMC_COLD_SCRATCH_STRIDE    0x4
#define SMC_COLD_SCRATCH_COUNT     8
#define SMC_COLD_SCRATCH(i)        (SMC_COLD_SCRATCH_BASE + (uint64_t)(i) * SMC_COLD_SCRATCH_STRIDE)
// Other plain, side-effect-free SMC cpu_ctrl registers (emulation reads these).
#define SMC_RESET_VECTOR_0         0x1202010000ULL
#define SMC_RESET_CTRL             0x1202010020ULL

struct reg_target {
	uint64_t spa;
	uint32_t expected;	// 0 => informational only (no pass/fail)
	const char *name;
};

// --- Curated read sweep: high-value, known-safe, fast ------------------------
static const struct reg_target sweep_targets[] = {
	{ SMC_CPUCTRL_SCRATCH_BASE, INIT_COMPLETE_SENTINEL, "SMC cpu_ctrl SCRATCH_0 (init sentinel)" },
	{ SMC_COLD_SCRATCH_BASE,    0,                      "SMC cold SCRATCH_0" },
};
#define NUM_SWEEP_TARGETS (sizeof(sweep_targets) / sizeof(sweep_targets[0]))

// --- Write read-modify-write self-test targets -------------------------------
// ONLY the SMC cold scratch bank. It is a separate hardware block from the
// cpu_ctrl SCRATCH bank, with no SIM or SMC/SEP-coordination semantics, so it
// is plain RW storage that is safe to clobber and restore.
//
// Do NOT add cpu_ctrl SCRATCH registers here. The host-side JTAG bring-up test
// (chippy .../smc/ker_load_fw/ker_load_fw_test.cpp) polls the cpu_ctrl scratch
// bank and exits its run loop when scratch[0]==0xACAFACA1/0xdeadbeef or
// scratch[1]==0xDEADBEEF. When that test process exits, the AXI/PCIe proxy that
// keeps the QEMU link alive tears down, and the next host access times out
// ("Timeout while waiting for PCI Completion"). Writing 0xDEADBEEF into cpu_ctrl
// SCRATCH_1 did exactly this and wedged the emulator. (These regs also carry
// SIM / SMC<->SEP-coordination semantics per smc_scratchpad.h.) The cold scratch
// bank is a separate block the test does not watch. Each reg is restored to its
// original value after the test.
static const struct reg_target wtest_targets[] = {
	{ SMC_COLD_SCRATCH(1), 0, "SMC cold SCRATCH_1" },
	{ SMC_COLD_SCRATCH(2), 0, "SMC cold SCRATCH_2" },
	{ SMC_COLD_SCRATCH(3), 0, "SMC cold SCRATCH_3" },
	{ SMC_COLD_SCRATCH(4), 0, "SMC cold SCRATCH_4" },
	{ SMC_COLD_SCRATCH(5), 0, "SMC cold SCRATCH_5" },
	{ SMC_COLD_SCRATCH(6), 0, "SMC cold SCRATCH_6" },
	{ SMC_COLD_SCRATCH(7), 0, "SMC cold SCRATCH_7" },
};
#define NUM_WTEST_TARGETS (sizeof(wtest_targets) / sizeof(wtest_targets[0]))

// --- Allowlist: SPA ranges confirmed safe to read on the emulator. Anything
// outside requires -f. Well clear of the SMC mailbox (0x1202018000) and SEP
// SRAM (0x1201800000), both of which misbehave. ------------------------------
struct safe_range {
	uint64_t base;
	uint64_t size;
	const char *name;
};

static const struct safe_range safe_ranges[] = {
	{ SMC_COLD_SCRATCH_BASE,    0x20, "SMC cold scratch bank (8 regs)" },
	{ SMC_CPUCTRL_SCRATCH_BASE, 0x80, "SMC cpu_ctrl scratch bank (16 regs)" },
	{ SMC_RESET_VECTOR_0,       0x4,  "SMC reset vector 0" },
	{ SMC_RESET_CTRL,           0x4,  "SMC reset control" },
};
#define NUM_SAFE_RANGES (sizeof(safe_ranges) / sizeof(safe_ranges[0]))

// True if a 4-byte access at spa lies entirely within an allowlisted range.
static int addr_is_safe(uint64_t spa)
{
	size_t i;

	for (i = 0; i < NUM_SAFE_RANGES; i++) {
		uint64_t lo = safe_ranges[i].base;
		uint64_t hi = safe_ranges[i].base + safe_ranges[i].size;

		if (spa >= lo && spa + 4 <= hi)
			return 1;
	}
	return 0;
}

// Writes are only known-safe to the SMC cold scratch bank: plain RW storage
// with no SIM / SMC-SEP-coordination semantics. Everything else that is
// read-safe is NOT write-safe -- the cpu_ctrl SCRATCH bank has SIM/coordination
// side effects (writing cpu_ctrl SCRATCH_1 hung the emulator), and the reset
// vector/control registers would reset the SMC CPU. Forcing (-f) is required to
// write anything outside the cold bank.
static int addr_is_write_safe(uint64_t spa)
{
	return spa >= SMC_COLD_SCRATCH_BASE &&
	       spa + 4 <= SMC_COLD_SCRATCH_BASE + 0x20;
}

static void print_safe_ranges(FILE *f)
{
	size_t i;

	for (i = 0; i < NUM_SAFE_RANGES; i++)
		fprintf(f, "    0x%012llx - 0x%012llx  %s\n",
			(unsigned long long)safe_ranges[i].base,
			(unsigned long long)(safe_ranges[i].base + safe_ranges[i].size - 1),
			safe_ranges[i].name);
}

static int do_read32(int fd, uint64_t spa, uint32_t *value)
{
	struct tenstorrent_ker_read32 io = {0};

	io.argsz = sizeof(io);
	io.addr = spa;
	if (ioctl(fd, TENSTORRENT_IOCTL_KER_READ32, &io) < 0)
		return -errno;

	*value = io.value;
	return 0;
}

static int do_write32(int fd, uint64_t spa, uint32_t value)
{
	struct tenstorrent_ker_write32 io = {0};

	io.argsz = sizeof(io);
	io.addr = spa;
	io.value = value;
	if (ioctl(fd, TENSTORRENT_IOCTL_KER_WRITE32, &io) < 0)
		return -errno;

	return 0;
}

static const char *decode_value(uint32_t val)
{
	if (val == INIT_COMPLETE_SENTINEL)
		return " <- init-complete sentinel";
	if (val == TEST_PASS_CODE)
		return " <- TEST_PASS";
	if (val == TEST_FAIL_CODE)
		return " <- TEST_FAIL";
	if (val == 0xFFFFFFFF)
		return " (all 1s - unmapped/no response?)";
	if (val == 0x00000000)
		return " (all 0s)";
	return "";
}

// Read one register, print a table row, update pass/fail counters.
static void sweep_one(int fd, uint64_t spa, uint32_t expected, const char *name,
		      int *checked, int *failures)
{
	uint32_t value;
	int rc = do_read32(fd, spa, &value);

	if (rc) {
		printf("  0x%012llx %-40s ERROR: %s\n",
		       (unsigned long long)spa, name, strerror(-rc));
		(*failures)++;
		return;
	}

	printf("  0x%012llx %-40s 0x%08x%s",
	       (unsigned long long)spa, name, value, decode_value(value));

	if (expected) {
		int ok = (value == expected);
		(*checked)++;
		if (!ok)
			(*failures)++;
		printf("  [expected 0x%08x: %s]", expected, ok ? "PASS" : "FAIL");
	}
	printf("\n");
}

// Read-modify-write a single register with several patterns, verifying each
// readback, then restore the original value. Each pattern counts as a check.
static void rmw_test_one(int fd, uint64_t spa, const char *name,
			 int *checked, int *failures)
{
	// Deliberately avoid the firmware sentinel values (0xACAFACA1,
	// 0xDEADBEEF, 0xA111600D): host-side tests/monitors poll the cpu_ctrl
	// scratch bank for those, and writing one can terminate a running test
	// and collapse the emulator link. These neutral walking-bit patterns
	// can't be mistaken for a sentinel even if forced (-f) at a bad address.
	uint32_t patterns[] = { 0xa5a5a5a5, 0x5a5a5a5a, 0xcafef00d,
				(uint32_t)spa /* address-in-data (aliasing) */ };
	size_t num_patterns = sizeof(patterns) / sizeof(patterns[0]);
	uint32_t orig, got;
	size_t p;
	int rc;

	rc = do_read32(fd, spa, &orig);
	if (rc) {
		printf("  0x%012llx %-24s read(orig) ERROR: %s\n",
		       (unsigned long long)spa, name, strerror(-rc));
		(*failures)++;
		return;
	}

	for (p = 0; p < num_patterns; p++) {
		uint32_t pat = patterns[p];
		int ok;

		rc = do_write32(fd, spa, pat);
		if (rc) {
			printf("  0x%012llx %-24s write 0x%08x ERROR: %s\n",
			       (unsigned long long)spa, name, pat, strerror(-rc));
			(*failures)++;
			goto restore;
		}

		rc = do_read32(fd, spa, &got);
		if (rc) {
			printf("  0x%012llx %-24s read 0x%08x ERROR: %s\n",
			       (unsigned long long)spa, name, pat, strerror(-rc));
			(*failures)++;
			goto restore;
		}

		ok = (got == pat);
		(*checked)++;
		if (!ok)
			(*failures)++;
		printf("  0x%012llx %-24s w=0x%08x r=0x%08x  %s\n",
		       (unsigned long long)spa, name, pat, got, ok ? "PASS" : "FAIL");
	}

restore:
	// Put the original value back and confirm it took.
	if (do_write32(fd, spa, orig) == 0 && do_read32(fd, spa, &got) == 0 && got == orig) {
		printf("  0x%012llx %-24s restored 0x%08x\n",
		       (unsigned long long)spa, name, orig);
	} else {
		printf("  0x%012llx %-24s WARNING: restore to 0x%08x FAILED\n",
		       (unsigned long long)spa, name, orig);
		(*failures)++;
	}
}

static void print_read_header(void)
{
	printf("  %-14s %-40s %s\n", "SPA", "Register", "Value");
	printf("  %-14s %-40s %s\n", "--------------",
	       "----------------------------------------", "-----");
}

static void print_rmw_header(void)
{
	printf("  %-14s %-24s %s\n", "SPA", "Register", "Result");
	printf("  %-14s %-24s %s\n", "--------------",
	       "------------------------", "------");
}

static int run_test_suite(int fd, int dump_all)
{
	int failures = 0;
	int checked = 0;
	size_t i;
	int j;
	char name[64];

	printf("=== Read checks ===\n");
	print_read_header();
	for (i = 0; i < NUM_SWEEP_TARGETS; i++)
		sweep_one(fd, sweep_targets[i].spa, sweep_targets[i].expected,
			  sweep_targets[i].name, &checked, &failures);

	if (dump_all) {
		printf("\nSMC cpu_ctrl scratch bank:\n");
		print_read_header();
		for (j = 0; j < SMC_CPUCTRL_SCRATCH_COUNT; j++) {
			uint32_t expected = (j == 0) ? INIT_COMPLETE_SENTINEL : 0;
			snprintf(name, sizeof(name), "SMC cpu_ctrl SCRATCH_%d", j);
			sweep_one(fd, SMC_CPUCTRL_SCRATCH(j), expected, name, &checked, &failures);
		}

		printf("\nSMC cold scratch bank:\n");
		print_read_header();
		for (j = 0; j < SMC_COLD_SCRATCH_COUNT; j++) {
			snprintf(name, sizeof(name), "SMC cold SCRATCH_%d", j);
			sweep_one(fd, SMC_COLD_SCRATCH(j), 0, name, &checked, &failures);
		}

		printf("\nSMC cpu_ctrl misc registers:\n");
		print_read_header();
		sweep_one(fd, SMC_RESET_VECTOR_0, 0, "SMC reset vector 0", &checked, &failures);
		sweep_one(fd, SMC_RESET_CTRL,     0, "SMC reset control",  &checked, &failures);
	}

	printf("\n=== Write read-modify-write checks (restored afterward) ===\n");
	print_rmw_header();
	for (i = 0; i < NUM_WTEST_TARGETS; i++) {
		// Defensive: the suite only ever writes the cold scratch bank.
		if (!addr_is_write_safe(wtest_targets[i].spa))
			continue;
		rmw_test_one(fd, wtest_targets[i].spa, wtest_targets[i].name,
			     &checked, &failures);
	}

	printf("\n");
	if (failures == 0)
		printf("All %d check(s) passed. PASS\n", checked);
	else
		printf("%d of %d check(s) FAILED.\n", failures, checked);

	if (!dump_all)
		printf("(Re-run with -a to also dump the full SMC scratch banks.)\n");

	return failures ? 1 : 0;
}

static int single_read(int fd, uint64_t spa, int have_expected, uint32_t expected, int force)
{
	uint32_t value;
	int rc;

	if (!addr_is_safe(spa) && !force) {
		fprintf(stderr,
			"REFUSING to read 0x%012llx: outside the known-safe allowlist.\n"
			"Reads to unmodeled/special regions can hang the ZeBu emulator.\n"
			"Safe ranges (no -f required):\n", (unsigned long long)spa);
		print_safe_ranges(stderr);
		fprintf(stderr, "If you are sure this address is modeled and safe, re-run with -f.\n");
		return 2;
	}
	if (!addr_is_safe(spa))
		printf("WARNING: 0x%012llx is outside the safe allowlist; forced read.\n",
		       (unsigned long long)spa);

	rc = do_read32(fd, spa, &value);
	if (rc) {
		fprintf(stderr, "READ32 0x%llx failed: %s\n",
			(unsigned long long)spa, strerror(-rc));
		return 1;
	}

	printf("read32(0x%012llx) = 0x%08x%s\n",
	       (unsigned long long)spa, value, decode_value(value));

	if (have_expected) {
		int ok = (value == expected);
		printf("expected        = 0x%08x  => %s\n", expected, ok ? "PASS" : "FAIL");
		return ok ? 0 : 1;
	}
	return 0;
}

static int single_write(int fd, uint64_t spa, uint32_t value, int force)
{
	uint32_t got;
	int rc;

	if (!addr_is_write_safe(spa) && !force) {
		fprintf(stderr,
			"REFUSING to write 0x%012llx: only the SMC cold scratch bank is\n"
			"write-safe (0x%012llx - 0x%012llx). Writing the cpu_ctrl SCRATCH bank\n"
			"(SIM/coordination semantics; SCRATCH_1 hung the emulator) or the reset\n"
			"vector/control registers can disrupt firmware or wedge ZeBu.\n"
			"Re-run with -f if you are certain.\n",
			(unsigned long long)spa,
			(unsigned long long)SMC_COLD_SCRATCH_BASE,
			(unsigned long long)(SMC_COLD_SCRATCH_BASE + 0x20 - 1));
		return 2;
	}
	if (!addr_is_write_safe(spa))
		printf("WARNING: forced write to 0x%012llx (outside the cold scratch bank).\n",
		       (unsigned long long)spa);

	rc = do_write32(fd, spa, value);
	if (rc) {
		fprintf(stderr, "WRITE32 0x%llx failed: %s\n",
			(unsigned long long)spa, strerror(-rc));
		return 1;
	}

	rc = do_read32(fd, spa, &got);
	if (rc) {
		fprintf(stderr, "wrote 0x%08x, but readback failed: %s\n", value, strerror(-rc));
		return 1;
	}

	printf("write32(0x%012llx, 0x%08x) -> readback 0x%08x  %s\n",
	       (unsigned long long)spa, value, got,
	       got == value ? "(matches)" : "(MISMATCH)");
	return got == value ? 0 : 1;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s [-a] [-f] <device_id>               # read+write test suite\n", prog);
	fprintf(stderr, "  %s [-f] <device_id> <addr> [expected]  # single read\n", prog);
	fprintf(stderr, "  %s [-f] <device_id> <addr> -w <value>  # single write (verified)\n", prog);
	fprintf(stderr, "\nSafe (no -f required) ranges:\n");
	print_safe_ranges(stderr);
}

int main(int argc, char *argv[])
{
	char dev_path[PATH_MAX];
	struct tenstorrent_get_device_info info = {0};
	const char *prog = argv[0];
	const char *pos[3];
	int npos = 0;
	int force = 0, dump_all = 0, have_write = 0;
	uint32_t write_val = 0;
	int fd, ret;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "-f") || !strcmp(a, "--force")) {
			force = 1;
		} else if (!strcmp(a, "-a") || !strcmp(a, "--all")) {
			dump_all = 1;
		} else if (!strcmp(a, "-w") || !strcmp(a, "--write")) {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s: -w requires a value argument\n", prog);
				return 2;
			}
			write_val = (uint32_t)strtoul(argv[++i], NULL, 0);
			have_write = 1;
		} else if (npos < 3) {
			pos[npos++] = a;
		} else {
			npos++;
		}
	}

	if (npos < 1 || npos > 3) {
		usage(prog);
		return 2;
	}

	int dev_id = atoi(pos[0]);
	snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		return 1;
	}

	info.in.output_size_bytes = sizeof(info.out);
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
		fprintf(stderr, "GET_DEVICE_INFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("=== Keraunos Test Tool (KER_READ32 / KER_WRITE32) ===\n");
	printf("Device: %s  Vendor: 0x%04x  Device: 0x%04x\n",
	       dev_path, info.out.vendor_id, info.out.device_id);
	if (info.out.device_id != 0xFEED)
		printf("WARNING: Device ID is not 0xFEED (Keraunos). Proceeding anyway...\n");
	printf("\n");

	if (have_write) {
		if (npos < 2) {
			fprintf(stderr, "%s: -w requires an <addr> argument\n", prog);
			close(fd);
			return 2;
		}
		ret = single_write(fd, strtoull(pos[1], NULL, 0), write_val, force);
	} else if (npos >= 2) {
		uint64_t spa = strtoull(pos[1], NULL, 0);
		int have_expected = (npos == 3);
		uint32_t expected = have_expected ? (uint32_t)strtoul(pos[2], NULL, 0) : 0;

		ret = single_read(fd, spa, have_expected, expected, force);
	} else {
		ret = run_test_suite(fd, dump_all);
	}

	close(fd);
	return ret;
}
