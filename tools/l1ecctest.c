// l1ecctest: tt-metal's L1EccHaltTest.UnpopulatedRowErrorsAreIgnored, without tt-metal.
//
// Standalone: needs only a C compiler and the tenstorrent kernel driver (any 2.x with ALLOCATE_TLB).
//   cc -O2 -Wall -o l1ecctest l1ecctest.c
//   ./l1ecctest [-d DEV] [--src A] [--dst A] [--len N] [--scrub on|off] [--all | X,Y ...]
//
//   X,Y          tiles to test (translated coordinates); default (1,2), the tile the tt-metal test runs on
//                (logical core (0,0))
//   --all        every Tensix tile present on all SKUs
//   --src A      read source (default 0x180000, row 3072: the first L1 row with no SRAM)
//   --dst A      read destination (default 0x1b800, where the tt-metal test lands; 0x15fc00 is the Mret test's)
//   --len N      read length (default 1024)
//   --scrub      on (default): the ECC scrubber configured as tt-metal's BRISC firmware does (enable, scrub on
//                error, delay 0x100); off: scrubber disabled
//
// For each tile it holds the RISC-Vs in reset and loads a small BRISC program, generated below, that does what
// tt-metal's firmware and test kernel do:
//   1. configures the scrubber, and arms the L1 DBE interrupt exactly as arm_l1_ecc_halt() does: ECC_CTRL clear,
//      then 0, then DBE IRQ enable with the counts view; the IRQ 3 handler in HW_INT_PC[3]; BRISC_HW_INT_EN bit 3
//   2. issues the overread from the core: one NOC0 read of LEN bytes from SRC to DST on its own L1 (the kernel's
//      noc_async_read), and waits for the response
// The IRQ 3 handler follows l1_ecc_check(): it reads the last L1 error; a valid error in row >= 3072 is false, so
// it clears the counts and IRQ and returns (mret, retried if the branch predictor skips it); anything else halts
// the core and records the ECC state. The test passes if the read completes without a halt.
//
// The program and its parameters use L1 0x0-0x4ff; the read writes DST. RISC-Vs are left in reset; the next
// tt-metal device open reloads firmware. Don't run it while anything else uses the device.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// tenstorrent KMD uapi (ioctl.h), the parts used here.
#define TT_IOCTL(n) (((0xFA) << 8) | (n))
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO TT_IOCTL(0)
#define TENSTORRENT_IOCTL_ALLOCATE_TLB TT_IOCTL(11)
#define TENSTORRENT_IOCTL_CONFIGURE_TLB TT_IOCTL(13)

struct tenstorrent_get_device_info {
	struct {
		uint32_t output_size_bytes;
	} in;
	struct {
		uint32_t output_size_bytes;
		uint16_t vendor_id, device_id, subsystem_vendor_id, subsystem_id, bus_dev_fn;
		uint16_t max_dma_buf_size_log2, pci_domain, reserved;
	} out;
};

struct tenstorrent_allocate_tlb {
	struct {
		uint64_t size, reserved;
	} in;
	struct {
		uint32_t id, reserved0;
		uint64_t mmap_offset_uc, mmap_offset_wc, reserved1;
	} out;
};

struct tenstorrent_configure_tlb {
	struct {
		uint32_t id, reserved;
		struct {
			uint64_t addr;
			uint16_t x_end, y_end, x_start, y_start;
			uint8_t noc, mcast, ordering, linked, static_vc;
			uint8_t reserved0[3];
			uint32_t reserved1[2];
		} config;
	} in;
	struct {
		uint64_t reserved;
	} out;
};

