// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/timekeeping.h>
#include <linux/hwmon.h>
#include <linux/stat.h>
#include <linux/bitops.h>
#include <asm/io.h>

#include "module.h"
#include "grayskull.h"
#include "ttkmd_arc_if.h"
#include "enumerate.h"
#include "pcie.h"
#include "hwmon.h"

#define GRID_SIZE_X 13
#define GRID_SIZE_Y 12

#define REG_IOMAP_BAR	0
#define REG_IOMAP_START 0x1FC00000	// Starting at PCI TLB config registers
#define REG_IOMAP_LEN   0x00400000	// Covering entire system register space

// Map the last 16MB TLB for kernel dynamic access.
#define KERNEL_TLB_BAR		0
#define KERNEL_TLB_START	(156*(1<<20) + 10*(1<<21) + 19*(1<<24))
#define KERNEL_TLB_LEN		(1u << 24)
#define KERNEL_TLB_REGS		((156+10+19)*2*sizeof(u32))

#define PCI_TLB_CONFIG_OFFSET	(0x1FC00000 - REG_IOMAP_START)
#define ARC_ICCM_MEMORY_OFFSET	(0x1FE00000 - REG_IOMAP_START)
#define ARC_CSM_MEMORY_OFFSET	(0x1FE80000 - REG_IOMAP_START)
#define ARC_ROM_MEMORY_OFFSET	(0x1FF00000 - REG_IOMAP_START)
#define RESET_UNIT_REG_OFFSET	(0x1FF30000 - REG_IOMAP_START)

#define TTKMD_ARC_IF_OFFSET 0x77000
#define ARC_CSM_ROW_HARVESTING_OFFSET 0x7836C

#define SCRATCH_REG(n) (0x60 + (n)*sizeof(u32))	/* byte offset */

#define POST_CODE_REG SCRATCH_REG(0)
#define POST_CODE_MASK ((u32)0x3FFF)
#define POST_CODE_ARC_SLEEP 2
#define POST_CODE_ARC_L2 0xC0DE0000
#define POST_CODE_ARC_L2_MASK 0xFFFF0000

#define SCRATCH_5_ARC_BOOTROM_DONE 0x60
#define SCRATCH_5_ARC_L2_DONE 0x0

#define ARC_MISC_CNTL_REG 0x100
#define ARC_MISC_CNTL_RESET_MASK (1 << 12)
#define ARC_MISC_CNTL_IRQ0_MASK (1 << 16)
#define ARC_UDMIAXI_REGION_REG 0x10C
#define ARC_UDMIAXI_REGION_ICCM(n) (0x3 * (n))
#define ARC_UDMIAXI_REGION_CSM 0x10

#define GPIO_PAD_VAL_REG 0x1B8
#define GPIO_ARC_SPI_BOOTROM_EN_MASK (1 << 12)


// Scratch register 5 is used for the firmware message protocol.
// Write 0xAA00 | message_id into scratch register 5, wait for message_id to appear.
// After reading the message, the firmware will immediately reset SR5 to 0 and write message_id when done.
// Appearance of any other value indicates a conflict with another message.
#define GS_FW_MESSAGE_PRESENT 0xAA00

#define GS_FW_MSG_GO_LONG_IDLE 0x54
#define GS_FW_MSG_SHUTDOWN 0x55
#define GS_FW_MSG_TYPE_PCIE_MUTEX_ACQUIRE 0x9E
#define GS_FW_MSG_ASTATE0 0xA0
#define GS_FW_MSG_ASTATE1 0xA1
#define GS_FW_MSG_ASTATE3 0xA3
#define GS_FW_MSG_ASTATE5 0xA5
#define GS_FW_MSG_CURR_DATE 0xB7
#define GS_FW_MSG_GET_VERSION 0xB9
#define GS_FW_MSG_GET_TELEMETRY_OFFSET 0x2C

#define GS_ARC_L2_FW_NAME "tenstorrent_gs_arc_l2_fw.bin"
#define GS_ARC_L2_FW_SIZE_BYTES 0xF000
#define GS_ICCM_FW_SIZE_BYTES 0x1000
#define GS_WATCHDOG_FW_NAME "tenstorrent_gs_wdg_fw.bin"
#define GS_WATCHDOG_FW_CORE_ID 3
#define GS_SMBUS_FW_NAME "tenstorrent_gs_smbus_fw.bin"
#define GS_SMBUS_FW_CORE_ID 1

#define DRAM_NOC0_REG_BASE 0xffff4000
#define DRAM_NOC1_REG_BASE 0xffff5000

