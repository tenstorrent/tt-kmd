// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Galaxy Mapping Tool -- Decoder Ring
//
// Enumerates all Tenstorrent devices and builds the mapping between:
//   BDF ↔ (UBB tray, ASIC slot) ↔ IPMI reset parameters
//
// Also identifies x8 vs x1 links and the upstream bridge BDF.
//
// Compile: gcc -o galaxy-map galaxy-map.c
// Run:     sudo ./galaxy-map

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/types.h>

// --- Driver UAPI (from ioctl.h) ---

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_GET_DEVICE_INFO _IO(TENSTORRENT_IOCTL_MAGIC, 0)

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

// --- Data ---

#define MAX_DEVICES 64

struct device_entry {
	int dev_id;
	uint16_t domain;
	uint8_t bus;
	char bdf[24];
	char bridge_bdf[24];
	int cur_gen, max_gen;
	int cur_width, max_width;
	int ubb;          // proposed UBB number (1-4), 0 if unknown
	int asic;          // ASIC number (1-8)
	uint8_t ipmi_ubb;
	uint8_t ipmi_dev;
};

// --- Sysfs helpers ---

static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	if (buf[n - 1] == '\n')
		n--;
	buf[n] = '\0';
	return 0;
}

static int link_speed_to_gen(const char *s)
{
	if (strncmp(s, "2.5 GT/s", 8) == 0) return 1;
	if (strncmp(s, "5.0 GT/s", 8) == 0) return 2;
	if (strncmp(s, "8.0 GT/s", 8) == 0) return 3;
	if (strncmp(s, "16.0 GT/s", 9) == 0) return 4;
	if (strncmp(s, "32.0 GT/s", 9) == 0) return 5;
	if (strncmp(s, "64.0 GT/s", 9) == 0) return 6;
	return 0;
}

static void get_pcie_info(const char *bdf, int *cur_gen, int *max_gen,
			  int *cur_width, int *max_width)
{
	char path[PATH_MAX], buf[64];

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/current_link_speed", bdf);
	*cur_gen = (read_sysfs(path, buf, sizeof(buf)) == 0)
		 ? link_speed_to_gen(buf) : 0;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/max_link_speed", bdf);
	*max_gen = (read_sysfs(path, buf, sizeof(buf)) == 0)
		 ? link_speed_to_gen(buf) : 0;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/current_link_width", bdf);
	*cur_width = (read_sysfs(path, buf, sizeof(buf)) == 0)
		   ? atoi(buf) : 0;

	snprintf(path, sizeof(path),
		 "/sys/bus/pci/devices/%s/max_link_width", bdf);
	*max_width = (read_sysfs(path, buf, sizeof(buf)) == 0)
		   ? atoi(buf) : 0;
}

// Find the upstream bridge BDF by resolving the sysfs symlink and
// taking the parent directory name.
static int find_bridge_bdf(const char *device_bdf, char *out, size_t len)
{
	char link[PATH_MAX], real[PATH_MAX];

	snprintf(link, sizeof(link),
		 "/sys/bus/pci/devices/%s", device_bdf);

	if (!realpath(link, real))
		return -1;

	char *slash = strrchr(real, '/');
	if (!slash)
		return -1;
	*slash = '\0';

	char *parent = strrchr(real, '/');
	if (!parent)
		return -1;
	parent++;

	if (strlen(parent) >= 10 && parent[4] == ':')
		snprintf(out, len, "%s", parent);
	else
		snprintf(out, len, "(root)");

	return 0;
}

// Proposed bus high nibble → UBB number mapping.
// Source: vendor documentation (UNVERIFIED -- Stage 0 must confirm).
static int bus_high_to_ubb(uint8_t high)
{
	switch (high) {
	case 0x0: return 1;
	case 0x4: return 2;
	case 0xC: return 3;
	case 0x8: return 4;
	default:  return 0;
	}
}

static int entry_cmp(const void *a, const void *b)
{
	const struct device_entry *ea = a, *eb = b;
	if (ea->ubb != eb->ubb)
		return ea->ubb - eb->ubb;
	return ea->asic - eb->asic;
}

