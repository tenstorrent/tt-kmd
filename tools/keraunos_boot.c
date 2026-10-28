// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Keraunos firmware boot helper for bring-up (ttsim/emulation).
//
//   keraunos_boot pack <sysbuild_dir> <out_dir> [--d2d <d2d_fw.bin>]
//       Build bun2.bin and bun3.bin from a tt-system-firmware sysbuild.
//   keraunos_boot stage <bundle.bin> [--seq N] [--fake-sep] [--no-wait]
//                 [--timeout-ms N] [device_id]
//       Stage one bundle with TENSTORRENT_IOCTL_DEV_STAGE_BUNDLE.
//   keraunos_boot bl0p5 <sysbuild_dir> [device_id]
//       Reset m0, m1 and K into BL0, then launch K BL0.5. Stands in for
//       K-SEP and SPI flash, which ttsim does not model.
//   keraunos_boot boot <sysbuild_dir> [--d2d <d2d_fw.bin>] [device_id]
//       bl0p5, then stage BUN2 and BUN3 (faking SEP validation).
//
// The bundle layouts and the bl0p5 sequence follow James Growden's
// keraunos_reset (jgrowdenTT/tt-kmd, branch keraunos), which matches the
// tt-system-firmware mmk_working branch. They are not the boot spec's final
// layout: manifests are unsigned and only payload_offset is filled in.
//
// Build:
//   gcc -O2 -Wall -Wextra -o keraunos_boot keraunos_boot.c

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../ioctl.h"

#define TENSTORRENT_PCI_VENDOR_ID	0x1e52
#define KERAUNOS_PCI_DEVICE_ID		0xfeed

// --- Addresses --------------------------------------------------------------

#define SPA_BASE_K		0x1202000000ULL
#define SPA_BASE_M0		0x1300000000ULL
#define SPA_BASE_M1		0x1308000000ULL
#define SMC_LOCAL_BASE		0xC0000000ULL

#define RESET_VECTOR0		0xC0010000ULL
#define RESET_CTRL		0xC0010020ULL
#define RESET_CTRL_CORE0	0x1u
#define SCRATCH(n)		(0xC0010100ULL + 8u * (n))
#define SCRATCH_COUNT		16u
#define SMC_STATUS_TO_SEP	SCRATCH(9)
#define SEP_STATUS_SRAM_INIT	0x1u

#define SRAM_BASE		0xC0060000ULL
#define SRAM_SIZE		0x00100000ULL
#define BL0_ENTRY		0xC0040000ULL
#define BL0P5_ENTRY		0xC0150000ULL
#define STAGING_BASE		0xC0066400ULL
#define KBL1_EXEC		0xC013B000ULL
#define KMIS_EXEC		0xC0067000ULL

// --- Bundle layout (tt_bundle_loader.h) -------------------------------------

#define MANIFEST_SIZE		1184u
#define MANIFEST_PAYLOAD_OFF	1160u	// u64 payload_offset
#define TOC_ID			0x434f5450u	// "PTOC"
#define TOC_HDR_SIZE		32u
#define TOC_ENTRY_SIZE		216u
#define IMG_TYPE_SMC_BL1	0x0000314c42434d53ULL
#define IMG_TYPE_SMC_MIS	0x0000324c42434d53ULL

struct image {
	const char *path;
	uint8_t *data;
	size_t size;
};

static void put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, sizeof(v));
}

static void put64(uint8_t *p, uint64_t v)
{
	memcpy(p, &v, sizeof(v));
}

static int read_file(struct image *img)
{
	struct stat st;
	FILE *f = fopen(img->path, "rb");

	if (!f || fstat(fileno(f), &st) < 0 || !S_ISREG(st.st_mode)) {
		fprintf(stderr, "cannot read %s: %s\n", img->path, strerror(errno ? errno : EINVAL));
		if (f)
			fclose(f);
		return -1;
	}

	img->size = st.st_size;
	img->data = malloc(img->size ? img->size : 1);
	if (!img->data || fread(img->data, 1, img->size, f) != img->size) {
		fprintf(stderr, "short read from %s\n", img->path);
		fclose(f);
		return -1;
	}

	fclose(f);
	return 0;
}

