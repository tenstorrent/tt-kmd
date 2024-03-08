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
#include "ioctl.h"

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


#define TLB_COUNT (156 + 10 + 20)
#define TLB_16M_SIZE_SHIFT 24
#define KERNEL_TLB_INDEX (TLB_COUNT - 1)
#define KERNEL_TLB_CONFIG_REGS	(TLB_CONFIG_START + (KERNEL_TLB_INDEX*2*sizeof(u32)))

#define WH_BOARD_N150 0x18
#define WH_BOARD_N300 0x14
#define WH_TELEMETRY_BOARD_ID_OFFSET 0x10

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

	wh_dev->bar2_mapping = pci_iomap(wh_dev->tt.pdev, 2, 0);
	if (wh_dev->bar2_mapping == NULL) goto fail_bar2;

	wh_dev->bar4_mapping = pci_iomap(wh_dev->tt.pdev, 4, 0);
	if (wh_dev->bar4_mapping == NULL) goto fail_bar4;

	return true;

fail_bar4:
	pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);
fail_bar2:
	return false;
}

#define ARC_NOC_X 0
#define ARC_NOC_Y 10
static int wormhole_hwmon_read(u64 offset, struct tt_hwmon_context *context, int channel, long *val) {
	struct tenstorrent_device *tt_dev = context->tt_dev;
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	u32 i;

	offset += context->telemetry_offset;

	if (channel == 0) {
		offset += ARC_CSM_START;
		*val = ioread32(wh_dev->bar4_mapping + offset);
		return 0;
	}

	if (wh_dev->num_connected_cores == 0)
		return -EOPNOTSUPP;

	mutex_lock(&tt_dev->chardev_mutex);
	if (tt_dev->open_handles_that_havent_used_lock_api > 0) {
		mutex_unlock(&tt_dev->chardev_mutex);
		return -EBUSY;
	}
	mutex_unlock(&tt_dev->chardev_mutex);

	// Need to do a remote read, so adjust the offset - the ARC will be accessed
	// via the NOC rather than via PCI BAR.
	offset += ARC_CSM_NOC;

	// Find a connected core to perform the read.
	for (i = 0; i < wh_dev->num_connected_cores; i++) {
		u32 eth_channel = wh_dev->connected_eth_cores[i].eth_channel;
		struct eth_addr_t *remote_addr = &wh_dev->connected_eth_cores[i].remote;
		u8 lock_index = TENSTORRENT_LOCK_ETH(eth_channel);

		// Check if userspace is using the core.
		if (!test_and_set_bit(lock_index, tt_dev->resource_lock)) {
			u32 value;
			bool ok = wormhole_remote_read32(wh_dev, eth_channel, remote_addr, ARC_NOC_X, ARC_NOC_Y, offset, &value);
			*val = value;
			clear_bit(lock_index, tt_dev->resource_lock);
			if (ok)
				return 0;
			else
				dev_warn(&tt_dev->pdev->dev, "Failed to read from ETH core %u\n", eth_channel);
		}
	}

	// We couldn't find a core to use or the ones we tried failed.
	return -EBUSY;
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
	{ hwmon_temp,  hwmon_temp_label,  "asic1_temp",    0 },
	{ hwmon_in,    hwmon_in_label,    "asic1_vcore",   0 },
	{ hwmon_curr,  hwmon_curr_label,  "asic1_current", 0 },
	{ hwmon_power, hwmon_power_label, "asic1_power",   0 },
	{ hwmon_temp,  hwmon_temp_label,  "asic2_temp",    1 },
	{ hwmon_in,    hwmon_in_label,    "asic2_vcore",   1 },
	{ hwmon_curr,  hwmon_curr_label,  "asic2_current", 1 },
	{ hwmon_power, hwmon_power_label, "asic2_power",   1 },
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
	u32 board_id;

	if (!grayskull_read_fw_telemetry_offset(reset_unit_regs(wh_dev), &telemetry_offset))
		goto wormhole_hwmon_init_err;

	board_id = tt_dev->pdev->subsystem_device;
	if (!(board_id == WH_BOARD_N150 || board_id == WH_BOARD_N300)) {
		dev_err(dev, "Unsupported board id for hwmon: 0x%x\n", board_id);
		goto wormhole_hwmon_init_err;
	}

	context->tt_dev = tt_dev;
	context->labels = wh_hwmon_labels;
	context->attributes = wh_hwmon_attributes;
	context->channels = (board_id == WH_BOARD_N300) ? 2 : 1;	// N300 2 channels; N150 1 channel.
	context->telemetry_offset = wh_arc_addr_adjust(telemetry_offset);
	context->hwmon_read = wormhole_hwmon_read;

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

	wormhole_eth_probe(wh_dev);
	wormhole_hwmon_init(wh_dev);

	return true;
}

