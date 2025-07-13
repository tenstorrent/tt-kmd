# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

obj-m += tenstorrent.o
tenstorrent-y := module.o chardev.o enumerate.o interrupt.o grayskull.o wormhole.o blackhole.o pcie.o hwmon.o sg_helpers.o memory.o tlb.o

KDIR := /lib/modules/$(shell uname -r)/build
KMAKE := $(MAKE) -C $(KDIR) M=$(CURDIR)

.PHONY: all modules modules_install clean help qemu-build archive dkms

all: modules

modules:
	+$(KMAKE) modules

modules_install:
	+$(KMAKE) modules_install

clean:
	+$(KMAKE) clean

help:
	+$(KMAKE) help

dkms:
	sudo dkms add .
	sudo dkms install tenstorrent/2.1.0
	sudo modprobe tenstorrent

akms:
	doas akms install .
	doas modprobe tenstorrent

# Helper for running the driver tests in a VM.
# Supposed to be paired with https://github.com/TTDRosen/qemu-utils
# make TT_QEMU_ARCH=x86_64
qemu-build:
	rsync --exclude=.git -r -e 'ssh -p 10022' ../tt-kmd dev@127.0.0.1:
	ssh -p 10022 dev@127.0.0.1 "cd tt-kmd && make && (sudo rmmod tenstorrent.ko || true) && sudo insmod tenstorrent.ko && sudo dmesg"
	ssh -p 10022 dev@127.0.0.1 "cd tt-kmd && make -C test && sudo ./test/ttkmd_test --skip-aer"

ifeq ($(VER),HEAD)
ARCHIVE_TAG_NAME=HEAD
else
ARCHIVE_TAG_NAME=ttdriver-$(VER)
endif

archive:
	git archive --prefix=ttdriver/ -o ttdriver-$(VER).tar.gz $(ARCHIVE_TAG_NAME):$(shell git rev-parse --show-prefix)
