// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bsearch.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/log2.h>

#include "blackhole.h"
#include "chardev_private.h"
#include "memory.h"
#include "pcie.h"
#include "module.h"
#include "msgqueue.h"
#include "tlb.h"
#include "telemetry.h"
#include "uring_cmd.h"

#define MAX_MRRS 4096

#define TLB_2M_WINDOW_COUNT 202
#define TLB_2M_SHIFT 21
#define TLB_2M_WINDOW_SIZE (1 << TLB_2M_SHIFT)
#define TLB_2M_WINDOW_MASK (TLB_2M_WINDOW_SIZE - 1)

#define TLB_4G_WINDOW_COUNT 8	// NB: Not all are guaranteed to be exposed in BAR4; see blackhole_init().
#define TLB_4G_SHIFT 32
#define TLB_4G_WINDOW_SIZE (1UL << TLB_4G_SHIFT)
#define TLB_4G_WINDOW_MASK (TLB_4G_WINDOW_SIZE - 1)

#define TLB_REG_SIZE 12	// Same for 2M and 4G
#define TLB_TOTAL_WINDOW_COUNT (TLB_2M_WINDOW_COUNT + TLB_4G_WINDOW_COUNT)

#define TLB_REGS_START 0x1FC00000   // BAR0
#define TLB_REGS_LEN 0x00001000     // Covers all TLB registers

#define TLB_STRIDED_COUNT 32	// First 32 2M windows support non-rectangular multicast patterns
#define TLB_STRIDED_REG_SIZE 4
#define TLB_STRIDED_REGS_OFFSET (TLB_TOTAL_WINDOW_COUNT * TLB_REG_SIZE)

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

// Vector 0 stays with firmware (ARC); the rest let other sources such as the
// DMA engine raise their own interrupts.
#define MSI_VECTOR_COUNT 2
#define DMA_MSI_VECTOR 1
// Multiple Message Capable, within the MSI capability's first dword.
#define DBI_MSI_MULTIPLE_MESSAGE_CAPABLE ((u32)PCI_MSI_FLAGS_QMASK << 16)

// ARC owns telemetry
#define ARC_X 8
#define ARC_Y 0
#define RESET_SCRATCH(N) (0x80030400 + ((N) * 4))
#define ARC_TELEMETRY_PTR RESET_SCRATCH(13)
#define ARC_TELEMETRY_DATA RESET_SCRATCH(12)

// ARC FW has a messaging interface, see msgqueue.c in tt-zephyr-platforms.git
#define ARC_MSG_QCB_PTR RESET_SCRATCH(11) // Message Queue Control Block
#define ARC_MSI_FIFO 0x800B0000           // Write 0 to trigger the ARC message queue processor
#define ARC_MSG_READY_MS 500              // Wait this long for ARC to be ready for message queue operations
#define ARC_MSG_TYPE_ASIC_STATE0 0xA0
#define ARC_MSG_TYPE_ASIC_STATE3 0xA3
#define ARC_MSG_TYPE_SET_WDT_TIMEOUT 0xC1
#define ARC_MSG_TYPE_TRIGGER_RESET 0x56
#define ARC_MSG_TYPE_POWER_SETTING 0x21
#define ARC_MSG_TYPE_TEST 0x90
#define ARC_MSG_TYPE_FW_LOG 0xC7          // TT_SMC_MSG_TT_PCIE_LOG
#define ARC_BOOT_STATUS RESET_SCRATCH(2)
#define ARC_BOOT_STATUS_READY_FOR_MSG 0x1
#define ARC_ERROR_STATUS0 RESET_SCRATCH(4)
#define ARC_ERROR_STATUS0_CABLE_FAULT BIT(1)

// Sub-commands carried in byte 1 of the ARC_MSG_TYPE_FW_LOG header.
#define FW_LOG_SUBCMD_SETUP   0x1
#define FW_LOG_SUBCMD_RELEASE 0x2

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

// IATU_REGION_CTRL_1_OUTBOUND fields
#define INCREASE_REGION_SIZE (1 << 13)

// IATU_REGION_CTRL_2_OUTBOUND fields
#define REGION_EN (1 << 31)

#ifndef SZ_1T
#define SZ_1T 0x10000000000ULL
#endif

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

	// Strided TLB configuration is unsupported by the CONFIGURE_TLB API.
	// Write zero to clear any strided configuration set by alternate means.
	if (tlb < TLB_STRIDED_COUNT) {
		u8 __iomem *strided_reg = bh->tlb_regs + TLB_STRIDED_REGS_OFFSET + (tlb * TLB_STRIDED_REG_SIZE);
		iowrite32(0, strided_reg);
	}

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

static u8 __iomem *bh_configure_kernel_tlb(struct blackhole_device *bh, u32 x, u32 y, u64 addr, int noc)
{
	struct tenstorrent_noc_tlb_config config = { 0 };
	u64 offset = addr & TLB_2M_WINDOW_MASK;

	config.addr = addr & ~TLB_2M_WINDOW_MASK;
	config.x_end = x;
	config.y_end = y;
	config.ordering	= 1; // strict
	config.noc = noc;

	blackhole_configure_tlb_2M(bh, KERNEL_TLB_INDEX, &config);
	return bh->kernel_tlb + offset;
}

static int blackhole_noc_read(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, void *value, u32 width, int noc)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u8 __iomem *tlb_window;
	int ret = 0;

	mutex_lock(&bh->kernel_tlb_mutex);

	tlb_window = bh_configure_kernel_tlb(bh, x, y, addr, noc);

	switch (width) {
	case 1: *(u8  *)value = ioread8(tlb_window);  break;
	case 2: *(u16 *)value = ioread16(tlb_window); break;
	case 4: *(u32 *)value = ioread32(tlb_window); break;
	case 8: *(u64 *)value = ioread64(tlb_window); break;
	default: ret = -EINVAL; break;
	}

	mutex_unlock(&bh->kernel_tlb_mutex);

	return ret;
}

static int blackhole_noc_write(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, const void *value, u32 width,
			       int noc)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u8 __iomem *tlb_window;
	int ret = 0;

	mutex_lock(&bh->kernel_tlb_mutex);

	tlb_window = bh_configure_kernel_tlb(bh, x, y, addr, noc);

	switch (width) {
	case 1: iowrite8(*(const u8  *)value, tlb_window);  break;
	case 2: iowrite16(*(const u16 *)value, tlb_window); break;
	case 4: iowrite32(*(const u32 *)value, tlb_window); break;
	case 8: iowrite64(*(const u64 *)value, tlb_window); break;
	default: ret = -EINVAL; break;
	}

	mutex_unlock(&bh->kernel_tlb_mutex);

	return ret;
}

static u32 noc_read32(struct blackhole_device *bh, u32 x, u32 y, u64 addr, int noc)
{
	u32 val = 0;

	blackhole_noc_read(&bh->tt, x, y, addr, &val, sizeof(val), noc);

	return val;
}

