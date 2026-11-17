/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Firmware telemetry table (SCRATCH_RAM[13] table, SCRATCH_RAM[12] data).
 * See /specs/fw/blackhole/telemetry.md.
 */
#ifndef TT_TELEMETRY_H
#define TT_TELEMETRY_H

#include <linux/types.h>

struct tenstorrent_device;

/* Tags used by the driver. */
#define TT_TAG_VCORE			6	/* mV */
#define TT_TAG_TDP			7	/* W */
#define TT_TAG_TDC			8	/* A */
#define TT_TAG_VDD_LIMITS		9	/* [31:16] max mV, [15:0] min mV */
#define TT_TAG_THM_LIMIT_SHUTDOWN	10	/* degC */
#define TT_TAG_ASIC_TEMPERATURE		11	/* degC, signed 16.16 */
#define TT_TAG_TDC_LIMIT_MAX		55	/* A */
#define TT_TAG_THM_LIMIT_THROTTLE	56	/* degC */
#define TT_TAG_TDP_LIMIT_MAX		64	/* W */

#define TT_TELEMETRY_MAX_TAG		128	/* tags tracked (offset map size) */
#define TT_TELEMETRY_NO_OFFSET		0xffff

struct tt_telemetry_state {
	bool valid;
	u32 data_addr;				/* ARC address of the data array */
	u16 offset[TT_TELEMETRY_MAX_TAG];	/* tag -> word offset, or NO_OFFSET */
};

/* Table discovery (process context, firmware booted; called once by the SMC
 * late-init work before hwmon registration). Failure is not fatal:
 * telemetry is then simply unavailable. */
void tt_telemetry_init(struct tenstorrent_device *dev);

bool tt_telemetry_has(struct tenstorrent_device *dev, unsigned int tag);

/* Read one tag. Caller keeps the device attached. 0, -ENOENT (tag not
 * published), -ENODEV (no table / device gone) or a NOC access error. */
int tt_telemetry_read(struct tenstorrent_device *dev, unsigned int tag, u32 *val);

#endif
