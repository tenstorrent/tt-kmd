/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Blackhole firmware log ("TLOG") framing and parser.
 *
 * This header and fwlog_parser.c are shared between the kernel module (built
 * as C) and the host-only unit test test/fwlog_test (built as C++ with
 * test/fwlog_compat.h force-included, which supplies u8/u16/u32 and __packed).
 * Keep both files free of libc and of anything C++ rejects.
 *
 * Firmware protocol (message 0xC7, SMC firmware >= 19.13/19.14):
 *
 *   request byte 0: FW_LOG_MSG_ID (0xC7)
 *   request byte 1: subcommand, FW_LOG_SUBCMD_SETUP or FW_LOG_SUBCMD_RELEASE
 *   request byte 2: host protocol version, FW_LOG_PROTOCOL_VERSION (0)
 *   request word 1/2: buffer IOVA low/high (used directly as NoC address on
 *                     translated tile (19, 24); must be < 2^58 and the buffer
 *                     must not cross a 16 MiB-aligned boundary)
 *   request word 3: buffer size in bytes, >= FW_LOG_MIN_BUFFER_SIZE (29)
 *
 *   result: 0 ok (RELEASE also when not set up), 1 buffer too small,
 *           19 (ENODEV) NoC translation disabled / recovery firmware,
 *           22 unknown subcommand, unknown-message reply (0xFF in data[0])
 *           when the backend is not built in.
 *
 * On SETUP the firmware writes a struct fw_log_buffer_header (write_offset =
 * 16, owner = FW) at the start of the buffer.  On every flush it checks that
 * magic is FW_LOG_BUFFER_MAGIC and owner is FW_LOG_BUFFER_OWNER_FW, writes a
 * batch of entries starting at offset 16 (truncated to buffer_size - 16),
 * sets write_offset = 16 + batch bytes, sets owner = FW_LOG_BUFFER_OWNER_HOST
 * and raises MSI vector FW_LOG_MSI_VECTOR.  Every batch starts again at 16.
 *
 * Host drain rules (kernel glue, fwlog.c):
 *   1. One drain at a time per device (mutex or a single work item); never
 *      parse or printk from hard IRQ context: the MSI / timer only schedules
 *      the drain work.
 *   2. Read the header from the DMA buffer; if magic is wrong or owner !=
 *      FW_LOG_BUFFER_OWNER_HOST there is nothing to do.  After seeing owner ==
 *      HOST, issue dma_rmb() before reading write_offset or any entry byte.
 *   3. Snapshot write_offset exactly once (READ_ONCE), then copy the bytes
 *      [0, min(write_offset, buffer_size)) into a host-private buffer of
 *      buffer_size + 1 bytes, NUL-terminate it, and call fw_log_parse() on
 *      that copy with buffer_size = the size the host allocated (never the
 *      firmware-written header field).  The parser writes NULs into the buffer
 *      it parses, and firmware must not be able to change bytes under it, so
 *      parsing the DMA buffer in place is not allowed.
 *   4. In the emit callback call fw_log_prepare_emit() and print the surviving
 *      records ratelimited, as "%s: %.*s" with fw_log_source_str(source),
 *      text_len and text (text is NUL-terminated at or before text_len).
 *   5. Hand the buffer back: barrier (dma_wmb()/wmb()) so all reads of the
 *      batch are complete, then write the 32-bit word at offset 12 with
 *      owner = FW_LOG_BUFFER_OWNER_FW (version kept).  Until then firmware
 *      drops batches.
 */

#ifndef TENSTORRENT_FWLOG_H_INCLUDED
#define TENSTORRENT_FWLOG_H_INCLUDED

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/build_bug.h>
#elif !defined(__cplusplus)
#include <stdbool.h>
#endif

#if defined(__KERNEL__) || defined(__cplusplus)
#define FW_LOG_STATIC_ASSERT(e, msg)	static_assert(e, msg)
#else
#define FW_LOG_STATIC_ASSERT(e, msg)	_Static_assert(e, msg)
#endif

/* ---- 0xC7 host log buffer message ---- */
#define FW_LOG_MSG_ID			0xC7
#define FW_LOG_SUBCMD_SETUP		1
#define FW_LOG_SUBCMD_RELEASE		2
#define FW_LOG_PROTOCOL_VERSION		0
/* buffer header (16) + one entry header (12) + one NUL byte */
#define FW_LOG_MIN_BUFFER_SIZE		29
/* MSI vector firmware raises after writing a batch */
#define FW_LOG_MSI_VECTOR		0
/* firmware result codes for 0xC7 */
#define FW_LOG_RC_OK			0
#define FW_LOG_RC_TOO_SMALL		1
#define FW_LOG_RC_NO_TRANSLATION	19
#define FW_LOG_RC_BAD_SUBCMD		22
/* NoC2AXI window: the buffer must not cross a boundary of this alignment */
#define FW_LOG_BUFFER_BOUNDARY		(1u << 24)

/*
 * Buffer size the driver allocates.  The firmware staging buffer is 4096
 * bytes by default; header + 4096 must fit so a full flush is never
 * truncated mid-record.  8 KiB (power of two) also keeps a coherent
 * allocation naturally aligned, so it cannot cross a 16 MiB boundary.
 */
#define FW_LOG_BUFFER_SIZE		8192

/* ---- wire format ---- */
#define FW_LOG_BUFFER_MAGIC		0x544C4F47u /* "TLOG" */
#define FW_LOG_BUFFER_OWNER_FW		0	/* firmware may write */
#define FW_LOG_BUFFER_OWNER_HOST	1	/* host must consume */