// Blackhole Tensix addresses (tt-metal hw/inc/internal/tt-1xx/blackhole, tt-isa-documentation BlackholeA0).
#define L1_SIZE 0x180000u
#define POPULATED_ROWS 3072u
#define SOFT_RESET 0xFFB121B0u
#define ALL_RISCS_RESET 0x47800u
#define BRISC_RESET 0x800u
#define ECC_CTRL 0xFFB121D0u		// ECC_STATUS is ECC_CTRL + 4
#define ECC_CTRL_COUNTS_CLR 0x10u
#define ECC_CTRL_IRQ_CLR 0x20u
#define ECC_CTRL_DBE_IRQ_EN 0x200u
#define VIEW_COUNTS 2u
#define VIEW_LAST_0_15 3u
#define VIEW_LAST_16_31 6u
#define PIC_BRISC_HW_INT_EN 0xFFB13004u
#define PIC_HW_INT_PC3 0xFFB13134u	// HW_INT_PC[3]
#define TENSIX_CFG_SCRUBBER 0xFFEF000Cu	// config register 3; BRISC can store to it, the NoC can't reach it
#define SCRUB_TT_METAL 0x803u		// enable, scrub on error, delay 0x100
#define NIU0_RD_CMD_BUF 0xFFB20800u	// BRISC_RD_CMD_BUF (1) on NOC0
#define NIU0_RD_RESP_RECEIVED 0xFFB20208u
#define NOC_CMD_RD_STATIC_VC1 0x2090u	// RD, RESP_MARKED, VC_STATIC, VC 1

// L1 layout used by the program.
#define PARAM 0x400u
#define P_SRC 0x00
#define P_DST 0x04
#define P_LEN 0x08
#define P_COORD 0x0C
#define P_SCRUB 0x10
#define R_STATE 0x20			// 0 running, 1 read done without a halt, 2 halted
#define R_IGNORED 0x24			// false errors the handler ignored and returned from
#define R_LAST_IGNORED 0x28		// view 3 of the last ignored error
#define R_HALT_V3 0x2C			// at a halt: views 3, 6 and 2
#define R_HALT_V6 0x30
#define R_HALT_COUNTS 0x34
#define STATE_DONE 1u
#define STATE_HALTED 2u
#define PROGRAM_MAX 0x400u

#define WINDOW (2u << 20)

static int fd = -1;
static uint32_t tlb_id;
static volatile uint8_t *win;
static uint64_t win_base = ~0ull;
static unsigned tx, ty;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "l1ecctest: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

// ---- RV32I encoder, enough for the program below. Branches and jumps take label numbers, resolved at the end.

enum { ZERO = 0, RA = 1, SP = 2, T0 = 5, T1 = 6, T2 = 7, S0 = 8, S1 = 9, A0 = 10, A1, A2, A3, A4, A5, A6, A7,
       T3 = 28, T4, T5, T6 };

static uint32_t prog[PROGRAM_MAX / 4];
static unsigned pc_words;
static int label_at[32];
static struct {
	unsigned at, label, kind; // kind 0: B-type with funct3/regs already in the word; 1: JAL
} fixups[64];
static unsigned nfixups;

static void emit(uint32_t insn)
{
	if (pc_words == PROGRAM_MAX / 4)
		die("program too long");
	prog[pc_words++] = insn;
}

static void i_type(unsigned op, unsigned f3, unsigned rd, unsigned rs1, int32_t imm)
{
	emit(((uint32_t)imm & 0xFFF) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op);
}

static void addi(unsigned rd, unsigned rs1, int32_t imm) { i_type(0x13, 0, rd, rs1, imm); }
static void srli(unsigned rd, unsigned rs1, unsigned sh) { i_type(0x13, 5, rd, rs1, sh); }
static void lw(unsigned rd, unsigned rs1, int32_t off) { i_type(0x03, 2, rd, rs1, off); }
static void and_(unsigned rd, unsigned rs1, unsigned rs2) { emit(rs2 << 20 | rs1 << 15 | 7 << 12 | rd << 7 | 0x33); }
static void or_(unsigned rd, unsigned rs1, unsigned rs2) { emit(rs2 << 20 | rs1 << 15 | 6 << 12 | rd << 7 | 0x33); }

static void sw(unsigned rs2, unsigned rs1, int32_t off)
{
	uint32_t imm = (uint32_t)off & 0xFFF;

	emit((imm >> 5) << 25 | rs2 << 20 | rs1 << 15 | 2 << 12 | (imm & 0x1F) << 7 | 0x23);
}

static void li(unsigned rd, uint32_t v)
{
	uint32_t hi = (v + 0x800) & 0xFFFFF000u;
	int32_t lo = (int32_t)(v - hi);

	if (hi) {
		emit(hi | rd << 7 | 0x37); // lui
		if (lo)
			addi(rd, rd, lo);
	} else {
		addi(rd, ZERO, lo);
	}
}

