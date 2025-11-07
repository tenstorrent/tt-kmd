// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main(int argc, char *argv[]) {
	int fd;
	unsigned long counter = 0;
	
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <device_id>\n", argv[0]);
		fprintf(stderr, "Example: %s 0\n", argv[0]);
		exit(1);
	}

	int dev_id = atoi(argv[1]);
	char dev_path[256];
	snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Failed to open %s: %s\n", dev_path, strerror(errno));
		exit(1);
	}

	printf("Simple loop: PID=%d, device=%s, FD=%d\n", getpid(), dev_path, fd);
	printf("Press Ctrl+C to exit\n");
	fflush(stdout);

	while (1) {
		counter++;
		printf("Counter: %lu\n", counter);
		fflush(stdout);
		usleep(100);
	}

	close(fd);
	return 0;
}

