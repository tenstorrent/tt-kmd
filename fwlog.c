// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware log forwarding: hands the firmware a host log buffer with message
 * 0xC7 SETUP and forwards the records it writes to the kernel log, following
 * the drain rules in fwlog.h. The MSI (vector 0) and a 1 s fallback poll only
 * schedule the drain work; parsing and printing happen in that work item.
 */
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "device.h"
#include "smc.h"
#include "fwlog.h"
#include "fwlog_drv.h"

static unsigned int fw_log_level = FW_LOG_LEVEL_WARN;
module_param(fw_log_level, uint, 0644);
MODULE_PARM_DESC(fw_log_level,
		 "Most verbose firmware log level forwarded: 0 none, 1 error, 2 warning (default), 3 info, 4 debug");

#define FWLOG_POLL_INTERVAL	HZ
#define FWLOG_LINE_SIZE		(FW_LOG_BUFFER_SIZE + 32)
#define FWLOG_OFF_WRITE_OFFSET	0
#define FWLOG_OFF_MAGIC		8
#define FWLOG_OFF_OWNER_WORD	12

/*
 * Strings that kernel log scanners treat as kernel failures. fw_log_defang()
 * already alters them, but crafted text can still assemble one (its changes
 * are case flips); as the last step before printk, any match left in the
 * whole output line loses its first character. '#' occurs in no token, so a
 * replacement cannot create a new match.
 */
static const char *const fwlog_triggers[] = {
	"BUG:", "WARNING:", "Oops", "general protection", "KASAN", "refcount_t",
	"list_add corruption", "list_del corruption", "kernel BUG", "Call Trace",
};

static void fwlog_scrub(char *line)
{
	unsigned int i;
	char *p;

	for (i = 0; i < ARRAY_SIZE(fwlog_triggers); i++)
		while ((p = strstr(line, fwlog_triggers[i])))
			*p = '#';
}

static u32 fwlog_read32(const void *buf, unsigned int off)
{
	return le32_to_cpu(READ_ONCE(*(const __le32 *)((const u8 *)buf + off)));
}

struct fwlog_ctx {
	struct tt_fwlog_state *fl;
	unsigned int gaps;
};

static void fwlog_emit(void *ctx, const struct fw_log_record *rec)
{
	struct fwlog_ctx *c = ctx;
	struct tt_fwlog_state *fl = c->fl;
	struct device *d = &fl->dev->pdev->dev;

	if (rec->seq_gap)
		c->gaps++;

	if (!fw_log_prepare_emit(rec, READ_ONCE(fw_log_level)))
		return;
	if (!__ratelimit(&fl->rs))
		return;

	snprintf(fl->line, FWLOG_LINE_SIZE, "%s: %.*s", fw_log_source_str(rec->source),
		 (int)rec->text_len, rec->text);
	fwlog_scrub(fl->line);

	switch (rec->log_level) {
	case FW_LOG_LEVEL_ERROR:
		dev_err(d, "fw %s\n", fl->line);
		break;
	case FW_LOG_LEVEL_WARN:
		dev_warn(d, "fw %s\n", fl->line);
		break;
	case FW_LOG_LEVEL_DEBUG:
		dev_dbg(d, "fw %s\n", fl->line);
		break;
	default:
		dev_info(d, "fw %s\n", fl->line);
		break;
	}
}

/* One drain; only ever called from the work item (rule 1). */
static void fwlog_drain(struct tt_fwlog_state *fl)
{
	struct device *d = &fl->dev->pdev->dev;
	struct fwlog_ctx ctx = { .fl = fl };
	struct fw_log_parse_result res;
	u32 magic, owner_word, write_offset, n;
	u8 *buf = fl->buf;

	/* Rule 2: nothing to do unless the firmware handed the buffer over. */
	magic = fwlog_read32(buf, FWLOG_OFF_MAGIC);
	owner_word = fwlog_read32(buf, FWLOG_OFF_OWNER_WORD);
	if (magic != FW_LOG_BUFFER_MAGIC || (owner_word & 0xff) != FW_LOG_BUFFER_OWNER_HOST)
		return;
	dma_rmb();

	/* Rule 3: one snapshot of write_offset, parse a private copy. */
	write_offset = fwlog_read32(buf, FWLOG_OFF_WRITE_OFFSET);
	n = min_t(u32, write_offset, FW_LOG_BUFFER_SIZE);
	memcpy(fl->copy, buf, n);
	fl->copy[n] = '\0';

	res = fw_log_parse(fl->copy, FW_LOG_BUFFER_SIZE, write_offset, &fl->seq,
			   fwlog_emit, &ctx);
	if (res.bad_write_offset || res.framing_error)
		dev_dbg_ratelimited(d, "fw log: malformed batch (write_offset %u, error at %u size %u)\n",
				    write_offset, res.error_offset, res.error_msg_size);
	if (ctx.gaps)
		dev_info_ratelimited(d, "fw log: messages lost (%u sequence gaps)\n", ctx.gaps);

	/* Rule 5: all reads of the batch done, then hand the buffer back. */
	mb();
	owner_word = fwlog_read32(buf, FWLOG_OFF_OWNER_WORD);
	WRITE_ONCE(*(__le32 *)(buf + FWLOG_OFF_OWNER_WORD),
		   cpu_to_le32((owner_word & ~0xffu) | FW_LOG_BUFFER_OWNER_FW));
}