static void fence(void) { emit(0x0FF0000F); }
static void mret(void) { emit(0x30200073); }
static void label(unsigned l) { label_at[l] = (int)pc_words; }

static void branch(unsigned f3, unsigned rs1, unsigned rs2, unsigned l)
{
	fixups[nfixups].at = pc_words;
	fixups[nfixups].label = l;
	fixups[nfixups++].kind = 0;
	emit(rs2 << 20 | rs1 << 15 | f3 << 12 | 0x63);
}

static void beq(unsigned a, unsigned b, unsigned l) { branch(0, a, b, l); }
static void bne(unsigned a, unsigned b, unsigned l) { branch(1, a, b, l); }
static void bltu(unsigned a, unsigned b, unsigned l) { branch(6, a, b, l); }

static void j(unsigned l)
{
	fixups[nfixups].at = pc_words;
	fixups[nfixups].label = l;
	fixups[nfixups++].kind = 1;
	emit(0x6F); // jal zero
}

static void resolve(void)
{
	for (unsigned i = 0; i < nfixups; i++) {
		if (label_at[fixups[i].label] < 0)
			die("unresolved label %u", fixups[i].label);
		uint32_t off = (uint32_t)((label_at[fixups[i].label] - (int)fixups[i].at) * 4);
		uint32_t *w = &prog[fixups[i].at];

		if (fixups[i].kind == 0)
			*w |= ((off >> 12) & 1) << 31 | ((off >> 5) & 0x3F) << 25 | ((off >> 1) & 0xF) << 8 |
			      ((off >> 11) & 1) << 7;
		else
			*w |= ((off >> 20) & 1) << 31 | ((off >> 1) & 0x3FF) << 21 | ((off >> 11) & 1) << 20 |
			      ((off >> 12) & 0xFF) << 12;
	}
}

// Main code uses only a0-a7; the handler uses only t0-t6 and s0, so it needs no stack and saves nothing.
enum { L_SKIP_SCRUB, L_READY, L_WAIT_RESP, L_DELAY, L_SPIN, L_HANDLER, L_MRET, L_HALT, L_HANG };