#define ARC_NOC0_REG_BASE 0x1ff50000
#define ARC_NOC1_REG_BASE 0x1ff58000

#define PCI_NOC0_REG_BASE 0x1fd00000
#define PCI_NOC1_REG_BASE 0x1fd08000

#define TENSIX_NOC0_REG_BASE 0xffb20000
#define TENSIX_NOC1_REG_BASE 0xffb30000

#define NIU_CFG_0	0x100
#define ROUTER_CFG_0	0x104
#define ROUTER_CFG_1	0x108
#define ROUTER_CFG_3	0x110

// NIU_CFG_0
#define NIU_CLOCK_GATING_ENABLE	(1u << 0)
#define NIU_TILE_CLOCK_DISABLE	(1u << 12)

// ROUTER_CFG_0
#define ROUTER_CLOCK_GATING_ENABLE	(1u << 0)
#define ROUTER_MAX_BACKOFF_EXP		(0xFu << 8)

struct TLB_16M_REG {
	union {
		struct {
			u32 low32;
			u32 high32;
		};
		struct {
			u64 local_offset: 8;
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

#define TLB_16M_SIZE_SHIFT 24

static u32 gs_arc_addr_to_sysreg(u32 arc_addr) {
	return ARC_CSM_MEMORY_OFFSET + (arc_addr - 0x10000000);
}

static bool is_hardware_hung(struct pci_dev *pdev, u8 __iomem *reset_unit_regs) {
	u16 vendor_id;

	if (pdev != NULL
	    && (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL
		|| vendor_id != PCI_VENDOR_ID_TENSTORRENT))
		return true;

	return (ioread32(reset_unit_regs + SCRATCH_REG(6)) == 0xFFFFFFFF);
}

static int wait_reg32_with_timeout(u8 __iomem* reset_unit_regs, u8 __iomem* reg,
				   u32 expected_val, u32 timeout_us) {
	// Scale poll_period for around 100 polls, and at least 10 us
	u32 poll_period_us = max((u32)10, timeout_us / 100);

	ktime_t end_time = ktime_add_us(ktime_get(), timeout_us);

	while (1) {
		u32 read_val = ioread32(reg);
		if (read_val == expected_val)
			return 0;

		if (read_val == 0xFFFFFFFFu && is_hardware_hung(NULL, reset_unit_regs))
			return -2;

		if (ktime_after(ktime_get(), end_time))
			return -1;

		usleep_range(poll_period_us, 2 * poll_period_us);
	}
}

static int arc_msg_poll_completion(u8 __iomem* reset_unit_regs, u8 __iomem* msg_reg,
				   u32 msg_code, u32 timeout_us, u16* exit_code) {
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

bool arc_l2_is_running(u8 __iomem* reset_unit_regs) {
	u32 post_code = ioread32(reset_unit_regs + POST_CODE_REG);
	return ((post_code & POST_CODE_ARC_L2_MASK) == POST_CODE_ARC_L2);
}

bool grayskull_send_arc_fw_message_with_args(u8 __iomem* reset_unit_regs,
					    u8 message_id, u16 arg0, u16 arg1,
					    u32 timeout_us, u16* exit_code) {
	void __iomem *args_reg = reset_unit_regs + SCRATCH_REG(3);
	void __iomem *message_reg = reset_unit_regs + SCRATCH_REG(5);
	void __iomem *arc_misc_cntl_reg = reset_unit_regs + ARC_MISC_CNTL_REG;
	u32 args = arg0 | ((u32)arg1 << 16);
	u32 arc_misc_cntl;

	if (!arc_l2_is_running(reset_unit_regs)) {
		pr_warn("Skipping message %08X due to FW not running.\n",
			(unsigned int)message_id);
		return false;
	}

	iowrite32(args, args_reg);
	iowrite32(GS_FW_MESSAGE_PRESENT | message_id, message_reg);

	// Trigger IRQ to ARC
	arc_misc_cntl = ioread32(arc_misc_cntl_reg);
	iowrite32(arc_misc_cntl | ARC_MISC_CNTL_IRQ0_MASK, arc_misc_cntl_reg);

	if (arc_msg_poll_completion(reset_unit_regs, message_reg, message_id, timeout_us, exit_code) < 0) {
		return false;
	} else {
		return true;
	}
}

bool grayskull_send_arc_fw_message(u8 __iomem* reset_unit_regs, u8 message_id, u32 timeout_us, u16* exit_code) {
	return grayskull_send_arc_fw_message_with_args(reset_unit_regs, message_id, 0, 0, timeout_us, exit_code);
}

static int grayskull_load_arc_fw(struct grayskull_device *gs_dev) {
	const struct firmware *firmware;
	int ret = 0;
	u32 reset_vector;
	u8 __iomem* reset_unit_regs = gs_dev->reset_unit_regs;
	u8 __iomem* fw_target_mem = gs_dev->reg_iomap + ARC_CSM_MEMORY_OFFSET;
	u8 __iomem* reset_vec_target_mem = gs_dev->reg_iomap + ARC_ROM_MEMORY_OFFSET;

	ret = request_firmware(&firmware, GS_ARC_L2_FW_NAME, &gs_dev->tt.pdev->dev);
	if (ret)
		goto grayskull_load_arc_fw_cleanup;

	if (firmware->size != GS_ARC_L2_FW_SIZE_BYTES) {
		ret = -EINVAL;
		goto grayskull_load_arc_fw_cleanup;
	}

	iowrite32(ARC_UDMIAXI_REGION_CSM, reset_unit_regs + ARC_UDMIAXI_REGION_REG);
	memcpy_toio(fw_target_mem, firmware->data, GS_ARC_L2_FW_SIZE_BYTES);
	reset_vector = le32_to_cpu(*(u32 *)firmware->data);
	iowrite32(reset_vector, reset_vec_target_mem);

grayskull_load_arc_fw_cleanup:
	release_firmware(firmware);
	return ret;
}



static int grayskull_load_iccm_fw(struct grayskull_device *gs_dev,
					const char* fw_name,
					u32 core_id,
					u32 *reset_vector) {
	const struct firmware *firmware;
	int ret = 0;
	u8 __iomem* reset_unit_regs = gs_dev->reset_unit_regs;
	u8 __iomem* fw_target_mem = gs_dev->reg_iomap + ARC_ICCM_MEMORY_OFFSET;

	ret = request_firmware(&firmware, fw_name, &gs_dev->tt.pdev->dev);
	if (ret)
		goto grayskull_load_iccm_fw_cleanup;

	if (firmware->size != GS_ICCM_FW_SIZE_BYTES) {
		ret = -EINVAL;
		goto grayskull_load_iccm_fw_cleanup;
	}

	iowrite32(ARC_UDMIAXI_REGION_ICCM(core_id),
		reset_unit_regs + ARC_UDMIAXI_REGION_REG);
	memcpy_toio(fw_target_mem, firmware->data, GS_ICCM_FW_SIZE_BYTES);
	// Reset vector needs to be passed to FW through ttkmd_arc_if
	*reset_vector = le32_to_cpu(*(u32 *)firmware->data);
	iowrite32(ARC_UDMIAXI_REGION_CSM, reset_unit_regs + ARC_UDMIAXI_REGION_REG);

grayskull_load_iccm_fw_cleanup:
	release_firmware(firmware);
	return ret;
}

static int grayskull_populate_arc_if(struct grayskull_device *gs_dev) {
	ttkmd_arc_if_u *ttkmd_arc_if = kzalloc(sizeof(ttkmd_arc_if_u), GFP_KERNEL);
	u8 __iomem* reset_unit_regs = gs_dev->reset_unit_regs;
	u8 __iomem* device_ttkmd_arc_if = gs_dev->reg_iomap + ARC_CSM_MEMORY_OFFSET + TTKMD_ARC_IF_OFFSET;

	if (ttkmd_arc_if == NULL)
		return -ENOMEM;

	// ARC is little-endian. Convert to little-endian so we can use memcpy_toio
	ttkmd_arc_if->f.magic_number[0] = cpu_to_le32(TTKMD_ARC_MAGIC_NUMBER_0);
	ttkmd_arc_if->f.magic_number[1] = cpu_to_le32(TTKMD_ARC_MAGIC_NUMBER_1);
	ttkmd_arc_if->f.version = cpu_to_le32(TTKMD_ARC_IF_VERSION);
	ttkmd_arc_if->f.stage2_init = arc_fw_stage2_init;
	ttkmd_arc_if->f.ddr_train_en = ddr_train_en;
	ttkmd_arc_if->f.ddr_test_mode = ddr_test_mode;
	ttkmd_arc_if->f.ddr_freq_ovr = cpu_to_le32(ddr_frequency_override);
	ttkmd_arc_if->f.aiclk_ppm_en = aiclk_ppm_en;
	ttkmd_arc_if->f.aiclk_ppm_ovr = cpu_to_le32(aiclk_fmax_override);
	ttkmd_arc_if->f.feature_disable_ovr = cpu_to_le32(arc_fw_feat_dis_override);
	ttkmd_arc_if->f.watchdog_fw_en = watchdog_fw_en;
	ttkmd_arc_if->f.watchdog_fw_load = !watchdog_fw_override;
	ttkmd_arc_if->f.watchdog_fw_reset_vec =
		cpu_to_le32(gs_dev->watchdog_fw_reset_vec);
	ttkmd_arc_if->f.smbus_fw_en = smbus_fw_en;
	ttkmd_arc_if->f.smbus_fw_load = !smbus_fw_override;
	ttkmd_arc_if->f.smbus_fw_reset_vec =
		cpu_to_le32(gs_dev->smbus_fw_reset_vec);

	iowrite32(ARC_UDMIAXI_REGION_CSM, reset_unit_regs + ARC_UDMIAXI_REGION_REG);
	memcpy_toio(device_ttkmd_arc_if, ttkmd_arc_if, sizeof(ttkmd_arc_if_u));

	kfree(ttkmd_arc_if);
	return 0;
}

static int toggle_arc_reset(u8 __iomem* reset_unit_regs) {
	u32 arc_misc_cntl;
	arc_misc_cntl = ioread32(reset_unit_regs + ARC_MISC_CNTL_REG);
	iowrite32(arc_misc_cntl | ARC_MISC_CNTL_RESET_MASK,
			reset_unit_regs + ARC_MISC_CNTL_REG);
	udelay(1);
	iowrite32(arc_misc_cntl & ~ARC_MISC_CNTL_RESET_MASK,
			reset_unit_regs + ARC_MISC_CNTL_REG);
	return 0;
}

static int grayskull_arc_init(struct grayskull_device *gs_dev) {
	void __iomem *reset_unit_regs = gs_dev->reset_unit_regs;
	u32 gpio_val;
	int ret;

	gpio_val = ioread32(reset_unit_regs + GPIO_PAD_VAL_REG);
	if ((gpio_val & GPIO_ARC_SPI_BOOTROM_EN_MASK) == GPIO_ARC_SPI_BOOTROM_EN_MASK) {
		ret = wait_reg32_with_timeout(reset_unit_regs, reset_unit_regs + SCRATCH_REG(5),
						SCRATCH_5_ARC_BOOTROM_DONE, 1000);
		if (ret) {
			pr_warn("Timeout waiting for SPI bootrom init done.\n");
			goto grayskull_arc_init_err;
		}
	} else {
		pr_warn("SPI bootrom not enabled.\n");
		goto grayskull_arc_init_err;
	}

	if (arc_fw_override) {
		if (grayskull_load_arc_fw(gs_dev)) {
			pr_warn("ARC FW Override unsuccessful.\n");
			goto grayskull_arc_init_err;
		}
	}

	if (watchdog_fw_override) {
		if (grayskull_load_iccm_fw(gs_dev,
					GS_WATCHDOG_FW_NAME,
					GS_WATCHDOG_FW_CORE_ID,
					&gs_dev->watchdog_fw_reset_vec)) {
			pr_warn("Watchdog FW Override unsuccessful.\n");
			goto grayskull_arc_init_err;
		}
	}

	if (smbus_fw_override) {
		if (grayskull_load_iccm_fw(gs_dev,
					GS_SMBUS_FW_NAME,
					GS_SMBUS_FW_CORE_ID,
					&gs_dev->smbus_fw_reset_vec)) {
			pr_warn("Watchdog FW Override unsuccessful.\n");
			goto grayskull_arc_init_err;
		}
	}


	if (grayskull_populate_arc_if(gs_dev)) {
		pr_warn("Driver to ARC table init failed.\n");
		goto grayskull_arc_init_err;
	}

	if (toggle_arc_reset(reset_unit_regs))
		goto grayskull_arc_init_err;

	if (wait_reg32_with_timeout(reset_unit_regs, reset_unit_regs + SCRATCH_REG(5),
					SCRATCH_5_ARC_L2_DONE, 5000000)) {
		pr_warn("Timeout waiting for ARC FW initialization to complete.");
		goto grayskull_arc_init_err;
	}

	pr_info("ARC initialization done.\n");
	return 0;

grayskull_arc_init_err:
	pr_warn("ARC initialization failed.\n");
	return -1;
}

// Compute gs_dev->enabled_rows which has 1 bit set for each enabled row.
// Indexed by NOC0 Y coordinate. 0 and 6 are "disabled", the router setup
// code depends on this.
static void grayskull_harvesting_init(struct grayskull_device *gs_dev) {
	static const u8 fuse_row_to_noc0[] = { 5, 7, 4, 8, 3, 9, 2, 10, 1, 11 };

	u32 harvesting_fuses;
	u32 bad_mem_bits, bad_logic_bits, bad_row_bits;
	int i;

	harvesting_fuses = tensix_harvest_override;
	if (harvesting_fuses == 0xFFFFFFFF)
		harvesting_fuses = ioread32(gs_dev->reg_iomap + ARC_CSM_MEMORY_OFFSET + ARC_CSM_ROW_HARVESTING_OFFSET);
	if (harvesting_fuses == 0xFFFFFFFF)
		harvesting_fuses = 0;

	// harvesting_fuses contains 10 bits for bad rows due to memory failures
	// followed by 10 bits for bad rows due to logic failures.
	// These are physically-mapped in "bottom-up" order.

	bad_mem_bits = harvesting_fuses & 0x3FF;
	bad_logic_bits = (harvesting_fuses >> 10) & 0x3FF;
	bad_row_bits = bad_mem_bits | bad_logic_bits;

	gs_dev->enabled_rows = 0;
	for (i = 0; i < 10; i++) {
		if (!(bad_row_bits & (1 << i)))
			gs_dev->enabled_rows |= 1 << fuse_row_to_noc0[i];
	}

	// pr_info("harvesting enabled_rows = %08x\n", gs_dev->enabled_rows);
}

static u32 program_tlb(struct grayskull_device *gs_dev, unsigned int x, unsigned int y, unsigned int noc, u32 addr) {
	struct TLB_16M_REG tlb;

	tlb.low32 = 0;
	tlb.high32 = 0;

	tlb.local_offset = addr >> TLB_16M_SIZE_SHIFT;
	tlb.x_end = x;
	tlb.y_end = y;
	tlb.noc_sel = noc;

	// pr_info("TLB %08x %d-%d/%d @ %08x = %08x:%08x\n", tlb.local_offset, tlb.x_end, tlb.y_end, tlb.noc_sel, addr,
	// 	tlb.high32, tlb.low32);

	iowrite32(tlb.low32, gs_dev->reg_iomap + PCI_TLB_CONFIG_OFFSET + KERNEL_TLB_REGS);
	iowrite32(tlb.high32, gs_dev->reg_iomap + PCI_TLB_CONFIG_OFFSET + KERNEL_TLB_REGS + sizeof(u32));

	return addr % (1u << TLB_16M_SIZE_SHIFT);
}

// setup_noc_common handles two cases:
// 1. NOC registers are mapped at a fixed offset within BAR0
// 2. NOC registers are mapped through a TLB
// We don't have a single mapping that covers both the fixed mappings and the TLB windows so we must accept a pointer.
static void setup_noc_common(u8 __iomem *noc_reg_base, const u32 *router_cfg) {
	u32 reg;

	iowrite32(router_cfg[0], noc_reg_base + ROUTER_CFG_1);
	iowrite32(router_cfg[1], noc_reg_base + ROUTER_CFG_3);

	reg = ioread32(noc_reg_base + NIU_CFG_0);
	reg |= NIU_CLOCK_GATING_ENABLE;
	iowrite32(reg, noc_reg_base + NIU_CFG_0);

	reg = ioread32(noc_reg_base + ROUTER_CFG_0);
	reg |= ROUTER_CLOCK_GATING_ENABLE;
	reg |= ROUTER_MAX_BACKOFF_EXP;
	iowrite32(reg, noc_reg_base + ROUTER_CFG_0);
}

// noc0,1_reg_base are the BAR0-relative addresses of the NOC registers
static void setup_noc_by_address(struct grayskull_device *gs_dev, u32 noc0_reg_base, u32 noc1_reg_base, const u32 *router_cfg) {
	setup_noc_common(gs_dev->reg_iomap + noc0_reg_base - REG_IOMAP_START, router_cfg);
	setup_noc_common(gs_dev->reg_iomap + noc1_reg_base - REG_IOMAP_START, router_cfg+2);
}

// x,y are NOC0 coordinates. noc0,1_reg_base are the local addresses of the NOC registers
// router_cfg is { NOC0 ROUTER_CFG_1, NOC0 ROUTER_CFG_3, NOC1 ROUTER_CFG_1, NOC1 ROUTER_CFG_3 }
static void setup_noc_by_xy(struct grayskull_device *gs_dev,
			    unsigned int x, unsigned int y,
			    u32 noc0_reg_base, u32 noc1_reg_base,
			    const u32 *router_cfg) {
	unsigned int noc1_x = GRID_SIZE_X - x - 1;
	unsigned int noc1_y = GRID_SIZE_Y - y - 1;

	u32 tlb_offset;

	tlb_offset = program_tlb(gs_dev, x, y, 0, noc0_reg_base);
	setup_noc_common(gs_dev->kernel_tlb + tlb_offset, router_cfg);

	tlb_offset = program_tlb(gs_dev, noc1_x, noc1_y, 1, noc1_reg_base);
	setup_noc_common(gs_dev->kernel_tlb + tlb_offset, router_cfg+2);
}

// Set NIU_CFG_0 tile clock disable based on core harvesting.
static void set_tile_clock_disable(struct grayskull_device *gs_dev, unsigned int x, unsigned int y) {
	unsigned int noc1_x = GRID_SIZE_X - x - 1;
	unsigned int noc1_y = GRID_SIZE_Y - y - 1;

	bool enabled = ((gs_dev->enabled_rows & (1 << y)) != 0);

	u32 reg, tlb_offset;

	tlb_offset = program_tlb(gs_dev, x, y, 0, TENSIX_NOC0_REG_BASE);
	reg = ioread32(gs_dev->kernel_tlb + tlb_offset + NIU_CFG_0);
	if (enabled)
		reg &= ~NIU_TILE_CLOCK_DISABLE;
	else
		reg |= NIU_TILE_CLOCK_DISABLE;
	iowrite32(reg, gs_dev->kernel_tlb + tlb_offset + NIU_CFG_0);

	tlb_offset = program_tlb(gs_dev, noc1_x, noc1_y, 1, TENSIX_NOC1_REG_BASE);
	reg = ioread32(gs_dev->kernel_tlb + tlb_offset + NIU_CFG_0);
	if (enabled)
		reg &= ~NIU_TILE_CLOCK_DISABLE;
	else
		reg |= NIU_TILE_CLOCK_DISABLE;
	iowrite32(reg, gs_dev->kernel_tlb + tlb_offset + NIU_CFG_0);
}

#define TENSIX_NODE_TYPE 0
#define DRAM_NODE_TYPE 1
#define ARC_NODE_TYPE 2
#define PCI_NODE_TYPE 3
#define EXTRA_ROUTER_NODE_TYPE 4

#define D DRAM_NODE_TYPE
#define A ARC_NODE_TYPE
#define P PCI_NODE_TYPE
#define E EXTRA_ROUTER_NODE_TYPE
#define T TENSIX_NODE_TYPE

// This is indexed by NOC0 coordinates.
static const u8 node_types[GRID_SIZE_Y][GRID_SIZE_X] = {
	{ E, D, E, E, D, E, E, D, E, E, D, E, E, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ A, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ P, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, D, E, E, D, E, E, D, E, E, D, E, E, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
	{ E, T, T, T, T, T, T, T, T, T, T, T, T, },
};

#undef D
#undef A
#undef P
#undef E
#undef T

static void grayskull_noc_init(struct grayskull_device *gs_dev) {
	u32 router_cfg[4]; // NOC0 ROUTER_CFG_1, NOC0, ROUTER_CFG_3, NOC1 ROUTER_CFG_1, NOC1 ROUTER_CFG_3
	unsigned x, y;

	// NOC0 & NOC1 column broadcast disable bits (column 0 aka 12)
	router_cfg[0] = 1 << 0;
	router_cfg[2] = 1 << 12;

	// NOC0 & NOC1 row broadcast disable bits (rows 0, 6 and any disabled by harvesting)
	router_cfg[1] = ~gs_dev->enabled_rows & ((1 << GRID_SIZE_Y) - 1);
	router_cfg[3] = 0;
	for (y = 0; y < GRID_SIZE_Y; y++) {
		router_cfg[3] |= ((router_cfg[1] >> y) & 1) << (GRID_SIZE_Y - y - 1);
	}
	// pr_info("router_cfg %08x %08x %08x %08x\n", router_cfg[0], router_cfg[1], router_cfg[2], router_cfg[3]);

	for (y = 0; y < GRID_SIZE_Y; y++) {
		for (x = 0; x < GRID_SIZE_X; x++) {
			switch (node_types[y][x]) {
				case DRAM_NODE_TYPE:
					setup_noc_by_xy(gs_dev, x, y, DRAM_NOC0_REG_BASE, DRAM_NOC1_REG_BASE, router_cfg);
					break;

				case ARC_NODE_TYPE:
					setup_noc_by_address(gs_dev, ARC_NOC0_REG_BASE, ARC_NOC1_REG_BASE, router_cfg);
					break;

				case PCI_NODE_TYPE:
					setup_noc_by_address(gs_dev, PCI_NOC0_REG_BASE, PCI_NOC1_REG_BASE, router_cfg);
					break;

				case EXTRA_ROUTER_NODE_TYPE:
					setup_noc_by_xy(gs_dev, x, y, TENSIX_NOC0_REG_BASE, TENSIX_NOC1_REG_BASE, router_cfg);
					break;

				case TENSIX_NODE_TYPE:
					setup_noc_by_xy(gs_dev, x, y, TENSIX_NOC0_REG_BASE, TENSIX_NOC1_REG_BASE, router_cfg);
					set_tile_clock_disable(gs_dev, x, y);
					break;
			}
		}
	}
}

static bool grayskull_read_fw_version(struct grayskull_device *gs_dev, u32 *fw_version) {
	void __iomem *arc_return_reg = gs_dev->reset_unit_regs + SCRATCH_REG(3);

	if (!grayskull_send_arc_fw_message(gs_dev->reset_unit_regs, GS_FW_MSG_GET_VERSION, 10000, NULL))
		return false;

	*fw_version = ioread32(arc_return_reg);

	return true;
}

bool grayskull_read_fw_telemetry_offset(u8 __iomem *reset_unit_regs, u32 *offset) {
	u8 __iomem *arc_return_reg = reset_unit_regs + SCRATCH_REG(3);

	if (!grayskull_send_arc_fw_message(reset_unit_regs, GS_FW_MSG_GET_TELEMETRY_OFFSET, 10000, NULL))
		return false;

	*offset = ioread32(arc_return_reg);

	return true;
}


static const struct tt_hwmon_attr gs_hwmon_attributes[] = {
	{ hwmon_temp,  hwmon_temp_input,  0x64, 0,  GENMASK(15, 0), 64   },
	{ hwmon_temp,  hwmon_temp_max,    0x78, 0,  GENMASK(15, 0), 1000 },
	{ hwmon_in,    hwmon_in_input,    0x60, 0,  GENMASK(31, 0), 1    },
	{ hwmon_in,    hwmon_in_max,      0x74, 16, GENMASK(15, 0), 1    },
	{ hwmon_curr,  hwmon_curr_input,  0x70, 0,  GENMASK(15, 0), 1000 },
	{ hwmon_curr,  hwmon_curr_max,    0x70, 16, GENMASK(15, 0), 1000 },
	{ hwmon_power, hwmon_power_input, 0x6c, 0,  GENMASK(15, 0), 1000000 },
	{ hwmon_power, hwmon_power_max,   0x6c, 16, GENMASK(15, 0), 1000000 },
	{ .reg_offset = TT_HWMON_ATTR_END },
};

static const struct tt_hwmon_label gs_hwmon_labels[] = {
	{ hwmon_temp,  hwmon_temp_label,  "asic_temp" },
	{ hwmon_in,    hwmon_in_label,    "vcore"     },
	{ hwmon_curr,  hwmon_curr_label,  "current"   },
	{ hwmon_power, hwmon_power_label, "power"     },
	{ .name = NULL },
};

static const struct hwmon_channel_info *gs_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_MAX),
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT | HWMON_I_LABEL | HWMON_I_MAX),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL | HWMON_C_MAX),
	HWMON_CHANNEL_INFO(power, HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_MAX),
	NULL
};

static const struct hwmon_chip_info gs_hwmon_chip_info = {
	.ops = &tt_hwmon_ops,
	.info = gs_hwmon_info,
};

static void grayskull_hwmon_init(struct grayskull_device *gs_dev) {
	struct tenstorrent_device *tt_dev = &gs_dev->tt;
	struct device *dev = &tt_dev->pdev->dev;
	struct tt_hwmon_context *context = &tt_dev->hwmon_context;
	struct device *hwmon_device;
	u32 fw_version;
	u32 telemetry_offset;

	if (!grayskull_read_fw_version(gs_dev, &fw_version)) {
		dev_warn(dev, "Failed to read ARC FW version (this is normal for old firmware).\n");
		goto grayskull_hwmon_init_err;
	}

	if (fw_version <= 0x01030000) {
		dev_warn(dev, "ARC FW version %08X is too old for hwmon support.\n", fw_version);
		goto grayskull_hwmon_init_err;
	}

	if (!grayskull_read_fw_telemetry_offset(gs_dev->reset_unit_regs, &telemetry_offset))
		goto grayskull_hwmon_init_err;

	context->attributes = gs_hwmon_attributes;
	context->labels = gs_hwmon_labels;
	context->telemetry_base = gs_dev->reg_iomap + gs_arc_addr_to_sysreg(telemetry_offset);

	hwmon_device = devm_hwmon_device_register_with_info(dev, "grayskull", context, &gs_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon_device))
		goto grayskull_hwmon_init_err;


