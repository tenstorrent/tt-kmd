// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/delay.h>
#include <linux/pci.h>

#include "wormhole.h"
#include "pcie.h"
#include "module.h"
#include "hwmon.h"
#include "tlb.h"
#include "telemetry.h"
#include "pcie.h"
#include "enumerate.h"

#define TLB_1M_WINDOW_COUNT 156
#define TLB_1M_SHIFT 20
#define TLB_1M_WINDOW_SIZE (1 << TLB_1M_SHIFT)
#define TLB_1M_WINDOW_BASE 0   // BAR0

#define TLB_2M_WINDOW_COUNT 10
#define TLB_2M_SHIFT 21
#define TLB_2M_WINDOW_SIZE (1 << TLB_2M_SHIFT)
#define TLB_2M_WINDOW_BASE (TLB_1M_WINDOW_COUNT * TLB_1M_WINDOW_SIZE)

#define TLB_16M_WINDOW_COUNT 20
#define TLB_16M_SHIFT 24
#define TLB_16M_WINDOW_SIZE (1 << TLB_16M_SHIFT)
#define TLB_16M_WINDOW_BASE (TLB_2M_WINDOW_BASE + TLB_2M_WINDOW_COUNT * TLB_2M_WINDOW_SIZE)
#define TLB_16M_WINDOW_MASK (TLB_16M_WINDOW_SIZE - 1)

#define TLB_WINDOW_COUNT (TLB_1M_WINDOW_COUNT + TLB_2M_WINDOW_COUNT + TLB_16M_WINDOW_COUNT)
#define WH_NOC_BITS 36

#define WH_FW_MSG_PCIE_INDEX 0x51
#define WH_FW_MSG_ASTATE0 0xA0
#define WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT 0xBC
#define WH_FW_MSG_TRIGGER_RESET 0x56
#define WH_FW_MSG_NOP 0x11

// The iATU can be used to match & remap PCIE transactions.
#define IATU_BASE 0x1200	// Relative to the start of BAR2
#define IATU_OUTBOUND 0
#define IATU_INBOUND 1
#define IATU_OUTBOUND_REGIONS 16
#define IATU_REGION_STRIDE 0x100
#define IATU_REGION_CTRL_1_INBOUND	0x00
#define IATU_REGION_CTRL_2_INBOUND	0x04
#define IATU_LOWER_TARGET_ADDR_INBOUND	0x14
#define IATU_UPPER_TARGET_ADDR_INBOUND	0x18
#define IATU_REGION_CTRL_1_OUTBOUND 0x00
#define IATU_REGION_CTRL_2_OUTBOUND 0x04
#define IATU_LOWER_BASE_ADDR_OUTBOUND 0x08
#define IATU_UPPER_BASE_ADDR_OUTBOUND 0x0C
#define IATU_LIMIT_ADDR_OUTBOUND 0x10
#define IATU_LOWER_TARGET_ADDR_OUTBOUND 0x14
#define IATU_UPPER_TARGET_ADDR_OUTBOUND 0x18

// IATU_REGION_CTRL_2_INBOUND fields
#define REGION_EN (1 << 31)
#define BAR_MATCH_MODE (1 << 30)
#define FUZZY_TYPE_MATCH (1 << 27) // MRd, MWr, MRdLk are considered the same.
#define BAR_NUM(n) ((n) << 8)

// IATU_REGION_CTRL_2_OUTBOUND fields
#define DMA_BYPASS (1 << 27)
#define TLP_BYPASS (1 << 21)
#define FUNC_BYPASS (1 << 19)

// BAR4 is 32MB and will be mapped to the system registers from 0x1E000000
// to 0x20000000.
#define BAR4_SOC_TARGET_ADDRESS 0x1E000000

#define RESET_UNIT_START (0x1FF30000 - BAR4_SOC_TARGET_ADDRESS)
#define ARC_CSM_START    (0x1FE80000 - BAR4_SOC_TARGET_ADDRESS)
#define TLB_REGS_START   (0x1FC00000 - BAR4_SOC_TARGET_ADDRESS)
#define NOC2AXI_START    (0x1FD02000 - BAR4_SOC_TARGET_ADDRESS)

#define ARC_TELEMETRY_PTR  (RESET_UNIT_START + 0x01D0)
#define ARC_TELEMETRY_DATA (RESET_UNIT_START + 0x01D4)

// kernel TLB is the last 16MB TLB
#define KERNEL_TLB_INDEX (TLB_WINDOW_COUNT - 1)
#define KERNEL_TLB_START (0x1E000000 - BAR4_SOC_TARGET_ADDRESS)

#define SCRATCH_REG(n) (0x60 + (n)*sizeof(u32))	/* byte offset */

#define PCIE_DBI_ADDR 0x800000000ULL
#define PCIE_NOC_X 0
#define PCIE_NOC_Y 3
#define PCIE_NOC1_X 9
#define PCIE_NOC1_Y 8
#define DBI_ENABLE 0x00200000
#define PCIE_ARMISC_INFO_REG SCRATCH_REG(6)
#define PCIE_AWMISC_INFO_REG SCRATCH_REG(7)

#define POST_CODE_REG SCRATCH_REG(0)
#define POST_CODE_ARC_L2 0xC0DE0000
#define POST_CODE_ARC_L2_MASK 0xFFFF0000

#define ARC_MISC_CNTL_REG 0x100
#define ARC_MISC_CNTL_IRQ0_MASK (1 << 16)

// Scratch register 5 is used for the firmware message protocol.
// Write 0xAA00 | message_id into scratch register 5, wait for message_id to appear.
// After reading the message, the firmware will immediately reset SR5 to 0 and write message_id when done.
// Appearance of any other value indicates a conflict with another message.
#define WH_FW_MESSAGE_PRESENT 0xAA00

