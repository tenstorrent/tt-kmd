# SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
# SPDX-License-Identifier: GPL-2.0-only

CFLAGS ?= -O2 -Wall -Wextra

PROGS := nic_bh_p2p_dma

all: $(PROGS)

nic_bh_p2p_dma: LDLIBS := -libverbs

clean:
	rm -f $(PROGS)

.PHONY: all clean
