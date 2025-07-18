// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/delay.h>

#include "blackhole.h"
#include "pcie.h"
#include "module.h"
#include "tlb.h"
#include "telemetry.h"

#define MAX_MRRS 4096

#define TLB_2M_WINDOW_COUNT 202
#define TLB_2M_SHIFT 21
#define TLB_2M_WINDOW_SIZE (1 << TLB_2M_SHIFT)
#define TLB_2M_WINDOW_MASK (TLB_2M_WINDOW_SIZE - 1)

#define TLB_4G_WINDOW_COUNT 8
#define TLB_4G_SHIFT 32
#define TLB_4G_WINDOW_SIZE (1UL << TLB_4G_SHIFT)
#define TLB_4G_WINDOW_MASK (TLB_4G_WINDOW_SIZE - 1)

#define TLB_REG_SIZE 12	// Same for 2M and 4G

#define TLB_REGS_START 0x1FC00000   // BAR0
#define TLB_REGS_LEN 0x00001000     // Covers all TLB registers

#define KERNEL_TLB_INDEX (TLB_2M_WINDOW_COUNT - 1)	// Last 2M window is ours
#define KERNEL_TLB_START (KERNEL_TLB_INDEX * TLB_2M_WINDOW_SIZE)
#define KERNEL_TLB_LEN TLB_2M_WINDOW_SIZE

#define NOC2AXI_CFG_START 0x1FD00000
#define NOC2AXI_CFG_LEN 0x00100000
#define NOC_ID_OFFSET 0x4044
#define NOC_STATUS_OFFSET 0x4200
#define NOC1_NOC2AXI_OFFSET 0x10000

// this points to outbound NOC_TLB_62 configured by CMFW
#define PCIE_DBI_ADDR 0xF800000000000000ULL

// ARC owns telemetry
#define ARC_X 8
#define ARC_Y 0
#define RESET_SCRATCH(N) (0x80030400 + ((N) * 4))
#define ARC_TELEMETRY_PTR RESET_SCRATCH(13)
#define ARC_TELEMETRY_DATA RESET_SCRATCH(12)

// ARC FW has a messaging interface, see msgqueue.c in tt-zephyr-platforms.git
#define ARC_MSG_QCB_PTR RESET_SCRATCH(11) // Message Queue Control Block
#define ARC_MSI_FIFO 0x800B0000           // Write 0 to trigger the ARC message queue processor
#define ARC_MSG_QUEUE_HEADER_SIZE 32      // Header contains request and response read/write pointers
#define ARC_MSG_TIMEOUT_MS 100            // Wait this long for ARC message queue operations
#define ARC_MSG_QUEUE_REQ_WPTR(base) ((base) + 0x00)
#define ARC_MSG_QUEUE_RES_RPTR(base) ((base) + 0x04)
#define ARC_MSG_QUEUE_REQ_RPTR(base) ((base) + 0x10)
#define ARC_MSG_QUEUE_RES_WPTR(base) ((base) + 0x14)
#define ARC_MSG_TYPE_ASIC_STATE0 0xA0
#define ARC_MSG_TYPE_ASIC_STATE3 0xA3
#define ARC_MSG_TYPE_SET_WDT_TIMEOUT 0xC1

#define IATU_BASE 0x1000	// Relative to the start of BAR2
#define IATU_OUTBOUND 0
#define IATU_OUTBOUND_REGIONS 16
#define IATU_REGION_STRIDE 0x100
#define IATU_REGION_CTRL_1_OUTBOUND 0x00
#define IATU_REGION_CTRL_2_OUTBOUND 0x04
#define IATU_LOWER_BASE_ADDR_OUTBOUND 0x08
#define IATU_UPPER_BASE_ADDR_OUTBOUND 0x0C
#define IATU_LOWER_LIMIT_ADDR_OUTBOUND 0x10
#define IATU_LOWER_TARGET_ADDR_OUTBOUND 0x14
#define IATU_UPPER_TARGET_ADDR_OUTBOUND 0x18
#define IATU_REGION_CTRL_3_OUTBOUND 0x1C
#define IATU_UPPER_LIMIT_ADDR_OUTBOUND 0x20

// IATU_REGION_CTRL_2_OUTBOUND fields
#define REGION_EN (1 << 31)

