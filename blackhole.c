// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/types.h>

#include "blackhole.h"
#include "pcie.h"
#include "module.h"

#define MAX_MRRS 4096

#define TLB_2M_WINDOW_COUNT 202
#define TLB_2M_SHIFT 21
#define TLB_2M_REG_SIZE 12
#define TLB_2M_WINDOW_SIZE (1 << TLB_2M_SHIFT)
#define TLB_2M_WINDOW_MASK (TLB_2M_WINDOW_SIZE - 1)

#define TLB_REGS_START 0x1FC00000   // BAR0
#define TLB_REGS_LEN 0x00001000     // Covers all TLB registers

#define KERNEL_TLB_INDEX (TLB_2M_WINDOW_COUNT - 1)	// Last 2M window is ours
#define KERNEL_TLB_START (KERNEL_TLB_INDEX * TLB_2M_WINDOW_SIZE)
#define KERNEL_TLB_LEN TLB_2M_WINDOW_SIZE

// ARC owns telemetry
#define ARC_X 8
#define ARC_Y 0
#define RESET_SCRATCH(N) (0x80030400 + ((N) * 4))
#define ARC_TELEMETRY_PTR RESET_SCRATCH(13)
#define ARC_TELEMETRY_DATA RESET_SCRATCH(12)

// These are from ARC FW telemetry.h, guaranteed not to change
#define TELEMETRY_VCORE 6
#define TELEMETRY_POWER 7
#define TELEMETRY_CURRENT 8
#define TELEMETRY_ASIC_TEMP 11

struct TLB_2M_REG {
	union {
		struct {
			u32 low32;
			u32 mid32;
			u32 high32;
		};
		// packed to make y_start straddle mid32 and high32
		struct __attribute__((packed)) {
			u64 address : 43;
			u64 x_end : 6;
			u64 y_end : 6;
			u64 x_start : 6;
			u64 y_start : 6;
			u64 noc : 2;
			u64 multicast : 1;
			u64 ordering : 2;
			u64 linked : 1;
			u64 use_static_vc : 1;
			u64 stream_header : 1;
			u64 static_vc : 3;
			u64 reserved : 18;
		};
	};
};
static_assert(sizeof(struct TLB_2M_REG) == TLB_2M_REG_SIZE, "TLB_2M_REG size mismatch");

static u64 program_tlb(struct blackhole_device *bh, u32 x, u32 y, u64 addr) {
	struct TLB_2M_REG conf = {0};
	u8 __iomem *regs = bh->tlb_regs + (KERNEL_TLB_INDEX * TLB_2M_REG_SIZE);

	conf.address = addr >> TLB_2M_SHIFT;
	conf.x_end = x;
	conf.y_end = y;
	conf.ordering = 1;	// strict

	iowrite32(conf.low32, regs + 0);
	iowrite32(conf.mid32, regs + 4);
	iowrite32(conf.high32, regs + 8);

	return addr & TLB_2M_WINDOW_MASK;
}

static u32 noc_read32(struct blackhole_device *bh, u32 x, u32 y, u64 addr) {
	u64 offset;
	u32 val;

	mutex_lock(&bh->kernel_tlb_mutex);

	offset = program_tlb(bh, x, y, addr);
	val = ioread32(bh->kernel_tlb + offset);

	mutex_unlock(&bh->kernel_tlb_mutex);

	return val;
}

struct blackhole_hwmon_label {
	enum hwmon_sensor_types type;
	u32 attr;
	const char *label;
};

struct blackhole_hwmon_attr {
	u32 tag_id;
	enum hwmon_sensor_types type;
	u32 attr;
	u32 addr;
};

static const struct blackhole_hwmon_label bh_hwmon_labels[] = {
	{ hwmon_temp,  hwmon_temp_label,  "asic_temp" },
	{ hwmon_in,    hwmon_in_label,    "vcore"     },
	{ hwmon_curr,  hwmon_curr_label,  "current"   },
	{ hwmon_power, hwmon_power_label, "power"     },
};

// Addresses are set by telemetry_probe
static struct blackhole_hwmon_attr bh_hwmon_attrs[] = {
	{ TELEMETRY_ASIC_TEMP, hwmon_temp,  hwmon_temp_input,  0x0 },
	{ TELEMETRY_VCORE,     hwmon_in,    hwmon_in_input,    0x0 },
	{ TELEMETRY_CURRENT,   hwmon_curr,  hwmon_curr_input,  0x0 },
	{ TELEMETRY_POWER,     hwmon_power, hwmon_power_input, 0x0 },
};

static umode_t bh_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel) {
	int i;

	for (i = 0; i < ARRAY_SIZE(bh_hwmon_labels); ++i) {
		if (type == bh_hwmon_labels[i].type && attr == bh_hwmon_labels[i].attr) {
			return S_IRUGO;
		}
	}

	for (i = 0; i < ARRAY_SIZE(bh_hwmon_attrs); ++i) {
		if (type == bh_hwmon_attrs[i].type && attr == bh_hwmon_attrs[i].attr) {
			return S_IRUGO;
		}
	}

	return 0;
}

static int bh_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel, long *val) {
	struct blackhole_device *bh = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(bh_hwmon_attrs); ++i) {
		if (type == bh_hwmon_attrs[i].type && attr == bh_hwmon_attrs[i].attr) {
			u32 raw;

			if (bh_hwmon_attrs[i].addr == 0)
				return -ENOTSUPP;

			raw = noc_read32(bh, ARC_X, ARC_Y, bh_hwmon_attrs[i].addr);

			if (type == hwmon_temp) {
				u32 int_part = raw >> 16;
				u32 frac_part = raw & 0xFFFF;
				*val = (int_part * 1000) + ((frac_part * 1000) / 0x10000);
			} else if (type == hwmon_curr) {
				*val = raw * 1000;     // Convert A to mA
			} else if (type == hwmon_power) {
				*val = raw * 1000000;  // Convert W to uW
			} else if (type == hwmon_in) {
				*val = raw;            // Reported in mV
			}
			return 0;
		}
	}

	return -ENOTSUPP;
}

