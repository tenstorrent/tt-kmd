# SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

obj-m += tenstorrent.o
tenstorrent-y := module.o chardev.o enumerate.o interrupt.o grayskull.o wormhole.o pcie.o hwmon.o sg_helpers.o

KDIR := /lib/modules/$(shell uname -r)/build
KMAKE := $(MAKE) -C $(KDIR) M=$(CURDIR)

.PHONY: all
all: modules

.PHONY: modules
modules:
	+$(KMAKE) modules

.PHONY: modules_install
modules_install:
	+$(KMAKE) modules_install

.PHONY: clean
clean:
	+$(KMAKE) clean

.PHONY: help
help:
	+$(KMAKE) help

# Helper for running the driver tests in a VM.
# Supposed to be paired with https://github.com/TTDRosen/qemu-utils
# make TT_QEMU_ARCH=x86_64
.PHONY: qemu-build
qemu-build:
	rsync --exclude=.git -r -e 'ssh -p 10022' ../tt-kmd dev@127.0.0.1:
	ssh -p 10022 dev@127.0.0.1 "cd tt-kmd && make && (sudo rmmod tenstorrent.ko || true) && sudo insmod tenstorrent.ko && sudo dmesg"
	ssh -p 10022 dev@127.0.0.1 "cd tt-kmd && make -C test && sudo ./test/ttkmd_test --skip-aer"

ifeq ($(VER),HEAD)
ARCHIVE_TAG_NAME=HEAD
else
ARCHIVE_TAG_NAME=ttdriver-$(VER)
endif

.PHONY: archive
archive:
	git archive --prefix=ttdriver/ -o ttdriver-$(VER).tar.gz $(ARCHIVE_TAG_NAME):$(shell git rev-parse --show-prefix)
