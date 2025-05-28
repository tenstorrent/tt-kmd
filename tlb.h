// SPDX-FileCopyrightText: © 2025 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_TLB_H_INCLUDED
#define TTDRIVER_TLB_H_INCLUDED

#include <linux/types.h>

struct tenstorrent_device;
struct tenstorrent_noc_tlb_config;

struct tlb_descriptor {
	int bar;
	unsigned long size;
	unsigned long bar_offset;
};

int tenstorrent_device_allocate_tlb(struct tenstorrent_device *tt_dev,
				    size_t *size);
int tenstorrent_device_free_tlb(struct tenstorrent_device *tt_dev,
				unsigned int id);
int tenstorrent_device_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				     struct tenstorrent_noc_tlb_config *config);

#endif // TTDRIVER_TLB_H_INCLUDED
