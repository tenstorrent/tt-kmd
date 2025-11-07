// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "../ioctl.h"

int main(int argc, char *argv[]) {
	int fd, ret;
	struct tenstorrent_dev dev_cmd;
	
	if (argc != 4) {
		fprintf(stderr, "Usage: %s <device_id> <signal_num> <payload>\n", argv[0]);
		fprintf(stderr, "Example: %s 0 34 42  (send SIGRTMIN with payload 42)\n", argv[0]);
		exit(1);
	}

	int dev_id = atoi(argv[1]);
	int signum = atoi(argv[2]);
	int payload = atoi(argv[3]);

	char dev_path[256];
	snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	memset(&dev_cmd, 0, sizeof(dev_cmd));
	dev_cmd.argsz = sizeof(dev_cmd);
	dev_cmd.flags = TENSTORRENT_DEV_RT_SIGNAL_ALL_PIDS;
	dev_cmd.signal = signum;
	dev_cmd.param = payload;

	printf("Sending RT signal %d (payload=%d) to all PIDs with open FDs on %s\n", 
	       signum, payload, dev_path);

	ret = ioctl(fd, TENSTORRENT_IOCTL_DEV, &dev_cmd);
	if (ret < 0) {
		perror("ioctl");
		close(fd);
		exit(1);
	}

	printf("RT signal sent successfully (check dmesg for count)\n");

	close(fd);
	return 0;
}

