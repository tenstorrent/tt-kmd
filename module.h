// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_MODULE_H_INCLUDED
#define TTDRIVER_MODULE_H_INCLUDED

#include <linux/types.h>
#include <linux/pci.h>

#define TENSTORRENT_DRIVER_VERSION_MAJOR 1
#define TENSTORRENT_DRIVER_VERSION_MINOR 34
#define TENSTORRENT_DRIVER_VERSION_PATCH 0
#define TENSTORRENT_DRIVER_VERSION_SUFFIX ""    // e.g. "-rc1"

// Module options that need to be passed to other files
extern bool arc_fw_init;
extern bool arc_fw_override;
extern bool arc_fw_stage2_init;
extern bool ddr_train_en;
extern bool ddr_test_mode;
extern uint ddr_frequency_override;
extern bool aiclk_ppm_en;
extern uint aiclk_fmax_override;
extern uint arc_fw_feat_dis_override;
extern bool watchdog_fw_en;
extern bool watchdog_fw_override;
extern bool smbus_fw_en;
extern bool smbus_fw_override;
extern uint axiclk_override;
extern uint tensix_harvest_override;
extern uint dma_address_bits;
extern uint reset_limit;
extern unsigned char auto_reset_timeout;

extern struct tenstorrent_device_class grayskull_class;
extern struct tenstorrent_device_class wormhole_class;
extern struct tenstorrent_device_class blackhole_class;
extern const struct pci_device_id tenstorrent_ids[];

#endif
