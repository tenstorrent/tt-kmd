# SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

obj-m += tenstorrent.o
tenstorrent-y := module.o chardev.o enumerate.o interrupt.o wormhole.o blackhole.o pcie.o sg_helpers.o memory.o tlb.o telemetry.o

# Capture the module directory at the top level before kernel build system changes context
MODULE_DIR := $(CURDIR)

KDIR := /lib/modules/$(shell uname -r)/build
KMAKE := $(MAKE) -C $(KDIR) M=$(MODULE_DIR)

# Extract version from dkms.conf (use conditional to avoid errors in kernel build context)
VERSION := $(shell [ -x $(MODULE_DIR)/tools/current-version ] && $(MODULE_DIR)/tools/current-version || echo "unknown")

.PHONY: all modules modules_install clean help akms dkms dkms-remove akms-remove show-version

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
	@echo "Removing all installed tenstorrent DKMS modules..."
	@for ver in $$(dkms status tenstorrent 2>/dev/null | sed -nE 's|^tenstorrent/([^,]+),.*|\1|p' | sort -u); do \
		echo "Removing tenstorrent/$$ver..."; \
		sudo dkms remove tenstorrent/$$ver --all || true; \
	done
	@echo "DKMS removal complete."

akms:
	doas akms install .
	doas modprobe tenstorrent

akms-remove:
	doas modprobe -r tenstorrent
	doas akms remove tenstorrent