#define WH_FW_MSG_ASTATE0 0xA0
#define WH_FW_MSG_ASTATE3 0xA3
#define WH_FW_MSG_CURR_DATE 0xB7
#define WH_FW_MSG_GET_TELEMETRY_OFFSET 0x2C

#define WRITE_IATU_REG(wh_dev, direction, region, reg, value) \
	write_iatu_reg(wh_dev, IATU_##direction, region, \
		       IATU_##reg##_##direction, (value))

static u32 noc_read32(struct wormhole_device *wh, u32 x, u32 y, u64 addr, int noc);

static bool is_hardware_hung(struct pci_dev *pdev, u8 __iomem *reset_unit_regs)
{
	u16 vendor_id;

	if (pdev != NULL && (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL ||
			     vendor_id != PCI_VENDOR_ID_TENSTORRENT))
		return true;

	return (ioread32(reset_unit_regs + SCRATCH_REG(6)) == 0xFFFFFFFF);
}

static int arc_msg_poll_completion(u8 __iomem *reset_unit_regs, u8 __iomem *msg_reg, u32 msg_code, u32 timeout_us,
				   u16 *exit_code)
{
	// Scale poll_period for around 100 polls, and at least 10 us
	u32 poll_period_us = max((u32)10, timeout_us / 100);

	ktime_t end_time = ktime_add_us(ktime_get(), timeout_us);

	while (true) {
		u32 read_val = ioread32(msg_reg);

		if ((read_val & 0xffff) == msg_code) {
			if (exit_code)
				*exit_code = read_val >> 16;
			return 0;
		}

		if (read_val == 0xFFFFFFFFu && is_hardware_hung(NULL, reset_unit_regs)) {
			pr_debug("Tenstorrent Device is hung executing message: %08X.", msg_code);
			return -3;
		}

		if (read_val == 0xFFFFFFFFu) {
			pr_debug("Tenstorrent FW message unrecognized: %08X.", msg_code);
			return -2;
		}

		if (ktime_after(ktime_get(), end_time)) {
			pr_debug("Tenstorrent FW message timeout: %08X.", msg_code);
			return -1;
		}

		usleep_range(poll_period_us, 2 * poll_period_us);
	}
}

static bool arc_l2_is_running(u8 __iomem *reset_unit_regs)
{
	u32 post_code = ioread32(reset_unit_regs + POST_CODE_REG);
	return ((post_code & POST_CODE_ARC_L2_MASK) == POST_CODE_ARC_L2);
}

bool wormhole_send_arc_fw_message_with_args(u8 __iomem *reset_unit_regs, u8 message_id, u16 arg0, u16 arg1,
					    u32 timeout_us, u16 *exit_code)
{
	void __iomem *args_reg = reset_unit_regs + SCRATCH_REG(3);
	void __iomem *message_reg = reset_unit_regs + SCRATCH_REG(5);
	void __iomem *arc_misc_cntl_reg = reset_unit_regs + ARC_MISC_CNTL_REG;
	u32 args = arg0 | ((u32)arg1 << 16);
	u32 arc_misc_cntl;

	if (!arc_l2_is_running(reset_unit_regs)) {
		pr_warn("Skipping message %08X due to FW not running.\n", (unsigned int)message_id);
		return false;
	}

	iowrite32(args, args_reg);
	iowrite32(WH_FW_MESSAGE_PRESENT | message_id, message_reg);

	// Trigger IRQ to ARC
	arc_misc_cntl = ioread32(arc_misc_cntl_reg);
	iowrite32(arc_misc_cntl | ARC_MISC_CNTL_IRQ0_MASK, arc_misc_cntl_reg);

	if (timeout_us == 0)
		return false;

	if (arc_msg_poll_completion(reset_unit_regs, message_reg, message_id, timeout_us, exit_code) < 0) {
		return false;
	} else {
		return true;
	}
}

static bool wormhole_send_arc_fw_message(u8 __iomem *reset_unit_regs, u8 message_id, u32 timeout_us, u16 *exit_code)
{
	return wormhole_send_arc_fw_message_with_args(reset_unit_regs, message_id, 0, 0, timeout_us, exit_code);
}

static bool wormhole_read_fw_telemetry_offset(u8 __iomem *reset_unit_regs, u32 *offset)
{
	u8 __iomem *arc_return_reg = reset_unit_regs + SCRATCH_REG(3);

	if (!wormhole_send_arc_fw_message(reset_unit_regs, WH_FW_MSG_GET_TELEMETRY_OFFSET, 10000, NULL))
		return false;

	*offset = ioread32(arc_return_reg);

	return true;
}

static bool wormhole_shutdown_firmware(struct pci_dev *pdev, u8 __iomem *reset_unit_regs)
{
	if (is_hardware_hung(pdev, reset_unit_regs))
		return false;

	if (!wormhole_send_arc_fw_message(reset_unit_regs, WH_FW_MSG_ASTATE3, 10000, NULL))
		return false;
	return true;
}

static void month_lookup(u32 days_into_year, u32 *day, u32 *month)
{
	static const u8 days_in_month[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	u32 i;

	u32 d_tmp = days_into_year;

	for (i = 0; i < ARRAY_SIZE(days_in_month); i++) {
		if (d_tmp < days_in_month[i])
			break;

		d_tmp -= days_in_month[i];
	}

	*day = d_tmp;
	*month = i;
}

static void wormhole_send_curr_date(u8 __iomem *reset_unit_regs)
{
	const u32 SECONDS_TO_2020 = 1577836800; // date -d "Jan 1, 2020 UTC" +%s
	const u32 DAYS_PER_FOUR_YEARS = 4 * 365 + 1;
	const u32 DAYS_TO_FEB_29 = 31 + 28;
	const u32 SECONDS_PER_DAY = 86400;

	u32 day, month;
	u32 days_into_year;
	u32 Y, M, DD, HH, MM, packed_datetime_low, packed_datetime_high;

	u32 seconds_since_2020 = ktime_get_real_seconds() - SECONDS_TO_2020;

	u32 seconds_into_day = seconds_since_2020 % SECONDS_PER_DAY;
	u32 days_since_2020 = seconds_since_2020 / SECONDS_PER_DAY;

	u32 four_years = days_since_2020 / DAYS_PER_FOUR_YEARS;
	u32 days_into_four_years = days_since_2020 % DAYS_PER_FOUR_YEARS;

	bool leap_day = (days_into_four_years == DAYS_TO_FEB_29);
	days_into_four_years -= (days_into_four_years >= DAYS_TO_FEB_29);
	days_into_year = days_into_four_years % 365;

	month_lookup(days_into_year, &day, &month);

	day += leap_day;

	Y = 4 * four_years + days_into_four_years / 365;
	M = month + 1;
	DD = day + 1;

	HH = seconds_into_day / 3600;
	MM = seconds_into_day / 60 % 60;

	packed_datetime_low = (HH << 8) | MM;
	packed_datetime_high = (Y << 12) | (M << 8) | DD;

	wormhole_send_arc_fw_message_with_args(reset_unit_regs, WH_FW_MSG_CURR_DATE, packed_datetime_low,
					       packed_datetime_high, 1000, NULL);
}

static void write_iatu_reg(struct wormhole_device *wh_dev, unsigned direction,
			   unsigned region, unsigned reg, u32 value) {
	u32 offset = IATU_BASE + (2 * region + direction) * IATU_REGION_STRIDE
		   + reg;

	iowrite32(value, wh_dev->bar2_mapping + offset);
}

static ssize_t sysfs_show_u32_dec(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t sysfs_show_u64_hex(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t sysfs_show_u32_ver(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t sysfs_show_card_type(struct device *dev, struct device_attribute *attr, char *buf);
static umode_t sysfs_telemetry_is_visible(struct kobject *kobj, struct attribute *attr, int n);

static struct tenstorrent_sysfs_attr wh_sysfs_attributes[] = {
	{ TELEMETRY_AICLK, __ATTR(tt_aiclk,  S_IRUGO, sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_AXICLK, __ATTR(tt_axiclk, S_IRUGO, sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_ARCCLK, __ATTR(tt_arcclk, S_IRUGO, sysfs_show_u32_dec, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_serial, S_IRUGO, sysfs_show_u64_hex, NULL) },
	{ TELEMETRY_BOARD_ID, __ATTR(tt_card_type, S_IRUGO, sysfs_show_card_type, NULL) },
	{ TELEMETRY_FLASH_BUNDLE_VERSION, __ATTR(tt_fw_bundle_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_BM_APP_FW_VERSION, __ATTR(tt_m3app_fw_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_TT_FLASH_VERSION, __ATTR(tt_ttflash_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_BM_BL_FW_VERSION, __ATTR(tt_m3bl_fw_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_CM_FW_VERSION, __ATTR(tt_arc_fw_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_ETH_FW_VERSION, __ATTR(tt_eth_fw_ver, S_IRUGO, sysfs_show_u32_ver, NULL) },
	{ TELEMETRY_ASIC_ID, __ATTR(tt_asic_id, S_IRUGO, sysfs_show_u64_hex, NULL) },
};

static ssize_t sysfs_show_u32_dec(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - wh_sysfs_attributes;
	u64 offset = wh->sysfs_attr_offsets[i];
	u32 value = 0;

	value = ioread32(wh->bar4_mapping + offset);
	return snprintf(buf, PAGE_SIZE, "%u\n", value);
}

static ssize_t sysfs_show_u64_hex(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - wh_sysfs_attributes;
	u64 offset = wh->sysfs_attr_offsets[i];
	u32 hi, lo;

	hi = ioread32(wh->bar4_mapping + offset);
	lo = ioread32(wh->bar4_mapping + offset + 4);
	return scnprintf(buf, PAGE_SIZE, "%08X%08X\n", hi, lo);
}

static ssize_t sysfs_show_u32_ver(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - wh_sysfs_attributes;
	u64 offset = wh->sysfs_attr_offsets[i];
	u32 value = 0;
	u32 major, minor, patch, ver;

	value = ioread32(wh->bar4_mapping + offset);

	// HACK: preserve behavior for eth_fw_ver.
	if (data->tag_id == TELEMETRY_ETH_FW_VERSION) {
		major = (value >> 16) & 0xFF;
		minor = (value >> 12) & 0xF;
		patch = (value >>  0) & 0xFFF;
		return scnprintf(buf, PAGE_SIZE, "%u.%u.%u\n", major, minor, patch);
	}

	major = (value >> 24) & 0xFF;
	minor = (value >> 16) & 0xFF;
	patch = (value >>  8) & 0xFF;
	ver = value & 0xFF;

	return scnprintf(buf, PAGE_SIZE, "%u.%u.%u.%u\n", major, minor, patch, ver);
}

static ssize_t sysfs_show_card_type(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	struct tenstorrent_sysfs_attr *data = container_of(attr, struct tenstorrent_sysfs_attr, attr);
	unsigned i = data - wh_sysfs_attributes;
	u64 offset = wh->sysfs_attr_offsets[i];
	u32 value;
	u16 card_type;
	char *card_name;

	value = ioread32(wh->bar4_mapping + offset);
	card_type = (value >> 4) & 0xFFFF;
	switch (card_type) {
	case 0x14: card_name = "n300"; break;
	case 0x18: card_name = "n150"; break;
	case 0x35: card_name = "galaxy-wormhole"; break;
	default: card_name = "unknown"; break;
	}

	return scnprintf(buf, PAGE_SIZE, "%s\n", card_name);
}

static umode_t sysfs_telemetry_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct device_attribute *dev_attr = container_of(attr, struct device_attribute, attr);
	struct tenstorrent_sysfs_attr *ts_attr = container_of(dev_attr, struct tenstorrent_sysfs_attr, attr);
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	unsigned i = ts_attr - wh_sysfs_attributes;
	bool visible = wh->sysfs_attr_offsets[i] != 0;

	return visible ? attr->mode : 0;
}


#define NIU_COUNTERS_START (NOC2AXI_START + 0x200)
#define NIU_NOC1_OFFSET 0x8000

static ssize_t wh_show_pcie_single_counter(struct device *dev, char *buf, u32 counter_offset, int noc)
{
	struct tenstorrent_device *tt_dev = dev_get_drvdata(dev);
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	u8 __iomem *noc2axi = wh_dev->bar4_mapping + NIU_COUNTERS_START;
	u64 addr = (4 * counter_offset) + (noc * NIU_NOC1_OFFSET);
	u32 value = ioread32(noc2axi + addr);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

// The pair are for NOC0 and NOC1; counters for each NOC exposed separately.
#define WH_PCIE_COUNTER_ATTR_RO_PAIR(_base_name_str, _counter_type_id_const) \
static ssize_t _base_name_str##0_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	return wh_show_pcie_single_counter(dev, buf, _counter_type_id_const, 0); \
} \
static ssize_t _base_name_str##1_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	return wh_show_pcie_single_counter(dev, buf, _counter_type_id_const, 1); \
} \
static DEVICE_ATTR_RO(_base_name_str##0); \
static DEVICE_ATTR_RO(_base_name_str##1)

#define SLV_POSTED_WR_DATA_WORD_RECEIVED 0x39
#define SLV_NONPOSTED_WR_DATA_WORD_RECEIVED 0x38
#define SLV_RD_DATA_WORD_SENT 0x33
#define MST_POSTED_WR_DATA_WORD_SENT 0x9
#define MST_NONPOSTED_WR_DATA_WORD_SENT 0x8
#define MST_RD_DATA_WORD_RECEIVED 0x3

WH_PCIE_COUNTER_ATTR_RO_PAIR(slv_posted_wr_data_word_received, SLV_POSTED_WR_DATA_WORD_RECEIVED);
WH_PCIE_COUNTER_ATTR_RO_PAIR(slv_nonposted_wr_data_word_received, SLV_NONPOSTED_WR_DATA_WORD_RECEIVED);
WH_PCIE_COUNTER_ATTR_RO_PAIR(slv_rd_data_word_sent, SLV_RD_DATA_WORD_SENT);
WH_PCIE_COUNTER_ATTR_RO_PAIR(mst_posted_wr_data_word_sent, MST_POSTED_WR_DATA_WORD_SENT);
WH_PCIE_COUNTER_ATTR_RO_PAIR(mst_nonposted_wr_data_word_sent, MST_NONPOSTED_WR_DATA_WORD_SENT);
WH_PCIE_COUNTER_ATTR_RO_PAIR(mst_rd_data_word_received, MST_RD_DATA_WORD_RECEIVED);

#define ATTR_LIST(name) (&dev_attr_##name.attr)

static struct attribute *wh_pcie_perf_counters_attrs[] = {
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

static const struct attribute_group wh_pcie_perf_counters_group = {
	.name = "pcie_perf_counters",
	.attrs = wh_pcie_perf_counters_attrs,
};

static u32 wh_arc_addr_to_sysreg(u32 arc_addr)
{
	return ARC_CSM_START + (arc_addr - ARC_CSM_BASE);
}

// Program the iATU so that BAR4 is directed to the system registers.
static void map_bar4_to_system_registers(struct wormhole_device *wh_dev) {
	u32 region_ctrl_2 = REGION_EN | BAR_MATCH_MODE | FUZZY_TYPE_MATCH | BAR_NUM(4);

	WRITE_IATU_REG(wh_dev, INBOUND, 1, LOWER_TARGET_ADDR, BAR4_SOC_TARGET_ADDRESS);
	WRITE_IATU_REG(wh_dev, INBOUND, 1, UPPER_TARGET_ADDR, 0);

	WRITE_IATU_REG(wh_dev, INBOUND, 1, REGION_CTRL_1, 0);
	WRITE_IATU_REG(wh_dev, INBOUND, 1, REGION_CTRL_2, region_ctrl_2);
}

static u8 __iomem *reset_unit_regs(struct wormhole_device *wh_dev) {
	return wh_dev->bar4_mapping + RESET_UNIT_START;
}

static void update_device_index(struct wormhole_device *wh_dev) {
	static const u8 INDEX_VALID = 0x80;

	wormhole_send_arc_fw_message_with_args(reset_unit_regs(wh_dev),
						WH_FW_MSG_PCIE_INDEX,
						wh_dev->tt.ordinal | INDEX_VALID, 0,
						10*1000, NULL);
}

static bool wormhole_reset(struct tenstorrent_device *tt_dev, u32 reset_flag)
{
	struct pci_dev *pdev = tt_dev->pdev;
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	u16 reset_arg = (reset_flag == TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET) ? 3 : 0;
	bool responsive;

	// See if the device is responsive.
	responsive = wormhole_send_arc_fw_message(reset_unit_regs(wh_dev), WH_FW_MSG_NOP, 1000, NULL);

	// If not responsive, wait for the watchdog.
	if (!responsive) {
		ktime_t end_time;

		// Give up immediately if the watchdog is disabled.
		if (auto_reset_timeout == 0) {
			dev_err(&pdev->dev, "Watchdog is disabled and device is unresponsive, cannot reset.\n");
			return false;
		}

		end_time = ktime_add_ms(ktime_get(), (auto_reset_timeout * 1000) + 500);
		while (ktime_before(ktime_get(), end_time)) {
			pcie_hot_reset_and_restore_state(pdev);

			responsive = wormhole_send_arc_fw_message(reset_unit_regs(wh_dev), WH_FW_MSG_NOP, 1000, NULL);
			if (responsive)
				break;

			if (msleep_interruptible(1000))
				return false;
		}
	}

	// If the device is responsive, finalize the reset.
	if (responsive) {
		set_reset_marker(pdev);
		wormhole_send_arc_fw_message_with_args(reset_unit_regs(wh_dev), WH_FW_MSG_TRIGGER_RESET, reset_arg, 0,
							0, NULL);
		return true; // Assumes the reset was successful.
	}

	// If we've reached this point, the device is still unresponsive.
	dev_err(&pdev->dev, "Device is unresponsive, cannot reset.\n");
	return false;
}

static int telemetry_probe(struct tenstorrent_device *tt_dev)
{
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);

	u32 base_addr = ioread32(wh->bar4_mapping + ARC_TELEMETRY_PTR);
	u32 data_addr = ioread32(wh->bar4_mapping + ARC_TELEMETRY_DATA);

	u32 version, major_ver, minor_ver, patch_ver;
	u32 tags_addr = base_addr + 8;
	u32 num_entries;
	u32 i, j;

	if (!is_range_within_csm(base_addr, sizeof(u32)) || !is_range_within_csm(data_addr, sizeof(u32))) {
		dev_err(&tt_dev->pdev->dev, "Telemetry not available\n");
		return -ENODEV;
	}

	version = ioread32(wh->bar4_mapping + wh_arc_addr_to_sysreg(base_addr));
	major_ver = (version >> 16) & 0xFF;
	minor_ver = (version >> 8) & 0xFF;
	patch_ver = version & 0xFF;

	if (major_ver > 1) {
		dev_err(&tt_dev->pdev->dev, "Unsupported telemetry version %u.%u.%u\n", major_ver, minor_ver, patch_ver);
		return -ENOTSUPP;
	}

	num_entries = ioread32(wh->bar4_mapping + wh_arc_addr_to_sysreg(base_addr + 4));

	for (i = 0; i < num_entries; i++) {
		u32 tag_entry = ioread32(wh->bar4_mapping + wh_arc_addr_to_sysreg(tags_addr + (i * 4)));
		u16 tag_id = tag_entry & 0xFFFF;
		u16 offset = (tag_entry >> 16) & 0xFFFF;
		u32 addr = data_addr + (offset * sizeof(u32));

		if (!is_range_within_csm(addr, sizeof(u32))) {
			dev_err(&tt_dev->pdev->dev, "Telemetry tag %u has invalid address 0x%08X\n", tag_id, addr);
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(wh_sysfs_attributes); ++j) {
			if (wh_sysfs_attributes[j].tag_id == tag_id)
				wh->sysfs_attr_offsets[j] = wh_arc_addr_to_sysreg(addr);
		}
	}

	return 0;
}

static const struct tt_hwmon_attr wh_hwmon_attributes[] = {
	{ hwmon_temp,  hwmon_temp_input,  0x74, 0,  GENMASK(15, 0), 1000,    16 },
	{ hwmon_temp,  hwmon_temp_max,    0x8c, 0,  GENMASK(15, 0), 1000,    1 },
	{ hwmon_in,    hwmon_in_input,    0x70, 0,  GENMASK(31, 0), 1,       1 },
	{ hwmon_in,    hwmon_in_max,      0x88, 16, GENMASK(15, 0), 1,       1 },
	{ hwmon_curr,  hwmon_curr_input,  0x84, 0,  GENMASK(15, 0), 1000,    1 },
	{ hwmon_curr,  hwmon_curr_max,    0x84, 16, GENMASK(15, 0), 1000,    1 },
	{ hwmon_power, hwmon_power_input, 0x80, 0,  GENMASK(15, 0), 1000000, 1 },
	{ hwmon_power, hwmon_power_max,   0x80, 16, GENMASK(15, 0), 1000000, 1 },
	{ .reg_offset = TT_HWMON_ATTR_END },
};

static const struct tt_hwmon_label wh_hwmon_labels[] = {
	{ hwmon_temp,  hwmon_temp_label,  "asic1_temp" },
	{ hwmon_in,    hwmon_in_label,    "vcore1"     },
	{ hwmon_curr,  hwmon_curr_label,  "current1"   },
	{ hwmon_power, hwmon_power_label, "power1"     },
	{ .name = NULL },
};

static const struct hwmon_channel_info *wh_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX),
	NULL,
};

static const struct hwmon_chip_info wh_hwmon_chip_info = {
	.ops = &tt_hwmon_ops,
	.info = wh_hwmon_info,
};

static void wormhole_hwmon_init(struct wormhole_device *wh_dev)
{
	struct tenstorrent_device *tt_dev = &wh_dev->tt;
	struct device *dev = &tt_dev->pdev->dev;
	struct tt_hwmon_context *context = &tt_dev->hwmon_context;
	struct device *hwmon_device;
	u32 telemetry_offset;

	if (!wormhole_read_fw_telemetry_offset(reset_unit_regs(wh_dev), &telemetry_offset))
		goto wormhole_hwmon_init_err;

	context->attributes = wh_hwmon_attributes;
	context->labels = wh_hwmon_labels;
	context->telemetry_base = wh_dev->bar4_mapping + wh_arc_addr_to_sysreg(telemetry_offset);

	hwmon_device = devm_hwmon_device_register_with_info(dev, "wormhole", context, &wh_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon_device))
		goto wormhole_hwmon_init_err;

	// Notify udev that telemetry attributes are now available.
	kobject_uevent(&tt_dev->dev.kobj, KOBJ_CHANGE);

	return;

wormhole_hwmon_init_err:
	dev_warn(dev, "Failed to initialize hwmon.\n");
}

static bool is_fw_ready_for_telemetry(struct wormhole_device *wh)
{
	u32 base_addr = ioread32(wh->bar4_mapping + ARC_TELEMETRY_PTR);
	u32 data_addr = ioread32(wh->bar4_mapping + ARC_TELEMETRY_DATA);
	bool ready = is_range_within_csm(base_addr, 4) && is_range_within_csm(data_addr, 4);

	return ready;
}

static void wormhole_hwmon_init(struct wormhole_device *wh_dev);

static int init_sysfs_and_hwmon_telemetry(struct tenstorrent_device *tt_dev)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	int r = telemetry_probe(tt_dev);

	if (!r) {
		r = devm_device_add_group(&tt_dev->dev, &tt_dev->telemetry_group);
		wormhole_hwmon_init(wh_dev);
	}
	return r;
}

static void fw_ready_work_func(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct wormhole_device *wh = container_of(dwork, struct wormhole_device, fw_ready_work);
	struct tenstorrent_device *tt_dev = &wh->tt;

	if (tt_dev->detached)
		return;

	if (is_fw_ready_for_telemetry(wh))
		init_sysfs_and_hwmon_telemetry(tt_dev);
	else if (wh->telemetry_retries-- > 0)
		schedule_delayed_work(&wh->fw_ready_work, msecs_to_jiffies(1000));
	else
		dev_err(&tt_dev->pdev->dev, "Timed out waiting for FW telemetry; sysfs/hwmon will be unavailable\n");
}

static bool wormhole_init(struct tenstorrent_device *tt_dev)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	struct device *dev = &tt_dev->pdev->dev;
	int i;

	INIT_DELAYED_WORK(&wh_dev->fw_ready_work, fw_ready_work_func);
	wh_dev->telemetry_retries = 120;	// If telemetry is not ready, defer initialization for up to 2 minutes.

	wh_dev->sysfs_attr_offsets = devm_kcalloc(dev, ARRAY_SIZE(wh_sysfs_attributes), sizeof(u64), GFP_KERNEL);
	tt_dev->telemetry_attrs = devm_kcalloc(dev, ARRAY_SIZE(wh_sysfs_attributes) + 1, sizeof(struct attribute *), GFP_KERNEL);

	if (!wh_dev->sysfs_attr_offsets || !tt_dev->telemetry_attrs)
		return false;

	wh_dev->bar2_mapping = pci_iomap(wh_dev->tt.pdev, 2, 0);
	if (wh_dev->bar2_mapping == NULL) goto fail_bar2;

	wh_dev->bar4_mapping = pci_iomap(wh_dev->tt.pdev, 4, 0);
	if (wh_dev->bar4_mapping == NULL) goto fail_bar4;

	set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs);
	mutex_init(&wh_dev->kernel_tlb_mutex);

	for (i = 0; i < ARRAY_SIZE(wh_sysfs_attributes); ++i)
		tt_dev->telemetry_attrs[i] = &wh_sysfs_attributes[i].attr.attr;
	tt_dev->telemetry_group.attrs = tt_dev->telemetry_attrs;
	tt_dev->telemetry_group.is_visible = sysfs_telemetry_is_visible;

	return true;

fail_bar4:
	pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);
fail_bar2:
	return false;
}

static bool wormhole_init_hardware(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	map_bar4_to_system_registers(wh_dev);

	if (arc_l2_is_running(reset_unit_regs(wh_dev))) {
		wormhole_send_curr_date(reset_unit_regs(wh_dev));
		wormhole_send_arc_fw_message(reset_unit_regs(wh_dev), WH_FW_MSG_ASTATE0, 10000, NULL);
		update_device_index(wh_dev);
		wormhole_complete_pcie_init(&wh_dev->tt, reset_unit_regs(wh_dev));
		wormhole_send_arc_fw_message_with_args(reset_unit_regs(wh_dev), WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT, auto_reset_timeout, 0, 10000, NULL);
	}

	return true;
}

static bool wormhole_init_telemetry(struct tenstorrent_device *tt_dev)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	int r;

	r = devm_device_add_group(&tt_dev->dev, &wh_pcie_perf_counters_group);
	if (r)
		dev_err(&tt_dev->dev, "PCIe perf counters unavailable: %d\n", r);

	// If the firmware is already ready, initialize telemetry immediately.
	// Otherwise, wait for the fw_ready_work to detect when it becomes ready.
	if (is_fw_ready_for_telemetry(wh_dev))
		init_sysfs_and_hwmon_telemetry(tt_dev);
	else
		schedule_delayed_work(&wh_dev->fw_ready_work, msecs_to_jiffies(1000));

	return true;
}

static void wormhole_cleanup_hardware(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	if (!tt_dev->detached)
		wormhole_shutdown_firmware(tt_dev->pdev, reset_unit_regs(wh_dev));
}

static void wormhole_cleanup(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	if (wh_dev->bar2_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);

	if (wh_dev->bar4_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar4_mapping);
}

#define NUM_TLB_KINDS 3
static const u32 TLB_WINDOW_INDEX[NUM_TLB_KINDS] = { 0, TLB_1M_WINDOW_COUNT, TLB_1M_WINDOW_COUNT + TLB_2M_WINDOW_COUNT };
static const u32 TLB_SHIFTS[NUM_TLB_KINDS] = { TLB_1M_SHIFT, TLB_2M_SHIFT, TLB_16M_SHIFT };
static const u64 TLB_WINDOW_SIZES[NUM_TLB_KINDS] = { TLB_1M_WINDOW_SIZE, TLB_2M_WINDOW_SIZE, TLB_16M_WINDOW_SIZE };
static const u64 TLB_WINDOW_BASES[NUM_TLB_KINDS] = { TLB_1M_WINDOW_BASE, TLB_2M_WINDOW_BASE, TLB_16M_WINDOW_BASE };

struct noc_tlb_non_address_bits {
	   union {
			   u32 reg;
			   struct {
					   u64 x_end: 6;
					   u64 y_end: 6;
					   u64 x_start: 6;
					   u64 y_start: 6;
					   u64 noc_sel : 1;
					   u64 mcast: 1;
					   u64 ordering: 2;
					   u64 linked: 1;
			   };
	   };
};

static int wormhole_tlb_kind(int tlb)
{
	if (tlb >= 0 && tlb < TLB_1M_WINDOW_COUNT)
		return 0;
	if (tlb >= TLB_1M_WINDOW_COUNT &&
	    tlb < TLB_1M_WINDOW_COUNT + TLB_2M_WINDOW_COUNT)
		return 1;
	if (tlb >= TLB_1M_WINDOW_COUNT + TLB_2M_WINDOW_COUNT &&
	    tlb < TLB_1M_WINDOW_COUNT + TLB_2M_WINDOW_COUNT +
			    TLB_16M_WINDOW_COUNT)
		return 2;

	return -EINVAL;
}

static int construct_tlb_config(const struct tenstorrent_noc_tlb_config *config,
				int tlb, u64 *regs)
{
	int kind = wormhole_tlb_kind(tlb);
	struct noc_tlb_non_address_bits non_address_bits = {
		.x_end = config->x_end,
		.y_end = config->y_end,
		.x_start = config->x_start,
		.y_start = config->y_start,
		.noc_sel = config->noc,
		.mcast = config->mcast,
		.ordering = config->ordering,
		.linked = config->linked,
	};

	if (kind < 0)
		return -EINVAL;

	// Address must be aligned to the window size.
	if (config->addr & (TLB_WINDOW_SIZES[kind] - 1))
		return -EINVAL;

	// Addresses must fit in 36 bits.
	if (config->addr >= (1UL << WH_NOC_BITS))
		return -EINVAL;

	*regs = 0;
	*regs |= (u64)config->addr >> TLB_SHIFTS[kind];
	*regs |= (u64)non_address_bits.reg << (WH_NOC_BITS - TLB_SHIFTS[kind]);

	return 0;
}

static int wh_configure_tlb(struct wormhole_device *wh, int tlb,
				  struct tenstorrent_noc_tlb_config *config)
{
	u8 __iomem *tlb_regs = wh->bar4_mapping + TLB_REGS_START;
	u64 regs = 0;
	u32 offset;

	if (tlb < 0 || tlb >= TLB_WINDOW_COUNT)
		return -EINVAL;

	if (construct_tlb_config(config, tlb, &regs))
		return -EINVAL;

	offset = tlb * 2 * sizeof(u32);
	iowrite32(regs & 0xFFFFFFFF, tlb_regs + offset);
	iowrite32((regs >> 32) & 0xFFFFFFFF, tlb_regs + offset + sizeof(u32));

	return 0;
}

static int wormhole_configure_tlb(struct tenstorrent_device *tt_dev, int tlb,
				  struct tenstorrent_noc_tlb_config *config)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	return wh_configure_tlb(wh_dev, tlb, config);
}

static int wormhole_describe_tlb(struct tenstorrent_device *tt_dev, int tlb,
				 struct tlb_descriptor *desc)
{
	int kind = wormhole_tlb_kind(tlb);

	if (kind < 0 || kind >= NUM_TLB_KINDS)
		return -EINVAL;

	desc->bar = 0;
	desc->size = TLB_WINDOW_SIZES[kind];
	desc->bar_offset =
		TLB_WINDOW_BASES[kind] +
		TLB_WINDOW_SIZES[kind] * (tlb - TLB_WINDOW_INDEX[kind]);

	return 0;
}

static u8 __iomem *wh_configure_kernel_tlb(struct wormhole_device *wh, u32 x, u32 y, u64 addr, int noc) {
	struct tenstorrent_noc_tlb_config config = { 0 };
	u64 offset = addr & TLB_16M_WINDOW_MASK;

	config.addr = addr & ~TLB_16M_WINDOW_MASK;
	config.x_end = x;
	config.y_end = y;
	config.ordering	= 1; // strict
	config.noc = noc;

	wh_configure_tlb(wh, KERNEL_TLB_INDEX, &config);
	return wh->bar4_mapping + KERNEL_TLB_START + offset;
}

static u32 noc_read32(struct wormhole_device *wh, u32 x, u32 y, u64 addr, int noc) {
	u32 val;
	u8 __iomem *tlb_window;

	mutex_lock(&wh->kernel_tlb_mutex);

	tlb_window = wh_configure_kernel_tlb(wh, x, y, addr, noc);
	val = ioread32(tlb_window);

	mutex_unlock(&wh->kernel_tlb_mutex);

	return val;
}

static void noc_write32(struct wormhole_device *wh, u32 x, u32 y, u64 addr, u32 data, int noc) {
	u8 __iomem *tlb_window;

	mutex_lock(&wh->kernel_tlb_mutex);

	tlb_window = wh_configure_kernel_tlb(wh, x, y, addr, noc);
	iowrite32(data, tlb_window);

	mutex_unlock(&wh->kernel_tlb_mutex);
}

// open_dbi disrupts normal NOC DMA because all outbound traffic are routed to DBI
// only invokes open_dbi when there is no outbound traffic
static void open_dbi(struct wormhole_device *wh) {
	iowrite32(DBI_ENABLE, reset_unit_regs(wh) + PCIE_ARMISC_INFO_REG);
	iowrite32(DBI_ENABLE, reset_unit_regs(wh) + PCIE_AWMISC_INFO_REG);
}

static void close_dbi(struct wormhole_device *wh) {
	iowrite32(0x0, reset_unit_regs(wh) + PCIE_ARMISC_INFO_REG);
	iowrite32(0x0, reset_unit_regs(wh) + PCIE_AWMISC_INFO_REG);
}

static void wormhole_save_reset_state(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	u32 device_control;

	open_dbi(wh);
	device_control = noc_read32(wh, PCIE_NOC_X, PCIE_NOC_Y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, 0);
	wh->saved_mps = FIELD_GET(PCI_EXP_DEVCTL_PAYLOAD, device_control);
	close_dbi(wh);
}

static void wormhole_restore_reset_state(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	u32 device_control;

	open_dbi(wh);
	device_control = noc_read32(wh, PCIE_NOC_X, PCIE_NOC_Y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, 0);
	device_control &= ~PCI_EXP_DEVCTL_PAYLOAD;
	device_control |= FIELD_PREP(PCI_EXP_DEVCTL_PAYLOAD, wh->saved_mps);
	noc_write32(wh, PCIE_NOC_X, PCIE_NOC_Y, PCIE_DBI_ADDR + DBI_DEVICE_CONTROL_DEVICE_STATUS, device_control, 0);
	close_dbi(wh);
}

static int wormhole_configure_outbound_atu(struct tenstorrent_device *tt_dev, u32 region, u64 base, u64 limit,
					   u64 target)
{
	struct wormhole_device *wh = tt_dev_to_wh_dev(tt_dev);
	u32 region_ctrl_1 = 0x0; // MEM type
	u32 region_ctrl_2 = (limit == 0) ? 0 : (REGION_EN | DMA_BYPASS | TLP_BYPASS | FUNC_BYPASS);
	u32 lower_base = lower_32_bits(base);
	u32 upper_base = upper_32_bits(base);
	u32 lower_target = lower_32_bits(target);
	u32 upper_target = upper_32_bits(target);

	if (region >= IATU_OUTBOUND_REGIONS)
		return -EINVAL;

	if (limit > U32_MAX)
		return -EINVAL;

	WRITE_IATU_REG(wh, OUTBOUND, region, LOWER_BASE_ADDR, lower_base);
	WRITE_IATU_REG(wh, OUTBOUND, region, UPPER_BASE_ADDR, upper_base);
	WRITE_IATU_REG(wh, OUTBOUND, region, LOWER_TARGET_ADDR, lower_target);
	WRITE_IATU_REG(wh, OUTBOUND, region, UPPER_TARGET_ADDR, upper_target);
	WRITE_IATU_REG(wh, OUTBOUND, region, LIMIT_ADDR, limit);
	WRITE_IATU_REG(wh, OUTBOUND, region, REGION_CTRL_1, region_ctrl_1);
	WRITE_IATU_REG(wh, OUTBOUND, region, REGION_CTRL_2, region_ctrl_2);

	return 0;
}

static void wormhole_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, u32 data, int noc)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	noc_write32(wh_dev, x, y, addr, data, noc);
}

static int wormhole_set_power_state(struct tenstorrent_device *tt_dev, struct tenstorrent_power_state *power_state)
{
	return 0; // Treat as success/no-op rather than an error.
}

struct tenstorrent_device_class wormhole_class = {
	.name = "Wormhole",
	.instance_size = sizeof(struct wormhole_device),
	.dma_address_bits = 32,
	.noc_dma_limit = (0xFFFE0000 - 1),
	.noc_pcie_offset = 0x800000000ULL,
	.tlb_kinds = NUM_TLB_KINDS,
	.tlb_counts = { TLB_1M_WINDOW_COUNT, TLB_2M_WINDOW_COUNT, TLB_16M_WINDOW_COUNT },
	.tlb_sizes = { TLB_1M_WINDOW_SIZE, TLB_2M_WINDOW_SIZE, TLB_16M_WINDOW_SIZE },
	.reset = wormhole_reset,
	.init_device = wormhole_init,
	.init_hardware = wormhole_init_hardware,
	.init_telemetry = wormhole_init_telemetry,
	.cleanup_hardware = wormhole_cleanup_hardware,
	.cleanup_device = wormhole_cleanup,
	.reboot = wormhole_cleanup_hardware,
	.configure_tlb = wormhole_configure_tlb,
	.describe_tlb = wormhole_describe_tlb,
	.save_reset_state = wormhole_save_reset_state,
	.restore_reset_state = wormhole_restore_reset_state,
	.configure_outbound_atu = wormhole_configure_outbound_atu,
	.noc_write32 = wormhole_noc_write32,
	.set_power_state = wormhole_set_power_state,
};
