obj-m += tenstorrent.o
tenstorrent-y := module.o chardev.o enumerate.o interrupt.o grayskull.o wormhole.o

KDIR := /lib/modules/$(shell uname -r)/build
KMAKE := $(MAKE) -C $(KDIR) M=$(CURDIR)

.PHONY: all
all: modules

.PHONY: modules
modules:
	$(KMAKE) modules

.PHONY: modules_install
modules_install:
	$(KMAKE) modules_install

.PHONY: clean
clean:
	$(KMAKE) clean

.PHONY: help
help:
	$(KMAKE) help


ifeq ($(VER),HEAD)
ARCHIVE_TAG_NAME=HEAD
else
ARCHIVE_TAG_NAME=ttdriver-$(VER)
endif

.PHONY: archive
archive:
	git archive --prefix=ttdriver/ -o ttdriver-$(VER).tar.gz $(ARCHIVE_TAG_NAME):$(shell git rev-parse --show-prefix)