static void build_program(void)
{
	memset(label_at, -1, sizeof(label_at));

	// 1. Scrubber, as tt-metal's firmware configures it (skipped when the parameter is ~0).
	li(A0, PARAM);
	lw(A4, A0, P_SCRUB);
	li(A5, 0xFFFFFFFFu);
	beq(A4, A5, L_SKIP_SCRUB);
	li(A5, TENSIX_CFG_SCRUBBER);
	sw(A4, A5, 0);
	label(L_SKIP_SCRUB);

	// 2. arm_l1_ecc_halt(): clear, 0, armed with the counts view; handler; BRISC_HW_INT_EN bit 3 last.
	li(A1, ECC_CTRL);
	li(A4, ECC_CTRL_COUNTS_CLR | ECC_CTRL_IRQ_CLR);
	sw(A4, A1, 0);
	sw(ZERO, A1, 0);
	li(A4, ECC_CTRL_DBE_IRQ_EN | VIEW_COUNTS);
	sw(A4, A1, 0);
	fence();
	li(A5, PIC_HW_INT_PC3);
	unsigned handler_li = pc_words; // two words, patched below with the handler's address
	emit(0x13);
	emit(0x13);
	sw(A4, A5, 0);
	li(A5, PIC_BRISC_HW_INT_EN);
	lw(A4, A5, 0);
	li(A6, 1u << 3);
	or_(A4, A4, A6);
	sw(A4, A5, 0);
	fence();

	// 3. The overread: one NOC0 read of the tile's own L1, as noc_async_read does, then wait for the response.
	li(A2, NIU0_RD_CMD_BUF);
	label(L_READY);
	lw(A4, A2, 0x40); // NOC_CMD_CTRL: 0 when the initiator is free
	bne(A4, ZERO, L_READY);
	li(A6, NIU0_RD_RESP_RECEIVED);
	lw(A3, A6, 0);
	li(A4, NOC_CMD_RD_STATIC_VC1);
	sw(A4, A2, 0x1C);  // NOC_CTRL
	sw(ZERO, A2, 0x18); // NOC_PACKET_TAG
	lw(A4, A0, P_SRC);
	sw(A4, A2, 0x00);  // TARG_ADDR_LO
	sw(ZERO, A2, 0x04);
	lw(A5, A0, P_COORD);
	sw(A5, A2, 0x08);  // TARG_ADDR_HI: x | y << 6
	lw(A4, A0, P_DST);
	sw(A4, A2, 0x0C);  // RET_ADDR_LO
	sw(ZERO, A2, 0x10);
	sw(A5, A2, 0x14);  // RET_ADDR_HI
	lw(A4, A0, P_LEN);
	sw(A4, A2, 0x20);  // NOC_AT_LEN_BE
	li(A4, 1);
	sw(A4, A2, 0x40);  // NOC_CMD_CTRL: send
	label(L_WAIT_RESP);
	lw(A4, A6, 0);
	beq(A4, A3, L_WAIT_RESP);
	// Give interrupts raised by the last words time to arrive before declaring the read done.
	li(A4, 100000);
	label(L_DELAY);
	addi(A4, A4, -1);
	bne(A4, ZERO, L_DELAY);
	li(A4, STATE_DONE);
	sw(A4, A0, R_STATE);
	fence();
	label(L_SPIN);
	j(L_SPIN);

	// IRQ 3 handler: l1_ecc_check().
	label(L_HANDLER);
	uint32_t handler_addr = (uint32_t)pc_words * 4;
	li(T0, ECC_CTRL);
	li(T1, ECC_CTRL_DBE_IRQ_EN | VIEW_LAST_0_15);
	sw(T1, T0, 0);
	fence();
	lw(T2, T0, 4); // last error, banks 0-15: valid [31], row [11:0]
	li(T1, ECC_CTRL_DBE_IRQ_EN | VIEW_LAST_16_31);
	sw(T1, T0, 0);
	fence();
	lw(T3, T0, 4);
	li(T1, ECC_CTRL_DBE_IRQ_EN | VIEW_COUNTS);
	sw(T1, T0, 0);
	fence();
	lw(T4, T0, 4);
	srli(T5, T2, 31);
	beq(T5, ZERO, L_HALT); // no valid L1 error: IRQ 3 came from elsewhere
	li(T6, 0xFFF);
	and_(T5, T2, T6);
	li(T6, POPULATED_ROWS);
	bltu(T5, T6, L_HALT); // error in a populated row: halt
	li(T1, ECC_CTRL_DBE_IRQ_EN | VIEW_COUNTS | ECC_CTRL_COUNTS_CLR | ECC_CTRL_IRQ_CLR);
	sw(T1, T0, 0);
	li(T1, ECC_CTRL_DBE_IRQ_EN | VIEW_COUNTS);
	sw(T1, T0, 0);
	li(S0, PARAM);
	lw(T1, S0, R_IGNORED);
	addi(T1, T1, 1);
	sw(T1, S0, R_IGNORED);
	sw(T2, S0, R_LAST_IGNORED);
	fence();
	label(L_MRET);
	mret();
	j(L_MRET); // the branch predictor can make BRISC skip mret; retry it
	label(L_HALT);
	li(S0, PARAM);
	sw(T2, S0, R_HALT_V3);
	sw(T3, S0, R_HALT_V6);
	sw(T4, S0, R_HALT_COUNTS);
	li(T1, STATE_HALTED);
	sw(T1, S0, R_STATE);
	fence();
	label(L_HANG);
	j(L_HANG);

	resolve();
	// Patch the handler address into the li at handler_li (lui + addi, or addi alone if it is small).
	if (handler_addr >= 0x800)
		die("handler beyond 0x800");
	prog[handler_li] = 0x13; // nop
	prog[handler_li + 1] = (handler_addr & 0xFFF) << 20 | ZERO << 15 | A4 << 7 | 0x13; // addi a4, zero, addr
	if (pc_words * 4 > PARAM)
		die("program overlaps its parameters");
}

// ---- Device access

static int tile_ok(unsigned x, unsigned y)
{
	return ((x >= 1 && x <= 7) || (x >= 10 && x <= 14)) && y >= 2 && y <= 11;
}