	return;

grayskull_hwmon_init_err:
	dev_warn(dev, "Failed to initialize hwmon.\n");
}

// This is shared with wormhole.
bool grayskull_shutdown_firmware(struct pci_dev *pdev, u8 __iomem* reset_unit_regs) {
	if (is_hardware_hung(pdev, reset_unit_regs))
		return false;

	if (!grayskull_send_arc_fw_message(reset_unit_regs, GS_FW_MSG_ASTATE3, 10000, NULL))
		return false;
	return true;
}

static void month_lookup(u32 days_into_year, u32* day, u32* month) {
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

void grayskull_send_curr_date(u8 __iomem* reset_unit_regs) {
	const u32 SECONDS_TO_2020 = 1577836800; // date -d "Jan 1, 2020 UTC" +%s
	const u32 DAYS_PER_FOUR_YEARS = 4*365 + 1;
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

	grayskull_send_arc_fw_message_with_args(reset_unit_regs, GS_FW_MSG_CURR_DATE,
						packed_datetime_low, packed_datetime_high, 1000, NULL);
}

static bool grayskull_init(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);

	gs_dev->reg_iomap = pci_iomap_range(gs_dev->tt.pdev, 0, REG_IOMAP_START, REG_IOMAP_LEN);
	gs_dev->kernel_tlb = pci_iomap_range(gs_dev->tt.pdev, KERNEL_TLB_BAR, KERNEL_TLB_START, KERNEL_TLB_LEN);

	if (gs_dev->reg_iomap == NULL || gs_dev->kernel_tlb == NULL) {
		if (gs_dev->reg_iomap != NULL)
			pci_iounmap(gs_dev->tt.pdev, gs_dev->reg_iomap);

		if (gs_dev->kernel_tlb != NULL)
			pci_iounmap(gs_dev->tt.pdev, gs_dev->kernel_tlb);
		return false;
	}

	gs_dev->reset_unit_regs = gs_dev->reg_iomap + RESET_UNIT_REG_OFFSET;

	return true;
}