static int bh_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
				   const char **str) {
	int i;

	for (i = 0; i < ARRAY_SIZE(bh_hwmon_labels); i++) {
		if (type == bh_hwmon_labels[i].type && attr == bh_hwmon_labels[i].attr) {
			*str = bh_hwmon_labels[i].label;
			return 0;
		}
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_channel_info *bh_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL),
	NULL,
};

static const struct hwmon_ops bh_hwmon_ops = {
	.is_visible = bh_hwmon_is_visible,
	.read = bh_hwmon_read,
	.read_string = bh_hwmon_read_string,
};

static const struct hwmon_chip_info bh_hwmon_chip_info = {
	.ops = &bh_hwmon_ops,
	.info = bh_hwmon_channel_info,
};

static int telemetry_probe(struct tenstorrent_device *tt_dev) {
	struct device *hwmon_device;
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 base_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_TELEMETRY_PTR);
	u32 data_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_TELEMETRY_DATA);
	u32 version = noc_read32(bh, ARC_X, ARC_Y, base_addr);
	u32 major_ver = (version >> 16) & 0xFF;
	u32 minor_ver = (version >> 8) & 0xFF;
	u32 patch_ver = version & 0xFF;
	u32 tags_addr = base_addr + 8;
	u32 num_entries;
	u32 i, j;

	if (major_ver > 1) {
		dev_err(&tt_dev->pdev->dev, "Unsupported telemetry version %u.%u.%u\n", major_ver, minor_ver, patch_ver);
		return -ENOTSUPP;
	}

	num_entries = noc_read32(bh, ARC_X, ARC_Y, base_addr + 4);

	bh->hwmon_attr_addrs = kzalloc(sizeof(u64) * ARRAY_SIZE(bh_hwmon_attrs), GFP_KERNEL);
	if (!bh->hwmon_attr_addrs)
		return -ENOMEM;

	bh->sysfs_attr_addrs = kzalloc(sizeof(u64) * ARRAY_SIZE(bh_attributes), GFP_KERNEL);
	if (!bh->sysfs_attr_addrs) {
		kfree(bh->hwmon_attr_addrs);
		bh->hwmon_attr_addrs = NULL;
		return -ENOMEM;
	}

	for (i = 0; i < num_entries; ++i) {
		u32 tag_entry = noc_read32(bh, ARC_X, ARC_Y, tags_addr + (i * 4));
		u16 tag_id = tag_entry & 0xFFFF;
		u16 offset = (tag_entry >> 16) & 0xFFFF;
		u32 addr = data_addr + (offset * 4);

		// Check if this tag is one hwmon cares about
		for (j = 0; j < ARRAY_SIZE(bh_hwmon_attrs); ++j) {
			if (bh_hwmon_attrs[j].tag_id == tag_id) {
				bh_hwmon_attrs[j].addr = addr;
				break;
			}
		}
	}

	hwmon_device = devm_hwmon_device_register_with_info(&bh->tt.pdev->dev, "blackhole", bh, &bh_hwmon_chip_info, NULL);

	if (IS_ERR(hwmon_device)) {
		kfree(bh->hwmon_attr_addrs);
		kfree(bh->sysfs_attr_addrs);
		bh->hwmon_attr_addrs = NULL;
		bh->sysfs_attr_addrs = NULL;
		return PTR_ERR(hwmon_device);
	}

	return 0;
}

static bool blackhole_init(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	bh->tlb_regs = pci_iomap_range(bh->tt.pdev, 0, TLB_REGS_START, TLB_REGS_LEN);
	bh->kernel_tlb = pci_iomap_range(bh->tt.pdev, 0, KERNEL_TLB_START, KERNEL_TLB_LEN);

	if (!bh->tlb_regs || !bh->kernel_tlb) {
		if (bh->tlb_regs)
			pci_iounmap(bh->tt.pdev, bh->tlb_regs);

		if (bh->kernel_tlb)
			pci_iounmap(bh->tt.pdev, bh->kernel_tlb);
		return false;
	}

	telemetry_probe(tt_dev);

	return true;
}

static bool blackhole_init_hardware(struct tenstorrent_device *tt_dev) {
	struct pci_dev *pdev = tt_dev->pdev;
	pcie_set_readrq(pdev, MAX_MRRS);
	return true;
}

static bool blackhole_post_hardware_init(struct tenstorrent_device *tt_dev) {
	return true;
}

static void blackhole_cleanup_hardware(struct tenstorrent_device *tt_dev) {
}

static void blackhole_cleanup(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (bh->tlb_regs)
		pci_iounmap(tt_dev->pdev, bh->tlb_regs);
	if (bh->kernel_tlb)
		pci_iounmap(tt_dev->pdev, bh->kernel_tlb);

	kfree(bh->hwmon_attr_addrs);
	kfree(bh->sysfs_attr_addrs);
}

struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	.dma_address_bits = 58,
	.init_device = blackhole_init,
	.init_hardware = blackhole_init_hardware,
	.post_hardware_init = blackhole_post_hardware_init,
	.cleanup_hardware = blackhole_cleanup_hardware,
	.cleanup_device = blackhole_cleanup,
};