static void put_toc_header(uint8_t *toc, uint64_t payload_len, uint32_t count)
{
	put32(toc + 0, TOC_ID);
	put32(toc + 4, 1);		// major 1, minor 0
	put64(toc + 8, payload_len);
	put64(toc + 16, count);
}

static void put_toc_entry(uint8_t *e, uint64_t type, uint64_t offset, uint64_t length,
			  uint64_t load_addr, uint64_t entry_point)
{
	put64(e + 0, type);
	put64(e + 8, offset);		// from payload (TOC) start
	put64(e + 16, length);
	put64(e + 32, load_addr);
	put64(e + 40, entry_point);
}

static size_t align8(size_t n)
{
	return (n + 7) & ~(size_t)7;
}

// BUN2: manifest | TOC (3 entries) | K-BL1 | M-BL1 | D2D firmware. BL0.5
// copies TOC[0] to KBL1_EXEC; K-BL1 sends TOC[1] and TOC[2] to the Mimirs.
// TODO: boot spec Table 78 orders this K-BL1, K-D2D, M-BL1, M-D2D, M-DRAM.
static uint8_t *build_bun2(const struct image *kbl1, const struct image *mbl1,
			   const struct image *d2d, size_t *size)
{
	size_t entries = TOC_HDR_SIZE + 3 * TOC_ENTRY_SIZE;
	size_t mbl1_off = entries + kbl1->size;
	size_t d2d_off = mbl1_off + mbl1->size;
	size_t payload_len = d2d_off + d2d->size;
	size_t total = align8(MANIFEST_SIZE + payload_len);
	uint8_t *b = calloc(1, total);
	uint8_t *toc;

	if (!b)
		return NULL;

	put64(b + MANIFEST_PAYLOAD_OFF, MANIFEST_SIZE);
	toc = b + MANIFEST_SIZE;
	put_toc_header(toc, payload_len, 3);
	put_toc_entry(toc + TOC_HDR_SIZE + 0 * TOC_ENTRY_SIZE, IMG_TYPE_SMC_BL1,
		      entries, kbl1->size, KBL1_EXEC, KBL1_EXEC);
	put_toc_entry(toc + TOC_HDR_SIZE + 1 * TOC_ENTRY_SIZE, IMG_TYPE_SMC_BL1,
		      mbl1_off, mbl1->size, KBL1_EXEC, KBL1_EXEC);
	// TODO: D2D firmware has no image type of its own yet.
	put_toc_entry(toc + TOC_HDR_SIZE + 2 * TOC_ENTRY_SIZE, IMG_TYPE_SMC_BL1,
		      d2d_off, d2d->size, 0, 0);

	memcpy(toc + entries, kbl1->data, kbl1->size);
	memcpy(toc + mbl1_off, mbl1->data, mbl1->size);
	memcpy(toc + d2d_off, d2d->data, d2d->size);

	*size = total;
	return b;
}

// BUN3: manifest | TOC (2 entries) | K-MIS | M-MIS. payload_offset is chosen
// so K-MIS lands at its link address, KMIS_EXEC, and K-BL1 jumps to it in
// place. K-BL1 copies TOC[1] to the Mimirs.
// TODO: boot spec Table 79 puts K-SEP-MIS first.
static uint8_t *build_bun3(const struct image *kmis, const struct image *mmis, size_t *size)
{
	size_t entries = TOC_HDR_SIZE + 2 * TOC_ENTRY_SIZE;
	size_t payload_off = KMIS_EXEC - STAGING_BASE - entries;
	size_t mmis_off = entries + align8(kmis->size);
	size_t payload_len = mmis_off + mmis->size;
	size_t total = align8(payload_off + payload_len);
	uint8_t *b = calloc(1, total);
	uint8_t *toc;

	if (!b)
		return NULL;

	put64(b + MANIFEST_PAYLOAD_OFF, payload_off);
	toc = b + payload_off;
	put_toc_header(toc, payload_len, 2);
	put_toc_entry(toc + TOC_HDR_SIZE + 0 * TOC_ENTRY_SIZE, IMG_TYPE_SMC_MIS,
		      entries, kmis->size, KMIS_EXEC, KMIS_EXEC);
	// Core-local; firmware translates it to each Mimir's SPA.
	put_toc_entry(toc + TOC_HDR_SIZE + 1 * TOC_ENTRY_SIZE, IMG_TYPE_SMC_MIS,
		      mmis_off, mmis->size, KMIS_EXEC, KMIS_EXEC);

	memcpy(toc + entries, kmis->data, kmis->size);
	memcpy(toc + mmis_off, mmis->data, mmis->size);

	*size = total;
	return b;
}

