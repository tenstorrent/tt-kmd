// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only
//
// Host-side unit test for the firmware-log framing/parser (fwlog.h).
//
// This exercises the exact fw_log_parse() the driver uses, feeding it batches
// laid out the way firmware produces them, and checking the decoded records
// and framing-error handling.  It needs no hardware or kernel module, so it
// runs in the plain build-tests CI job.
//
// Build & run:  make -C test fwlog_test && ./test/fwlog_test

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "fwlog_compat.h"
#include "fwlog.h"

// ---- tiny non-aborting check framework (so every case runs) ----
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                             \
	do {                                                                    \
		++g_checks;                                                     \
		if (!(cond)) {                                                  \
			++g_failures;                                           \
			std::cerr << "FAIL " << __func__ << ":" << __LINE__     \
				  << ": " #cond "\n";                           \
		}                                                               \
	} while (0)

// ---- capture emitted records ----
struct Captured {
	uint8_t level;
	uint8_t source;
	uint32_t sequence;
	uint32_t timestamp;
	std::string text;
	int seq_gap;
	uint32_t expected_seq;
};

static void collect(void *ctx, const struct fw_log_record *r)
{
	auto *out = static_cast<std::vector<Captured> *>(ctx);
	out->push_back({ r->log_level, r->source, r->sequence, r->timestamp,
			 std::string(r->text), r->seq_gap, r->expected_seq });
}

// ---- helper that lays out a batch the way firmware does ----
struct Batch {
	std::vector<uint8_t> buf;

	explicit Batch(uint32_t buffer_size = FW_LOG_BUFFER_SIZE)
	{
		buf.assign(buffer_size, 0);
		hdr()->write_offset = sizeof(struct fw_log_buffer_header);
		hdr()->buffer_size = buffer_size;
		hdr()->magic = FW_LOG_BUFFER_MAGIC;
		hdr()->owner = FW_LOG_BUFFER_OWNER_HOST;
		hdr()->version = FW_LOG_PROTOCOL_VERSION;
	}

	struct fw_log_buffer_header *hdr()
	{
		return reinterpret_cast<struct fw_log_buffer_header *>(buf.data());
	}

	// Append a record with a NUL-terminated payload (firmware's normal case).
	void add(uint8_t level, uint8_t source, uint32_t seq, const std::string &text)
	{
		uint32_t payload = static_cast<uint32_t>(text.size()) + 1; // include NUL
		uint32_t off = write_entry(level, source, seq, payload);
		std::memcpy(buf.data() + off, text.c_str(), payload);
	}

	// Append a record whose payload is NOT NUL-terminated (malformed FW).
	void add_unterminated(uint8_t level, uint8_t source, uint32_t seq, uint32_t payload)
	{
		uint32_t off = write_entry(level, source, seq, payload);
		std::memset(buf.data() + off, 'A', payload); // no NUL anywhere
	}

	// Append an entry header with an explicit (possibly bogus) msg_size and
	// no payload bytes reserved beyond it.  Used to craft framing errors.
	void add_raw_header(uint16_t msg_size, uint32_t seq)
	{
		uint32_t off = hdr()->write_offset;
		auto *e = reinterpret_cast<struct fw_log_entry_header *>(buf.data() + off);
		e->msg_size = msg_size;
		e->sequence = seq;
		hdr()->write_offset = off + sizeof(struct fw_log_entry_header);
	}

	void set_write_offset(uint32_t v) { hdr()->write_offset = v; }

    private:
	uint32_t write_entry(uint8_t level, uint8_t source, uint32_t seq, uint32_t payload)
	{
		uint32_t off = hdr()->write_offset;
		auto *e = reinterpret_cast<struct fw_log_entry_header *>(buf.data() + off);
		e->msg_size = static_cast<uint16_t>(sizeof(struct fw_log_entry_header) + payload);
		e->log_level = level;
		e->source = source;
		e->timestamp = 1000 + seq;
		e->sequence = seq;
		hdr()->write_offset = off + e->msg_size;
		return off + sizeof(struct fw_log_entry_header);
	}
};

