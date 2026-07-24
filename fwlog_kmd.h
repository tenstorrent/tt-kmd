// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_FWLOG_KMD_H_INCLUDED
#define TTDRIVER_FWLOG_KMD_H_INCLUDED

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "fwlog.h"

// KMD-owned state for a firmware-log DMA buffer.  The wire format and parser
// declarations live in fwlog.h; this header contains kernel-only lifecycle
// state.
struct fw_log_state {
	struct device *dev;
	void *hw_ctx;
	void *buffer_virt;
	dma_addr_t buffer_dma;
	struct work_struct work;
	struct fw_log_seq seq;
	bool enabled;
};

// Hardware-control shims.  fwlog.c owns generic DMA-buffer lifecycle and
// parsing; the active device implementation supplies these ARC/NOC-specific
// operations in blackhole.c.
extern bool fw_log_hw_setup(void *hw_ctx, dma_addr_t buffer_dma, u32 buffer_size);
extern bool fw_log_hw_release(void *hw_ctx);

// Lifecycle, split into allocation vs. FW-interaction phases (mirroring the
// device class's init_device/init_hardware).  fw_log_alloc()/fw_log_free()
// own the DMA buffer for the device's lifetime; fw_log_setup()/
// fw_log_notify_release() do the per-hardware-init firmware handshake.
void fw_log_init(struct fw_log_state *log, struct device *dev, void *hw_ctx);
void fw_log_alloc(struct fw_log_state *log);
void fw_log_setup(struct fw_log_state *log);
void fw_log_notify_release(struct fw_log_state *log);
void fw_log_quiesce(struct fw_log_state *log);
void fw_log_free(struct fw_log_state *log);
void fw_log_interrupt(struct fw_log_state *log);

#endif // TTDRIVER_FWLOG_KMD_H_INCLUDED
