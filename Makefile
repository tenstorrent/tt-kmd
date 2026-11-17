# SPDX-License-Identifier: GPL-2.0-only
# Tenstorrent Blackhole kernel driver.

ifneq ($(KERNELRELEASE),)

obj-m := tenstorrent.o
tenstorrent-y := module.o pcie.o chardev.o blackhole.o debugfs.o procfs.o \
		 memory.o tlb.o noc.o smc.o power.o telemetry.o hwmon.o \
		 fwlog.o fwlog_parser.o

else

# Build against the running kernel, or the target kernel if its headers are
# not installed here.
KVER ?= $(shell uname -r)
KDIR ?= $(if $(wildcard /lib/modules/$(KVER)/build),/lib/modules/$(KVER)/build,/lib/modules/6.8.0-149-generic/build)

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all modules clean

endif