static void blackhole_report_cable_fault(struct blackhole_device *bh)
{
	u32 error_status0 = noc_read32(bh, ARC_X, ARC_Y, ARC_ERROR_STATUS0, 0);

	if (error_status0 == U32_MAX)
		return;

	if (error_status0 & ARC_ERROR_STATUS0_CABLE_FAULT)
		dev_err(&bh->tt.pdev->dev,
			"Firmware reported a power cable fault (error status 0x%08x); "
			"shut down the system and check that the 12V-2x6 power cable is fully seated\n",
			error_status0);
}

static void noc_write32(struct blackhole_device *bh, u32 x, u32 y, u64 addr, u32 data, int noc)
{
	blackhole_noc_write(&bh->tt, x, y, addr, &data, sizeof(data), noc);
}

static int csm_read32(struct blackhole_device *bh, u64 addr, u32 *value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	*value = noc_read32(bh, ARC_X, ARC_Y, addr, 0);
	return 0;
}

static int csm_write32(struct blackhole_device *bh, u64 addr, u32 value)
{
	if (!is_range_within_csm(addr, sizeof(u32)))
		return -EINVAL;

	noc_write32(bh, ARC_X, ARC_Y, addr, value, 0);
	return 0;
}

static int blackhole_csm_read32(struct tenstorrent_device *tt_dev, u64 addr, u32 *value)
{
	return csm_read32(tt_dev_to_bh_dev(tt_dev), addr, value);
}

static int blackhole_csm_write32(struct tenstorrent_device *tt_dev, u64 addr, u32 value)
{
	return csm_write32(tt_dev_to_bh_dev(tt_dev), addr, value);
}

// BH has two PCIE instances, the function reads NOC ID to find out which one is active
static bool blackhole_detect_pcie_noc_x(struct blackhole_device *bh, u32 *noc_x) {
	*noc_x = ioread32(bh->noc2axi_cfg + NOC_ID_OFFSET) & 0x3F;
	return (*noc_x == 2 || *noc_x == 11);
}

// The device advertises one MSI vector after every reset, and that field is
// read-only from host config space, so raise it through DBI.
static bool blackhole_set_msi_capability(struct blackhole_device *bh, u32 x)
{
	u8 msi_cap = bh->tt.pdev->msi_cap;
	u64 addr = PCIE_DBI_ADDR + msi_cap;
	u32 cap;

	// A failed read would write garbage over the capability header.
	if (!msi_cap || blackhole_noc_read(&bh->tt, x, 0, addr, &cap, sizeof(cap), 0) != 0 ||
	    (cap & 0xFF) != PCI_CAP_ID_MSI)
		return false;

	cap &= ~DBI_MSI_MULTIPLE_MESSAGE_CAPABLE;
	cap |= FIELD_PREP(DBI_MSI_MULTIPLE_MESSAGE_CAPABLE, ilog2(MSI_VECTOR_COUNT));
	noc_write32(bh, x, 0, addr, cap, 0);
	return true;
}

static void blackhole_save_reset_state(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;
	u32 y = 0;
	u32 device_control;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return;

	device_control = noc_read32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, 0);
	bh->saved_mps = FIELD_GET(PCI_EXP_DEVCTL_PAYLOAD, device_control);
}

static void blackhole_restore_reset_state(struct tenstorrent_device *tt_dev) {
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;
	u32 y = 0;
	u32 device_control;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return;

	device_control = noc_read32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, 0);
	device_control &= ~PCI_EXP_DEVCTL_PAYLOAD;
	device_control |= FIELD_PREP(PCI_EXP_DEVCTL_PAYLOAD, bh->saved_mps);
	noc_write32(bh, x, y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, device_control, 0);

	// Reset reverts the capability while the host keeps the vectors it granted.
	blackhole_set_msi_capability(bh, x);
}