// Compatibility runner for the mid-session tests: the baseline is treated as
// already established at *expected (seq.valid = 1), so gap detection is active
// from the first record.  Writes the running counter back to *expected.
static struct fw_log_parse_result run(Batch &b, uint32_t *expected, std::vector<Captured> &out)
{
	struct fw_log_seq seq = { *expected, 1 };
	auto r = fw_log_parse(b.buf.data(), b.hdr()->buffer_size, b.hdr()->write_offset,
			      &seq, collect, &out);
	*expected = seq.next;
	return r;
}

// --------------------------- test cases ---------------------------

static void test_layout_contract()
{
	// Sizes must match the firmware's packed headers.
	static_assert(sizeof(struct fw_log_buffer_header) == 16, "buffer header size");
	static_assert(sizeof(struct fw_log_entry_header) == 12, "entry header size");

	CHECK(offsetof(struct fw_log_buffer_header, write_offset) == 0);
	CHECK(offsetof(struct fw_log_buffer_header, buffer_size) == 4);
	CHECK(offsetof(struct fw_log_buffer_header, magic) == 8);
	CHECK(offsetof(struct fw_log_buffer_header, owner) == 12);
	CHECK(offsetof(struct fw_log_buffer_header, version) == 13);

	CHECK(offsetof(struct fw_log_entry_header, msg_size) == 0);
	CHECK(offsetof(struct fw_log_entry_header, log_level) == 2);
	CHECK(offsetof(struct fw_log_entry_header, source) == 3);
	CHECK(offsetof(struct fw_log_entry_header, timestamp) == 4);
	CHECK(offsetof(struct fw_log_entry_header, sequence) == 8);

	CHECK(FW_LOG_BUFFER_MAGIC == 0x544C4F47);
	CHECK(FW_LOG_BUFFER_OWNER_FW == 0);
	CHECK(FW_LOG_BUFFER_OWNER_HOST == 1);
	CHECK(FW_LOG_LEVEL_ERROR == 1 && FW_LOG_LEVEL_WARN == 2 &&
	      FW_LOG_LEVEL_INFO == 3 && FW_LOG_LEVEL_DEBUG == 4);
	CHECK(FW_LOG_SOURCE_SMC == 0 && FW_LOG_SOURCE_DMC == 1);
}

static void test_single_record()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, "boot complete");

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(!r.framing_error);
	CHECK(r.records == 1);
	CHECK(out.size() == 1);
	CHECK(out[0].level == FW_LOG_LEVEL_INFO);
	CHECK(out[0].source == FW_LOG_SOURCE_SMC);
	CHECK(out[0].sequence == 0);
	CHECK(out[0].text == "boot complete");
	CHECK(out[0].seq_gap == 0);
	CHECK(expected == 1);
}

static void test_multiple_records()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, "one");
	b.add(FW_LOG_LEVEL_WARN, FW_LOG_SOURCE_SMC, 1, "two");
	b.add(FW_LOG_LEVEL_ERROR, FW_LOG_SOURCE_DMC, 2, "three");

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(!r.framing_error);
	CHECK(r.records == 3 && out.size() == 3);
	CHECK(out[0].text == "one" && out[1].text == "two" && out[2].text == "three");
	CHECK(out[2].source == FW_LOG_SOURCE_DMC);
	CHECK(out[0].seq_gap == 0 && out[1].seq_gap == 0 && out[2].seq_gap == 0);
	CHECK(expected == 3);
}

static void test_empty_batch()
{
	Batch b; // write_offset left at header size -> no records
	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(!r.framing_error);
	CHECK(r.records == 0 && out.empty());
	CHECK(expected == 0);
}

static void test_empty_payload()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, ""); // payload = just the NUL

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(!r.framing_error && r.records == 1);
	CHECK(out[0].text.empty());
}

static void test_sequence_gap()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 5, "late"); // expected 0, got 5
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 6, "next");

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(r.records == 2);
	CHECK(out[0].seq_gap == 1);
	CHECK(out[0].expected_seq == 0);
	CHECK(out[1].seq_gap == 0); // resynced
	CHECK(expected == 7);
}