int main(void)
{
	DIR *d = opendir("/dev/tenstorrent/");
	if (!d) {
		fprintf(stderr, "Cannot open /dev/tenstorrent/: %s\n",
			strerror(errno));
		return 1;
	}

	struct device_entry devices[MAX_DEVICES];
	int count = 0;
	struct dirent *ent;

	while ((ent = readdir(d)) != NULL && count < MAX_DEVICES) {
		char *endptr;
		long dev_id = strtol(ent->d_name, &endptr, 10);
		if (*endptr != '\0')
			continue;

		char dev_path[PATH_MAX];
		snprintf(dev_path, sizeof(dev_path),
			 "/dev/tenstorrent/%ld", dev_id);

		int fd = open(dev_path, O_RDWR | O_APPEND);
		if (fd < 0)
			continue;

		struct tenstorrent_get_device_info info = {0};
		info.in.output_size_bytes = sizeof(info.out);

		if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) < 0) {
			close(fd);
			continue;
		}
		close(fd);

		struct device_entry *e = &devices[count];
		e->dev_id = (int)dev_id;
		e->domain = info.out.pci_domain;
		e->bus = (info.out.bus_dev_fn >> 8) & 0xFF;

		snprintf(e->bdf, sizeof(e->bdf), "%04x:%02x:%02x.%x",
			 e->domain,
			 e->bus,
			 (info.out.bus_dev_fn >> 3) & 0x1F,
			 info.out.bus_dev_fn & 0x7);

		get_pcie_info(e->bdf, &e->cur_gen, &e->max_gen,
			      &e->cur_width, &e->max_width);

		find_bridge_bdf(e->bdf, e->bridge_bdf, sizeof(e->bridge_bdf));

		uint8_t high = (e->bus >> 4) & 0xF;
		uint8_t low  = e->bus & 0xF;

		e->ubb      = bus_high_to_ubb(high);
		e->asic     = low;
		e->ipmi_ubb = e->ubb ? (1 << (e->ubb - 1)) : 0;
		e->ipmi_dev = low ? (1 << (low - 1)) : 0;

		count++;
	}
	closedir(d);

	if (count == 0) {
		fprintf(stderr, "No devices found.\n");
		return 1;
	}

	qsort(devices, count, sizeof(devices[0]), entry_cmp);

	// Table header
	printf("# %-3s %-4s %-14s %-14s %-4s %-5s  %-4s %-5s  %-10s  %s\n",
	       "UBB", "ASIC", "BDF", "Bridge", "Wdth", "Gen",
	       "iUBB", "iDEV", "IPMI params",
	       "Reset cmd");
	printf("# %-3s %-4s %-14s %-14s %-4s %-5s  %-4s %-5s  %-10s  %s\n",
	       "---", "----", "--------------", "--------------",
	       "----", "-----", "----", "-----", "----------",
	       "---");

	int prev_ubb = -1;

	for (int i = 0; i < count; i++) {
		struct device_entry *e = &devices[i];

		if (e->ubb != prev_ubb && prev_ubb != -1)
			printf("#\n");
		prev_ubb = e->ubb;

		const char *tag = (e->cur_width >= 8) ? " [x8]" : "";

		printf("  %-3d %-4d %-14s %-14s x%-3d %d/%-3d  0x%-2X 0x%02X   "
		       "0x%X 0x%02X     "
		       "ipmitool raw 0x30 0x8B 0x%X 0x%02X 0x0 0xF%s\n",
		       e->ubb, e->asic, e->bdf, e->bridge_bdf,
		       e->cur_width, e->cur_gen, e->max_gen,
		       e->ipmi_ubb, e->ipmi_dev,
		       e->ipmi_ubb, e->ipmi_dev,
		       e->ipmi_ubb, e->ipmi_dev,
		       tag);
	}

	// Summary
	printf("#\n# --- Summary ---\n");
	printf("# Devices: %d\n", count);

	printf("# Bus-to-UBB mapping (PROPOSED, verify by resetting one chip):\n");
	uint8_t seen[16] = {0};
	for (int i = 0; i < count; i++) {
		uint8_t h = (devices[i].bus >> 4) & 0xF;
		if (seen[h])
			continue;
		seen[h] = 1;
		int ubb = bus_high_to_ubb(h);
		printf("#   Bus 0x%X_ -> UBB%d (ipmi 0x%X)\n",
		       h, ubb, ubb ? (1 << (ubb - 1)) : 0);
	}

	printf("# x8 link per tray:\n");
	memset(seen, 0, sizeof(seen));
	for (int i = 0; i < count; i++) {
		uint8_t h = (devices[i].bus >> 4) & 0xF;
		if (devices[i].cur_width >= 8 && !seen[h]) {
			seen[h] = 1;
			printf("#   UBB%d: ASIC %d (bus 0x%02X)\n",
			       devices[i].ubb, devices[i].asic, devices[i].bus);
		}
	}

	printf("#\n# Verification: pick one chip, reset it, confirm the\n");
	printf("# expected BDF disappears and returns. Example:\n");
	if (count >= 2) {
		struct device_entry *e = &devices[1];
		printf("#   sudo ipmitool raw 0x30 0x8B 0x%X 0x%02X 0x0 0xF\n",
		       e->ipmi_ubb, e->ipmi_dev);
		printf("#   (proposed UBB%d/ASIC%d -> expected BDF %s)\n",
		       e->ubb, e->asic, e->bdf);
		printf("#   Then re-run: sudo ./galaxy-map\n");
	}

	return 0;
}
