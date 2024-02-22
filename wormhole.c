// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/types.h>

#include "wormhole.h"
#include "grayskull.h"
#include "pcie.h"
#include "module.h"
#include "hwmon.h"
#include "eth.h"
#include "tlb.h"

#define WH_FW_MSG_PCIE_INDEX 0x51
#define WH_FW_MSG_ASTATE0 0xA0
#define WH_FW_MSG_UPDATE_M3_AUTO_RESET_TIMEOUT 0xBC

// The iATU can be used to match & remap PCIE transactions.
#define IATU_BASE 0x1200	// Relative to the start of BAR2
#define IATU_OUTBOUND 0
#define IATU_INBOUND 1
#define IATU_REGION_STRIDE 0x100
#define IATU_REGION_CTRL_1_INBOUND	0x00
#define IATU_REGION_CTRL_2_INBOUND	0x04
#define IATU_LOWER_TARGET_ADDR_INBOUND	0x14
#define IATU_UPPER_TARGET_ADDR_INBOUND	0x18

// IATU_REGION_CTRL_2_INBOUND fields
#define REGION_EN (1 << 31)
#define BAR_MATCH_MODE (1 << 30)
#define FUZZY_TYPE_MATCH (1 << 27) // MRd, MWr, MRdLk are considered the same.
#define BAR_NUM(n) ((n) << 8)

// BAR4 is 32MB and will be mapped to the system registers from 0x1E000000
// to 0x20000000.
#define BAR4_SOC_TARGET_ADDRESS 0x1E000000

#define RESET_UNIT_START (0x1FF30000 - BAR4_SOC_TARGET_ADDRESS)
#define ARC_CSM_START    (0x1FE80000 - BAR4_SOC_TARGET_ADDRESS)
#define TLB_CONFIG_START (0x1FC00000 - BAR4_SOC_TARGET_ADDRESS)

#define ARC_CSM_NOC 0x810000000ULL

#define WRITE_IATU_REG(wh_dev, direction, region, reg, value) \
	write_iatu_reg(wh_dev, IATU_##direction, region, \
		       IATU_##reg##_##direction, (value))


static void write_iatu_reg(struct wormhole_device *wh_dev, unsigned direction,
			   unsigned region, unsigned reg, u32 value) {
	u32 offset = IATU_BASE + (2 * region + direction) * IATU_REGION_STRIDE
		   + reg;

	iowrite32(value, wh_dev->bar2_mapping + offset);
}

// Returns an address relative to ARC_CSM_START
static u32 wh_arc_addr_adjust(u32 arc_addr) {
	return (arc_addr - 0x10000000);
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

	wh_dev->bar0_mapping = pci_iomap(wh_dev->tt.pdev, 0, 0);
	if (wh_dev->bar0_mapping == NULL) goto fail_bar0;

	wh_dev->bar2_mapping = pci_iomap(wh_dev->tt.pdev, 2, 0);
	if (wh_dev->bar2_mapping == NULL) goto fail_bar2;

	wh_dev->bar4_mapping = pci_iomap(wh_dev->tt.pdev, 4, 0);
	if (wh_dev->bar4_mapping == NULL) goto fail_bar4;

	return true;

fail_bar4:
	pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);
fail_bar2:
	pci_iounmap(wh_dev->tt.pdev, wh_dev->bar0_mapping);
fail_bar0:
	return false;
}

#define NOC_ADDR_NODE_ID_BITS     6 
#define NOC_ADDR_LOCAL_BITS       36
static u64 encode_sys_addr(u32 chip_x, u32 chip_y, u32 noc_x, u32 noc_y, u64 offset) {
	u64 result = chip_y; // shelf
	u64 noc_addr_local_bits_mask = (1UL << NOC_ADDR_LOCAL_BITS) - 1;
	result <<= 6;
	result |= chip_x;
	result <<= 6;
	result |= noc_y;
	result <<= 6;
	result |= noc_x;
	result <<= NOC_ADDR_LOCAL_BITS;
	result |= (noc_addr_local_bits_mask & offset);
	return result;
}