#define WRITE_IATU_REG(wh_dev, direction, region, reg, value) \
	write_iatu_reg(wh_dev, IATU_##direction, region, \
		       IATU_##reg##_##direction, (value))

static void write_iatu_reg(struct blackhole_device *bh_dev, unsigned direction,
			   unsigned region, unsigned reg, u32 value) {
	u32 offset = IATU_BASE + (2 * region + direction) * IATU_REGION_STRIDE
		   + reg;

	iowrite32(value, bh_dev->bar2_mapping + offset);
}

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
static_assert(sizeof(struct TLB_2M_REG) == TLB_REG_SIZE, "TLB_2M_REG size mismatch");

struct TLB_4G_REG {
	union {
		struct {
			u32 low32;
			u32 mid32;
			u32 high32;
		};
		struct __attribute__((packed)) {
			u32 address : 32;
			u32 x_end : 6;
			u32 y_end : 6;
			u32 x_start : 6;
			u32 y_start : 6;
			u32 noc : 2;
			u32 multicast : 1;
			u32 ordering : 2;
			u32 linked : 1;
			u32 use_static_vc : 1;
			u32 stream_header : 1;
			u32 static_vc : 3;
			u32 reserved : 29;
		};
	};
};
static_assert(sizeof(struct TLB_4G_REG) == TLB_REG_SIZE, "TLB_4G_REG size mismatch");

static int blackhole_configure_tlb_2M(struct blackhole_device *bh, int tlb,
				      struct tenstorrent_noc_tlb_config *config)
{
	u8 __iomem *regs = bh->tlb_regs + (tlb * TLB_REG_SIZE);
	struct TLB_2M_REG reg = { 0 };

	// Not possible to program a 2M window that doesn't start on a 2M boundary.
	if (config->addr & TLB_2M_WINDOW_MASK)
		return -EINVAL;

	reg.address = config->addr >> TLB_2M_SHIFT;
	reg.x_end = config->x_end;
	reg.y_end = config->y_end;
	reg.x_start = config->x_start;
	reg.y_start = config->y_start;
	reg.noc = config->noc;
	reg.multicast = config->mcast;
	reg.ordering = config->ordering;
	reg.linked = config->linked;
	reg.use_static_vc = config->static_vc;

	iowrite32(reg.low32, regs + 0);
	iowrite32(reg.mid32, regs + 4);
	iowrite32(reg.high32, regs + 8);

	return 0;
}

static int blackhole_configure_tlb_4G(struct blackhole_device *bh, int tlb,
				      struct tenstorrent_noc_tlb_config *config)
{
	u8 __iomem *regs = bh->tlb_regs + (tlb * TLB_REG_SIZE);
	struct TLB_4G_REG reg = { 0 };

	// Not possible to program a 4G window that doesn't start on a 4G boundary.
	if (config->addr & TLB_4G_WINDOW_MASK)
		return -EINVAL;

	reg.address = config->addr >> TLB_4G_SHIFT;
	reg.x_end = config->x_end;
	reg.y_end = config->y_end;
	reg.x_start = config->x_start;
	reg.y_start = config->y_start;
	reg.noc = config->noc;
	reg.multicast = config->mcast;
	reg.ordering = config->ordering;
	reg.linked = config->linked;
	reg.use_static_vc = config->static_vc;

	iowrite32(reg.low32, regs + 0);
	iowrite32(reg.mid32, regs + 4);
	iowrite32(reg.high32, regs + 8);

	return 0;
}

static u8 __iomem *bh_configure_kernel_tlb(struct blackhole_device *bh, u32 x, u32 y, u64 addr) {
	struct tenstorrent_noc_tlb_config config = { 0 };
	u64 offset = addr & TLB_2M_WINDOW_MASK;

	config.addr = addr & ~TLB_2M_WINDOW_MASK;
	config.x_end = x;
	config.y_end = y;
	config.ordering	= 1; // strict

	blackhole_configure_tlb_2M(bh, KERNEL_TLB_INDEX, &config);
	return bh->kernel_tlb + offset;
}

static u32 noc_read32(struct blackhole_device *bh, u32 x, u32 y, u64 addr) {
	u32 val;
	u8 __iomem *tlb_window;

	mutex_lock(&bh->kernel_tlb_mutex);

	tlb_window = bh_configure_kernel_tlb(bh, x, y, addr);
	val = ioread32(tlb_window);

	mutex_unlock(&bh->kernel_tlb_mutex);

	return val;
}

static void noc_write32(struct blackhole_device *bh, u32 x, u32 y, u64 addr, u32 data) {
	u8 __iomem *tlb_window;

	mutex_lock(&bh->kernel_tlb_mutex);

	tlb_window = bh_configure_kernel_tlb(bh, x, y, addr);
	iowrite32(data, tlb_window);

	mutex_unlock(&bh->kernel_tlb_mutex);
}

static int csm_read32(struct blackhole_device *bh, u64 addr, u32 *value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	*value = noc_read32(bh, ARC_X, ARC_Y, addr);
	return 0;
}

static int csm_write32(struct blackhole_device *bh, u64 addr, u32 value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	noc_write32(bh, ARC_X, ARC_Y, addr, value);
	return 0;
}


// BH has two PCIE instances, the function reads NOC ID to find out which one is active
static bool blackhole_detect_pcie_noc_x(struct blackhole_device *bh, u32 *noc_x) {
	*noc_x = ioread32(bh->noc2axi_cfg + NOC_ID_OFFSET) & 0x3F;
	return (*noc_x == 2 || *noc_x == 11);
}

static void blackhole_save_reset_state(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;
	u32 y = 0;
	u32 device_control;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return;

	device_control = noc_read32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS);
	bh->saved_mps = FIELD_GET(PCI_EXP_DEVCTL_PAYLOAD, device_control);
}

