// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_MODULE_H_INCLUDED
#define TTDRIVER_MODULE_H_INCLUDED

#include <linux/types.h>
#include <linux/pci.h>

// Module options that need to be passed to other files
extern uint dma_address_bits;
extern uint reset_limit;
extern unsigned char auto_reset_timeout;

extern struct tenstorrent_device_class wormhole_class;
extern struct tenstorrent_device_class blackhole_class;
extern const struct pci_device_id tenstorrent_ids[];

#endif
