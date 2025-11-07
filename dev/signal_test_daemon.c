// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>

#include "../ioctl.h"

static volatile sig_atomic_t signal_count = 0;
static volatile sig_atomic_t last_signal = 0;

void signal_handler(int signum) {
	signal_count++;
	last_signal = signum;
	write(STDOUT_FILENO, ">>> SIGNAL RECEIVED <<<\n", 24);
}

int main(int argc, char *argv[]) {
	int fd, signum;
	struct sigaction sa;
	unsigned long counter = 0;
	
	if (argc != 3) {
		fprintf(stderr, "Usage: %s <device_id> <signal_num>\n", argv[0]);
		fprintf(stderr, "Example: %s 0 10  (SIGUSR1)\n", argv[0]);
		exit(1);
	}

	int dev_id = atoi(argv[1]);
	signum = atoi(argv[2]);

	char dev_path[256];
	snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);

	fd = open(dev_path, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(signum, &sa, NULL) < 0) {
		perror("sigaction");
		close(fd);
		exit(1);
	}

	printf("Daemon running: PID=%d, device=%s, waiting for signal %d\n",
	       getpid(), dev_path, signum);
	printf("Press Ctrl+C to exit\n");

	while (1) {
		counter++;
		if (counter % 10 == 0) {
			printf("Counter: %lu, signals received: %d (last=%d)\n",
			       counter, signal_count, last_signal);
			fflush(stdout);
		}
		
		// Simulate some work
		sleep(1);
	}

	close(fd);
	return 0;
}