#define ARC_NOC_X 0
#define ARC_NOC_Y 10
static u32 wormhole_hwmon_read32(u64 offset, struct tt_hwmon_context *context, int channel) {
	struct tenstorrent_device *tt_dev = context->tt_dev;
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	struct connected_eth_core *core_info;
	bool ok;
	u32 value;
	u64 sys_addr;
	u16 rack;

	offset += context->telemetry_offset;

	if (channel == 0) {
		u8 __iomem *addr = wh_dev->bar4_mapping + ARC_CSM_START + offset;
		return ioread32(addr);
	}

	if (wh_dev->num_connected_cores == 0)
		return -EOPNOTSUPP;

	offset += ARC_CSM_NOC;
	core_info = &wh_dev->connected_eth_cores[0];
	sys_addr = encode_sys_addr(core_info->remote_shelf_x, core_info->remote_shelf_y, ARC_NOC_X, ARC_NOC_Y, offset);
	rack = ((u16)core_info->remote_rack_y) << 8 | core_info->remote_rack_x;

	// FIXME: This must be synchronized with UMD somehow.
	// Using eth core 0 to do the actual read.
	ok = wormhole_eth_read32(wh_dev, 0, sys_addr, rack, &value);
	if (!ok)
		return -EOPNOTSUPP;

	return value;
}

// N150 cards have a single ASIC, N300 cards have two.
// Channel 1 represents the attributes/labels for the second ASIC.
static const struct tt_hwmon_attr wh_hwmon_attributes[] = {
	{ hwmon_temp,  hwmon_temp_input,  0x74, 0,  GENMASK(15, 0), 64,		 0 },
	{ hwmon_temp,  hwmon_temp_max,    0x8c, 0,  GENMASK(15, 0), 1000,    0 },
	{ hwmon_in,    hwmon_in_input,    0x70, 0,  GENMASK(31, 0), 1,	     0 },
	{ hwmon_in,    hwmon_in_max,      0x88, 16, GENMASK(15, 0), 1,		 0 },
	{ hwmon_curr,  hwmon_curr_input,  0x84, 0,  GENMASK(15, 0), 1000,    0 },
	{ hwmon_curr,  hwmon_curr_max,    0x84, 16, GENMASK(15, 0), 1000,    0 },
	{ hwmon_power, hwmon_power_input, 0x80, 0,  GENMASK(15, 0), 1000000, 0 },
	{ hwmon_power, hwmon_power_max,   0x80, 16, GENMASK(15, 0), 1000000, 0 },
	{ hwmon_temp,  hwmon_temp_input,  0x74, 0,  GENMASK(15, 0), 64,      1 },
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
	{ hwmon_temp,  hwmon_temp_label,  "asic_temp1", 0 },
	{ hwmon_in,    hwmon_in_label,    "vcore1"    , 0 },
	{ hwmon_curr,  hwmon_curr_label,  "current1"  , 0 },
	{ hwmon_power, hwmon_power_label, "power1"    , 0 },
	{ hwmon_temp,  hwmon_temp_label,  "asic_temp2", 1 },
	{ hwmon_in,    hwmon_in_label,    "vcore2"    , 1 },
	{ hwmon_curr,  hwmon_curr_label,  "current2"  , 1 },
	{ hwmon_power, hwmon_power_label, "power2"    , 1 },
	{ .name = NULL },
};

static const struct hwmon_channel_info *wh_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX,
							 HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX,
						   HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX,
							HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX,
							  HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX),
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

	context->tt_dev = tt_dev;
	context->attributes = wh_hwmon_attributes;
	context->labels = wh_hwmon_labels;
	context->telemetry_offset = wh_arc_addr_adjust(telemetry_offset);
	context->read32 = wormhole_hwmon_read32;

	hwmon_device = devm_hwmon_device_register_with_info(dev, "wormhole", context, &wh_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon_device))
		goto wormhole_hwmon_init_err;

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

	tlb_pool_init(&wh_dev->tlb_pool);
	wormhole_eth_probe(wh_dev);
	wormhole_hwmon_init(wh_dev);

	return true;
}

