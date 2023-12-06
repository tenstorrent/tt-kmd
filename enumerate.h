// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_ENUMERATE_H_INCLUDED
#define TTDRIVER_ENUMERATE_H_INCLUDED

#include <linux/list.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/cdev.h>

#define TENSTORRENT "tenstorrent"

#define PCI_VENDOR_ID_TENSTORRENT 0x1E52
#define PCI_DEVICE_ID_GRAYSKULL	0xFACA
#define PCI_DEVICE_ID_WORMHOLE	0x401E
#define PCI_DEVICE_ID_BLACKHOLE	0xB140

struct pci_dev;
struct cdev;

int tenstorrent_pci_register_driver(void);
void tenstorrent_pci_unregister_driver(void);

struct tenstorrent_device *tenstorrent_lookup_device(unsigned minor);

#endif