// Option B: after a (re)attach the host has no baseline (seq.valid == 0), so
// the first record firmware sends is adopted as the baseline instead of
// producing a spurious gap against the host's fresh counter.
static void test_seq_baseline_on_attach()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 34, "first after reattach");
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 35, "next");

	struct fw_log_seq seq = { 0, 0 }; // baseline not yet established
	std::vector<Captured> out;
	auto r = fw_log_parse(b.buf.data(), b.hdr()->buffer_size, b.hdr()->write_offset,
			      &seq, collect, &out);

	CHECK(r.records == 2);
	CHECK(out[0].seq_gap == 0); // adopted 34, no spurious warning
	CHECK(out[1].seq_gap == 0);
	CHECK(seq.valid == 1);
	CHECK(seq.next == 36);
}

// A genuine drop is still detected once the baseline has been adopted.
static void test_seq_gap_after_baseline()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 34, "baseline");
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 40, "after drop"); // lost 35-39

	struct fw_log_seq seq = { 0, 0 };
	std::vector<Captured> out;
	auto r = fw_log_parse(b.buf.data(), b.hdr()->buffer_size, b.hdr()->write_offset,
			      &seq, collect, &out);

	CHECK(r.records == 2);
	CHECK(out[0].seq_gap == 0);          // baseline adopted
	CHECK(out[1].seq_gap == 1);          // real gap reported
	CHECK(out[1].expected_seq == 35);
	CHECK(seq.next == 41);               // resynced past the gap
}

// The baseline persists across batches: a first batch establishes it, and a
// contiguous second batch (parsed with the same seq state) shows no gap.
static void test_seq_baseline_persists_across_batches()
{
	struct fw_log_seq seq = { 0, 0 };

	Batch b1;
	b1.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 100, "b1");
	std::vector<Captured> out1;
	fw_log_parse(b1.buf.data(), b1.hdr()->buffer_size, b1.hdr()->write_offset,
		     &seq, collect, &out1);
	CHECK(out1[0].seq_gap == 0);
	CHECK(seq.next == 101);

	Batch b2;
	b2.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 101, "b2");
	std::vector<Captured> out2;
	fw_log_parse(b2.buf.data(), b2.hdr()->buffer_size, b2.hdr()->write_offset,
		     &seq, collect, &out2);
	CHECK(out2[0].seq_gap == 0); // contiguous, no spurious gap
	CHECK(seq.next == 102);
}

static void test_zero_msg_size()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, "ok");
	uint32_t bad_off = b.hdr()->write_offset;
	b.add_raw_header(0, 1); // msg_size 0 -> framing error

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(r.records == 1 && out.size() == 1); // the good one before the bad one
	CHECK(r.framing_error);
	CHECK(r.error_offset == bad_off);
	CHECK(r.error_msg_size == 0);
}

static void test_msg_size_too_small()
{
	Batch b;
	uint32_t bad_off = b.hdr()->write_offset;
	b.add_raw_header(5, 0); // < sizeof(entry header)

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(r.records == 0);
	CHECK(r.framing_error && r.error_offset == bad_off && r.error_msg_size == 5);
}

static void test_msg_size_overruns()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, "good");
	// Craft the largest possible wire size with no corresponding payload.
	uint32_t bad_off = b.hdr()->write_offset;
	b.add_raw_header(0xffff, 1);
	// write_offset only advanced by the header, so msg_size overruns it.

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(r.records == 1);            // the valid record was emitted
	CHECK(r.framing_error && r.error_offset == bad_off);
	CHECK(r.error_msg_size == 0xffff);
}

static void test_truncated_trailing_header()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, "only");
	// Pretend firmware left a few dangling bytes (< entry header) at the end.
	b.set_write_offset(b.hdr()->write_offset + 4);

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	// Loop condition stops before reading a partial header -> no error, no crash.
	CHECK(r.records == 1);
	CHECK(!r.framing_error);
}

