// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_MODULE_H_INCLUDED
#define TTDRIVER_MODULE_H_INCLUDED

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/version.h>

// RHEL 9.4 backports API changes that normally key off newer upstream kernels.
// Keep RHEL_RELEASE_VERSION() guarded because non-RHEL kernels do not define it.
#if defined(RHEL_RELEASE_CODE) && defined(RHEL_RELEASE_VERSION)
#define TT_RHEL_RELEASE_GE(a, b) (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(a, b))
#else
#define TT_RHEL_RELEASE_GE(a, b) 0
#endif

#define TENSTORRENT_DRIVER_VERSION_MAJOR 2
#define TENSTORRENT_DRIVER_VERSION_MINOR 10
#define TENSTORRENT_DRIVER_VERSION_PATCH 1
#define TENSTORRENT_DRIVER_VERSION_SUFFIX "-pre"

// Module options that need to be passed to other files
extern uint dma_address_bits;
extern uint reset_limit;
extern unsigned char auto_reset_timeout;
extern bool power_policy;
extern uint idle_power_down_grace_ms;
extern bool fw_logging;
extern uint fw_log_level;

extern struct tenstorrent_device_class wormhole_class;
extern struct tenstorrent_device_class blackhole_class;
extern const struct pci_device_id tenstorrent_ids[];

extern struct dentry *tt_debugfs_root;
extern struct proc_dir_entry *tt_procfs_root;

#endif
