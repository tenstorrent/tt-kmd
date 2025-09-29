# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

obj-m += tenstorrent.o
tenstorrent-y := module.o chardev.o enumerate.o interrupt.o wormhole.o blackhole.o pcie.o hwmon.o sg_helpers.o memory.o tlb.o

KDIR := /lib/modules/$(shell uname -r)/build
KMAKE := $(MAKE) -C $(KDIR) M=$(CURDIR)

# Extract version from dkms.conf
VERSION := $(shell tools/current-version)

.PHONY: all modules modules_install clean help qemu-build archive akms dkms dkms-remove akms-remove show-version

all: modules

modules:
	+$(KMAKE) modules

modules_install:
	+$(KMAKE) modules_install

clean:
	+$(KMAKE) clean

help:
	+$(KMAKE) help

show-version:
	@echo "Current version: $(VERSION)"

dkms:
	sudo dkms add .
	sudo dkms install --force tenstorrent/$(VERSION)
	sudo modprobe tenstorrent

dkms-remove:
	sudo modprobe -r tenstorrent
	sudo dkms remove tenstorrent/$(VERSION) --all

akms:
	doas akms install .
	doas modprobe tenstorrent

akms-remove:
	doas modprobe -r tenstorrent
	doas akms remove tenstorrent

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
