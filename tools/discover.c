// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Discover Tool for Tenstorrent Devices.
//
// Enumerates all devices created by the tenstorrent driver and prints
// their device path, PCI BDF, PCIe generation, and link width.
//
// To Compile:
//  gcc -o discover discover.c
//
// To Run:
//  ./discover

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/types.h>

// --- Driver UAPI Definitions (from ioctl.h) ---

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
		__u16 bus_dev_fn;		// [0:2] function, [3:7] device, [8:15] bus
		__u16 max_dma_buf_size_log2;
		__u16 pci_domain;
		__u16 reserved;
	} out;
};

// --- PCIe speed/width from sysfs ---

// Read a sysfs attribute into buf. Returns 0 on success.
static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, bufsz - 1);
	close(fd);
	if (n <= 0)
		return -1;
	// Strip trailing newline
	if (buf[n - 1] == '\n')
		n--;
	buf[n] = '\0';
	return 0;
}

// Map "X GT/s ..." to PCIe generation number.
static int link_speed_to_gen(const char *speed_str)
{
	if (strncmp(speed_str, "2.5 GT/s", 8) == 0) return 1;
	if (strncmp(speed_str, "5.0 GT/s", 8) == 0) return 2;
	if (strncmp(speed_str, "8.0 GT/s", 8) == 0) return 3;
	if (strncmp(speed_str, "16.0 GT/s", 9) == 0) return 4;
	if (strncmp(speed_str, "32.0 GT/s", 9) == 0) return 5;
	if (strncmp(speed_str, "64.0 GT/s", 9) == 0) return 6;
	return 0;
}

static void get_pcie_info(const char *bdf, int *cur_gen, int *max_gen,
			  int *cur_width, int *max_width)
{
	char path[PATH_MAX];
	char buf[64];

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

// --- Main ---

int main(void)
{
	DIR *d = opendir("/dev/tenstorrent/");
	if (!d) {
		fprintf(stderr, "Cannot open /dev/tenstorrent/: %s\n",
			strerror(errno));
		return 1;
	}

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
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

		char bdf[24];
		snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%x",
			 info.out.pci_domain,
			 (info.out.bus_dev_fn >> 8) & 0xFF,
			 (info.out.bus_dev_fn >> 3) & 0x1F,
			 info.out.bus_dev_fn & 0x7);

		int cur_gen, max_gen, cur_width, max_width;
		get_pcie_info(bdf, &cur_gen, &max_gen, &cur_width, &max_width);

		printf("/dev/tenstorrent/%ld %s Gen%d/Gen%d x%d/x%d\n",
		       dev_id, bdf,
		       cur_gen, max_gen,
		       cur_width, max_width);
	}

	closedir(d);
	return 0;
}