static void blackhole_restore_reset_state(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;
	u32 y = 0;
	u32 device_control;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return;

	device_control = noc_read32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS);
	device_control &= ~PCI_EXP_DEVCTL_PAYLOAD;
	device_control |= FIELD_PREP(PCI_EXP_DEVCTL_PAYLOAD, bh->saved_mps);
	noc_write32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, device_control);
}

static ssize_t bh_show_pcie_single_counter(struct device *dev, char *buf, u32 counter_offset, int noc)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u64 offset = NOC_STATUS_OFFSET + (4 * counter_offset) + (noc * NOC1_NOC2AXI_OFFSET);
	u32 value = ioread32(bh->noc2axi_cfg + offset);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

// The pair are for NOC0 and NOC1; counters for each NOC exposed separately.
#define BH_PCIE_COUNTER_ATTR_RO_PAIR(_base_name_str, _counter_type_id_const) \
static ssize_t _base_name_str##0_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	return bh_show_pcie_single_counter(dev, buf, _counter_type_id_const, 0); \
} \
static ssize_t _base_name_str##1_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	return bh_show_pcie_single_counter(dev, buf, _counter_type_id_const, 1); \
} \
static DEVICE_ATTR_RO(_base_name_str##0); \
static DEVICE_ATTR_RO(_base_name_str##1)

#define SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39
#define SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38
#define SLV_RD_DATA_WORD_SENT 0x33
#define MST_POSTED_WR_DATA_WORD_SENT 0x9
#define MST_NONPOSTED_WR_DATA_WORD_SENT 0x8
#define MST_RD_DATA_WORD_RECEIVED 0x3

BH_PCIE_COUNTER_ATTR_RO_PAIR(slv_posted_wr_data_word_received, SLV_POSTED_WR_DATA_WORD_RECEIVED);
BH_PCIE_COUNTER_ATTR_RO_PAIR(slv_nonposted_wr_data_word_received, SLV_NONPOSTED_WR_DATA_WORD_RECEIVED);
BH_PCIE_COUNTER_ATTR_RO_PAIR(slv_rd_data_word_sent, SLV_RD_DATA_WORD_SENT);
BH_PCIE_COUNTER_ATTR_RO_PAIR(mst_posted_wr_data_word_sent, MST_POSTED_WR_DATA_WORD_SENT);
BH_PCIE_COUNTER_ATTR_RO_PAIR(mst_nonposted_wr_data_word_sent, MST_NONPOSTED_WR_DATA_WORD_SENT);
BH_PCIE_COUNTER_ATTR_RO_PAIR(mst_rd_data_word_received, MST_RD_DATA_WORD_RECEIVED);

#define ATTR_LIST(name) (&dev_attr_##name.attr)

static struct attribute *bh_pcie_perf_counters_attrs[] = {
	ATTR_LIST(slv_posted_wr_data_word_received0),
	ATTR_LIST(slv_nonposted_wr_data_word_received0),
	ATTR_LIST(slv_rd_data_word_sent0),
	ATTR_LIST(mst_posted_wr_data_word_sent0),
	ATTR_LIST(mst_nonposted_wr_data_word_sent0),
	ATTR_LIST(mst_rd_data_word_received0),
	ATTR_LIST(slv_posted_wr_data_word_received1),
	ATTR_LIST(slv_nonposted_wr_data_word_received1),
	ATTR_LIST(slv_rd_data_word_sent1),
	ATTR_LIST(mst_posted_wr_data_word_sent1),
	ATTR_LIST(mst_nonposted_wr_data_word_sent1),
	ATTR_LIST(mst_rd_data_word_received1),
	NULL,
};

static const struct attribute_group bh_pcie_perf_counters_group = {
	.name = "pcie_perf_counters",
	.attrs = bh_pcie_perf_counters_attrs,
};

struct blackhole_hwmon_label {
	enum hwmon_sensor_types type;
	u32 attr;
	const char *label;
};

struct blackhole_hwmon_attr {
	u32 tag_id;
	enum hwmon_sensor_types type;
	u32 attr;
};

static const struct blackhole_hwmon_label bh_hwmon_labels[] = {
	{ hwmon_temp,  hwmon_temp_label,  "asic_temp" },
	{ hwmon_in,    hwmon_in_label,    "vcore"     },
	{ hwmon_curr,  hwmon_curr_label,  "current"   },
	{ hwmon_power, hwmon_power_label, "power"     },
	{ hwmon_fan,   hwmon_fan_label,   "fan_rpm"   },
};

static const struct blackhole_hwmon_attr bh_hwmon_attrs[] = {
	{ TELEMETRY_ASIC_TEMP, hwmon_temp,  hwmon_temp_input  },
	{ TELEMETRY_VCORE,     hwmon_in,    hwmon_in_input    },
	{ TELEMETRY_CURRENT,   hwmon_curr,  hwmon_curr_input  },
	{ TELEMETRY_POWER,     hwmon_power, hwmon_power_input },
	{ TELEMETRY_FAN_RPM,   hwmon_fan,   hwmon_fan_input   },
};

static ssize_t sysfs_show_u32_dec(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t sysfs_show_u64_hex(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t sysfs_show_u32_ver(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t sysfs_show_card_type(struct device *dev, struct device_attribute *attr, char *buf);

static struct tenstorrent_sysfs_attr bh_sysfs_attributes[] = {
	{ TELEMETRY_AICLK, __ATTR(tt_aiclk,  S_IRUGO, sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_AXICLK, __ATTR(tt_axiclk, S_IRUGO, sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_ARCCLK, __ATTR(tt_arcclk, S_IRUGO, sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_serial, S_IRUGO, sysfs_show_u64_hex, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_card_type, S_IRUGO, sysfs_show_card_type, NULL) },
	{ TELEMETRY_FLASH_BUNDLE_VERSION, __ATTR(tt_fw_bundle_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_BM_APP_FW_VERSION, __ATTR(tt_m3app_fw_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_ASIC_ID, __ATTR(tt_asic_id, S_IRUGO, sysfs_show_u64_hex, NULL) },
	{ 0, __ATTR_NULL }
};

static ssize_t sysfs_show_u32_dec(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - bh_sysfs_attributes;
	u64 addr = bh->sysfs_attr_addrs[i];
	u32 value = 0;

	if (csm_read32(bh, addr, &value) != 0)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "%u\n", value);
}

static ssize_t sysfs_show_u64_hex(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - bh_sysfs_attributes;
	u64 addr = bh->sysfs_attr_addrs[i];
	u32 hi, lo;

	if (csm_read32(bh, addr, &hi) != 0)
		return -EINVAL;

	if (csm_read32(bh, addr + 4, &lo) != 0)
		return -EINVAL;

	return scnprintf(buf, PAGE_SIZE, "%08X%08X\n", hi, lo);
}

static ssize_t sysfs_show_u32_ver(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - bh_sysfs_attributes;
	u64 addr = bh->sysfs_attr_addrs[i];
	u32 fw_ver = 0;
	u32 major, minor, patch, ver;

	if (csm_read32(bh, addr, &fw_ver) != 0)
		return -EINVAL;

	major = (fw_ver >> 24) & 0xFF;
	minor = (fw_ver >> 16) & 0xFF;
	patch = (fw_ver >>  8) & 0xFF;
	ver = fw_ver & 0xFF;

	return scnprintf(buf, PAGE_SIZE, "%u.%u.%u.%u\n", major, minor, patch, ver);
}

static ssize_t sysfs_show_card_type(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - bh_sysfs_attributes;
	u64 addr = bh->sysfs_attr_addrs[i];
	u32 board_id_hi;
	u16 card_type;
	char *card_name;

	if (csm_read32(bh, addr, &board_id_hi) != 0)
		return -EINVAL;

	card_type = (board_id_hi >> 4) & 0xFFFF;
	switch (card_type) {
	case 0x36: card_name = "p100"; break;
	case 0x40: card_name = "p150a"; break;
	case 0x41: card_name = "p150b"; break;
	case 0x42: card_name = "p150c"; break;
	case 0x43: card_name = "p100a"; break;
	case 0x44: card_name = "p300b"; break;
	case 0x45: card_name = "p300a"; break;
	case 0x46: card_name = "p300c"; break;
	case 0x47: card_name = "galaxy-blackhole"; break;
	default: card_name = "unknown"; break;
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", card_name);
}

static umode_t bh_hwmon_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr, int channel) {
	struct blackhole_device *bh = (struct blackhole_device *)drvdata;
	int i;

	for (i = 0; i < ARRAY_SIZE(bh_hwmon_labels); ++i) {
		if (type == bh_hwmon_labels[i].type && attr == bh_hwmon_labels[i].attr) {
			return S_IRUGO;
		}
	}

	for (i = 0; i < ARRAY_SIZE(bh_hwmon_attrs); ++i) {
		bool valid = (bh->hwmon_attr_addrs[i] != 0); // Whether the attribute was probed successfully.
		if (valid && type == bh_hwmon_attrs[i].type && attr == bh_hwmon_attrs[i].attr) {
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

			if (bh->hwmon_attr_addrs[i] == 0)
				return -ENOTSUPP;

			raw = noc_read32(bh, ARC_X, ARC_Y, bh->hwmon_attr_addrs[i]);

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
			} else if (type == hwmon_fan) {
				*val = raw;            // Reported in RPM
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
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
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
	u32 version, major_ver, minor_ver, patch_ver;
	u32 tags_addr = base_addr + 8;
	u32 num_entries;
	u32 i, j;

	if (!is_range_within_csm(base_addr, 1) || !is_range_within_csm(data_addr, 1)) {
		dev_err(&tt_dev->pdev->dev, "Telemetry not available\n");
		return -ENODEV;
	}

	version = noc_read32(bh, ARC_X, ARC_Y, base_addr);
	major_ver = (version >> 16) & 0xFF;
	minor_ver = (version >> 8) & 0xFF;
	patch_ver = version & 0xFF;

	if (major_ver > 1) {
		dev_err(&tt_dev->pdev->dev, "Unsupported telemetry version %u.%u.%u\n", major_ver, minor_ver, patch_ver);
		return -ENOTSUPP;
	}

	num_entries = noc_read32(bh, ARC_X, ARC_Y, base_addr + 4);

	bh->hwmon_attr_addrs = kzalloc(sizeof(u64) * ARRAY_SIZE(bh_hwmon_attrs), GFP_KERNEL);
	if (!bh->hwmon_attr_addrs)
		return -ENOMEM;

	bh->sysfs_attr_addrs = kzalloc(sizeof(u64) * ARRAY_SIZE(bh_sysfs_attributes), GFP_KERNEL);
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

		// First, check if this tag is one hwmon cares about
		for (j = 0; j < ARRAY_SIZE(bh_hwmon_attrs); ++j) {
			if (bh_hwmon_attrs[j].tag_id == tag_id) {
				bh->hwmon_attr_addrs[j] = addr;
				break;
			}
		}

		// Check if it's a device attribute we will expose in sysfs
		for (j = 0; j < ARRAY_SIZE(bh_sysfs_attributes); ++j) {
			if (bh_sysfs_attributes[j].tag_id == tag_id)
				bh->sysfs_attr_addrs[j] = addr;
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

	tt_dev->sysfs_attrs = bh_sysfs_attributes;

	return 0;
}

struct arc_msg {
	u32 header;
	u32 payload[7];
};

static bool push_arc_msg(struct blackhole_device *bh, const struct arc_msg *msg, u32 queue_base, u32 num_entries)
{
	u32 request_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE;
	unsigned long timeout;
	u32 wptr;
	u32 slot;
	u32 req_offset;
	int i;

	if (csm_read32(bh, ARC_MSG_QUEUE_REQ_WPTR(queue_base), &wptr) != 0)
		return false;

	// Wait until there is space in the request queue or we timeout.
	timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS);
	for (;;) {
		u32 rptr;
		u32 num_occupied;

		if (csm_read32(bh, ARC_MSG_QUEUE_REQ_RPTR(queue_base), &rptr) != 0)
			return false;

		num_occupied = (wptr - rptr) % (2 * num_entries);
		if (num_occupied < num_entries)
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(&bh->tt.pdev->dev, "Timeout waiting for space in ARC message queue\n");
			return false;
		}

		usleep_range(100, 200);
	}

	// Write the message header and payload to the request queue.
	slot = wptr % num_entries;
	req_offset = slot * sizeof(struct arc_msg);
	for (i = 0; i < 8; ++i) {
		u32 addr = request_base + req_offset + (i * sizeof(u32));
		u32 value = (i == 0) ? msg->header : msg->payload[i - 1];

		if (csm_write32(bh, addr, value) != 0)
			return false;
	}

	// Increment the request write pointer.
	wptr = (wptr + 1) % (2 * num_entries);
	if (csm_write32(bh, ARC_MSG_QUEUE_REQ_WPTR(queue_base), wptr) != 0)
		return false;

	return true;
}

static bool pop_arc_msg(struct blackhole_device *bh, struct arc_msg *msg, u32 queue_base, u32 num_entries)
{
	u32 response_base = queue_base + ARC_MSG_QUEUE_HEADER_SIZE + (num_entries * sizeof(struct arc_msg));
	unsigned long timeout;
	u32 rptr;
	u32 slot;
	u32 response_offset ;
	int i;

	if (csm_read32(bh, ARC_MSG_QUEUE_RES_RPTR(queue_base), &rptr) != 0)
		return false;

	// Wait until there is a message in the response queue or we timeout.
	timeout = jiffies + msecs_to_jiffies(ARC_MSG_TIMEOUT_MS);
	for (;;) {
		u32 wptr;
		u32 num_occupied;

		if (csm_read32(bh, ARC_MSG_QUEUE_RES_WPTR(queue_base), &wptr) != 0)
			return false;

		num_occupied = (wptr - rptr) % (2 * num_entries);
		if (num_occupied > 0)
			break;

		if (time_after(jiffies, timeout)) {
			dev_err(&bh->tt.pdev->dev, "Timeout waiting for ARC response\n");
			return false;
		}

		usleep_range(100, 200);
	}

	// Read the message header and payload from the response queue.
	slot = rptr % num_entries;
	response_offset = slot * sizeof(struct arc_msg);
	if (csm_read32(bh, response_base + response_offset, &msg->header) != 0)
		return false;

	for (i = 0; i < 7; ++i) {
		u32 addr = response_base + response_offset + ((i + 1) * sizeof(u32));

		if (csm_read32(bh, addr, &msg->payload[i]) != 0)
			return false;
	}

	// Increment the response read pointer.
	rptr = (rptr + 1) % (2 * num_entries);
	if (csm_write32(bh, ARC_MSG_QUEUE_RES_RPTR(queue_base), rptr) != 0)
		return false;

	return true;
}

static bool send_arc_message(struct blackhole_device *bh, struct arc_msg *msg)
{
	u32 queue_ctrl_addr;
	u32 queue_base;
	u32 queue_info;
	u32 num_entries;

	queue_ctrl_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_MSG_QCB_PTR);

	if (csm_read32(bh, queue_ctrl_addr + 0, &queue_base) != 0)
		return false;

	if (csm_read32(bh, queue_ctrl_addr + 4, &queue_info) != 0)
		return false;

	num_entries = queue_info & 0xFF;

	if (!push_arc_msg(bh, msg, queue_base, num_entries))
		return false;

	// Trigger ARC interrupt
	noc_write32(bh, ARC_X, ARC_Y, ARC_MSI_FIFO, 0);

	if (!pop_arc_msg(bh, msg, queue_base, num_entries))
		return false;

	return msg->header == 0;
}

static bool blackhole_init(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	bh->tlb_regs = pci_iomap_range(bh->tt.pdev, 0, TLB_REGS_START, TLB_REGS_LEN);
	bh->kernel_tlb = pci_iomap_range(bh->tt.pdev, 0, KERNEL_TLB_START, KERNEL_TLB_LEN);
	bh->noc2axi_cfg = pci_iomap_range(bh->tt.pdev, 0, NOC2AXI_CFG_START, NOC2AXI_CFG_LEN);
	bh->bar2_mapping = pci_iomap(bh->tt.pdev, 2, 0);

	if (!bh->tlb_regs || !bh->kernel_tlb || !bh->noc2axi_cfg) {
		if (bh->tlb_regs)
			pci_iounmap(bh->tt.pdev, bh->tlb_regs);

		if (bh->kernel_tlb)
			pci_iounmap(bh->tt.pdev, bh->kernel_tlb);

		if (bh->noc2axi_cfg)
			pci_iounmap(bh->tt.pdev, bh->noc2axi_cfg);

		if (bh->bar2_mapping)
			pci_iounmap(bh->tt.pdev, bh->bar2_mapping);

		return false;
	}

	// Claim the topmost 2M window for kernel use.
	set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs);
	mutex_init(&bh->kernel_tlb_mutex);

	return true;
}

static bool blackhole_init_hardware(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;
	struct arc_msg msg = { 0 };

	pcie_set_readrq(pdev, MAX_MRRS);

	msg.header = ARC_MSG_TYPE_ASIC_STATE0;
	if (!send_arc_message(bh, &msg))
		dev_err(&tt_dev->pdev->dev, "Failed to send ARC message for A0 state\n");

	memset(&msg, 0, sizeof(msg));
	msg.header = ARC_MSG_TYPE_SET_WDT_TIMEOUT;
	msg.payload[0] = 1000 * auto_reset_timeout; // Convert seconds to milliseconds
	if (!send_arc_message(bh, &msg))
		dev_warn(&tt_dev->pdev->dev, "Failed to set ARC watchdog timeout (this is normal for old FW)\n");

	return true;
}

static bool blackhole_post_hardware_init(struct tenstorrent_device *tt_dev)
{
	telemetry_probe(tt_dev);
	return true;
}

static void blackhole_cleanup_hardware(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct arc_msg msg = { 0 };

	msg.header = ARC_MSG_TYPE_ASIC_STATE3;
	if (!send_arc_message(bh, &msg))
		dev_err(&tt_dev->dev, "Failed to send ARC message for A3 state\n");
}

static void blackhole_cleanup(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (bh->tlb_regs)
		pci_iounmap(tt_dev->pdev, bh->tlb_regs);
	if (bh->kernel_tlb)
		pci_iounmap(tt_dev->pdev, bh->kernel_tlb);
	if (bh->noc2axi_cfg)
		pci_iounmap(tt_dev->pdev, bh->noc2axi_cfg);
	if (bh->bar2_mapping)
		pci_iounmap(tt_dev->pdev, bh->bar2_mapping);

	kfree(bh->hwmon_attr_addrs);
	kfree(bh->sysfs_attr_addrs);
}

static int blackhole_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				   struct tenstorrent_noc_tlb_config *config)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (tlb >= 0 && tlb < TLB_2M_WINDOW_COUNT)
		return blackhole_configure_tlb_2M(bh, tlb, config);

	if (tlb >= TLB_2M_WINDOW_COUNT &&
	    tlb < TLB_2M_WINDOW_COUNT + TLB_4G_WINDOW_COUNT)
		return blackhole_configure_tlb_4G(bh, tlb, config);

	return -EINVAL;
}

static int blackhole_describe_tlb(struct tenstorrent_device *tt_dev, int tlb,
				  struct tlb_descriptor *desc)
{
	bool is_2M = tlb < TLB_2M_WINDOW_COUNT;

	if (tlb < 0 || tlb >= TLB_2M_WINDOW_COUNT + TLB_4G_WINDOW_COUNT)
		return -EINVAL;

	desc->bar = is_2M ? 0 : 4;
	desc->size = is_2M ? TLB_2M_WINDOW_SIZE : TLB_4G_WINDOW_SIZE;
	desc->bar_offset =
		is_2M ? tlb * TLB_2M_WINDOW_SIZE :
			(tlb - TLB_2M_WINDOW_COUNT) * TLB_4G_WINDOW_SIZE;

	return 0;
}

static int blackhole_configure_outbound_atu(struct tenstorrent_device *tt_dev, u32 region, u64 base, u64 limit,
					    u64 target)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u64 size = limit - base + 1;
	u32 region_ctrl_1 = 0;
	u32 region_ctrl_2 = REGION_EN;
	u32 region_ctrl_3 = 0;
	u32 lower_base = lower_32_bits(base);
	u32 upper_base = upper_32_bits(base);
	u32 lower_target = lower_32_bits(target);
	u32 upper_target = upper_32_bits(target);
	u32 lower_limit = lower_32_bits(limit);
	u32 upper_limit = upper_32_bits(limit);

	// iATU has a max region size of 4G.
	if (size > U32_MAX)
		return -EINVAL;

	if (region >= IATU_OUTBOUND_REGIONS)
		return -EINVAL;

	WRITE_IATU_REG(bh, OUTBOUND, region, LOWER_BASE_ADDR, lower_base);
	WRITE_IATU_REG(bh, OUTBOUND, region, UPPER_BASE_ADDR, upper_base);
	WRITE_IATU_REG(bh, OUTBOUND, region, LOWER_TARGET_ADDR, lower_target);
	WRITE_IATU_REG(bh, OUTBOUND, region, UPPER_TARGET_ADDR, upper_target);
	WRITE_IATU_REG(bh, OUTBOUND, region, LOWER_LIMIT_ADDR, lower_limit);
	WRITE_IATU_REG(bh, OUTBOUND, region, UPPER_LIMIT_ADDR, upper_limit);
	WRITE_IATU_REG(bh, OUTBOUND, region, REGION_CTRL_1, region_ctrl_1);
	WRITE_IATU_REG(bh, OUTBOUND, region, REGION_CTRL_2, region_ctrl_2);
	WRITE_IATU_REG(bh, OUTBOUND, region, REGION_CTRL_3, region_ctrl_3);

	return 0;
}

static void blackhole_create_sysfs_groups(struct tenstorrent_device *tt_dev) {
	int ret = devm_device_add_group(&tt_dev->dev, &bh_pcie_perf_counters_group);
	if (ret)
		dev_err(&tt_dev->dev, "PCIe perf counters unavailable: %d\n", ret);
}

struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	.dma_address_bits = 58,
	.noc_dma_limit = (1ULL << 58) - 1,
	.noc_pcie_offset = (4ULL << 58),
	.tlb_kinds = 2,
	.tlb_counts = { TLB_2M_WINDOW_COUNT, TLB_4G_WINDOW_COUNT },
	.tlb_sizes = { TLB_2M_WINDOW_SIZE, TLB_4G_WINDOW_SIZE },
	.init_device = blackhole_init,
	.init_hardware = blackhole_init_hardware,
	.post_hardware_init = blackhole_post_hardware_init,
	.cleanup_hardware = blackhole_cleanup_hardware,
	.cleanup_device = blackhole_cleanup,
	.configure_tlb = blackhole_configure_tlb,
	.describe_tlb = blackhole_describe_tlb,
	.save_reset_state = blackhole_save_reset_state,
	.restore_reset_state = blackhole_restore_reset_state,
	.configure_outbound_atu = blackhole_configure_outbound_atu,
	.create_sysfs_groups = blackhole_create_sysfs_groups,
};
