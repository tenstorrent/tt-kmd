// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_MODULE_H_INCLUDED
#define TTDRIVER_MODULE_H_INCLUDED

#include <linux/types.h>
#include <linux/pci.h>

#define TENSTORRENT_DRIVER_VERSION_MAJOR 2
#define TENSTORRENT_DRIVER_VERSION_MINOR 6
#define TENSTORRENT_DRIVER_VERSION_PATCH 0
#define TENSTORRENT_DRIVER_VERSION_SUFFIX "-rc1"

// Module options that need to be passed to other files
extern uint dma_address_bits;
extern uint reset_limit;
extern unsigned char auto_reset_timeout;
extern bool power_policy;

extern struct tenstorrent_device_class wormhole_class;
extern struct tenstorrent_device_class blackhole_class;
extern const struct pci_device_id tenstorrent_ids[];

extern struct dentry *tt_debugfs_root;
extern struct proc_dir_entry *tt_procfs_root;

#endif