static unsigned int blackhole_prepare_msi_vectors(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 x;

	if (!blackhole_detect_pcie_noc_x(bh, &x))
		return 1;

	return blackhole_set_msi_capability(bh, x) ? MSI_VECTOR_COUNT : 1;
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

static const struct tt_hwmon_label bh_hwmon_labels[] = {
	{ "asic_temp", hwmon_temp,  hwmon_temp_label  },
	{ "vcore",     hwmon_in,    hwmon_in_label    },
	{ "current",   hwmon_curr,  hwmon_curr_label  },
	{ "power",     hwmon_power, hwmon_power_label },
	{ "fan_rpm",   hwmon_fan,   hwmon_fan_label   },
	{ NULL },	// sentinel
};

static const struct tt_hwmon_attr bh_hwmon_attrs[] = {
	{ TELEMETRY_ASIC_TEMP,          hwmon_temp,  hwmon_temp_input  },
	{ TELEMETRY_THM_LIMIT_THROTTLE, hwmon_temp,  hwmon_temp_max    },
	{ TELEMETRY_VCORE,              hwmon_in,    hwmon_in_input    },
	{ TELEMETRY_VDD_LIMITS,         hwmon_in,    hwmon_in_max      },
	{ TELEMETRY_CURRENT,            hwmon_curr,  hwmon_curr_input  },
	{ TELEMETRY_TDC_LIMIT_MAX,      hwmon_curr,  hwmon_curr_max    },
	{ TELEMETRY_POWER,              hwmon_power, hwmon_power_input },
	{ TELEMETRY_TDP_LIMIT_MAX,      hwmon_power, hwmon_power_max   },
	{ TELEMETRY_FAN_RPM,            hwmon_fan,   hwmon_fan_input   },
	{ 0 },	// sentinel
};

static struct tenstorrent_sysfs_attr bh_sysfs_attributes[] = {
	{ TELEMETRY_AICLK, __ATTR(tt_aiclk,  S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_AXICLK, __ATTR(tt_axiclk, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_ARCCLK, __ATTR(tt_arcclk, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_serial, S_IRUGO, tt_sysfs_show_u64_hex, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_card_type, S_IRUGO, tt_sysfs_show_card_type, NULL) },
	{ TELEMETRY_FLASH_BUNDLE_VERSION, __ATTR(tt_fw_bundle_ver, S_IRUGO, tt_sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_BM_APP_FW_VERSION, __ATTR(tt_m3app_fw_ver, S_IRUGO, tt_sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_ASIC_ID, __ATTR(tt_asic_id, S_IRUGO, tt_sysfs_show_u64_hex, NULL) },
	{ TELEMETRY_TIMER_HEARTBEAT, __ATTR(tt_heartbeat, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_THERM_TRIP_COUNT, __ATTR(tt_therm_trip_count, S_IRUGO, tt_sysfs_show_u32_dec, NULL) },
};

static const struct hwmon_channel_info *bh_hwmon_channel_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	NULL,
};

static const struct hwmon_chip_info bh_hwmon_chip_info = {
	.ops = &tt_hwmon_ops,
	.info = bh_hwmon_channel_info,
};

static int blackhole_read_telemetry_tag(struct tenstorrent_device *tt_dev, u64 address, u32 *value)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	return csm_read32(bh, address, value);
}

static int blackhole_populate_telemetry_cache(struct tenstorrent_device *tt_dev,
					      struct telem_cache_entry *cache,
					      u16 count)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 base_addr, data_addr;
	u32 version, major_ver, minor_ver, patch_ver;
	u32 tags_addr, num_entries, i;

	base_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_TELEMETRY_PTR, 0);
	data_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_TELEMETRY_DATA, 0);

	// The telemetry header is two u32s: a version, then an entry count.
	if (!is_range_within_csm(base_addr, 2 * sizeof(u32)) || !is_range_within_csm(data_addr, sizeof(u32))) {
		dev_err(&tt_dev->pdev->dev, "Telemetry not available\n");
		return -ENODEV;
	}

	version = noc_read32(bh, ARC_X, ARC_Y, base_addr, 0);
	major_ver = (version >> 16) & 0xFF;
	minor_ver = (version >> 8) & 0xFF;
	patch_ver = version & 0xFF;

	if (major_ver > 1) {
		dev_err(&tt_dev->pdev->dev, "Unsupported telemetry version %u.%u.%u\n", major_ver, minor_ver, patch_ver);
		return -ENOTSUPP;
	}

	tags_addr = base_addr + 8;
	num_entries = noc_read32(bh, ARC_X, ARC_Y, base_addr + 4, 0);

	if (num_entries > TELEMETRY_MAX_ENTRIES ||
	    !is_range_within_csm(tags_addr, num_entries * sizeof(u32))) {
		dev_err(&tt_dev->pdev->dev, "Bad telemetry table: %u entries at 0x%08X\n", num_entries, tags_addr);
		return -ENODEV;
	}

	for (i = 0; i < num_entries; i++) {
		u32 tag_entry = noc_read32(bh, ARC_X, ARC_Y, tags_addr + (i * 4), 0);
		u16 tag_id = tag_entry & 0xFFFF;
		u16 offset = (tag_entry >> 16) & 0xFFFF;
		u32 addr = data_addr + (offset * sizeof(u32));
		struct telem_cache_entry key = { .tag_id = tag_id };
		struct telem_cache_entry *entry;

		cond_resched();

		if (!is_range_within_csm(addr, sizeof(u32))) {
			dev_err(&tt_dev->pdev->dev, "Telemetry tag %u has invalid address 0x%08X\n", tag_id, addr);
			continue;
		}

		entry = bsearch(&key, cache, count, sizeof(*cache),
				telem_cache_entry_cmp);
		if (entry)
			entry->address = addr;
	}

	return 0;
}

static int blackhole_arc_msg_locate_queue(struct tenstorrent_device *tt_dev, u32 *queue_base, u32 *num_entries)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	u32 boot_status;
	u32 queue_ctrl_addr;
	u32 base;
	u32 queue_info;
	unsigned long timeout = jiffies + msecs_to_jiffies(ARC_MSG_READY_MS);

	do {
		boot_status = noc_read32(bh, ARC_X, ARC_Y, ARC_BOOT_STATUS, 0);
		if (boot_status == 0xFFFFFFFFu)
			return -EIO; // NOC is hung
		if (boot_status & ARC_BOOT_STATUS_READY_FOR_MSG)
			break;
	} while (time_before(jiffies, timeout));

	if (!(boot_status & ARC_BOOT_STATUS_READY_FOR_MSG))
		return -EIO;

	queue_ctrl_addr = noc_read32(bh, ARC_X, ARC_Y, ARC_MSG_QCB_PTR, 0);

	if (csm_read32(bh, queue_ctrl_addr + 0, &base) != 0)
		return -EIO;

	if (csm_read32(bh, queue_ctrl_addr + 4, &queue_info) != 0)
		return -EIO;

	*queue_base = base;
	*num_entries = queue_info & 0xFF;
	return 0;
}

static void blackhole_arc_msg_trigger(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	noc_write32(bh, ARC_X, ARC_Y, ARC_MSI_FIFO, 0, 0);
}

// The generic fwlog implementation invokes these shims for its control path.
// ARC message construction and arc_msg_send_sync() are Blackhole-specific;
// buffer management, parsing, output, and ownership handoff live in fwlog.c.
bool fw_log_hw_setup(void *hw_ctx, dma_addr_t buffer_dma, u32 buffer_size)
{
	struct blackhole_device *bh = hw_ctx;
	struct arc_msg msg = { 0 };

	msg.header = ARC_MSG_TYPE_FW_LOG | (FW_LOG_SUBCMD_SETUP << 8) |
		     (FW_LOG_PROTOCOL_VERSION << 16);
	msg.payload[0] = lower_32_bits(buffer_dma);
	msg.payload[1] = upper_32_bits(buffer_dma);
	msg.payload[2] = buffer_size;
	return arc_msg_send_sync(&bh->tt, &msg) == 0;
}

bool fw_log_hw_release(void *hw_ctx)
{
	struct blackhole_device *bh = hw_ctx;
	struct arc_msg msg = { 0 };

	msg.header = ARC_MSG_TYPE_FW_LOG | (FW_LOG_SUBCMD_RELEASE << 8);
	return arc_msg_send_sync(&bh->tt, &msg) == 0;
}

#if TT_URING_CMD_SUPPORTED

// HDMA read channel 0, which copies host memory to the device. Offsets in BAR2.
#define HDMA_RD_EN		0x100
#define HDMA_RD_DOORBELL	0x104
#define HDMA_RD_ELEM_PF		0x108
#define HDMA_RD_LLP		0x110
#define HDMA_RD_CYCLE		0x118
#define HDMA_RD_CONTROL1	0x134
#define HDMA_RD_QOS		0x13C
#define HDMA_RD_INT_STATUS	0x184
#define HDMA_RD_INT_SETUP	0x188
#define HDMA_RD_INT_CLEAR	0x18C
#define HDMA_RD_MSI_STOP	0x190
#define HDMA_RD_MSI_WATERMARK	0x198
#define HDMA_RD_MSI_ABORT	0x1A0
#define HDMA_RD_MSI_MSGD	0x1A8

#define HDMA_CONTROL1_LLEN	BIT(0)
#define HDMA_CYCLE_BIT		BIT(0)
#define HDMA_CYCLE_STATE	BIT(1)
#define HDMA_QOS_PF_DEPTH	GENMASK(25, 16)
#define HDMA_INT_STOP		BIT(0)
#define HDMA_INT_ABORT		BIT(2)
// INT_SETUP: local enables latch INT_STATUS, remote enables send the MSI, and
// the STOP/ABORT bits mask the engine's local interrupt pins.
#define HDMA_INT_REMOTE_STOP_EN	BIT(3)
#define HDMA_INT_LOCAL_STOP_EN	BIT(4)
#define HDMA_INT_REMOTE_ABORT_EN BIT(5)
#define HDMA_INT_LOCAL_ABORT_EN	BIT(6)
#define HDMA_ELEMENT_CB		BIT(0)

// The engine reaches 2M TLB window N at AXI address N * 2M.

