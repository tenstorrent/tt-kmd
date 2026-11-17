// SPDX-License-Identifier: GPL-2.0-only
/*
 * Blackhole firmware log ("TLOG") parser.  See fwlog.h.
 *
 * Shared by the kernel module (C) and test/fwlog_test (C++, with
 * test/fwlog_compat.h force-included).  No libc on the kernel side.
 */

#ifdef __KERNEL__
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "fwlog.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define FW_LOG_ESC	0x1b

struct fw_log_parse_result fw_log_parse(u8 *buf, u32 buffer_size, u32 write_offset,
					struct fw_log_seq *seq, fw_log_emit_fn emit,
					void *ctx)
{
	const u32 hdr_size = sizeof(struct fw_log_buffer_header);
	const u32 ent_size = sizeof(struct fw_log_entry_header);
	struct fw_log_parse_result res;
	u32 end, off;

	memset(&res, 0, sizeof(res));

	if (!buf)
		return res;

	if (write_offset < hdr_size || write_offset > buffer_size)
		res.bad_write_offset = true;

	end = write_offset < buffer_size ? write_offset : buffer_size;
	off = hdr_size;

	/* off <= end here, so end - off cannot wrap. */
	while (off <= end && end - off >= ent_size) {
		struct fw_log_entry_header eh;
		struct fw_log_record rec;
		u32 text_len, n;
		char *text;

		/* Entries are not necessarily aligned: copy the header out. */
		memcpy(&eh, buf + off, ent_size);

		if (eh.msg_size < ent_size + 1 || eh.msg_size > end - off) {
			res.framing_error = true;
			res.error_offset = off;
			res.error_msg_size = eh.msg_size;
			break;
		}

		/* Payload is text + NUL; force the NUL in case firmware did not. */
		text = (char *)(buf + off + ent_size);
		text_len = eh.msg_size - ent_size - 1;
		text[text_len] = '\0';
		for (n = 0; n < text_len && text[n]; n++)
			;

		memset(&rec, 0, sizeof(rec));
		rec.log_level = eh.log_level;
		rec.source = eh.source;
		rec.sequence = eh.sequence;
		rec.timestamp = eh.timestamp;
		rec.text = text;
		rec.text_len = n;
		rec.offset = off;

		if (seq) {
			if (!seq->valid) {
				seq->next = eh.sequence;
				seq->valid = true;
			}
			rec.expected_seq = seq->next;
			rec.seq_gap = eh.sequence != seq->next;
			seq->next = eh.sequence + 1;
		} else {
			rec.expected_seq = eh.sequence;
		}

		if (emit)
			emit(ctx, &rec);
		res.records++;

		off += eh.msg_size;
	}

	return res;
}

static bool fw_log_is_csi_final(unsigned char c)
{
	return c >= 0x40 && c <= 0x7e;
}

u32 fw_log_sanitize_n(char *s, u32 len)
{
	u32 r = 0, w = 0;

	if (!s)
		return 0;

	while (r < len && s[r]) {
		unsigned char c = (unsigned char)s[r];

		if (c == FW_LOG_ESC) {
			r++;
			if (r >= len || !s[r])
				break;			/* lone trailing ESC */
			if (s[r] != '[') {
				r++;			/* ESC + one character */
				continue;
			}
			r++;				/* CSI: skip to final byte */
			while (r < len && s[r] &&
			       !fw_log_is_csi_final((unsigned char)s[r]))
				r++;
			if (r < len && s[r])
				r++;			/* consume the final byte */
			continue;
		}

		r++;
		if (c < 0x20 || c == 0x7f)
			continue;			/* control characters, DEL */
		s[w++] = c >= 0x80 ? '?' : (char)c;
	}
	if (w < len)
		s[w] = '\0';

	return w;
}

static u32 fw_log_strlen(const char *s)
{
	u32 n = 0;

	while (s[n])
		n++;
	return n;
}

u32 fw_log_sanitize(char *s)
{
	if (!s)
		return 0;
	/* s[len] is already NUL, so the result is always terminated. */
	return fw_log_sanitize_n(s, fw_log_strlen(s));
}

/*
 * Tokens the kernel-log checks treat as kernel failures.  A match is changed
 * in place without changing length: tokens containing upper case are lower
 * cased, all-lower-case tokens get an upper-case first letter.
 */
static const char *const fw_log_bad_tokens[] = {
	"WARNING:",
	"BUG:",
	"kernel BUG",
	"Oops",
	"Call Trace",
	"KASAN",
	"general protection",
	"refcount_t",
	"corruption",
};

/* Does tok occur at s, within the len bytes available? */
static bool fw_log_match(const char *s, u32 len, const char *tok)
{
	u32 i;

	for (i = 0; tok[i]; i++)
		if (i >= len || s[i] != tok[i])
			return false;
	return true;
}

static bool fw_log_has_upper(const char *tok)
{
	for (; *tok; tok++)
		if (*tok >= 'A' && *tok <= 'Z')
			return true;
	return false;
}

void fw_log_defang_n(char *s, u32 len)
{
	u32 i, t, j;

	if (!s)
		return;

	for (i = 0; i < len && s[i]; i++) {
		for (t = 0; t < ARRAY_SIZE(fw_log_bad_tokens); t++) {
			const char *tok = fw_log_bad_tokens[t];

			if (!fw_log_match(s + i, len - i, tok))
				continue;

			if (fw_log_has_upper(tok)) {
				for (j = 0; tok[j]; j++)
					if (s[i + j] >= 'A' && s[i + j] <= 'Z')
						s[i + j] = (char)(s[i + j] - 'A' + 'a');
			} else if (s[i] >= 'a' && s[i] <= 'z') {
				s[i] = (char)(s[i] - 'a' + 'A');
			}
		}
	}
}

void fw_log_defang(char *s)
{
	if (!s)
		return;
	fw_log_defang_n(s, fw_log_strlen(s));
}

bool fw_log_prepare_emit(const struct fw_log_record *rec, u32 max_level)
{
	u32 len;

	if (!rec || !rec->text)
		return false;

	if (rec->log_level != FW_LOG_LEVEL_NONE && rec->log_level > max_level)
		return false;

	len = fw_log_sanitize_n(rec->text, rec->text_len);
	if (len == 0)
		return false;

	fw_log_defang_n(rec->text, len);
	return true;
}

const char *fw_log_source_str(u8 source)
{
	switch (source) {
	case FW_LOG_SOURCE_SMC:
		return "SMC";
	case FW_LOG_SOURCE_DMC:
		return "DMC";
	default:
		return "FW";
	}
}
