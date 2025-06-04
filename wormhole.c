// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>

#include "wormhole.h"
#include "grayskull.h"
#include "pcie.h"
#include "module.h"
#include "hwmon.h"
#include "tlb.h"

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

// kernel TLB is the last 16MB TLB
#define KERNEL_TLB_INDEX (TLB_WINDOW_COUNT - 1)
#define KERNEL_TLB_START (0x1E000000 - BAR4_SOC_TARGET_ADDRESS)

#define PCIE_DBI_ADDR 0x800000000ULL
#define PCIE_NOC_X 0
#define PCIE_NOC_Y 3
#define PCIE_NOC1_X 9
#define PCIE_NOC1_Y 8
#define DBI_ENABLE 0x00200000
#define PCIE_ARMISC_INFO_REG SCRATCH_REG(6)
#define PCIE_AWMISC_INFO_REG SCRATCH_REG(7)

#define WRITE_IATU_REG(wh_dev, direction, region, reg, value) \
	write_iatu_reg(wh_dev, IATU_##direction, region, \
		       IATU_##reg##_##direction, (value))

static u32 noc_read32(struct wormhole_device *wh, u32 x, u32 y, u64 addr, int noc);

static void write_iatu_reg(struct wormhole_device *wh_dev, unsigned direction,
			   unsigned region, unsigned reg, u32 value) {
	u32 offset = IATU_BASE + (2 * region + direction) * IATU_REGION_STRIDE
		   + reg;

	iowrite32(value, wh_dev->bar2_mapping + offset);
}

static struct tt_attribute_data wh_attributes[] = {
	{ __ATTR(tt_aiclk,  S_IRUGO, tt_show_attribute, NULL), 0x60, 0xFFFF },
	{ __ATTR(tt_axiclk, S_IRUGO, tt_show_attribute, NULL), 0x64, 0xFFFF },
	{ __ATTR(tt_arcclk, S_IRUGO, tt_show_attribute, NULL), 0x68, 0xFFFF },
	{ __ATTR(tt_card_type, S_IRUGO, tt_show_card_type, NULL), 0x10, 0x0 },
	{ __ATTR(tt_serial, S_IRUGO, tt_show_card_serial, NULL), 0x10, 0x0 },
	{ __ATTR(tt_arc_fw_ver, S_IRUGO, tt_show_fw_ver, NULL), 0x18, 0x0 },
	{ __ATTR(tt_eth_fw_ver, S_IRUGO, tt_show_eth_fw_ver, NULL), 0x2C, 0x0 },
	{ __ATTR(tt_m3bl_fw_ver, S_IRUGO, tt_show_fw_ver, NULL), 0x30, 0x0 },
	{ __ATTR(tt_m3app_fw_ver, S_IRUGO, tt_show_fw_ver, NULL), 0x34, 0x0 },
	{ __ATTR(tt_ttflash_ver, S_IRUGO, tt_show_fw_ver, NULL), 0xB8, 0x0 },
	{ __ATTR(tt_fw_bundle_ver, S_IRUGO, tt_show_fw_ver, NULL), 0xC4, 0x0 },
	{ __ATTR_NULL, 0, 0 }
};


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

static u32 wh_arc_addr_to_sysreg(u32 arc_addr) {
	return ARC_CSM_START + (arc_addr - 0x10000000);
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

	grayskull_send_arc_fw_message_with_args(reset_unit_regs(wh_dev),
						WH_FW_MSG_PCIE_INDEX,
						wh_dev->tt.ordinal | INDEX_VALID, 0,
						10*1000, NULL);
}

static bool wormhole_init(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	wh_dev->bar2_mapping = pci_iomap(wh_dev->tt.pdev, 2, 0);
	if (wh_dev->bar2_mapping == NULL) goto fail_bar2;

	wh_dev->bar4_mapping = pci_iomap(wh_dev->tt.pdev, 4, 0);
	if (wh_dev->bar4_mapping == NULL) goto fail_bar4;

	set_bit(KERNEL_TLB_INDEX, tt_dev->tlbs);
	mutex_init(&wh_dev->kernel_tlb_mutex);

	return true;

fail_bar4:
	pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);
fail_bar2:
	return false;
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

static void wormhole_hwmon_init(struct wormhole_device *wh_dev) {
	struct tenstorrent_device *tt_dev = &wh_dev->tt;
	struct device *dev = &tt_dev->pdev->dev;
	struct tt_hwmon_context *context = &tt_dev->hwmon_context;
	struct device *hwmon_device;
	u32 telemetry_offset;

	if (!grayskull_read_fw_telemetry_offset(reset_unit_regs(wh_dev), &telemetry_offset))
		goto wormhole_hwmon_init_err;

	context->attributes = wh_hwmon_attributes;
	context->labels = wh_hwmon_labels;
	context->telemetry_base = wh_dev->bar4_mapping + wh_arc_addr_to_sysreg(telemetry_offset);

	hwmon_device = devm_hwmon_device_register_with_info(dev, "wormhole", context, &wh_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon_device))
		goto wormhole_hwmon_init_err;

	tt_dev->attributes = wh_attributes;

	return;

wormhole_hwmon_init_err:
	dev_warn(dev, "Failed to initialize hwmon.\n");
}

static bool wormhole_init_hardware(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	map_bar4_to_system_registers(wh_dev);

	if (arc_l2_is_running(reset_unit_regs(wh_dev))) {
		grayskull_send_curr_date(reset_unit_regs(wh_dev));
		grayskull_send_arc_fw_message(reset_unit_regs(wh_dev), WH_FW_MSG_ASTATE0, 10000, NULL);
		update_device_index(wh_dev);
		complete_pcie_init(&wh_dev->tt, reset_unit_regs(wh_dev));
		grayskull_send_arc_fw_message_with_args(reset_unit_regs(wh_dev), WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT, auto_reset_timeout, 0, 10000, NULL);
	}

	return true;
}

static bool wormhole_post_hardware_init(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	wormhole_hwmon_init(wh_dev);

	return true;
}

static void wormhole_cleanup_hardware(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	grayskull_shutdown_firmware(tt_dev->pdev, reset_unit_regs(wh_dev));
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

static void wormhole_create_sysfs_groups(struct tenstorrent_device *tt_dev) {
	int ret = devm_device_add_group(&tt_dev->dev, &wh_pcie_perf_counters_group);
	if (ret)
		dev_err(&tt_dev->dev, "PCIe perf counters unavailable: %d\n", ret);
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
	.init_device = wormhole_init,
	.init_hardware = wormhole_init_hardware,
	.post_hardware_init = wormhole_post_hardware_init,
	.cleanup_hardware = wormhole_cleanup_hardware,
	.cleanup_device = wormhole_cleanup,
	.reboot = wormhole_cleanup_hardware,
	.configure_tlb = wormhole_configure_tlb,
	.describe_tlb = wormhole_describe_tlb,
	.save_reset_state = wormhole_save_reset_state,
	.restore_reset_state = wormhole_restore_reset_state,
	.configure_outbound_atu = wormhole_configure_outbound_atu,
	.create_sysfs_groups = wormhole_create_sysfs_groups,
};
