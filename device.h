// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_DEVICE_H_INCLUDED
#define TTDRIVER_DEVICE_H_INCLUDED

#include <linux/types.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/reboot.h>
#include <linux/kref.h>

#include "ioctl.h"
#include "hwmon.h"
#include "memory.h"

struct tenstorrent_device_class;

struct tenstorrent_device {
	struct kref kref;

	struct device dev;
	struct cdev chardev;
	struct pci_dev *pdev;
	const struct tenstorrent_device_class *dev_class;

	unsigned int ordinal;
	bool dma_capable;
	bool interrupt_enabled;

	struct mutex chardev_mutex;
	unsigned int chardev_open_count;

	struct notifier_block reboot_notifier;

	DECLARE_BITMAP(resource_lock, TENSTORRENT_RESOURCE_LOCK_COUNT);

	struct tt_hwmon_context hwmon_context;

	const struct tenstorrent_sysfs_attr *sysfs_attrs;

	struct list_head open_fds_list;	// List of struct chardev_private, linked through open_fds field

	DECLARE_BITMAP(tlbs, TENSTORRENT_MAX_INBOUND_TLBS);
	atomic_t tlb_refs[TENSTORRENT_MAX_INBOUND_TLBS];	// TLB mapping refecounts

	struct mutex iatu_mutex;
	struct tenstorrent_outbound_iatu_region outbound_iatus[TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS];
};

struct tlb_descriptor;

#define MAX_TLB_KINDS 4
struct tenstorrent_device_class {
	const char *name;
	u32 instance_size;
	u32 dma_address_bits;
	u64 noc_dma_limit;
	u64 noc_pcie_offset;
	u32 tlb_kinds;
	u32 tlb_counts[MAX_TLB_KINDS];
	u64 tlb_sizes[MAX_TLB_KINDS];
	bool (*init_device)(struct tenstorrent_device *ttdev);
	bool (*init_hardware)(struct tenstorrent_device *ttdev);
	bool (*post_hardware_init)(struct tenstorrent_device *ttdev);
	void (*cleanup_hardware)(struct tenstorrent_device *ttdev);
	void (*cleanup_device)(struct tenstorrent_device *ttdev);
	void (*first_open_cb)(struct tenstorrent_device *ttdev);
	void (*last_release_cb)(struct tenstorrent_device *ttdev);
	void (*reboot)(struct tenstorrent_device *ttdev);
	int (*configure_tlb)(struct tenstorrent_device *ttdev, int tlb, struct tenstorrent_noc_tlb_config *config);
	int (*describe_tlb)(struct tenstorrent_device *ttdev, int tlb, struct tlb_descriptor *tlb_desc);
	void (*save_reset_state)(struct tenstorrent_device *ttdev);
	void (*restore_reset_state)(struct tenstorrent_device *ttdev);
	int (*configure_outbound_atu)(struct tenstorrent_device *ttdev, u32 region, u64 base, u64 limit, u64 target);
	void (*create_sysfs_groups)(struct tenstorrent_device *ttdev);
};

void tenstorrent_device_put(struct tenstorrent_device *);

#endif