static void wormhole_cleanup(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	grayskull_shutdown_firmware(tt_dev->pdev, reset_unit_regs(wh_dev));

	if (wh_dev->bar2_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar2_mapping);

	if (wh_dev->bar4_mapping != NULL)
		pci_iounmap(wh_dev->tt.pdev, wh_dev->bar4_mapping);
}

static void wormhole_reboot(struct tenstorrent_device *tt_dev) {
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);

	grayskull_shutdown_firmware(tt_dev->pdev, reset_unit_regs(wh_dev));
}

struct TLB_16M_REG {
	union {
		struct {
			u32 low32;
			u32 high32;
		};
		struct {
			u64 local_offset: 12;
			u64 x_end: 6;
			u64 y_end: 6;
			u64 x_start: 6;
			u64 y_start: 6;
			u64 noc_sel: 1;
			u64 mcast: 1;
			u64 ordering: 2;
			u64 linked: 1;
		};
	};
};

static u32 program_tlb(struct wormhole_device *wh_dev, unsigned int x, unsigned int y, unsigned int noc, u32 addr) {
	struct TLB_16M_REG tlb;

	tlb.low32 = 0;
	tlb.high32 = 0;

	tlb.local_offset = addr >> TLB_16M_SIZE_SHIFT;
	tlb.x_end = x;
	tlb.y_end = y;
	tlb.noc_sel = noc;

	iowrite32(tlb.low32, wh_dev->bar4_mapping + KERNEL_TLB_CONFIG_REGS);
	iowrite32(tlb.high32, wh_dev->bar4_mapping + KERNEL_TLB_CONFIG_REGS + sizeof(u32));

	return addr % (1u << TLB_16M_SIZE_SHIFT);
}

u32 wh_noc_read32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	u32 offset;
	u32 val;

	mutex_lock(&wh_dev->tlb_mutex);

	offset = program_tlb(wh_dev, x, y, 0, addr);
	val = ioread32(wh_dev->bar4_mapping + offset);

	mutex_unlock(&wh_dev->tlb_mutex);

	return val;
}

void wh_noc_write32(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, u32 val)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	u32 offset;

	mutex_lock(&wh_dev->tlb_mutex);

	offset = program_tlb(wh_dev, x, y, 0, addr);
	iowrite32(val, wh_dev->bar4_mapping + offset);

	mutex_unlock(&wh_dev->tlb_mutex);
}

void wh_noc_write_block(struct tenstorrent_device *tt_dev, u32 x, u32 y, u64 addr, const void *src, size_t size)
{
	struct wormhole_device *wh_dev = tt_dev_to_wh_dev(tt_dev);
	u32 offset;

	mutex_lock(&wh_dev->tlb_mutex);

	offset = program_tlb(wh_dev, x, y, 0, addr);
	memcpy_toio(wh_dev->bar4_mapping + offset, src, size);

	mutex_unlock(&wh_dev->tlb_mutex);
}

struct tenstorrent_device_class wormhole_class = {
	.name = "Wormhole",
	.instance_size = sizeof(struct wormhole_device),
	.init_device = wormhole_init,
	.init_hardware = wormhole_init_hardware,
	.cleanup_device = wormhole_cleanup,
	.reboot = wormhole_reboot,
	.noc_read32 = wh_noc_read32,
	.noc_write32 = wh_noc_write32,
};