static void open_device(const char *dev)
{
	char path[64];
	struct tenstorrent_get_device_info info = { .in.output_size_bytes = sizeof(info.out) };
	struct tenstorrent_allocate_tlb alloc = { .in.size = WINDOW };

	if (strchr(dev, '/'))
		snprintf(path, sizeof(path), "%s", dev);
	else
		snprintf(path, sizeof(path), "/dev/tenstorrent/%s", dev);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("open %s: %s", path, strerror(errno));
	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info))
		die("GET_DEVICE_INFO: %s", strerror(errno));
	if (info.out.vendor_id != 0x1e52 || info.out.device_id != 0xb140)
		die("%s is not a Blackhole (device id 0x%04x)", path, info.out.device_id);
	if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc))
		die("ALLOCATE_TLB: %s", strerror(errno));
	tlb_id = alloc.out.id;
	win = mmap(NULL, WINDOW, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc.out.mmap_offset_uc);
	if (win == MAP_FAILED)
		die("mmap TLB: %s", strerror(errno));
}

static void select_tile(unsigned x, unsigned y)
{
	tx = x;
	ty = y;
	win_base = ~0ull;
}

// Only L1 and the tile control/debug/PIC registers are touched from the host: other tile ranges are not mapped to
// the NoC and an access there hangs the chip.
static volatile uint32_t *at(uint32_t addr)
{
	uint64_t base = addr & ~(uint64_t)(WINDOW - 1);

	if (!(addr < L1_SIZE || (addr >= 0xFFB12000u && addr < 0xFFB14000u)))
		die("refusing NoC access to 0x%08x", addr);
	if (base != win_base) {
		struct tenstorrent_configure_tlb c = { .in.id = tlb_id };

		c.in.config.addr = base;
		c.in.config.x_end = tx;
		c.in.config.y_end = ty;
		c.in.config.ordering = 1; // strict
		if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &c))
			die("CONFIGURE_TLB: %s", strerror(errno));
		win_base = base;
	}
	return (volatile uint32_t *)(win + (addr - base));
}

static uint32_t rd(uint32_t addr) { return *at(addr); }
static void wr(uint32_t addr, uint32_t v) { *at(addr) = v; }

static double now_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned bank_of(uint32_t v3, uint32_t v6)
{
	uint32_t lo = (v3 >> 12) & 0xFFFF, hi = (v6 >> 12) & 0xFFFF;

	return lo ? (unsigned)__builtin_ctz(lo) : hi ? 16 + (unsigned)__builtin_ctz(hi) : 99;
}

// Returns 0 pass, 1 halt, 2 no result.
static int run_tile(uint32_t src, uint32_t dst, uint32_t len, uint32_t scrub)
{
	wr(SOFT_RESET, ALL_RISCS_RESET);
	wr(PIC_BRISC_HW_INT_EN, 0); // the program enables it after installing its handler
	for (unsigned i = 0; i < pc_words; i++)
		wr(4 * i, prog[i]);
	wr(PARAM + P_SRC, src);
	wr(PARAM + P_DST, dst);
	wr(PARAM + P_LEN, len);
	wr(PARAM + P_COORD, tx | ty << 6); // translated, as the host addresses the tile
	wr(PARAM + P_SCRUB, scrub);
	for (unsigned o = R_STATE; o <= R_HALT_COUNTS; o += 4)
		wr(PARAM + o, 0);
	wr(SOFT_RESET, ALL_RISCS_RESET & ~BRISC_RESET);

	double t0 = now_s();
	uint32_t state;
	while ((state = rd(PARAM + R_STATE)) == 0 && now_s() - t0 < 1.0)
		usleep(1000);
	wr(SOFT_RESET, ALL_RISCS_RESET);
	wr(PIC_BRISC_HW_INT_EN, 0);

	uint32_t ignored = rd(PARAM + R_IGNORED), last = rd(PARAM + R_LAST_IGNORED);
	printf("(%2u,%2u) ", tx, ty);
	if (state == STATE_DONE) {
		printf("PASS: read done, %u false error(s) ignored", ignored);
		if (ignored)
			printf(", last in row %u", last & 0xFFF);
		printf("\n");
		return 0;
	}
	if (state == STATE_HALTED) {
		uint32_t v3 = rd(PARAM + R_HALT_V3), v6 = rd(PARAM + R_HALT_V6), counts = rd(PARAM + R_HALT_COUNTS);
		unsigned row = v3 & 0xFFF, bank = bank_of(v3, v6);

		if (!(v3 >> 31))
			printf("HALT: IRQ 3 with no valid L1 error");
		else
			printf("HALT: L1 double-bit error at bank %u row %u (L1 0x%x), DBE %u SBE %u", bank, row,
			       row * 512 + bank * 16, counts & 0x3F, (counts >> 7) & 0xFF);
		printf(" (after %u ignored)\n", ignored);
		return 1;
	}
	printf("NO RESULT: the program did not finish in 1 s (ignored %u)\n", ignored);
	return 2;
}