static void test_forces_nul_termination()
{
	Batch b;
	b.add_unterminated(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, 8); // 8 'A's, no NUL

	uint32_t expected = 0;
	std::vector<Captured> out;
	auto r = run(b, &expected, out);

	CHECK(r.records == 1);
	CHECK(out[0].text.size() == 7);      // last byte forced to NUL
	CHECK(out[0].text == "AAAAAAA");
}

static void test_write_offset_clamped()
{
	Batch b;
	b.add(FW_LOG_LEVEL_INFO, FW_LOG_SOURCE_SMC, 0, "hi");
	// Firmware reports a wildly-too-large write_offset; parser must clamp to
	// buffer_size and never read past the allocation (ASAN would catch it).
	b.set_write_offset(0xFFFFFFF0u);

	uint32_t expected = 0;
	std::vector<Captured> out;
	run(b, &expected, out);

	// It should stop cleanly at the first malformed/zeroed record within the
	// buffer rather than running off the end.
	CHECK(out.size() >= 1);
	CHECK(out[0].text == "hi");
}

// --------------------- sanitizer unit tests ----------------------

static std::string sanitized(const std::string &in)
{
	std::vector<char> buf(in.begin(), in.end());
	buf.push_back('\0');
	uint32_t len = fw_log_sanitize(buf.data());
	std::string out(buf.data());
	CHECK(out.size() == len); // returned length matches resulting C-string
	return out;
}

static void test_sanitize_plain_text_unchanged()
{
	CHECK(sanitized("boot complete") == "boot complete");
	CHECK(sanitized("") == "");
	CHECK(sanitized("with spaces and 123!") == "with spaces and 123!");
}

static void test_sanitize_strips_ansi()
{
	CHECK(sanitized("\x1b[0m<inf> hello\x1b[0m") == "<inf> hello"); // colour wrap
	CHECK(sanitized("\x1b[1;31mred\x1b[0m") == "red");              // multi-param SGR
	CHECK(sanitized("\x1b[0m") == "");                             // bare reset -> empty
}

static void test_sanitize_strips_control_chars()
{
	CHECK(sanitized("line one\nline two") == "line oneline two"); // embedded LF
	CHECK(sanitized("a\r\nb") == "ab");                            // CRLF
	CHECK(sanitized("tab\there") == "tabhere");                    // TAB
	CHECK(sanitized("del\x7f end") == "del end");                  // DEL
}

static void test_sanitize_two_char_escape()
{
	CHECK(sanitized("a\x1b" "cb") == "ab"); // ESC + one char dropped
	CHECK(sanitized("done\x1b") == "done"); // trailing lone ESC, no overread
}

static void test_sanitize_truncated_ansi()
{
	CHECK(sanitized("keep\x1b[0") == "keep"); // CSI with no final byte
}

// ------------- full-pipeline (parse + policy) tests --------------
//
// These exercise the exact path the driver uses end to end: fw_log_parse()
// framing -> fw_log_prepare_emit() verbosity gate + sanitize -> the rendered
// "SOURCE: text" line the driver hands to printk. This is the closest thing
// to an integration test that runs without hardware.

struct Rendered {
	std::string line; // "SMC: text", as printk would receive it
	uint8_t level;    // severity it would be printed at
};

struct PipelineCtx {
	uint32_t max_level;
	std::vector<Rendered> *out;
};

static void pipeline_emit(void *ctx, const struct fw_log_record *r)
{
	auto *pc = static_cast<PipelineCtx *>(ctx);
	if (!fw_log_prepare_emit(r, pc->max_level))
		return;
	pc->out->push_back({ std::string(fw_log_source_str(r->source)) + ": " + r->text,
			     r->log_level });
}

static std::vector<Rendered> run_pipeline(Batch &b, uint32_t max_level)
{
	std::vector<Rendered> out;
	PipelineCtx pc{ max_level, &out };
	struct fw_log_seq seq = { 0, 0 };
	fw_log_parse(b.buf.data(), b.hdr()->buffer_size, b.hdr()->write_offset,
		     &seq, pipeline_emit, &pc);
	return out;
}

