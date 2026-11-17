/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Firmware log forwarding (kernel glue for the 0xC7 host log buffer; the
 * buffer format and parser are in fwlog.h / fwlog_parser.c).
 */
#ifndef TT_FWLOG_DRV_H
#define TT_FWLOG_DRV_H

#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/ratelimit.h>

#include "fwlog.h"

struct tenstorrent_device;

struct tt_fwlog_state {
	void *buf;			/* coherent DMA buffer, FW_LOG_BUFFER_SIZE */
	dma_addr_t iova;
	bool registered;		/* firmware may write the buffer (SETUP sent) */
	bool active;			/* draining enabled; READ_ONCE in IRQ */
	struct delayed_work work;	/* the one drain context */
	u8 *copy;			/* private parse copy, FW_LOG_BUFFER_SIZE + 1 */
	char *line;			/* output line scratch */
	struct fw_log_seq seq;
	struct ratelimit_state rs;
	struct tenstorrent_device *dev;
};

/* Device init, software only (so fini is always safe). */
void tt_fwlog_setup(struct tenstorrent_device *dev);
/* From the SMC late-init work, firmware booted: allocate and SETUP. */
void tt_fwlog_start(struct tenstorrent_device *dev);
/*
 * Before the SMC queue shuts down; IRQ already freed, late-init work
 * cancelled. The buffer is freed only after the firmware acknowledged
 * RELEASE; without that it is leaked (bus mastering is still on here, and a
 * firmware that never answered may still write it).
 */
void tt_fwlog_fini(struct tenstorrent_device *dev);
/* Hard IRQ: schedule a drain. */
void tt_fwlog_irq(struct tenstorrent_device *dev);

#endif