// The list lives in host memory, so the engine fetches it over PCIe; prefetching
// hides that. QOS.PF_DEPTH resets to 1 and must be at least ELEM_PF, or the
// engine hangs, so it is raised while the queue is busy.
#define DMA_ELEMENT_PREFETCH	7
#define DMA_LIST_ELEMENTS	TENSTORRENT_DMA_WRITE_MAX_DESCS
// Prefetch may read up to 128 elements past the terminator.
#define DMA_LIST_BYTES		((DMA_LIST_ELEMENTS + 1 + 128) * sizeof(struct hdma_element))
// Base timeout per chain, plus 1 ms per MiB.
#define DMA_TIMEOUT_MS		2000

// A linked-list data element as the engine reads it.
struct hdma_element {
	u32 control;
	u32 size;
	u64 sar;
	u64 dar;
};

// One URING_CMD_DMA_WRITE command, from submission until its CQE is posted.
struct blackhole_dma_command {
	struct list_head node;	// In blackhole_dma.queue
	struct io_uring_cmd *cmd;
	struct chardev_private *priv;
	int result;
	u32 count;
	u32 next;		// First descriptor not fully copied
	u32 offset;		// Bytes of descs[next] already copied
	// Where next and offset move to once the running chain finishes; equal to
	// them while the command has nothing in the chain.
	u32 chain_next;
	u32 chain_offset;
	struct tenstorrent_dma_desc descs[];
};

static struct blackhole_dma_command **dma_command_slot(struct io_uring_cmd *cmd)
{
	BUILD_BUG_ON(sizeof(struct blackhole_dma_command *) > sizeof(cmd->pdu));
	return (struct blackhole_dma_command **)cmd->pdu;
}

static void dma_write_reg(struct blackhole_device *bh, u32 offset, u32 value)
{
	iowrite32(value, bh->bar2_mapping + offset);
}

static void dma_write_reg64(struct blackhole_device *bh, u32 offset, u64 value)
{
	dma_write_reg(bh, offset, lower_32_bits(value));
	dma_write_reg(bh, offset + 4, upper_32_bits(value));
}

static bool lock_mutex(struct mutex *mutex, bool nonblock)
{
	if (nonblock)
		return mutex_trylock(mutex);

	mutex_lock(mutex);
	return true;
}

// Bytes of desc from offset to the end of the 2M window that offset falls in:
// one element through one window.
static u32 dma_element_size(const struct tenstorrent_dma_desc *desc, u32 offset)
{
	u64 addr = desc->addr + offset;

	return min_t(u64, desc->size - offset, TLB_2M_WINDOW_SIZE - (addr & TLB_2M_WINDOW_MASK));
}

static bool is_same_target(const struct tenstorrent_noc_tlb_config *target,
			   const struct tenstorrent_dma_desc *desc, u64 addr)
{
	return target->addr == (addr & ~TLB_2M_WINDOW_MASK) &&
	       target->x_end == desc->x && target->y_end == desc->y && target->noc == desc->noc;
}

static void set_target(struct tenstorrent_noc_tlb_config *target,
		       const struct tenstorrent_dma_desc *desc, u64 addr)
{
	target->addr = addr & ~TLB_2M_WINDOW_MASK;
	target->x_end = desc->x;
	target->y_end = desc->y;
	target->noc = desc->noc;
}

// Reads what the channel needs from the device, and points the list window at
// the list, before a chain starts on an idle queue. Caller holds dma.lock.
static void blackhole_dma_setup(struct blackhole_device *bh)
{
	struct pci_dev *pdev = bh->tt.pdev;
	struct tenstorrent_noc_tlb_config config = { 0 };
	u64 list_noc = bh->tt.dev_class->noc_pcie_offset | bh->dma.list_iova;
	u32 addr_lo, addr_hi = 0;
	u16 flags, data;
	u32 pcie_x;

	pci_read_config_word(pdev, pdev->msi_cap + PCI_MSI_FLAGS, &flags);
	pci_read_config_dword(pdev, pdev->msi_cap + PCI_MSI_ADDRESS_LO, &addr_lo);
	if (flags & PCI_MSI_FLAGS_64BIT) {
		pci_read_config_dword(pdev, pdev->msi_cap + PCI_MSI_ADDRESS_HI, &addr_hi);
		pci_read_config_word(pdev, pdev->msi_cap + PCI_MSI_DATA_64, &data);
	} else {
		pci_read_config_word(pdev, pdev->msi_cap + PCI_MSI_DATA_32, &data);
	}

	// Multi-message MSI raises vector N for message data base + N.
	if (bh->tt.interrupt_vectors > DMA_MSI_VECTOR)
		data += DMA_MSI_VECTOR;

	bh->dma.msi_addr = ((u64)addr_hi << 32) | addr_lo;
	bh->dma.msi_data = data;

	bh->dma.saved_qos = ioread32(bh->bar2_mapping + HDMA_RD_QOS);
	if (FIELD_GET(HDMA_QOS_PF_DEPTH, bh->dma.saved_qos) < DMA_ELEMENT_PREFETCH)
		dma_write_reg(bh, HDMA_RD_QOS, (bh->dma.saved_qos & ~HDMA_QOS_PF_DEPTH) |
					       FIELD_PREP(HDMA_QOS_PF_DEPTH, DMA_ELEMENT_PREFETCH));

	// The engine fetches the list from its own PCIe tile, which forwards
	// (4 << 58) | IOVA to the host.
	if (!blackhole_detect_pcie_noc_x(bh, &pcie_x))
		pcie_x = 0;
	config.addr = list_noc & ~TLB_2M_WINDOW_MASK;
	config.x_end = pcie_x;
	blackhole_configure_tlb_2M(bh, bh->dma.list_window, &config);
	bh->dma.list_axi = (u64)bh->dma.list_window * TLB_2M_WINDOW_SIZE + (list_noc & TLB_2M_WINDOW_MASK);
}

