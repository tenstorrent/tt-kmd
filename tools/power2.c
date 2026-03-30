// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Like power.c but hits every /dev/tenstorrent/* device with the same settings.
// FDs are kept open (power state is removed on close) until Enter is pressed.
//
// To Compile:
//  gcc -o power2 power2.c
//
// To Run:
//  ./power2 [OPTIONS]
//
// Examples:
//  ./power2 -f 1,0           # bit 0=1, bit 1=0 on all chips
//  ./power2 -f 0,1,1,1       # AICLK=min, PHY=wake, tensix=on, l2cpu=on on all chips

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <getopt.h>

#define FATAL(fmt, ...) do { fprintf(stderr, "FATAL: " fmt "\n", ##__VA_ARGS__); exit(1); } while (0)

#define TENSTORRENT_IOCTL_MAGIC 0xFA
#define TENSTORRENT_IOCTL_SET_POWER_STATE _IO(TENSTORRENT_IOCTL_MAGIC, 15)

#define DEV_DIR "/dev/tenstorrent"
#define MAX_DEVICES 128

struct tenstorrent_power_state {
	__u32 argsz;
	__u32 flags;
	__u8 reserved0;
	__u8 validity;
#define TT_POWER_VALIDITY_FLAGS(n)      (((n) & 0xF) << 0)
#define TT_POWER_VALIDITY_SETTINGS(n)   (((n) & 0xF) << 4)
#define TT_POWER_VALIDITY(flags, settings) (TT_POWER_VALIDITY_FLAGS(flags) | TT_POWER_VALIDITY_SETTINGS(settings))
	__u16 power_flags;
#define TT_POWER_FLAG_MAX_AI_CLK        (1U << 0)
#define TT_POWER_FLAG_MRISC_PHY_WAKEUP  (1U << 1)
#define TT_POWER_FLAG_TENSIX_ENABLE     (1U << 2)
#define TT_POWER_FLAG_L2CPU_ENABLE      (1U << 3)
	__u16 power_settings[14];
};

static __u16 parse_flags(const char *flag_str, __u8 *num_flags) {
	__u16 flags = 0;
	*num_flags = 0;

	char *str = strdup(flag_str);
	char *saveptr;
	char *token = strtok_r(str, ",", &saveptr);

	while (token != NULL) {
		char *endptr;
		long value = strtol(token, &endptr, 10);
		if (*endptr != '\0' || value < 0 || value > 1)
			FATAL("Invalid flag value: %s (must be 0 or 1)", token);
		if (value == 1)
			flags |= (1U << (*num_flags));
		(*num_flags)++;
		token = strtok_r(NULL, ",", &saveptr);
	}

	free(str);
	if (*num_flags > 15)
		FATAL("Too many flag bits: %d (max 15)", *num_flags);

	return flags;
}

static void parse_settings(const char *settings_str, __u16 *settings, __u8 *num_settings) {
	*num_settings = 0;

	char *str = strdup(settings_str);
	char *saveptr;
	char *token = strtok_r(str, ",", &saveptr);

	while (token != NULL) {
		if (*num_settings >= 14)
			FATAL("Too many settings (max 14)");
		char *endptr;
		long value = strtol(token, &endptr, 10);
		if (*endptr != '\0' || value < 0 || value > 65535)
			FATAL("Invalid setting value: %s (must be 0-65535)", token);
		settings[*num_settings] = (__u16)value;
		(*num_settings)++;
		token = strtok_r(NULL, ",", &saveptr);
	}

	free(str);
}

static const char *flag_name(__u16 flags, int bit) {
	switch (bit) {
	case 0: return (flags & TT_POWER_FLAG_MAX_AI_CLK)       ? "MAX_AI_CLK"    : "min_ai_clk";
	case 1: return (flags & TT_POWER_FLAG_MRISC_PHY_WAKEUP) ? "PHY_WAKEUP"    : "phy_powerdown";
	case 2: return (flags & TT_POWER_FLAG_TENSIX_ENABLE)     ? "TENSIX_EN"     : "tensix_gate";
	case 3: return (flags & TT_POWER_FLAG_L2CPU_ENABLE)      ? "L2CPU_EN"      : "l2cpu_gate";
	default: return (flags & (1U << bit)) ? "1" : "0";
	}
}