static void wormhole_cleanup(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	grayskull_shutdown_firmware(tt_dev->pdev, reset_unit_regs(wh_dev));

	if (wh_dev->bar0_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar0_mapping);

	if (wh_dev->bar2_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);

	if (wh_dev->bar4_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar4_mapping);
}

static void wormhole_reboot(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	grayskull_shutdown_firmware(tt_dev->pdev, reset_unit_regs(wh_dev));
}

#define TLB_CONFIG_REG_LO(index) (TLB_CONFIG_START + (index * 2 * sizeof(u32)))
#define TLB_CONFIG_REG_HI(index) (TLB_CONFIG_REG_LO(index) + sizeof(u32))
static void wh_program_tlb(struct wormhole_device *wh_dev, struct tlb_t *tlb) {
	int wh_local_offset_width = tlb->size == TLB_SIZE_1M ? 16 : tlb->size == TLB_SIZE_2M ? 15 : 12;
	u64 encoded = tlb_encode_config(tlb, wh_local_offset_width);

	// Write only if the encoding has changed.
	if (tlb->encoded_config != encoded) {
		iowrite32((encoded >> 0x00) & 0xffffffffU, wh_dev->bar4_mapping + TLB_CONFIG_REG_LO(tlb->index));
		iowrite32((encoded >> 0x20) & 0xffffffffU, wh_dev->bar4_mapping + TLB_CONFIG_REG_HI(tlb->index));
		tlb->encoded_config = encoded;
	}
}

void wh_setup_tlb(struct wormhole_device *wh_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr) {
	tlb_set_config(tlb, noc_addr);
	wh_program_tlb(wh_dev, tlb);
}

bool wormhole_noc_read(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr, void *dst, size_t size) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	struct tlb_pool *pool = &wh_dev->tlb_pool;
	bool manage_tlb = (tlb == NULL);
	u8 __iomem *iomem;

	if (manage_tlb)
		tlb = tlb_alloc(pool);

	if (!tlb)
		return false;

	wh_setup_tlb(wh_dev, tlb, noc_addr);
	iomem = wh_dev->bar0_mapping + TLB_OFFSET(tlb->index) + (noc_addr->addr % tlb->size);
	memcpy_fromio(dst, iomem, size);

	if (manage_tlb)
		tlb_free(pool, tlb);

	return true;
}

bool wormhole_noc_write(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr, const void *src, size_t size) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	struct tlb_pool *pool = &wh_dev->tlb_pool;
	bool manage_tlb = (tlb == NULL);
	u8 __iomem *iomem;

	if (manage_tlb)
		tlb = tlb_alloc(pool);

	if (!tlb)
		return false;

	wh_setup_tlb(wh_dev, tlb, noc_addr);
	iomem = wh_dev->bar0_mapping + TLB_OFFSET(tlb->index) + (noc_addr->addr % tlb->size);
	memcpy_toio(iomem, src, size);

	if (manage_tlb)
		tlb_free(pool, tlb);

	return true;
}

bool wormhole_noc_read32(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr, u32 *val) {
	return wormhole_noc_read(tt_dev, tlb, noc_addr, val, sizeof(*val));
}

bool wormhole_noc_write32(struct tenstorrent_device *tt_dev, struct tlb_t *tlb, struct noc_addr_t *noc_addr, u32 val) {
	return wormhole_noc_write(tt_dev, tlb, noc_addr, &val, sizeof(val));
}

struct tenstorrent_device_class wormhole_class = {
	.name = "Wormhole",
	.instance_size = sizeof(struct wormhole_device),
	.init_device = wormhole_init,
	.init_hardware = wormhole_init_hardware,
	.cleanup_device = wormhole_cleanup,
	.reboot = wormhole_reboot,
	.noc_read32 = wormhole_noc_read32,
	.noc_write32 = wormhole_noc_write32,
};
