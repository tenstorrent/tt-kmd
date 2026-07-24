// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Pure firmware-log framing/parse/sanitize logic.  This file is compiled into
// both the kernel module and the host-side unit test (test/fwlog_test.cpp), so
// it must stay free of kernel-only dependencies.

#include "fwlog.h"

u32 fw_log_sanitize(char *s)
{
	char *dst = s;
	const char *src = s;

	while (*src) {
		unsigned char c = (unsigned char)*src;

		if (c == 0x1b) {
			src++;
			if (*src == '[') {
				src++;
				while (*src && ((unsigned char)*src < 0x40 ||
						(unsigned char)*src > 0x7e))
					src++;
				if (*src)
					src++;
			} else if (*src) {
				src++;
			}
			continue;
		}

		if (c < 0x20 || c == 0x7f) {
			src++;
			continue;
		}

		*dst++ = (char)c;
		src++;
	}

	*dst = '\0';
	return dst - s;
}

const char *fw_log_source_str(u8 source)
{
	return source == FW_LOG_SOURCE_DMC ? "DMC" : "SMC";
}

bool fw_log_prepare_emit(const struct fw_log_record *rec, u32 log_level)
{
	// Skip empty payloads.  This also guards fw_log_sanitize() below from
	// writing to the read-only "" literal that fw_log_parse() assigns when
	// a record carries no payload bytes.
	if (rec->text[0] == '\0')
		return false;

	if (rec->log_level > log_level)
		return false;

	return fw_log_sanitize((char *)rec->text) != 0;
}

struct fw_log_parse_result
fw_log_parse(void *buf, u32 buffer_size, u32 write_offset, struct fw_log_seq *seq,
	     void (*emit)(void *ctx, const struct fw_log_record *rec), void *ctx)
{
	struct fw_log_parse_result result = { 0 };
	u32 read_offset = sizeof(struct fw_log_buffer_header);

	if (write_offset > buffer_size)
		write_offset = buffer_size;

	if (write_offset < read_offset)
		return result;

	while (write_offset - read_offset >= sizeof(struct fw_log_entry_header)) {
		struct fw_log_entry_header *entry =
			(struct fw_log_entry_header *)((char *)buf + read_offset);
		u16 msg_size = entry->msg_size;
		struct fw_log_record rec = { 0 };
		char *payload;
		u32 payload_len;

		// Subtraction is safe because read_offset <= write_offset.  Avoid
		// addition here so even an untrusted size cannot wrap the bound.
		if (msg_size < sizeof(struct fw_log_entry_header) ||
		    msg_size > write_offset - read_offset) {
			result.framing_error = true;
			result.error_offset = read_offset;
			result.error_msg_size = msg_size;
			break;
		}

		if (seq) {
			if (!seq->valid) {
				// Firmware's sequence counter is free-running, so the
				// first record establishes the host baseline.
				seq->next = entry->sequence;
				seq->valid = true;
			}
			if (entry->sequence != seq->next) {
				rec.seq_gap = true;
				rec.expected_seq = seq->next;
				seq->next = entry->sequence;
			}
			seq->next++;
		}

		payload = (char *)entry + sizeof(struct fw_log_entry_header);
		payload_len = msg_size - sizeof(struct fw_log_entry_header);
		if (payload_len > 0)
			payload[payload_len - 1] = '\0';

		rec.log_level = entry->log_level;
		rec.source = entry->source;
		rec.sequence = entry->sequence;
		rec.timestamp = entry->timestamp;
		rec.text = payload_len > 0 ? payload : "";

		if (emit)
			emit(ctx, &rec);
		result.records++;

		read_offset += msg_size;
	}

	return result;
}