// Writes as many queued descriptors into the list as it and the held windows
// allow, then starts the engine on them. Caller holds dma.lock and the queue is
// not empty.
static void blackhole_dma_start_chain(struct blackhole_device *bh)
{
	struct tenstorrent_noc_tlb_config targets[BLACKHOLE_DMA_MAX_WINDOWS] = { 0 };
	struct hdma_element *list = bh->dma.list;
	struct blackhole_dma_command *command;
	unsigned int used = 0;
	unsigned int n = 0;
	unsigned int w;
	u64 bytes = 0;
	unsigned long timeout;

	list_for_each_entry(command, &bh->dma.queue, node) {
		command->chain_next = command->next;
		command->chain_offset = command->offset;

		while (command->chain_next < command->count) {
			const struct tenstorrent_dma_desc *desc = &command->descs[command->chain_next];
			u64 addr = desc->addr + command->chain_offset;
			u32 size = dma_element_size(desc, command->chain_offset);

			for (w = 0; w < used && !is_same_target(&targets[w], desc, addr); w++)
				;

			if (n == DMA_LIST_ELEMENTS || w == bh->dma.window_count)
				goto start;

			if (w == used)
				set_target(&targets[used++], desc, addr);

			list[n].control = HDMA_ELEMENT_CB;
			list[n].size = size;
			list[n].sar = desc->src + command->chain_offset;
			list[n].dar = (u64)bh->dma.windows[w] * TLB_2M_WINDOW_SIZE + (addr & TLB_2M_WINDOW_MASK);
			bytes += size;
			n++;

			command->chain_offset += size;
			if (command->chain_offset == desc->size) {
				command->chain_next++;
				command->chain_offset = 0;
			}
		}
	}

start:
	// The engine stops at the first element whose cycle bit is not its own.
	list[n].control = 0;

	for (w = 0; w < used; w++)
		blackhole_configure_tlb_2M(bh, bh->dma.windows[w], &targets[w]);

	// The engine, not this CPU, uses the windows, so MMIO ordering does not
	// cover their configuration; a read back does.
	ioread32(bh->tlb_regs + bh->dma.list_window * TLB_REG_SIZE);

	// A stop or abort may have reset the channel, so program all of it.
	dma_write_reg(bh, HDMA_RD_EN, 1);
	dma_write_reg(bh, HDMA_RD_CONTROL1, HDMA_CONTROL1_LLEN);
	dma_write_reg(bh, HDMA_RD_ELEM_PF, DMA_ELEMENT_PREFETCH);
	dma_write_reg64(bh, HDMA_RD_MSI_STOP, bh->dma.msi_addr);
	dma_write_reg64(bh, HDMA_RD_MSI_WATERMARK, bh->dma.msi_addr);
	dma_write_reg64(bh, HDMA_RD_MSI_ABORT, bh->dma.msi_addr);
	dma_write_reg(bh, HDMA_RD_MSI_MSGD, bh->dma.msi_data);
	dma_write_reg(bh, HDMA_RD_INT_CLEAR, HDMA_INT_STOP | HDMA_INT_ABORT);
	dma_write_reg(bh, HDMA_RD_INT_SETUP,
		      HDMA_INT_LOCAL_STOP_EN | HDMA_INT_LOCAL_ABORT_EN |
		      HDMA_INT_REMOTE_STOP_EN | HDMA_INT_REMOTE_ABORT_EN |
		      HDMA_INT_STOP | HDMA_INT_ABORT);
	dma_write_reg64(bh, HDMA_RD_LLP, bh->dma.list_axi);
	dma_write_reg(bh, HDMA_RD_CYCLE, HDMA_CYCLE_STATE | HDMA_CYCLE_BIT);
	dma_write_reg(bh, HDMA_RD_DOORBELL, 1);

	// Allow at least 1 GB/s on top of the base timeout.
	timeout = msecs_to_jiffies(DMA_TIMEOUT_MS + (bytes >> 20));
	bh->dma.deadline = jiffies + timeout;
	mod_delayed_work(system_wq, &bh->dma.timeout, timeout);
}

static void blackhole_dma_finish(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
	struct blackhole_dma_command *command = *dma_command_slot(cmd);
	int result = command->result;

	kfree(command);
	tt_uring_cmd_done(cmd, result, issue_flags);
}

TT_URING_CMD_TASK_WORK(blackhole_dma_finish_tw, blackhole_dma_finish)

// Caller holds dma.lock.
static void blackhole_dma_complete(struct blackhole_dma_command *command, int result)
{
	list_del(&command->node);
	atomic_dec(&command->priv->dma_writes_inflight);
	command->result = result;

	// Posting the CQE has to happen in the submitter's task, not in IRQ.
	io_uring_cmd_complete_in_task(command->cmd, blackhole_dma_finish_tw);
}

// The engine finished or gave up on the running chain. Commands with
// descriptors in a failed chain fail as a whole. Caller holds dma.lock.
static void blackhole_dma_chain_done(struct blackhole_device *bh, int result)
{
	struct blackhole_dma_command *command, *tmp;

	// Disabling the channel clears an abort before the next start.
	if (result)
		dma_write_reg(bh, HDMA_RD_EN, 0);

	list_for_each_entry_safe(command, tmp, &bh->dma.queue, node) {
		if (command->chain_next == command->next && command->chain_offset == command->offset)
			break;

		command->next = command->chain_next;
		command->offset = command->chain_offset;
		if (result || command->next == command->count)
			blackhole_dma_complete(command, result);
	}

	if (!list_empty(&bh->dma.queue)) {
		blackhole_dma_start_chain(bh);
	} else {
		dma_write_reg(bh, HDMA_RD_QOS, bh->dma.saved_qos);
		schedule_work(&bh->dma.release_windows);
	}
}

static void blackhole_dma_interrupt(struct blackhole_device *bh)
{
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&bh->dma.lock, flags);

	status = ioread32(bh->bar2_mapping + HDMA_RD_INT_STATUS) & (HDMA_INT_STOP | HDMA_INT_ABORT);
	if (status) {
		dma_write_reg(bh, HDMA_RD_INT_CLEAR, status);

		// Nothing of ours ran if firmware drove the channel.
		if (!list_empty(&bh->dma.queue))
			blackhole_dma_chain_done(bh, status & HDMA_INT_ABORT ? -EIO : 0);
	}

	spin_unlock_irqrestore(&bh->dma.lock, flags);
}

static void blackhole_dma_timeout(struct work_struct *work)
{
	struct blackhole_device *bh = container_of(to_delayed_work(work), struct blackhole_device, dma.timeout);
	unsigned long flags;

	spin_lock_irqsave(&bh->dma.lock, flags);

	if (!list_empty(&bh->dma.queue)) {
		// The next chain may have started just before this ran.
		if (time_before(jiffies, bh->dma.deadline)) {
			mod_delayed_work(system_wq, &bh->dma.timeout, bh->dma.deadline - jiffies);
		} else {
			dev_warn(&bh->tt.pdev->dev, "DMA write timed out\n");
			blackhole_dma_chain_done(bh, -ETIMEDOUT);
		}
	}

	spin_unlock_irqrestore(&bh->dma.lock, flags);
}

// Returns every window to the pool once the queue is empty.
static void blackhole_dma_release_windows(struct work_struct *work)
{
	struct blackhole_device *bh = container_of(work, struct blackhole_device, dma.release_windows);
	int windows[BLACKHOLE_DMA_MAX_WINDOWS];
	unsigned int window_count = 0;
	int list_window = -1;
	unsigned long flags;
	unsigned int i;

	mutex_lock(&bh->dma.window_mutex);

	spin_lock_irqsave(&bh->dma.lock, flags);
	if (list_empty(&bh->dma.queue)) {
		// Register-mode DMA, as older UMDs drive it, needs linked-list mode off.
		if (bh->dma.list_window >= 0)
			dma_write_reg(bh, HDMA_RD_CONTROL1, 0);

		window_count = bh->dma.window_count;
		memcpy(windows, bh->dma.windows, sizeof(windows));
		list_window = bh->dma.list_window;
		bh->dma.window_count = 0;
		bh->dma.list_window = -1;
	}
	spin_unlock_irqrestore(&bh->dma.lock, flags);

	for (i = 0; i < window_count; i++)
		tenstorrent_device_free_tlb(&bh->tt, windows[i]);
	if (list_window >= 0)
		tenstorrent_device_free_tlb(&bh->tt, list_window);

	mutex_unlock(&bh->dma.window_mutex);
}