// A realistic mixed batch: ANSI colours, CRLF, embedded newlines, a record
// that sanitizes to nothing, and mixed levels/sources.
static void fill_golden(Batch &b)
{
	b.add(FW_LOG_LEVEL_ERROR, FW_LOG_SOURCE_SMC, 0, "\x1b[0m<err> boot fail\x1b[0m");
	b.add(FW_LOG_LEVEL_WARN,  FW_LOG_SOURCE_DMC, 1, "temp high\r\n");
	b.add(FW_LOG_LEVEL_INFO,  FW_LOG_SOURCE_SMC, 2, "\x1b[32minfo line\x1b[0m");
	b.add(FW_LOG_LEVEL_DEBUG, FW_LOG_SOURCE_SMC, 3, "verbose");
	b.add(FW_LOG_LEVEL_INFO,  FW_LOG_SOURCE_SMC, 4, "\x1b[0m"); // -> empty, dropped
	b.add(FW_LOG_LEVEL_ERROR, FW_LOG_SOURCE_SMC, 5, "multi\nline\nmsg");
}

static void test_pipeline_default_warn()
{
	Batch b;
	fill_golden(b);
	auto out = run_pipeline(b, FW_LOG_LEVEL_WARN);

	CHECK(out.size() == 3); // err + warn + err; info/debug/empty dropped
	CHECK(out[0].line == "SMC: <err> boot fail" && out[0].level == FW_LOG_LEVEL_ERROR);
	CHECK(out[1].line == "DMC: temp high"       && out[1].level == FW_LOG_LEVEL_WARN);
	CHECK(out[2].line == "SMC: multilinemsg"    && out[2].level == FW_LOG_LEVEL_ERROR);
}

static void test_pipeline_info_level()
{
	Batch b;
	fill_golden(b);
	auto out = run_pipeline(b, FW_LOG_LEVEL_INFO);

	CHECK(out.size() == 4); // err, warn, info, err; debug + empty dropped
	CHECK(out[0].line == "SMC: <err> boot fail");
	CHECK(out[1].line == "DMC: temp high");
	CHECK(out[2].line == "SMC: info line" && out[2].level == FW_LOG_LEVEL_INFO);
	CHECK(out[3].line == "SMC: multilinemsg");
}

static void test_pipeline_debug_level()
{
	Batch b;
	fill_golden(b);
	auto out = run_pipeline(b, FW_LOG_LEVEL_DEBUG);

	CHECK(out.size() == 5); // everything except the sanitized-empty record
	CHECK(out[3].line == "SMC: verbose" && out[3].level == FW_LOG_LEVEL_DEBUG);
}

static void test_pipeline_level_none_always_kept()
{
	// A record tagged NONE (0) does not fit the severity scale and must be
	// printed even at the strictest threshold.
	Batch b;
	b.add(FW_LOG_LEVEL_NONE, FW_LOG_SOURCE_SMC, 0, "raw printk line");
	auto out = run_pipeline(b, FW_LOG_LEVEL_ERROR);

	CHECK(out.size() == 1);
	CHECK(out[0].line == "SMC: raw printk line");
}

int main()
{
	test_layout_contract();
	test_single_record();
	test_multiple_records();
	test_empty_batch();
	test_empty_payload();
	test_sequence_gap();
	test_seq_baseline_on_attach();
	test_seq_gap_after_baseline();
	test_seq_baseline_persists_across_batches();
	test_zero_msg_size();
	test_msg_size_too_small();
	test_msg_size_overruns();
	test_truncated_trailing_header();
	test_forces_nul_termination();
	test_write_offset_clamped();
	test_sanitize_plain_text_unchanged();
	test_sanitize_strips_ansi();
	test_sanitize_strips_control_chars();
	test_sanitize_two_char_escape();
	test_sanitize_truncated_ansi();
	test_pipeline_default_warn();
	test_pipeline_info_level();
	test_pipeline_debug_level();
	test_pipeline_level_none_always_kept();

	std::cout << (g_failures ? "FAILED" : "PASSED") << ": " << (g_checks - g_failures)
		  << "/" << g_checks << " checks passed\n";
	return g_failures ? 1 : 0;
}