static void fwlog_work(struct work_struct *work)
{
	struct tt_fwlog_state *fl = container_of(to_delayed_work(work),
						 struct tt_fwlog_state, work);

	if (!READ_ONCE(fl->active))
		return;
	fwlog_drain(fl);
	if (READ_ONCE(fl->active))
		schedule_delayed_work(&fl->work, FWLOG_POLL_INTERVAL);
}

void tt_fwlog_irq(struct tenstorrent_device *dev)
{
	struct tt_fwlog_state *fl = &dev->smc.fwlog;

	if (READ_ONCE(fl->active))
		mod_delayed_work(system_wq, &fl->work, 0);
}

static void fwlog_free(struct tenstorrent_device *dev)
{
	struct tt_fwlog_state *fl = &dev->smc.fwlog;

	if (fl->buf) {
		if (!fl->registered)
			dma_free_coherent(&dev->pdev->dev, FW_LOG_BUFFER_SIZE, fl->buf, fl->iova);
		/* else: firmware may still write it; leak rather than free. */
		fl->buf = NULL;
	}
	kfree(fl->copy);
	fl->copy = NULL;
	kfree(fl->line);
	fl->line = NULL;
}

void tt_fwlog_setup(struct tenstorrent_device *dev)
{
	struct tt_fwlog_state *fl = &dev->smc.fwlog;

	fl->dev = dev;
	INIT_DELAYED_WORK(&fl->work, fwlog_work);
	ratelimit_state_init(&fl->rs, 5 * HZ, 50);
	ratelimit_set_flags(&fl->rs, RATELIMIT_MSG_ON_RELEASE);
	fl->active = false;
	fl->registered = false;
}

void tt_fwlog_start(struct tenstorrent_device *dev)
{
	struct tt_fwlog_state *fl = &dev->smc.fwlog;
	struct device *d = &dev->pdev->dev;
	u32 req[TT_SMC_MSG_WORDS] = { }, resp[TT_SMC_MSG_WORDS];
	dma_addr_t last;
	int ret;

	if (!READ_ONCE(dev->smc.queue_addr))
		return;
	if (!dev->noc_translation_en) {
		/* The firmware needs translation for (19, 24); it would say 19. */
		dev_dbg(d, "fw log: NOC translation off, not set up\n");
		return;
	}

	fl->copy = kzalloc(FW_LOG_BUFFER_SIZE + 1, GFP_KERNEL);
	fl->line = kmalloc(FWLOG_LINE_SIZE, GFP_KERNEL);
	fl->buf = dma_alloc_coherent(d, FW_LOG_BUFFER_SIZE, &fl->iova, GFP_KERNEL);
	if (!fl->copy || !fl->line || !fl->buf)
		goto out_free;

	/* The IOVA is the NoC address: below 2^58, inside one 16 MiB window. */
	last = fl->iova + FW_LOG_BUFFER_SIZE - 1;
	if (last >> 58 || (fl->iova & ~(dma_addr_t)(FW_LOG_BUFFER_BOUNDARY - 1)) !=
			  (last & ~(dma_addr_t)(FW_LOG_BUFFER_BOUNDARY - 1))) {
		dev_info(d, "fw log: unusable buffer address %pad\n", &fl->iova);
		goto out_free;
	}

	req[0] = FW_LOG_MSG_ID | FW_LOG_SUBCMD_SETUP << 8 | FW_LOG_PROTOCOL_VERSION << 16;
	req[1] = lower_32_bits(fl->iova);
	req[2] = upper_32_bits(fl->iova);
	req[3] = FW_LOG_BUFFER_SIZE;

	/* From here until a RELEASE is acknowledged the firmware may write. */
	fl->registered = true;
	ret = tt_smc_send_sync(dev, req, resp, 0);
	if (ret == -ETIMEDOUT) {
		/* May still take effect: drain whatever arrives, RELEASE at fini. */
		dev_info(d, "fw log: SETUP not answered, draining anyway\n");
		goto start;
	}
	if (ret) {
		/* Unknown whether it took effect: keep the buffer for RELEASE. */
		dev_info(d, "fw log: SETUP failed (%d)\n", ret);
		return;
	}
	if (resp[0] & 0xff) {
		/* 0xff: no 0xC7 handler; 19: no translation / recovery fw. */
		fl->registered = false;
		dev_info(d, "fw log: not supported by firmware (status %#x)\n", resp[0] & 0xff);
		goto out_free;
	}

	dev_info(d, "fw log: forwarding firmware log (level <= %u)\n", READ_ONCE(fw_log_level));
start:
	fl->seq.valid = false;
	WRITE_ONCE(fl->active, true);
	schedule_delayed_work(&fl->work, FWLOG_POLL_INTERVAL);
	return;

out_free:
	fwlog_free(dev);
}

void tt_fwlog_fini(struct tenstorrent_device *dev)
{
	struct tt_fwlog_state *fl = &dev->smc.fwlog;
	u32 req[TT_SMC_MSG_WORDS] = { }, resp[TT_SMC_MSG_WORDS];
	int ret;

	WRITE_ONCE(fl->active, false);
	cancel_delayed_work_sync(&fl->work);

	if (fl->registered) {
		req[0] = FW_LOG_MSG_ID | FW_LOG_SUBCMD_RELEASE << 8 | FW_LOG_PROTOCOL_VERSION << 16;
		ret = tt_smc_send_sync(dev, req, resp, 0);
		if (!ret)
			fl->registered = false;	/* any answer: no longer set up */
		else
			dev_info(&dev->pdev->dev,
				 "fw log: RELEASE failed (%d), leaking its buffer\n", ret);
	}
	fwlog_free(dev);
	ratelimit_state_exit(&fl->rs);
}
