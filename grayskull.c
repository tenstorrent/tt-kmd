// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// DEPRECATION NOTICE (July 2025)
//
// Grayskull devices are no longer supported by this driver. All logic and data
// structures specific to Grayskull hardware have been removed.
//
// These files (grayskull.c, grayskull.h) are temporarily retained because the
// Wormhole implementation depends on the shared firmware communication
// functions defined here (grayskull_send_arc_fw_message, etc.).
//
// TODO: Remove these files once wormhole.c is updated to either:
// 1. Use the new (Blackhole-style) ARC FW messaging mechanism, or
// 2. Re-home these functions to wormhole.c and rename them

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/timekeeping.h>
#include <linux/stat.h>

#include "grayskull.h"
#include "enumerate.h"

#define POST_CODE_REG SCRATCH_REG(0)
#define POST_CODE_MASK ((u32)0x3FFF)
#define POST_CODE_ARC_SLEEP 2
#define POST_CODE_ARC_L2 0xC0DE0000
#define POST_CODE_ARC_L2_MASK 0xFFFF0000

#define ARC_MISC_CNTL_REG 0x100
#define ARC_MISC_CNTL_RESET_MASK (1 << 12)
#define ARC_MISC_CNTL_IRQ0_MASK (1 << 16)
#define ARC_UDMIAXI_REGION_REG 0x10C
#define ARC_UDMIAXI_REGION_ICCM(n) (0x3 * (n))
#define ARC_UDMIAXI_REGION_CSM 0x10

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

static bool is_hardware_hung(struct pci_dev *pdev, u8 __iomem *reset_unit_regs) {
	u16 vendor_id;

	if (pdev != NULL
	    && (pci_read_config_word(pdev, PCI_VENDOR_ID, &vendor_id) != PCIBIOS_SUCCESSFUL
		|| vendor_id != PCI_VENDOR_ID_TENSTORRENT))
		return true;

	return (ioread32(reset_unit_regs + SCRATCH_REG(6)) == 0xFFFFFFFF);
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

bool grayskull_read_fw_telemetry_offset(u8 __iomem *reset_unit_regs, u32 *offset) {
	u8 __iomem *arc_return_reg = reset_unit_regs + SCRATCH_REG(3);

	if (!grayskull_send_arc_fw_message(reset_unit_regs, GS_FW_MSG_GET_TELEMETRY_OFFSET, 10000, NULL))
		return false;

	*offset = ioread32(arc_return_reg);

	return true;
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