static void print_usage(const char *prog) {
	fprintf(stderr, "Usage: %s [OPTIONS]\n\n", prog);
	fprintf(stderr, "Applies power settings to ALL /dev/tenstorrent/* devices.\n\n");
	fprintf(stderr, "OPTIONS:\n");
	fprintf(stderr, "  -f, --flags <bit_values>    Comma-delimited flag bits (0 or 1)\n");
	fprintf(stderr, "  -s, --settings <val_list>   Comma-delimited settings (0-65535)\n");
	fprintf(stderr, "  -h, --help                  Print this help\n\n");
	fprintf(stderr, "FLAG BITS:\n");
	fprintf(stderr, "  0 = MAX_AI_CLK    1 = MRISC_PHY_WAKEUP    2 = TENSIX_ENABLE    3 = L2CPU_ENABLE\n\n");
	fprintf(stderr, "EXAMPLES:\n");
	fprintf(stderr, "  %s -f 0,1,1,1       # AICLK=min, PHY=wake, tensix=on, l2cpu=on\n", prog);
	fprintf(stderr, "  %s -f 1,1,1,1       # Everything on\n", prog);
}

int main(int argc, char *argv[]) {
	__u16 power_flags = 0;
	__u8 num_flags = 0;
	__u16 power_settings[14] = {0};
	__u8 num_settings = 0;
	int has_flags = 0;
	int has_settings = 0;

	static struct option long_options[] = {
		{"flags",    required_argument, 0, 'f'},
		{"settings", required_argument, 0, 's'},
		{"help",     no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "f:s:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'f':
			power_flags = parse_flags(optarg, &num_flags);
			has_flags = 1;
			break;
		case 's':
			parse_settings(optarg, power_settings, &num_settings);
			has_settings = 1;
			break;
		case 'h':
			print_usage(argv[0]);
			exit(0);
		default:
			print_usage(argv[0]);
			exit(1);
		}
	}

	if (!has_flags && !has_settings) {
		fprintf(stderr, "Error: Must specify at least -f or -s\n\n");
		print_usage(argv[0]);
		exit(1);
	}

	printf("Power flags: 0x%04x (valid bits: %d) [", power_flags, num_flags);
	for (int i = 0; i < num_flags; i++)
		printf("%s%s", i ? " " : "", flag_name(power_flags, i));
	printf("]\n");

	if (has_settings) {
		printf("Settings:");
		for (int i = 0; i < num_settings; i++)
			printf(" [%d]=%u", i, power_settings[i]);
		printf("\n");
	}

	DIR *dir = opendir(DEV_DIR);
	if (!dir)
		FATAL("Cannot open %s: %s", DEV_DIR, strerror(errno));

	int fds[MAX_DEVICES];
	char paths[MAX_DEVICES][64];
	int ndevs = 0;

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
			continue;
		if (ndevs >= MAX_DEVICES)
			FATAL("Too many devices (max %d)", MAX_DEVICES);
		int n = snprintf(paths[ndevs], sizeof(paths[ndevs]), "%s/%s", DEV_DIR, ent->d_name);
		if (n < 0 || (size_t)n >= sizeof(paths[ndevs]))
			FATAL("Path too long: %s/%s", DEV_DIR, ent->d_name);
		ndevs++;
	}
	closedir(dir);

	if (ndevs == 0)
		FATAL("No devices found in %s", DEV_DIR);

	printf("Found %d device(s), applying power state to all...\n", ndevs);

	int ok_count = 0, fail_count = 0;

	for (int i = 0; i < ndevs; i++) {
		fds[i] = open(paths[i], O_RDWR | O_APPEND);
		if (fds[i] < 0) {
			fprintf(stderr, "  %s: open failed: %s\n", paths[i], strerror(errno));
			fds[i] = -1;
			fail_count++;
			continue;
		}

		struct tenstorrent_power_state ps = {0};
		ps.argsz = sizeof(ps);
		ps.validity = TT_POWER_VALIDITY(num_flags, num_settings);
		ps.power_flags = power_flags;
		if (num_settings > 0)
			memcpy(ps.power_settings, power_settings, num_settings * sizeof(__u16));

		if (ioctl(fds[i], TENSTORRENT_IOCTL_SET_POWER_STATE, &ps) < 0) {
			fprintf(stderr, "  %s: ioctl failed: %s\n", paths[i], strerror(errno));
			fail_count++;
		} else {
			printf("  %s: OK\n", paths[i]);
			ok_count++;
		}
	}

	printf("\nDone: %d OK, %d failed out of %d devices.\n", ok_count, fail_count, ndevs);
	printf("FDs held open. Press Enter to release and exit...");
	fflush(stdout);
	getchar();

	for (int i = 0; i < ndevs; i++) {
		if (fds[i] >= 0)
			close(fds[i]);
	}

	return fail_count > 0 ? 1 : 0;
}