static char *join(const char *a, const char *b)
{
	size_t n = strlen(a) + strlen(b) + 2;
	char *s = malloc(n);

	if (s)
		snprintf(s, n, "%s/%s", a, b);
	return s;
}

static int write_file(const char *path, const uint8_t *data, size_t size)
{
	FILE *f = fopen(path, "wb");

	if (!f || fwrite(data, 1, size, f) != size || fclose(f) != 0) {
		fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
		return -1;
	}

	printf("wrote %s (%zu bytes)\n", path, size);
	return 0;
}

static const char *default_d2d(const char *sysbuild)
{
	return join(sysbuild, "bl1_keraunos/grendel-sival-sdk/grendel_sival_sdk/"
			      "firmware/libs/chip_init/d2d_fw/d2d_fw.bin");
}

// Build both bundles in memory. Returns 0 and fills bun2/bun3 on success.
static int build_bundles(const char *sysbuild, const char *d2d_path,
			 uint8_t **bun2, size_t *bun2_size, uint8_t **bun3, size_t *bun3_size)
{
	struct image kbl1 = { .path = join(sysbuild, "bl1_keraunos/zephyr/zephyr.bin") };
	struct image mbl1 = { .path = join(sysbuild, "bl1_mimir/zephyr/zephyr.bin") };
	struct image d2d = { .path = d2d_path ? d2d_path : default_d2d(sysbuild) };
	struct image kmis = { .path = join(sysbuild, "mis/zephyr/zephyr.bin") };
	struct image mmis = { .path = join(sysbuild, "mis_mimir/zephyr/zephyr.bin") };

	if (read_file(&kbl1) || read_file(&mbl1) || read_file(&d2d) ||
	    read_file(&kmis) || read_file(&mmis))
		return -1;

	*bun2 = build_bun2(&kbl1, &mbl1, &d2d, bun2_size);
	*bun3 = build_bun3(&kmis, &mmis, bun3_size);
	return (*bun2 && *bun3) ? 0 : -1;
}

// --- Device access ----------------------------------------------------------

static int open_device(long id)
{
	char path[64];
	struct tenstorrent_get_device_info info = {
		.in.output_size_bytes = sizeof(info.out),
	};
	int fd;

	snprintf(path, sizeof(path), "/dev/tenstorrent/%ld", id);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0 ||
	    info.out.vendor_id != TENSTORRENT_PCI_VENDOR_ID ||
	    info.out.device_id != KERAUNOS_PCI_DEVICE_ID) {
		fprintf(stderr, "%s is not a Keraunos device\n", path);
		close(fd);
		return -1;
	}

	return fd;
}

