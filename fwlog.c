// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/compiler.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "fwlog_kmd.h"
#include "ioctl.h"
#include "memory.h"
#include "module.h"

// Wire contract with the firmware backend.
static_assert(sizeof(struct fw_log_buffer_header) == 16, "fw_log_buffer_header size mismatch");
static_assert(sizeof(struct fw_log_entry_header) == 12, "fw_log_entry_header size mismatch");

// Emit one decoded record to the kernel log. Invoked by fw_log_parse().
static void fw_log_emit_to_kernel(void *ctx, const struct fw_log_record *rec)
{
	struct device *dev = ctx;
	const char *source_str = fw_log_source_str(rec->source);

	if (rec->seq_gap)
		dev_warn(dev, "FW log: sequence gap (expected %u, got %u)\n",
			 rec->expected_seq, rec->sequence);

	if (!fw_log_prepare_emit(rec, fw_log_level))
		return;

	switch (rec->log_level) {
	case FW_LOG_LEVEL_ERROR:
		dev_err(dev, "%s: %s", source_str, rec->text);
		break;
	case FW_LOG_LEVEL_WARN:
		dev_warn(dev, "%s: %s", source_str, rec->text);
		break;
	case FW_LOG_LEVEL_INFO:
		dev_info(dev, "%s: %s", source_str, rec->text);
		break;
	case FW_LOG_LEVEL_DEBUG:
	default:
		dev_dbg(dev, "%s: %s", source_str, rec->text);
		break;
	}
}

// Drain the framed logs firmware has published into the shared buffer.
static void fw_log_work_handler(struct work_struct *work)
{
	struct fw_log_state *log = container_of(work, struct fw_log_state, work);
	struct fw_log_buffer_header *buf_header;
	struct fw_log_parse_result result;

	if (!READ_ONCE(log->enabled) || !log->buffer_virt)
		return;

	buf_header = log->buffer_virt;

	// Gate on the firmware's ownership handoff before touching anything else
	// in the buffer.
	if (READ_ONCE(buf_header->owner) != FW_LOG_BUFFER_OWNER_HOST)
		return;

	// The ownership read above must be observed before metadata and payload
	// reads below.
	dma_rmb();

	if (buf_header->magic != FW_LOG_BUFFER_MAGIC) {
		// Disable rather than return: other interrupt sources (e.g. ML)
		// keep waking this handler, and without disabling we would log
		// this on every one of them.  A device reset re-runs fw_log_setup
		// and re-enables.
		dev_err(log->dev, "FW log buffer corrupted (magic=0x%08x); disabling until reset\n",
			buf_header->magic);
		WRITE_ONCE(log->enabled, false);
		return;
	}

	result = fw_log_parse(log->buffer_virt, FW_LOG_BUFFER_SIZE, buf_header->write_offset,
			      &log->seq, fw_log_emit_to_kernel, log->dev);

	if (result.framing_error)
		dev_warn(log->dev, "FW log: bad record at offset %u (size %u)\n",
			 result.error_offset, result.error_msg_size);

	// Every payload read above must complete before the store below becomes
	// visible to firmware.
	mb();
	WRITE_ONCE(buf_header->owner, FW_LOG_BUFFER_OWNER_FW);
}

void fw_log_init(struct fw_log_state *log, struct device *dev, void *hw_ctx)
{
	log->dev = dev;
	log->hw_ctx = hw_ctx;
	INIT_WORK(&log->work, fw_log_work_handler);
}

// Allocation-only phase (analogous to init_device): reserve the shared DMA
// buffer.  Split from fw_log_setup() so buffer ownership follows the device
// lifetime; the buffer is freed only by fw_log_free().  Safe to call once.
void fw_log_alloc(struct fw_log_state *log)
{
	if (!fw_logging)
		return;

	// Firmware DMAs into this buffer asynchronously and may ignore a release
	// handshake after a failure.  Without an IOMMU a stray write could corrupt
	// freed memory; IOMMU translation converts it into a fault instead.
	if (!is_iommu_translated(log->dev)) {
		dev_info(log->dev, "FW log forwarding disabled: requires IOMMU translation\n");
		return;
	}

	if (log->buffer_virt)
		return;

	log->buffer_virt = dma_alloc_coherent(log->dev, FW_LOG_BUFFER_SIZE,
					      &log->buffer_dma, GFP_KERNEL);
	if (!log->buffer_virt)
		dev_warn(log->dev, "FW log: buffer allocation failed\n");
}

// FW-interaction phase (analogous to init_hardware): hand the buffer to
// firmware and start forwarding.  Re-runs on resume.  On any failure the
// (already-allocated) buffer is left in place, just not enabled.
void fw_log_setup(struct fw_log_state *log)
{
	struct fw_log_buffer_header *buf_header;

	if (!log->buffer_virt)
		return;

	memset(log->buffer_virt, 0, FW_LOG_BUFFER_SIZE);
	log->seq = (struct fw_log_seq){ 0 };

	if (!fw_log_hw_setup(log->hw_ctx, log->buffer_dma, FW_LOG_BUFFER_SIZE)) {
		dev_dbg(log->dev, "FW log: setup message not acknowledged, unsupported FW\n");
		return;
	}

	// Firmware initializes the header synchronously before acknowledging
	// setup, so validate the framing contract before publishing enable.
	buf_header = log->buffer_virt;
	dma_rmb();
	if (buf_header->magic != FW_LOG_BUFFER_MAGIC) {
		dev_warn(log->dev, "FW log: firmware did not initialize buffer (magic=0x%08x); disabling\n",
			 buf_header->magic);
		return;
	}
	if (buf_header->version != FW_LOG_PROTOCOL_VERSION) {
		dev_warn(log->dev, "FW log: protocol version mismatch (fw=%u, kmd=%u); disabling\n",
			 buf_header->version, FW_LOG_PROTOCOL_VERSION);
		return;
	}

	smp_wmb();
	WRITE_ONCE(log->enabled, true);
	dev_info(log->dev, "FW log forwarding enabled\n");
}

void fw_log_notify_release(struct fw_log_state *log)
{
	if (!log->buffer_virt)
		return;

	if (!fw_log_hw_release(log->hw_ctx))
		dev_dbg(log->dev, "FW log: release message not acknowledged\n");
}

void fw_log_quiesce(struct fw_log_state *log)
{
	WRITE_ONCE(log->enabled, false);
	cancel_work_sync(&log->work);
}

// Allocation teardown (analogous to cleanup_device): quiesce the worker and
// release the DMA buffer.  Pairs with fw_log_alloc().
void fw_log_free(struct fw_log_state *log)
{
	if (!log->buffer_virt)
		return;

	fw_log_quiesce(log);
	dma_free_coherent(log->dev, FW_LOG_BUFFER_SIZE, log->buffer_virt, log->buffer_dma);
	log->buffer_virt = NULL;
}

void fw_log_interrupt(struct fw_log_state *log)
{
	if (READ_ONCE(log->enabled))
		schedule_work(&log->work);
}
