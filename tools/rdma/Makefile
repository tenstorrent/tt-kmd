# SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

CFLAGS ?= -O2 -Wall -Wextra

PROGS := rdma_loopback dmabuf_loopback nic_bh_loopback

all: $(PROGS)

# dmabuf_loopback is plain ioctl/mmap; the verbs tools link libibverbs.
rdma_loopback: LDLIBS := -libverbs
nic_bh_loopback: LDLIBS := -libverbs

clean:
	rm -f $(PROGS)

.PHONY: all clean