/* Zephyr log levels as carried in fw_log_entry_header.log_level */
#define FW_LOG_LEVEL_NONE		0
#define FW_LOG_LEVEL_ERROR		1
#define FW_LOG_LEVEL_WARN		2
#define FW_LOG_LEVEL_INFO		3
#define FW_LOG_LEVEL_DEBUG		4

#define FW_LOG_SOURCE_SMC		0
#define FW_LOG_SOURCE_DMC		1

struct fw_log_buffer_header {
	u32 write_offset;	/* end of valid payload, from buffer start */
	u32 buffer_size;
	u32 magic;		/* FW_LOG_BUFFER_MAGIC */
	u8 owner;		/* FW_LOG_BUFFER_OWNER_* */
	u8 version;
	u8 reserved[2];
} __packed;

struct fw_log_entry_header {
	u16 msg_size;		/* header + text + NUL */
	u8 log_level;		/* FW_LOG_LEVEL_* */
	u8 source;		/* FW_LOG_SOURCE_* */
	u32 timestamp;
	u32 sequence;		/* monotonic, also counts dropped messages */
} __packed;

FW_LOG_STATIC_ASSERT(sizeof(struct fw_log_buffer_header) == 16, "fw_log_buffer_header size");
FW_LOG_STATIC_ASSERT(sizeof(struct fw_log_entry_header) == 12, "fw_log_entry_header size");

/*
 * Sequence tracking state, persistent across batches.  While valid is false
 * there is no baseline (fresh attach / re-SETUP): the first record's sequence
 * is adopted without reporting a gap.  Afterwards next is the sequence number
 * expected for the next record.
 */
struct fw_log_seq {
	u32 next;
	bool valid;
};

/*
 * One decoded record, handed to the emit callback.  text points into the
 * parsed buffer; text_len is the number of bytes before the first NUL in the
 * payload, and text[text_len] lies inside the record.  The parser leaves
 * text[text_len] == NUL; consumers should still bound every access by
 * text_len (fw_log_prepare_emit does; print with "%.*s").  The text may be
 * modified in place until the callback returns.
 */
struct fw_log_record {
	u8 log_level;
	u8 source;
	u32 sequence;
	u32 timestamp;
	char *text;
	u32 text_len;		/* bytes before the NUL, excluding it */
	bool seq_gap;		/* sequence != expected_seq (records were lost) */
	u32 expected_seq;	/* sequence that was expected */
	u32 offset;		/* offset of the entry header in the buffer */
};

struct fw_log_parse_result {
	u32 records;		/* records passed to the callback */
	bool framing_error;	/* parsing stopped at a malformed entry */
	u32 error_offset;	/* offset of the malformed entry header */
	u32 error_msg_size;	/* its msg_size field */
	bool bad_write_offset;	/* write_offset was < 16 or > buffer_size */
};

typedef void (*fw_log_emit_fn)(void *ctx, const struct fw_log_record *rec);

/*
 * Parse the entries of one batch.
 *
 * buf/buffer_size: the whole log buffer including its 16-byte header;
 *   buffer_size must be the real size of buf (the parser never reads or
 *   writes outside [0, buffer_size)).
 * write_offset: end of valid data as reported by firmware; clamped to
 *   buffer_size (bad_write_offset is set when it was out of range).  Entries are parsed from offset 16 up to write_offset.
 *   Trailing bytes shorter than an entry header are ignored.
 * seq: sequence state (may be NULL to disable gap tracking).
 * emit: called once per well-formed record (may be NULL to only count).
 *
 * Each record's last payload byte is forced to NUL in buf.  An entry with
 * msg_size smaller than header + 1 or extending past write_offset is a
 * framing error: parsing stops and the result reports it.
 */
struct fw_log_parse_result fw_log_parse(u8 *buf, u32 buffer_size, u32 write_offset,
					struct fw_log_seq *seq, fw_log_emit_fn emit,
					void *ctx);

/*
 * Sanitize text in place for a single kernel log line: removes ANSI CSI
 * sequences (ESC '[' params... final), other ESC + one character sequences,
 * a trailing lone ESC, control characters (including \n \r \t) and DEL;
 * non-ASCII bytes become '?'.  Only s[0, len) is read (stopping early at a
 * NUL); if the result is shorter than len, s[result] is set to NUL.
 * Returns the new length.
 */
u32 fw_log_sanitize_n(char *s, u32 len);

/* fw_log_sanitize_n() on a NUL-terminated string. */
u32 fw_log_sanitize(char *s);

/*
 * Make sanitized text harmless for kernel log scanners: in-place,
 * length-preserving case changes of tokens such as "WARNING:", "BUG:",
 * "Oops", "Call Trace" that tooling treats as kernel failures.  Only s[0, len)
 * is accessed (stopping early at a NUL).
 */
void fw_log_defang_n(char *s, u32 len);

/* fw_log_defang_n() on a NUL-terminated string. */
void fw_log_defang(char *s);

/*
 * Output policy for one record: returns false if it must not be printed
 * (level above max_level, or empty after sanitizing).  FW_LOG_LEVEL_NONE
 * records are always kept.  Kept records have their text sanitized and
 * defanged in place, bounded by text_len; afterwards the text is
 * NUL-terminated at or before text_len.
 */
bool fw_log_prepare_emit(const struct fw_log_record *rec, u32 max_level);

/* "SMC", "DMC", or "FW" for an unknown source. */
const char *fw_log_source_str(u8 source);

#endif /* TENSTORRENT_FWLOG_H_INCLUDED */
