// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <iostream>
#include <string>
#include <memory>
#include <cstring>
#include <cstdio>
#include <csetjmp>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "../ioctl.h"

static std::jmp_buf jump_buffer;
static volatile sig_atomic_t signal_count = 0;
static volatile int last_payload = 0;

void rt_signal_handler(int signum, siginfo_t *info, void *ucontext) {
	signal_count++;
	last_payload = info->si_value.sival_int;
	
	char msg[64];
	int len = snprintf(msg, sizeof(msg), ">>> RT SIGNAL (payload=%d) - LONGJMP <<<\n", 
	                   info->si_value.sival_int);
	write(STDOUT_FILENO, msg, len);
	
	std::longjmp(jump_buffer, signum);
}

class DeviceHandle {
public:
	explicit DeviceHandle(int dev_id, unsigned int generation)
		: dev_id_(dev_id), generation_(generation), fd_(-1)
	{
		char dev_path[256];
		snprintf(dev_path, sizeof(dev_path), "/dev/tenstorrent/%d", dev_id);
		
		fd_ = open(dev_path, O_RDWR | O_CLOEXEC);
		if (fd_ < 0) {
			throw std::runtime_error(std::string("Failed to open ") + dev_path + ": " + strerror(errno));
		}
		
		std::cout << "DeviceHandle created (generation " << generation_ << ")\n";
	}

	~DeviceHandle() {
		if (fd_ >= 0) {
			close(fd_);
			std::cout << "DeviceHandle destroyed (generation " << generation_ << ")\n";
		}
	}

	std::string get_device_info() {
		tenstorrent_get_device_info info;
		memset(&info, 0, sizeof(info));
		info.in.output_size_bytes = sizeof(info.out);

		if (ioctl(fd_, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0) {
			return "ERROR: ioctl failed";
		}

		unsigned bus = (info.out.bus_dev_fn >> 8) & 0xFF;
		unsigned device = (info.out.bus_dev_fn >> 3) & 0x1F;
		unsigned function = info.out.bus_dev_fn & 0x7;
		unsigned domain = info.out.pci_domain;

		char buf[512];
		snprintf(buf, sizeof(buf),
			"Gen %u | PCI %04x:%02x:%02x.%x | VID:DID %04x:%04x | SVID:SID %04x:%04x",
			generation_,
			domain, bus, device, function,
			info.out.vendor_id, info.out.device_id,
			info.out.subsystem_vendor_id, info.out.subsystem_id);
		
		return std::string(buf);
	}

	int get_fd() const { return fd_; }
	unsigned int generation() const { return generation_; }

private:
	int dev_id_;
	unsigned int generation_;
	int fd_;
};

int main(int argc, char *argv[]) {
	if (argc != 3) {
		std::cerr << "Usage: " << argv[0] << " <device_id> <signal_num>\n";
		std::cerr << "Example: " << argv[0] << " 0 34  (SIGRTMIN)\n";
		return 1;
	}

	int dev_id = atoi(argv[1]);
	int signum = atoi(argv[2]);

	std::cout << "RT Signal Daemon starting: PID=" << getpid() 
		  << ", device=" << dev_id 
		  << ", signal=" << signum << "\n";
	std::cout << "Press Ctrl+C to exit\n\n";

	volatile unsigned int generation = 0;

	while (true) {
		// Install (or reinstall) signal handler with SA_SIGINFO
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = rt_signal_handler;  // Note: sa_sigaction, not sa_handler
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_SIGINFO | SA_NODEFER;  // SA_SIGINFO required for RT signals

		if (sigaction(signum, &sa, nullptr) < 0) {
			std::cerr << "sigaction failed: " << strerror(errno) << "\n";
			return 1;
		}

		// Explicitly unblock the signal in case it was blocked
		sigset_t sigset;
		sigemptyset(&sigset);
		sigaddset(&sigset, signum);
		sigprocmask(SIG_UNBLOCK, &sigset, nullptr);

		// Set the jump point for signal handler
		int sig = setjmp(jump_buffer);
		if (sig != 0) {
			// We just got back from longjmp - increment generation
			generation++;
			std::cout << "\nReturned from longjmp (signal " << sig 
				  << ", payload=" << last_payload
				  << "), creating new instance...\n\n";
		}

		// Create (or recreate) the device handle
		std::unique_ptr<DeviceHandle> device;
		try {
			device = std::make_unique<DeviceHandle>(dev_id, generation);
		} catch (const std::exception &e) {
			std::cerr << "Failed to create DeviceHandle: " << e.what() << "\n";
			return 1;
		}

		// Main loop - query device info periodically
		unsigned long counter = 0;
		while (true) {
			counter++;
			try {
				std::string info = device->get_device_info();
				std::cout << "Counter: " << counter 
					  << " | Signals: " << signal_count 
					  << " | Last payload: " << last_payload
					  << " | " << info << "\n";
			} catch (const std::exception &e) {
				std::cerr << "Error getting device info: " << e.what() << "\n";
			}

			sleep(1);
		}
		// If we get a signal during sleep, we'll longjmp back to setjmp above
	}

	return 0;
}