static void blackhole_dma_init(struct blackhole_device *bh)
{
	spin_lock_init(&bh->dma.lock);
	INIT_LIST_HEAD(&bh->dma.queue);
	mutex_init(&bh->dma.window_mutex);
	bh->dma.list_window = -1;
	INIT_WORK(&bh->dma.release_windows, blackhole_dma_release_windows);
	INIT_DELAYED_WORK(&bh->dma.timeout, blackhole_dma_timeout);

	// Without a list, DMA_WRITE fails with -ENOMEM.
	bh->dma.list = dma_alloc_coherent(&bh->tt.pdev->dev, DMA_LIST_BYTES, &bh->dma.list_iova, GFP_KERNEL);
}

static void blackhole_dma_cleanup(struct blackhole_device *bh)
{
	if (bh->dma.list)
		dma_free_coherent(&bh->tt.pdev->dev, DMA_LIST_BYTES, bh->dma.list, bh->dma.list_iova);
}

static int blackhole_dma_check_descs(struct chardev_private *priv,
				     const struct tenstorrent_dma_desc *descs, u32 count)
{
	u64 buf_start = 0, buf_len = 0;
	u32 i;

	lockdep_assert_held(&priv->mutex);

	for (i = 0; i < count; i++) {
		const struct tenstorrent_dma_desc *desc = &descs[i];

		// NOC coordinates are 6 bits wide in the TLB registers.
		if (desc->size == 0 || ((desc->src | desc->addr | desc->size) & 3) ||
		    desc->x > 63 || desc->y > 63 || desc->noc > 1 ||
		    memchr_inv(desc->reserved0, 0, sizeof(desc->reserved0)))
			return -EINVAL;

		if (desc->addr + desc->size < desc->addr)
			return -EINVAL;

		// Descriptors of a command mostly read one buffer, so search the
		// fd's buffers only when leaving the last one found.
		if (!tenstorrent_range_within(desc->src, desc->size, buf_start, buf_len) &&
		    !tenstorrent_find_dma_range(priv, desc->src, desc->size, &buf_start, &buf_len))
			return -EINVAL;
	}

	return 0;
}

// How many windows the command's descriptors target, up to the most the queue takes.
static unsigned int blackhole_dma_count_targets(const struct tenstorrent_dma_desc *descs, u32 count)
{
	struct tenstorrent_noc_tlb_config targets[BLACKHOLE_DMA_MAX_WINDOWS] = { 0 };
	unsigned int used = 0;
	unsigned int w;
	u32 i, offset;

	for (i = 0; i < count && used < BLACKHOLE_DMA_MAX_WINDOWS; i++) {
		for (offset = 0; offset < descs[i].size && used < BLACKHOLE_DMA_MAX_WINDOWS;
		     offset += dma_element_size(&descs[i], offset)) {
			u64 addr = descs[i].addr + offset;

			for (w = 0; w < used && !is_same_target(&targets[w], &descs[i], addr); w++)
				;

			if (w == used)
				set_target(&targets[used++], &descs[i], addr);
		}
	}

	return used;
}

// Takes the list window and up to wanted data windows; chains split across
// fewer windows when the pool runs short. Caller holds window_mutex.
static int blackhole_dma_take_windows(struct blackhole_device *bh, unsigned int wanted)
{
	unsigned long flags;
	int window;

	if (bh->dma.list_window < 0) {
		window = tenstorrent_device_allocate_tlb(&bh->tt, TLB_2M_WINDOW_SIZE);
		if (window < 0)
			return window == -ENOMEM ? -EBUSY : window;

		spin_lock_irqsave(&bh->dma.lock, flags);
		bh->dma.list_window = window;
		spin_unlock_irqrestore(&bh->dma.lock, flags);
	}

	while (bh->dma.window_count < wanted) {
		window = tenstorrent_device_allocate_tlb(&bh->tt, TLB_2M_WINDOW_SIZE);
		if (window < 0)
			break;

		spin_lock_irqsave(&bh->dma.lock, flags);
		bh->dma.windows[bh->dma.window_count++] = window;
		spin_unlock_irqrestore(&bh->dma.lock, flags);
	}

	return bh->dma.window_count ? 0 : -EBUSY;
}

static int blackhole_uring_cmd(struct chardev_private *priv, struct io_uring_cmd *cmd,
			       unsigned int issue_flags)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(priv->device);
	const struct tenstorrent_dma_write *write = tt_uring_cmd_payload(cmd, struct tenstorrent_dma_write);
	bool nonblock = issue_flags & IO_URING_F_NONBLOCK;
	u64 descs = READ_ONCE(write->descs);
	u32 count = READ_ONCE(write->count);
	struct blackhole_dma_command *command;
	unsigned long flags;
	int ret;

	// The engine can only report completion through MSI.
	if (cmd->cmd_op != TENSTORRENT_URING_CMD_DMA_WRITE || !bh->tt.pdev->msi_enabled)
		return -EOPNOTSUPP;

	if (READ_ONCE(write->flags) != 0 || count == 0 || count > TENSTORRENT_DMA_WRITE_MAX_DESCS)
		return -EINVAL;

	if (!bh->dma.list)
		return -ENOMEM;

	command = kmalloc(struct_size(command, descs, count), GFP_KERNEL);
	if (!command)
		return -ENOMEM;

	if (copy_from_user(command->descs, u64_to_user_ptr(descs), count * sizeof(command->descs[0]))) {
		ret = -EFAULT;
		goto out_free;
	}

	command->cmd = cmd;
	command->priv = priv;
	command->count = count;
	command->next = 0;
	command->offset = 0;
	command->chain_next = 0;
	command->chain_offset = 0;
	*dma_command_slot(cmd) = command;

	ret = -EAGAIN;
	if (!lock_mutex(&priv->mutex, nonblock))
		goto out_free;
	ret = blackhole_dma_check_descs(priv, command->descs, count);
	if (!ret)
		atomic_inc(&priv->dma_writes_inflight);
	mutex_unlock(&priv->mutex);
	if (ret)
		goto out_free;

	ret = -EAGAIN;
	if (!lock_mutex(&bh->dma.window_mutex, nonblock))
		goto out_inflight;

	ret = blackhole_dma_take_windows(bh, blackhole_dma_count_targets(command->descs, count));
	if (ret) {
		mutex_unlock(&bh->dma.window_mutex);
		goto out_inflight;
	}

	spin_lock_irqsave(&bh->dma.lock, flags);
	list_add_tail(&command->node, &bh->dma.queue);
	if (list_is_singular(&bh->dma.queue)) {
		blackhole_dma_setup(bh);
		blackhole_dma_start_chain(bh);
	}
	spin_unlock_irqrestore(&bh->dma.lock, flags);

	mutex_unlock(&bh->dma.window_mutex);
	return -EIOCBQUEUED;

out_inflight:
	atomic_dec(&priv->dma_writes_inflight);