static int stage(int fd, const uint8_t *data, size_t size, unsigned int seq,
		 uint32_t flags, uint32_t timeout_ms)
{
	struct tenstorrent_stage_bundle sb = {
		.argsz = sizeof(sb),
		.flags = flags,
		.seq = seq,
		.timeout_ms = timeout_ms,
		.data = (uintptr_t)data,
		.size = size,
	};
	struct timespec t0, t1;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	ret = ioctl(fd, TENSTORRENT_IOCTL_DEV_STAGE_BUNDLE, &sb);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	printf("stage seq %u, %zu bytes: %s, boot_state=0x%08x, %.3f s\n", seq, size,
	       ret ? strerror(errno) : "ok", sb.boot_state,
	       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
	return ret ? -1 : 0;
}

// SPA-addressed scalar access, for the reset prelude only.
static uint64_t g_spa_base;

static uint64_t spa(uint64_t local)
{
	return g_spa_base + (local - SMC_LOCAL_BASE);
}

static int noc_io(int fd, unsigned long req, uint64_t addr, uint64_t *value, uint8_t width)
{
	struct tenstorrent_noc_io io = {
		.argsz = sizeof(io),
		.width = width,
		.addr = addr,
		.value = *value,
	};

	if (ioctl(fd, req, &io) < 0) {
		fprintf(stderr, "NOC %s 0x%012llx: %s\n",
			req == TENSTORRENT_IOCTL_NOC_READ ? "read" : "write",
			(unsigned long long)addr, strerror(errno));
		return -1;
	}

	*value = io.value;
	return 0;
}

static int rd32(int fd, uint64_t local, uint32_t *v)
{
	uint64_t x = 0;

	if (noc_io(fd, TENSTORRENT_IOCTL_NOC_READ, spa(local), &x, 4))
		return -1;
	*v = (uint32_t)x;
	return 0;
}

static int wr(int fd, uint64_t local, uint64_t v, uint8_t width)
{
	return noc_io(fd, TENSTORRENT_IOCTL_NOC_WRITE, spa(local), &v, width);
}

static int set_reset(int fd, int run)
{
	uint32_t v;

	if (rd32(fd, RESET_CTRL, &v))
		return -1;
	v = run ? (v | RESET_CTRL_CORE0) : (v & ~RESET_CTRL_CORE0);
	return wr(fd, RESET_CTRL, v, 4);
}

static int load(int fd, uint64_t local, const struct image *img)
{
	size_t i;

	for (i = 0; i < img->size; i += 4) {
		uint32_t w = 0;

		memcpy(&w, img->data + i, img->size - i >= 4 ? 4 : img->size - i);
		if (wr(fd, local + i, w, 4))
			return -1;
	}
	return 0;
}

// Hold reset, clear scratch, zero SRAM, aim at BL0, release.
static int boot_bl0(int fd)
{
	uint64_t a;
	uint32_t i;

	if (set_reset(fd, 0))
		return -1;
	for (i = 0; i < SCRATCH_COUNT; i++)
		if (wr(fd, SCRATCH(i), 0, 4))
			return -1;
	for (a = SRAM_BASE; a < SRAM_BASE + SRAM_SIZE; a += 8)
		if (wr(fd, a, 0, 8))
			return -1;
	if (wr(fd, RESET_VECTOR0, BL0_ENTRY, 4))
		return -1;
	return set_reset(fd, 1);
}

static int launch_bl0p5(int fd, const char *sysbuild)
{
	static const uint64_t mimirs[] = { SPA_BASE_M0, SPA_BASE_M1 };
	struct image bl0p5 = { .path = join(sysbuild, "bl0p5_keraunos/zephyr/zephyr.bin") };
	uint32_t v = 0;
	size_t i;

	if (read_file(&bl0p5))
		return -1;

	// TODO: Mimir-less boots don't need these, but K-BL1 still probes the
	// Mimir SMCs over I3C and ttsim crashes on a core with no ROM to run.
	for (i = 0; i < sizeof(mimirs) / sizeof(mimirs[0]); i++) {
		g_spa_base = mimirs[i];
		printf("BL0 on M%zu\n", i);
		if (boot_bl0(fd))
			return -1;
	}

	g_spa_base = SPA_BASE_K;
	printf("BL0 on K\n");
	if (boot_bl0(fd))
		return -1;

	// BL0 publishes the SEP-safe region, then we take the core back.
	printf("waiting for K BL0 SRAM init\n");
	while (!(v & SEP_STATUS_SRAM_INIT)) {
		if (rd32(fd, SMC_STATUS_TO_SEP, &v))
			return -1;
		usleep(100000);
	}
	if (set_reset(fd, 0))
		return -1;

	// BL0.5 expects the BUN1 TOC it was launched from; fake its header.
	if (wr(fd, STAGING_BASE + MANIFEST_PAYLOAD_OFF, MANIFEST_SIZE, 4) ||
	    wr(fd, STAGING_BASE + MANIFEST_SIZE + 0, TOC_ID, 4) ||
	    wr(fd, STAGING_BASE + MANIFEST_SIZE + 4, 1, 4) ||
	    wr(fd, STAGING_BASE + MANIFEST_SIZE + 8, TOC_HDR_SIZE + 2 * TOC_ENTRY_SIZE, 4) ||
	    wr(fd, STAGING_BASE + MANIFEST_SIZE + 16, 3, 4))
		return -1;

	printf("loading BL0.5 (%zu bytes)\n", bl0p5.size);
	if (load(fd, BL0P5_ENTRY, &bl0p5) ||
	    wr(fd, RESET_VECTOR0, BL0P5_ENTRY, 4) ||
	    set_reset(fd, 1))
		return -1;

	return 0;
}

// --- Commands ---------------------------------------------------------------

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s pack <sysbuild_dir> <out_dir> [--d2d <d2d_fw.bin>]\n"
		"  %s stage <bundle.bin> [--seq N] [--fake-sep] [--no-wait] [--timeout-ms N] [device_id]\n"
		"  %s bl0p5 <sysbuild_dir> [device_id]\n"
		"  %s boot <sysbuild_dir> [--d2d <d2d_fw.bin>] [device_id]\n",
		prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	const char *cmd, *path, *out_dir = NULL, *d2d = NULL;
	unsigned int seq = 0;
	uint32_t flags = 0, timeout_ms = 0;
	long dev_id = 0;
	int i, fd, ret;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[1];
	path = argv[2];
	i = 3;
	if (!strcmp(cmd, "pack")) {
		if (argc < 4) {
			usage(argv[0]);
			return 2;
		}
		out_dir = argv[3];
		i = 4;
	}

	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--d2d") && i + 1 < argc)
			d2d = argv[++i];
		else if (!strcmp(argv[i], "--seq") && i + 1 < argc)
			seq = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc)
			timeout_ms = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--fake-sep"))
			flags |= TENSTORRENT_STAGE_BUNDLE_FAKE_SEP_VALIDATION;
		else if (!strcmp(argv[i], "--no-wait"))
			flags |= TENSTORRENT_STAGE_BUNDLE_NO_WAIT;
		else if (argv[i][0] != '-')
			dev_id = strtol(argv[i], NULL, 0);
		else {
			usage(argv[0]);
			return 2;
		}
	}

	if (!strcmp(cmd, "pack")) {
		uint8_t *bun2, *bun3;
		size_t bun2_size, bun3_size;

		if (build_bundles(path, d2d, &bun2, &bun2_size, &bun3, &bun3_size))
			return 1;
		return (write_file(join(out_dir, "bun2.bin"), bun2, bun2_size) ||
			write_file(join(out_dir, "bun3.bin"), bun3, bun3_size)) ? 1 : 0;
	}

	if (!strcmp(cmd, "stage")) {
		struct image b = { .path = path };

		if (read_file(&b) || (fd = open_device(dev_id)) < 0)
			return 1;
		ret = stage(fd, b.data, b.size, seq, flags, timeout_ms);
		close(fd);
		return ret ? 1 : 0;
	}

	if (!strcmp(cmd, "bl0p5")) {
		if ((fd = open_device(dev_id)) < 0)
			return 1;
		ret = launch_bl0p5(fd, path);
		close(fd);
		return ret ? 1 : 0;
	}

	if (!strcmp(cmd, "boot")) {
		uint8_t *bun2, *bun3;
		size_t bun2_size, bun3_size;

		if (build_bundles(path, d2d, &bun2, &bun2_size, &bun3, &bun3_size) ||
		    (fd = open_device(dev_id)) < 0)
			return 1;

		flags = TENSTORRENT_STAGE_BUNDLE_FAKE_SEP_VALIDATION;
		ret = launch_bl0p5(fd, path) ||
		      stage(fd, bun2, bun2_size, 0, flags, timeout_ms) ||
		      // TODO: K-BL1 never writes BUNDLE_CONSUMED for BUN3.
		      stage(fd, bun3, bun3_size, 1, flags | TENSTORRENT_STAGE_BUNDLE_NO_WAIT,
			    timeout_ms);
		close(fd);
		return ret ? 1 : 0;
	}

	usage(argv[0]);
	return 2;
}