static uint32_t num(const char *s)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*s == '\0' || *end != '\0')
		die("bad number '%s'", s);
	return (uint32_t)v;
}

int main(int argc, char **argv)
{
	static const unsigned xs[] = { 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14 };
	const char *dev = "0";
	uint32_t src = L1_SIZE, dst = 0x1b800, len = 1024, scrub = SCRUB_TT_METAL;
	unsigned tiles[256][2], n = 0, pass = 0, halt = 0, none = 0;
	int all = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (!strcmp(a, "-d") && v) {
			dev = v;
			i++;
		} else if (!strcmp(a, "--all")) {
			all = 1;
		} else if (!strcmp(a, "--src") && v) {
			src = num(v);
			i++;
		} else if (!strcmp(a, "--dst") && v) {
			dst = num(v);
			i++;
		} else if (!strcmp(a, "--len") && v) {
			len = num(v);
			i++;
		} else if (!strcmp(a, "--dump") && v) { // write the generated BRISC program and exit
			build_program();
			FILE *f = fopen(v, "wb");
			if (!f || fwrite(prog, 4, pc_words, f) != pc_words)
				die("--dump %s", v);
			fclose(f);
			return 0;
		} else if (!strcmp(a, "--scrub") && v) {
			scrub = !strcmp(v, "on") ? SCRUB_TT_METAL : !strcmp(v, "off") ? 0 : (die("--scrub on|off"), 0);
			i++;
		} else if (a[0] != '-' && n < 256 && sscanf(a, "%u,%u", &tiles[n][0], &tiles[n][1]) == 2) {
			if (!tile_ok(tiles[n][0], tiles[n][1]))
				die("(%u,%u) is not a Tensix tile on every Blackhole SKU", tiles[n][0], tiles[n][1]);
			n++;
		} else {
			fprintf(stderr, "usage: l1ecctest [-d DEV] [--src A] [--dst A] [--len N] [--scrub on|off] "
					"[--all | X,Y ...]\n");
			return 2;
		}
	}
	if (dst < PARAM + 0x100 || dst % 16 || len == 0 || len > 16384 || dst + len > L1_SIZE)
		die("dst must be 16 B aligned, past 0x500 and inside L1; len 1-16384");
	if (all && n)
		die("--all takes no tiles");
	if (n == 0 && !all) {
		tiles[0][0] = 1; // tt-metal's L1EccHaltTest: logical core (0,0)
		tiles[0][1] = 2;
		n = 1;
	}
	if (all)
		for (unsigned y = 2; y <= 11; y++)
			for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
				tiles[n][0] = xs[i];
				tiles[n][1] = y;
				n++;
			}

	build_program();
	open_device(dev);
	printf("overread 0x%x -> 0x%x, %u B, scrubber %s; BRISC program %u instructions\n", src, dst, len,
	       scrub ? "on" : "off", pc_words);
	for (unsigned i = 0; i < n; i++) {
		select_tile(tiles[i][0], tiles[i][1]);
		int r = run_tile(src, dst, len, scrub);
		pass += r == 0;
		halt += r == 1;
		none += r == 2;
	}
	printf("%u pass, %u halt, %u no result. RISC-Vs left in reset; the next tt-metal open reloads firmware.\n",
	       pass, halt, none);
	return halt || none;
}