out_free:
	kfree(command);
	return ret;
}

static void blackhole_quiesce_uring_cmds(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	unsigned long flags;

	spin_lock_irqsave(&bh->dma.lock, flags);
	if (!list_empty(&bh->dma.queue)) {
		dma_write_reg(bh, HDMA_RD_EN, 0);
		dma_write_reg(bh, HDMA_RD_QOS, bh->dma.saved_qos);
	}
	while (!list_empty(&bh->dma.queue))
		blackhole_dma_complete(list_first_entry(&bh->dma.queue, struct blackhole_dma_command, node),
				       -ENODEV);
	spin_unlock_irqrestore(&bh->dma.lock, flags);

	cancel_delayed_work_sync(&bh->dma.timeout);
	// Return the windows now rather than whenever the queued release runs.
	cancel_work_sync(&bh->dma.release_windows);
	blackhole_dma_release_windows(&bh->dma.release_windows);
}

#endif // TT_URING_CMD_SUPPORTED

static void blackhole_interrupt(struct tenstorrent_device *tt_dev, unsigned int vector)
{
#if TT_URING_CMD_SUPPORTED
	// Without a vector of its own, the engine's MSI arrives on vector 0.
	if (vector == DMA_MSI_VECTOR || tt_dev->interrupt_vectors <= DMA_MSI_VECTOR)
		blackhole_dma_interrupt(tt_dev_to_bh_dev(tt_dev));
#endif

	if (vector == 0)
		fw_log_interrupt(&tt_dev->fw_log);
}

static bool blackhole_reset(struct tenstorrent_device *tt_dev, u32 reset_flag)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;

	if (reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET) {
		struct arc_msg msg = { 0 };
		u16 reset_arg = 3; // Argument for ASIC + M3 reset

		msg.header = ARC_MSG_TYPE_TEST;
		if (arc_msg_send_sync(&bh->tt, &msg) != 0) {
			dev_warn(&tt_dev->pdev->dev, "Couldn't communicate with firmware; NOC is likely hung.\n");
			return false;
		}

		set_reset_marker(pdev);

		memset(&msg, 0, sizeof(msg));
		msg.header = ARC_MSG_TYPE_TRIGGER_RESET;
		msg.payload[0] = reset_arg;
		arc_msg_send_sync(&bh->tt, &msg);
		return true; // Possibly a lie...
	} else if (reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_RESET) {
		set_reset_marker(pdev);
		return pcie_timer_interrupt(pdev);
	}

	return false;
}

static bool blackhole_init(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct device *dev = &tt_dev->pdev->dev;
	resource_size_t bar4_len = pci_resource_len(tt_dev->pdev, 4);
	int i;

	// Limit 4G window count to what's available; partial windows not supported.
	tt_dev->tlb_counts[1] = bar4_len / TLB_4G_WINDOW_SIZE;

	tt_dev->telemetry_attrs = devm_kcalloc(dev, ARRAY_SIZE(bh_sysfs_attributes) + 1, sizeof(struct attribute *), GFP_KERNEL);

	if (!tt_dev->telemetry_attrs)
		return false;

	bh->tlb_regs = pci_iomap_range(bh->tt.pdev, 0, TLB_REGS_START, TLB_REGS_LEN);
	bh->kernel_tlb = pci_iomap_range(bh->tt.pdev, 0, KERNEL_TLB_START, KERNEL_TLB_LEN);
	bh->noc2axi_cfg = pci_iomap_range(bh->tt.pdev, 0, NOC2AXI_CFG_START, NOC2AXI_CFG_LEN);
	bh->bar2_mapping = pci_iomap(bh->tt.pdev, 2, 0);

	if (!bh->tlb_regs || !bh->kernel_tlb || !bh->noc2axi_cfg || !bh->bar2_mapping) {
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

#if TT_URING_CMD_SUPPORTED
	blackhole_dma_init(bh);
#endif

	// Initialize once and reserve the device-lifetime DMA buffer. The MSI
	// handler may schedule work after the firmware handshake, which happens
	// later in blackhole_init_hardware().
	fw_log_init(&tt_dev->fw_log, &tt_dev->pdev->dev, bh);

	tt_dev->hwmon_attributes = bh_hwmon_attrs;
	tt_dev->hwmon_labels = bh_hwmon_labels;
	tt_dev->telemetry_sysfs = bh_sysfs_attributes;
	tt_dev->telemetry_sysfs_count = ARRAY_SIZE(bh_sysfs_attributes);

	for (i = 0; i < ARRAY_SIZE(bh_sysfs_attributes); ++i)
		tt_dev->telemetry_attrs[i] = &bh_sysfs_attributes[i].attr.attr;
	tt_dev->telemetry_group.attrs = tt_dev->telemetry_attrs;
	tt_dev->telemetry_group.is_visible = tt_sysfs_telemetry_is_visible;

	return true;
}

static bool blackhole_init_hardware(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct pci_dev *pdev = tt_dev->pdev;
	struct arc_msg msg = { 0 };

	pcie_set_readrq(pdev, MAX_MRRS);

	msg.header = ARC_MSG_TYPE_ASIC_STATE0;
	if (arc_msg_send_sync(&bh->tt, &msg) != 0)
		dev_err(&tt_dev->pdev->dev, "Failed to send ARC message for A0 state\n");
	else
		blackhole_report_cable_fault(bh);

	memset(&msg, 0, sizeof(msg));
	msg.header = ARC_MSG_TYPE_SET_WDT_TIMEOUT;
	msg.payload[0] = 1000 * auto_reset_timeout; // Convert seconds to milliseconds
	if (arc_msg_send_sync(&bh->tt, &msg) != 0)
		dev_warn(&tt_dev->pdev->dev, "Failed to set ARC watchdog timeout (this is normal for old FW)\n");

	// Best-effort: start firmware log forwarding. Also re-runs on resume.
	fw_log_setup(&tt_dev->fw_log);

	return true;
}

static bool blackhole_init_telemetry(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	int r;

	r = device_add_group(&tt_dev->dev, &bh_pcie_perf_counters_group);
	if (r)
		dev_err(&tt_dev->pdev->dev, "PCIe perf counters unavailable: %d\n", r);
	else
		bh->pcie_perf_group_registered = true;

	r = tt_telemetry_probe(tt_dev);
	if (!r) {
		struct device *dev = &tt_dev->pdev->dev;
		struct device *hwmon_device;

		r = device_add_group(&tt_dev->dev, &tt_dev->telemetry_group);
		if (!r)
			bh->telemetry_group_registered = true;

		hwmon_device = hwmon_device_register_with_info(dev, "blackhole", tt_dev, &bh_hwmon_chip_info, NULL);
		if (IS_ERR(hwmon_device))
			return false;

		tt_dev->hwmon_dev = hwmon_device;

		// Notify udev that telemetry attributes are now available.
		kobject_uevent(&tt_dev->dev.kobj, KOBJ_CHANGE);
	}

	return true;
}

static void blackhole_cleanup_telemetry(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (tt_dev->hwmon_dev) {
		hwmon_device_unregister(tt_dev->hwmon_dev);
		tt_dev->hwmon_dev = NULL;
	}

	if (bh->telemetry_group_registered) {
		device_remove_group(&tt_dev->dev, &tt_dev->telemetry_group);
		bh->telemetry_group_registered = false;
	}

	if (bh->pcie_perf_group_registered) {
		device_remove_group(&tt_dev->dev, &bh_pcie_perf_counters_group);
		bh->pcie_perf_group_registered = false;
	}
}

static void blackhole_cleanup_hardware(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct arc_msg msg = { 0 };

	if (tt_dev->detached)
		return;

	// Suspend retains the DMA allocation and reuses it at resume. IRQs were
	// disabled by the common suspend path, so drain any batch that was queued
	// before firmware is told to stop; otherwise it could run concurrently
	// with fw_log_setup() clearing and reinitializing the shared buffer.
	fw_log_quiesce(&tt_dev->fw_log);

	// Tell firmware to stop writing the shared log buffer while the device
	// is still reachable. The buffer itself is freed later on the
	// always-run device cleanup path (blackhole_cleanup -> fw_log_free).
	fw_log_notify_release(&tt_dev->fw_log);

	msg.header = ARC_MSG_TYPE_ASIC_STATE3;
	if (arc_msg_send_sync(&bh->tt, &msg) != 0)
		dev_err(&tt_dev->pdev->dev, "Failed to send ARC message for A3 state\n");
}

static void blackhole_cleanup(struct tenstorrent_device *tt_dev)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	// Software teardown of firmware logging: disable, drain the workqueue,
	// and free the shared buffer. Runs unconditionally (no hardware
	// access) so the buffer is always released even on an unreachable
	// device. Firmware was asked to stop in blackhole_cleanup_hardware.
	fw_log_free(&tt_dev->fw_log);

#if TT_URING_CMD_SUPPORTED
	blackhole_dma_cleanup(bh);
#endif

	if (bh->tlb_regs)
		pci_iounmap(tt_dev->pdev, bh->tlb_regs);
	if (bh->kernel_tlb)
		pci_iounmap(tt_dev->pdev, bh->kernel_tlb);
	if (bh->noc2axi_cfg)
		pci_iounmap(tt_dev->pdev, bh->noc2axi_cfg);
	if (bh->bar2_mapping)
		pci_iounmap(tt_dev->pdev, bh->bar2_mapping);
}