static bool grayskull_init_hardware(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);

	if (arc_l2_is_running(gs_dev->reset_unit_regs)) {
		grayskull_send_arc_fw_message(gs_dev->reset_unit_regs, GS_FW_MSG_ASTATE0, 10000, NULL);
	} else if (!arc_fw_init) {
		pr_info("ARC initialization skipped.\n");
		return true;
	} else if (grayskull_arc_init(gs_dev) != 0) {
		return false;
	}

	grayskull_send_curr_date(gs_dev->reset_unit_regs);

	complete_pcie_init(&gs_dev->tt, gs_dev->reset_unit_regs);

	grayskull_harvesting_init(gs_dev);

	grayskull_noc_init(gs_dev);

	grayskull_hwmon_init(gs_dev);

	return true;
}

static void grayskull_cleanup(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);

	if (gs_dev->reset_unit_regs != NULL)
		grayskull_shutdown_firmware(tt_dev->pdev, gs_dev->reset_unit_regs);

	if (gs_dev->reg_iomap != NULL)
		pci_iounmap(gs_dev->tt.pdev, gs_dev->reg_iomap);

	if (gs_dev->kernel_tlb != NULL)
		pci_iounmap(gs_dev->tt.pdev, gs_dev->kernel_tlb);
}

static void grayskull_last_release_handler(struct tenstorrent_device *tt_dev) {
	struct grayskull_device *gs_dev = tt_dev_to_gs_dev(tt_dev);
	grayskull_send_arc_fw_message(gs_dev->reset_unit_regs,
					GS_FW_MSG_GO_LONG_IDLE,
					10000, NULL);

	// arg0 = 0 => release the PCIE mutex.
	grayskull_send_arc_fw_message_with_args(gs_dev->reset_unit_regs,
						GS_FW_MSG_TYPE_PCIE_MUTEX_ACQUIRE,
						0, 0, 10000, NULL);
}

struct tenstorrent_device_class grayskull_class = {
	.name = "Grayskull",
	.instance_size = sizeof(struct grayskull_device),
	.init_device = grayskull_init,
	.init_hardware = grayskull_init_hardware,
	.cleanup_device = grayskull_cleanup,
	.last_release_cb = grayskull_last_release_handler,
};
