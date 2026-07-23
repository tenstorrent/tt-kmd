// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TTDRIVER_FWLOG_H_INCLUDED
#define TTDRIVER_FWLOG_H_INCLUDED

// Wire contract for firmware log forwarding ("tt_pcie_log").
//
// Firmware writes framed log records into a host DMA buffer and raises a PCIe
// MSI.  The layout and constants below are shared with the firmware backend
// (tt-zephyr-platforms: lib/tenstorrent/bh_arc/tt_pcie_log.c) and must be kept
// in sync with it.
//
// This header (and its implementation, fwlog_parser.c) is compiled into both
// the kernel module and the host-side unit test (test/fwlog_test.cpp), so it
// must stay free of kernel-only dependencies.  The host build supplies the
// kernel integer names via test/fwlog_compat.h.
#include <linux/types.h>

// Metadata at the start of the shared buffer (16 bytes).
struct fw_log_buffer_header {
	u32 write_offset;	// End offset of valid payload bytes
	u32 buffer_size;	// Total buffer size in bytes
	u32 magic;		// FW_LOG_BUFFER_MAGIC when initialized by FW
	u8 owner;		// FW_LOG_BUFFER_OWNER_{FW,HOST}
	u8 version;		// Protocol version supported by FW
	u8 reserved[2];
} __packed;

// Header preceding each framed log record (12 bytes).
struct fw_log_entry_header {
	u16 msg_size;		// Total record size (this header + payload)
	u8 log_level;		// Zephyr log level, see FW_LOG_LEVEL_*
	u8 source;		// FW_LOG_SOURCE_{SMC,DMC}
	u32 timestamp;		// FW timestamp
	u32 sequence;		// Monotonic sequence number
} __packed;

// Zephyr log levels, as emitted by firmware.
#define FW_LOG_LEVEL_NONE	0
#define FW_LOG_LEVEL_ERROR	1
#define FW_LOG_LEVEL_WARN	2
#define FW_LOG_LEVEL_INFO	3
#define FW_LOG_LEVEL_DEBUG	4

#define FW_LOG_SOURCE_SMC	0
#define FW_LOG_SOURCE_DMC	1

#define FW_LOG_BUFFER_MAGIC		0x544C4F47	// "TLOG"
#define FW_LOG_BUFFER_OWNER_FW		0x0		// FW may write; Host has consumed
#define FW_LOG_BUFFER_OWNER_HOST	0x1		// FW has written; Host must consume

// Protocol version this driver implements.  Sent to FW on setup.
#define FW_LOG_PROTOCOL_VERSION		0

// Size of the host DMA buffer allocated for firmware logs.
#define FW_LOG_BUFFER_SIZE		4096

// A single decoded log record handed to the emit callback.
struct fw_log_record {
	u8 log_level;		// FW_LOG_LEVEL_*
	u8 source;		// FW_LOG_SOURCE_*
	u32 sequence;		// Record sequence number
	u32 timestamp;		// FW timestamp
	const char *text;	// NUL-terminated payload (may be empty)
	bool seq_gap;		// True if sequence != expected on entry
	u32 expected_seq;	// Expected sequence when a gap was detected
};

// Result of walking the framed records in a batch.
struct fw_log_parse_result {
	u32 records;		// Number of valid records emitted
	bool framing_error;	// True if iteration stopped on a bad record
	u32 error_offset;	// Offset of the bad record (when framing_error)
	u32 error_msg_size;	// msg_size of the bad record (when framing_error)
};

// Firmware's sequence counter is free-running and independent of host setup, so
// after setup KMD will just use firmware's current seq as it's baseline.
// Once valid, a mismatch against `next` is a genuine dropped-record gap.
struct fw_log_seq {
	u32 next;	// Next expected sequence number (meaningful once valid)
	bool valid;	// false until the first record establishes the baseline
};

u32 fw_log_sanitize(char *s);
const char *fw_log_source_str(u8 source);
bool fw_log_prepare_emit(const struct fw_log_record *rec, u32 log_level);

// Walk the framed log records the firmware published into `buf`.
//
// `buffer_size`     total size of the shared buffer.
// `write_offset`    end offset of valid bytes, as reported by firmware.
// `seq`             optional in/out sequence state for gap detection; may be
//                   NULL to skip it.  When seq->valid is 0 the first record
//                   establishes the baseline (no gap); afterwards a mismatch
//                   against seq->next is reported as a gap.
// `emit`            called once per valid record.  `ctx` is passed through.
//
// The payload of each record is forced to be NUL-terminated (the last payload
// byte is overwritten if needed), so this must be given a writable buffer.
// Iteration is strictly bounded by `write_offset` (itself clamped to
// `buffer_size`), so a malformed batch can never read past the buffer.
//
struct fw_log_parse_result
fw_log_parse(void *buf, u32 buffer_size, u32 write_offset, struct fw_log_seq *seq,
	     void (*emit)(void *ctx, const struct fw_log_record *rec), void *ctx);

#endif // TTDRIVER_FWLOG_H_INCLUDED