static int blackhole_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				   struct tenstorrent_noc_tlb_config *config)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);

	if (tlb >= 0 && tlb < TLB_2M_WINDOW_COUNT)
		return blackhole_configure_tlb_2M(bh, tlb, config);

	if (tlb >= TLB_2M_WINDOW_COUNT && tlb < TLB_TOTAL_WINDOW_COUNT)
		return blackhole_configure_tlb_4G(bh, tlb, config);

	return -EINVAL;
}

static int blackhole_describe_tlb(struct tenstorrent_device *tt_dev, int tlb,
				  struct tlb_descriptor *desc)
{
	bool is_2M = tlb < TLB_2M_WINDOW_COUNT;

	if (tlb < 0 || tlb >= TLB_TOTAL_WINDOW_COUNT)
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
	u32 region_ctrl_1 = INCREASE_REGION_SIZE;
	u32 region_ctrl_2 = (limit == 0) ? 0 : REGION_EN;
	u32 region_ctrl_3 = 0;
	u32 lower_base = lower_32_bits(base);
	u32 upper_base = upper_32_bits(base);
	u32 lower_target = lower_32_bits(target);
	u32 upper_target = upper_32_bits(target);
	u32 lower_limit = lower_32_bits(limit);
	u32 upper_limit = upper_32_bits(limit);

	// iATU has a max region size of 1T.
	if (size > SZ_1T)
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

static void blackhole_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, u32 data, int noc)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	noc_write32(bh, x, y, addr, data, noc);
}

static int blackhole_set_power_state(struct tenstorrent_device *tt_dev, struct tenstorrent_power_state *power_state)
{
	struct blackhole_device *bh = tt_dev_to_bh_dev(tt_dev);
	struct arc_msg msg = {0};

	msg.header = ARC_MSG_TYPE_POWER_SETTING | (power_state->validity << 8) | (power_state->power_flags << 16);
	BUILD_BUG_ON(sizeof(power_state->power_settings) != sizeof(msg.payload));
	memcpy(msg.payload, power_state->power_settings, sizeof(msg.payload));

	return arc_msg_send_sync(&bh->tt, &msg);
}

struct tenstorrent_device_class blackhole_class = {
	.name = "Blackhole",
	.instance_size = sizeof(struct blackhole_device),
	// An IOVA can be used directly as a NOC address, and the upper 6 bits of
	// a 64-bit NOC-outbound address select the PCIe outbound TLB window. An
	// IOVA wider than 58 bits would select the wrong window and silently
	// misdirect the DMA. Same limit for both masks.
	.coherent_dma_bits = 58,
	.streaming_dma_bits = 58,
	.noc_dma_limit = (1ULL << 58) - 1,
	.noc_pcie_offset = (4ULL << 58),
	.tlb_kinds = 2,
	.tlb_counts = { TLB_2M_WINDOW_COUNT, TLB_4G_WINDOW_COUNT },
	.tlb_sizes = { TLB_2M_WINDOW_SIZE, TLB_4G_WINDOW_SIZE },
	.reset = blackhole_reset,
	.init_device = blackhole_init,
	.init_hardware = blackhole_init_hardware,
	.init_telemetry = blackhole_init_telemetry,
	.cleanup_telemetry = blackhole_cleanup_telemetry,
	.read_telemetry_tag = blackhole_read_telemetry_tag,
	.populate_telemetry_cache = blackhole_populate_telemetry_cache,
	.probe_telemetry = tt_telemetry_probe,
	.cleanup_hardware = blackhole_cleanup_hardware,
	.cleanup_device = blackhole_cleanup,
	.configure_tlb = blackhole_configure_tlb,
	.describe_tlb = blackhole_describe_tlb,
	.save_reset_state = blackhole_save_reset_state,
	.restore_reset_state = blackhole_restore_reset_state,
	.configure_outbound_atu = blackhole_configure_outbound_atu,
	.noc_write32 = blackhole_noc_write32,
	.interrupt = blackhole_interrupt,
	.prepare_msi_vectors = blackhole_prepare_msi_vectors,
#if TT_URING_CMD_SUPPORTED
	.uring_cmd = blackhole_uring_cmd,
	.quiesce_uring_cmds = blackhole_quiesce_uring_cmds,
#endif
	.noc_read = blackhole_noc_read,
	.noc_write = blackhole_noc_write,
	.csm_read32 = blackhole_csm_read32,
	.csm_write32 = blackhole_csm_write32,
	.arc_msg_locate_queue = blackhole_arc_msg_locate_queue,
	.arc_msg_trigger = blackhole_arc_msg_trigger,
	.set_power_state = blackhole_set_power_state,
};
