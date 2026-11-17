// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware message queue 0 (see smc.h for the design): SMC_MSG ioctl and the
 * kernel-internal synchronous send API. Protocol: /specs/fw/blackhole/
 * host-messages.md.
 *
 * Note: user messages are passed through uninterpreted, including 0xC7
 * (TT_PCIE_LOG). A user SETUP/RELEASE would replace or drop the driver's own
 * firmware log buffer; this is not blocked (privileged tools own the device).
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/minmax.h>

#include "device.h"
#include "blackhole.h"
#include "smc.h"
#include "hwmon.h"
#include "power.h"

static unsigned int smc_timeout_ms = 60000;
module_param(smc_timeout_ms, uint, 0644);
MODULE_PARM_DESC(smc_timeout_ms,
		 "Time a user firmware message may take from POST to response before POLL fails with ETIMEDOUT (ms, 1000-3600000)");

static unsigned int smc_kernel_timeout_ms = 5000;
module_param(smc_kernel_timeout_ms, uint, 0644);
MODULE_PARM_DESC(smc_kernel_timeout_ms,
		 "Timeout of the driver's own firmware messages (ms, 100-600000)");

/* Queue info, pointed to by SCRATCH_RAM[11]. */
#define SMC_QUEUE_INFO_PTR		BH_ARC_SCRATCH_RAM(11)
/* Queue layout (bytes from the queue base). */
#define SMC_Q_REQ_WPTR			0x00	/* host */
#define SMC_Q_RESP_RPTR			0x04	/* host */
#define SMC_Q_REQ_RPTR			0x10	/* firmware */
#define SMC_Q_RESP_WPTR			0x14	/* firmware */
#define SMC_Q_HDR_WORDS			6	/* 0x00 - 0x17 */
#define SMC_Q_REQ_SLOTS			0x20
#define SMC_Q_RESP_SLOTS		0xA0
#define SMC_Q_SLOT_SIZE			(TT_SMC_MSG_WORDS * 4)
#define SMC_Q_SIZE			0x120
/* CSM, where the firmware keeps the queues. */
#define SMC_CSM_START			0x10000000u
#define SMC_CSM_END			0x10080000u
/* Doorbell: a 0 pushed into the ARC MSI catcher FIFO runs a queue pass. */
#define SMC_MSI_CATCHER			0x800B0000ULL
/* BOOT_STATUS0 bits [2:1] hw_init_status: 2 done, 3 error. */
#define SMC_BOOT_HW_INIT_STATUS(s)	(((s) >> 1) & 3)

/* Ring the doorbell again if the firmware has not taken our requests. */
#define SMC_REDOORBELL_MS		50
/* Messages in flight but no response for this long: firmware looks stalled. */
#define SMC_STALL_PROGRESS_MS		5000
/* Kernel messages while the firmware looks stalled: fail fast. */
#define SMC_STALLED_TIMEOUT_MS		500
/* Minimum interval between queue (re)discovery attempts. */
#define SMC_DISCOVER_INTERVAL		HZ
/* Late init (telemetry/hwmon/fwlog): poll for boot completion. */
#define SMC_INIT_RETRY_DELAY		HZ
#define SMC_INIT_MAX_TRIES		120

static unsigned long smc_user_timeout(void)
{
	return msecs_to_jiffies(clamp_t(unsigned int, READ_ONCE(smc_timeout_ms), 1000, 3600000));
}

static unsigned int smc_kernel_timeout(void)
{
	return clamp_t(unsigned int, READ_ONCE(smc_kernel_timeout_ms), 100, 600000);
}

static int smc_read(struct tenstorrent_device *dev, u64 addr, void *buf, size_t len)
{
	return tt_noc_read_block(dev, 0, BH_ARC_X, BH_ARC_Y, addr, buf, len);
}

static int smc_write(struct tenstorrent_device *dev, u64 addr, const void *buf, size_t len)
{
	return tt_noc_write_block(dev, 0, BH_ARC_X, BH_ARC_Y, addr, buf, len);
}

static int smc_write32(struct tenstorrent_device *dev, u64 addr, u32 val)
{
	return tt_noc_write32(dev, 0, BH_ARC_X, BH_ARC_Y, addr, val);
}

static bool smc_csm_range_ok(u32 addr, u32 len)
{
	return !(addr & 3) && addr >= SMC_CSM_START && addr < SMC_CSM_END &&
	       len <= SMC_CSM_END - addr;
}

/* ---- message state helpers (smc->lock held) ---- */

static void smc_complete(struct tt_smc_msg *m, int result)
{
	m->result = result;
	m->state = TT_SMC_MSG_DONE;
}

/* Detach @m from the queue machinery; it becomes IDLE. */
static void smc_abandon_locked(struct tt_smc_dev_state *smc, struct tt_smc_msg *m)
{
	switch (m->state) {
	case TT_SMC_MSG_QUEUED:
		list_del_init(&m->node);
		break;
	case TT_SMC_MSG_SUBMITTED:
		/* The slot stays in use; its response is discarded. */
		if (smc->inflight[m->ptr] == m)
			smc->inflight[m->ptr] = NULL;
		break;
	default:
		break;
	}
	m->state = TT_SMC_MSG_IDLE;
}

static void smc_abandon(struct tt_smc_dev_state *smc, struct tt_smc_msg *m)
{
	spin_lock(&smc->lock);
	smc_abandon_locked(smc, m);
	spin_unlock(&smc->lock);
}

/* ---- the pump (smc->pump_lock held) ---- */

static struct tenstorrent_device *smc_to_dev(struct tt_smc_dev_state *smc)
{
	return container_of(smc, struct tenstorrent_device, smc);
}

/*
 * A user POWER_SETTING that applies something reached the firmware (or may
 * have): the power code's view of the firmware state is stale. Marked when
 * its slot completes, expires or is lost, not at POST: kernel messages jump
 * the software queue, so the user message can reach the firmware after a
 * later kernel power update.
 */
static void smc_slot_power_mark(struct tt_smc_dev_state *smc, unsigned int ptr, bool clear)
{
	if (!smc->slot_power[ptr])
		return;
	WRITE_ONCE(smc_to_dev(smc)->power.fw_changed, true);
	if (clear)
		smc->slot_power[ptr] = false;
}

static void smc_fail_inflight(struct tt_smc_dev_state *smc, int err)
{
	int i;

	spin_lock(&smc->lock);
	for (i = 0; i < TT_SMC_PTR_WRAP; i++) {
		struct tt_smc_msg *m = smc->inflight[i];

		smc_slot_power_mark(smc, i, true);
		if (m) {
			smc->inflight[i] = NULL;
			smc_complete(m, err);
		}
	}
	spin_unlock(&smc->lock);
}

static void smc_fail_queued(struct tt_smc_dev_state *smc, int err)
{
	struct tt_smc_msg *m, *tmp;

	spin_lock(&smc->lock);
	list_for_each_entry_safe(m, tmp, &smc->queued, node) {
		list_del_init(&m->node);
		smc_complete(m, err);
	}
	spin_unlock(&smc->lock);
}

static void smc_expire(struct tt_smc_dev_state *smc)
{
	unsigned long now = jiffies;
	struct tt_smc_msg *m, *tmp;
	int i;

	spin_lock(&smc->lock);
	list_for_each_entry_safe(m, tmp, &smc->queued, node) {
		if (time_after_eq(now, m->deadline)) {
			list_del_init(&m->node);
			smc_complete(m, -ETIMEDOUT);
		}
	}
	for (i = 0; i < TT_SMC_PTR_WRAP; i++) {
		m = smc->inflight[i];
		if (m && time_after_eq(now, m->deadline)) {
			/* Slot stays accounted for: a late response is dropped. */
			smc->inflight[i] = NULL;
			if (m->kernel)
				smc->kernel_expired = true;
			smc_slot_power_mark(smc, i, false);	/* again at pop */
			smc_complete(m, -ETIMEDOUT);
		}
	}
	spin_unlock(&smc->lock);
}

static void smc_update_stalled(struct tt_smc_dev_state *smc)
{
	bool busy = smc->queue_addr &&
		    (smc->req_wptr - smc->resp_rptr) % TT_SMC_PTR_WRAP != 0;
	bool no_progress = busy &&
		time_after(jiffies, smc->last_progress + msecs_to_jiffies(SMC_STALL_PROGRESS_MS));

	WRITE_ONCE(smc->stalled, smc->kernel_expired || no_progress);
}

/*
 * Find queue 0 and adopt the firmware's current pointers. Responses to
 * requests a previous owner left in flight are discarded (no inflight entry).
 */
static int smc_discover(struct tenstorrent_device *dev)
{
	struct tt_smc_dev_state *smc = &dev->smc;
	u32 status, info_addr, info[2], hdr[SMC_Q_HDR_WORDS], q, outstanding;
	int ret;

	WRITE_ONCE(smc->queue_addr, 0);

	ret = tt_noc_read32(dev, 0, BH_ARC_X, BH_ARC_Y, BH_ARC_BOOT_STATUS0, &status);
	if (ret)
		return ret;
	if (status == 0xffffffff || !(status & BH_BOOT_STATUS0_MSG_QUEUE_READY))
		return -ENODEV;

	ret = tt_noc_read32(dev, 0, BH_ARC_X, BH_ARC_Y, SMC_QUEUE_INFO_PTR, &info_addr);
	if (ret)
		return ret;
	if (!smc_csm_range_ok(info_addr, sizeof(info)))
		return -ENODEV;

	ret = smc_read(dev, info_addr, info, sizeof(info));
	if (ret)
		return ret;
	q = info[0];
	/* Our slot layout assumes 4 entries per queue; need at least queue 0. */
	if ((info[1] & 0xff) != TT_SMC_QUEUE_SLOTS || !((info[1] >> 8) & 0xff) ||
	    !smc_csm_range_ok(q, SMC_Q_SIZE))
		return -ENODEV;

	ret = smc_read(dev, q, hdr, sizeof(hdr));
	if (ret)
		return ret;
	if (hdr[0] >= TT_SMC_PTR_WRAP || hdr[1] >= TT_SMC_PTR_WRAP ||
	    hdr[4] >= TT_SMC_PTR_WRAP || hdr[5] >= TT_SMC_PTR_WRAP)
		return -ENODEV;
	/* Same consistency rules as the pump. */
	outstanding = (hdr[0] - hdr[1]) % TT_SMC_PTR_WRAP;
	if (outstanding > TT_SMC_QUEUE_SLOTS ||
	    (hdr[5] - hdr[1]) % TT_SMC_PTR_WRAP > outstanding ||
	    (hdr[4] - hdr[1]) % TT_SMC_PTR_WRAP > outstanding)
		return -ENODEV;

	/* Rediscovery (firmware restarted): power state back to boot state. */
	if (smc->available)
		WRITE_ONCE(dev->power.fw_changed, true);

	memset(smc->slot_power, 0, sizeof(smc->slot_power));
	smc->req_wptr = hdr[0];
	smc->resp_rptr = hdr[1];
	smc->last_progress = jiffies;
	smc->kernel_expired = false;
	WRITE_ONCE(smc->queue_addr, q);
	WRITE_ONCE(smc->available, true);
	return 0;
}

/* Rate-limited (re)discovery while no queue is usable. */
static void smc_try_discover(struct tenstorrent_device *dev)
{
	struct tt_smc_dev_state *smc = &dev->smc;

	if (smc->queue_addr || smc->shutdown)
		return;
	if (smc->next_discover && time_before(jiffies, smc->next_discover))
		return;
	if (smc_discover(dev))
		smc->next_discover = jiffies + SMC_DISCOVER_INTERVAL;
	else
		smc->next_discover = 0;
}

static int smc_doorbell(struct tenstorrent_device *dev)
{
	dev->smc.last_doorbell = jiffies;
	return smc_write32(dev, SMC_MSI_CATCHER, 0);
}

static void smc_pump(struct tenstorrent_device *dev)
{
	struct tt_smc_dev_state *smc = &dev->smc;
	u32 hdr[SMC_Q_HDR_WORDS], words[TT_SMC_MSG_WORDS];
	unsigned int avail, n_inflight;
	bool popped = false, pushed = false;
	struct tt_smc_msg *m;
	u32 q, req_rptr, resp_wptr;
	int ret;

	lockdep_assert_held(&smc->pump_lock);

	if (smc->shutdown) {
		smc_fail_inflight(smc, -ENODEV);
		smc_fail_queued(smc, -ENODEV);
		return;
	}

	smc_try_discover(dev);
	q = smc->queue_addr;
	if (!q) {
		/* Lost (or never found): nothing queued can be sent. */
		if (smc->available)
			smc_fail_queued(smc, -EIO);
		goto expire;
	}

	ret = smc_read(dev, q, hdr, sizeof(hdr));
	if (ret)
		goto io_error;
	req_rptr = hdr[SMC_Q_REQ_RPTR / 4];
	resp_wptr = hdr[SMC_Q_RESP_WPTR / 4];
	n_inflight = (smc->req_wptr - smc->resp_rptr) % TT_SMC_PTR_WRAP;
	avail = (resp_wptr - smc->resp_rptr) % TT_SMC_PTR_WRAP;
	/*
	 * Only we write the host-owned words; a mismatch means the firmware
	 * restarted (init zeroes the headers) or the card reads all ones.
	 */
	if (hdr[SMC_Q_REQ_WPTR / 4] != smc->req_wptr || hdr[SMC_Q_RESP_RPTR / 4] != smc->resp_rptr ||
	    req_rptr >= TT_SMC_PTR_WRAP || resp_wptr >= TT_SMC_PTR_WRAP || avail > n_inflight ||
	    (req_rptr - smc->resp_rptr) % TT_SMC_PTR_WRAP > n_inflight)
		goto resync;

	/* Pop responses and hand them to their owners. */
	while (avail--) {
		u32 ptr = smc->resp_rptr;

		ret = smc_read(dev, q + SMC_Q_RESP_SLOTS + (ptr % TT_SMC_QUEUE_SLOTS) * SMC_Q_SLOT_SIZE,
			       words, sizeof(words));
		if (ret)
			goto io_error;

		spin_lock(&smc->lock);
		m = smc->inflight[ptr];
		smc->inflight[ptr] = NULL;
		smc_slot_power_mark(smc, ptr, true);
		if (m) {
			if (words[0] == 0xffffffff) {
				/* Not a firmware status: an unreachable read. */
				smc_complete(m, -EIO);
			} else {
				memcpy(m->resp, words, sizeof(words));
				smc_complete(m, 0);
			}
		}
		spin_unlock(&smc->lock);

		smc->resp_rptr = (ptr + 1) % TT_SMC_PTR_WRAP;
		popped = true;
	}
	if (popped) {
		smc->last_progress = jiffies;
		smc->kernel_expired = false;
		ret = smc_write32(dev, q + SMC_Q_RESP_RPTR, smc->resp_rptr);
		if (ret)
			goto io_error;
	}

	/* Expire before submitting: an expired message is never sent. */
	smc_expire(smc);

	/* Submit queued messages while the firmware has room. */
	while ((smc->req_wptr - smc->resp_rptr) % TT_SMC_PTR_WRAP < TT_SMC_QUEUE_SLOTS) {
		u32 ptr = smc->req_wptr;

		spin_lock(&smc->lock);
		m = list_first_entry_or_null(&smc->queued, struct tt_smc_msg, node);
		if (!m) {
			spin_unlock(&smc->lock);
			break;
		}
		list_del_init(&m->node);
		m->state = TT_SMC_MSG_SUBMITTED;
		m->ptr = ptr;
		smc->inflight[ptr] = m;
		/* Private copy: once abandoned, the owner may reuse m. */
		memcpy(words, m->req, sizeof(words));
		smc->slot_power[ptr] = !m->kernel && tt_power_msg_applies(words[0]);
		spin_unlock(&smc->lock);

		if (ptr == smc->resp_rptr)	/* ring was empty */
			smc->last_progress = jiffies;
		ret = smc_write(dev, q + SMC_Q_REQ_SLOTS + (ptr % TT_SMC_QUEUE_SLOTS) * SMC_Q_SLOT_SIZE,
				words, sizeof(words));
		if (ret)
			goto io_error;
		smc->req_wptr = (ptr + 1) % TT_SMC_PTR_WRAP;
		pushed = true;
	}
	if (pushed) {
		u32 dummy;

		/* Read back so the request words have landed before the
		 * pointer moves (the firmware reads the slot right away). */
		ret = smc_read(dev, q + SMC_Q_REQ_SLOTS, &dummy, sizeof(dummy));
		if (!ret)
			ret = smc_write32(dev, q + SMC_Q_REQ_WPTR, smc->req_wptr);
		if (!ret)
			ret = smc_doorbell(dev);
		if (ret)
			goto io_error;
	} else if (req_rptr != smc->req_wptr &&
		   time_after(jiffies, smc->last_doorbell + msecs_to_jiffies(SMC_REDOORBELL_MS))) {
		/* Requests not taken yet: the doorbell may have been lost. */
		ret = smc_doorbell(dev);
		if (ret)
			goto io_error;
	}
	smc_update_stalled(smc);
	return;

expire:
	smc_expire(smc);
	smc_update_stalled(smc);
	return;

resync:
	dev_info_ratelimited(&dev->pdev->dev,
			     "SMC queue state changed under us (host words %#x/%#x, expected %u/%u), resynchronizing\n",
			     hdr[SMC_Q_REQ_WPTR / 4], hdr[SMC_Q_RESP_RPTR / 4],
			     smc->req_wptr, smc->resp_rptr);
	smc_fail_inflight(smc, -EIO);
	WRITE_ONCE(smc->queue_addr, 0);
	/* Rediscover at once, unless resyncs repeat: then rate-limited. */
	if (smc->last_resync && time_before(jiffies, smc->last_resync + SMC_DISCOVER_INTERVAL))
		smc->next_discover = jiffies + SMC_DISCOVER_INTERVAL;
	else
		smc->next_discover = 0;
	smc->last_resync = jiffies ? jiffies : 1;
	smc_try_discover(dev);
	if (!smc->queue_addr)
		smc_fail_queued(smc, -EIO);
	goto expire;

io_error:
	/* The kernel window is gone (device removal) or unusable. */
	smc_fail_inflight(smc, -EIO);
	smc_fail_queued(smc, -EIO);
	WRITE_ONCE(smc->queue_addr, 0);
	smc->next_discover = jiffies + SMC_DISCOVER_INTERVAL;
	smc_update_stalled(smc);
}

/* Run a pump pass if nobody else is (user paths never wait for one). */
static void smc_pump_try(struct tenstorrent_device *dev)
{
	if (mutex_trylock(&dev->smc.pump_lock)) {
		smc_pump(dev);
		mutex_unlock(&dev->smc.pump_lock);
	}
}

/*
 * 0 if the queue can take a message; -EOPNOTSUPP if no queue was ever found,
 * -EIO if it is lost. @wait: may block on pump_lock for a (re)discovery.
 */
static int smc_queue_check(struct tenstorrent_device *dev, bool wait)
{
	struct tt_smc_dev_state *smc = &dev->smc;

	if (READ_ONCE(smc->queue_addr))
		return 0;

	if (wait)
		mutex_lock(&smc->pump_lock);
	else if (!mutex_trylock(&smc->pump_lock))
		goto out;
	smc_try_discover(dev);
	mutex_unlock(&smc->pump_lock);
out:
	if (READ_ONCE(smc->queue_addr))
		return 0;
	return READ_ONCE(smc->available) ? -EIO : -EOPNOTSUPP;
}

/* ---- kernel API ---- */

int tt_smc_send_sync(struct tenstorrent_device *dev, const u32 req[TT_SMC_MSG_WORDS],
		     u32 resp[TT_SMC_MSG_WORDS], unsigned int flags)
{
	struct tt_smc_dev_state *smc = &dev->smc;
	struct tt_smc_msg m = { };
	unsigned int timeout_ms, sleep_us = 20;
	unsigned long hard_deadline;
	bool done;
	int ret;

	might_sleep();

	if (READ_ONCE(smc->shutdown))
		return -ENODEV;
	ret = smc_queue_check(dev, true);
	if (ret)
		return ret;

	timeout_ms = smc_kernel_timeout();
	if (!(flags & TT_SMC_SYNC_FULL_TIMEOUT) && READ_ONCE(smc->stalled))
		timeout_ms = min_t(unsigned int, timeout_ms, SMC_STALLED_TIMEOUT_MS);

	INIT_LIST_HEAD(&m.node);
	memcpy(m.req, req, sizeof(m.req));
	m.kernel = true;
	m.deadline = jiffies + msecs_to_jiffies(timeout_ms);
	hard_deadline = m.deadline + HZ;

	/* Kernel messages go to the front of the software queue. */
	spin_lock(&smc->lock);
	m.state = TT_SMC_MSG_QUEUED;
	list_add(&m.node, &smc->queued);
	spin_unlock(&smc->lock);

	for (;;) {
		mutex_lock(&smc->pump_lock);
		smc_pump(dev);
		mutex_unlock(&smc->pump_lock);

		spin_lock(&smc->lock);
		done = m.state == TT_SMC_MSG_DONE;
		if (!done && time_after(jiffies, hard_deadline)) {
			/* Defensive: the pump expires messages itself. */
			smc_abandon_locked(smc, &m);
			spin_unlock(&smc->lock);
			return -ETIMEDOUT;
		}
		spin_unlock(&smc->lock);
		if (done)
			break;
		usleep_range(sleep_us, 2 * sleep_us);
		sleep_us = min(2 * sleep_us, 2000u);
	}

	ret = m.result;
	if (!ret)
		memcpy(resp, m.resp, sizeof(m.resp));
	return ret;
}

/* ---- ioctl ---- */

static long smc_post(struct chardev_private *priv, const struct tenstorrent_smc_msg *arg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_smc_dev_state *smc = &dev->smc;
	struct tt_smc_msg *m = &priv->smc.msg;
	int ret;

	ret = smc_queue_check(dev, false);
	if (ret)
		return ret;

	spin_lock(&smc->lock);
	if (m->state != TT_SMC_MSG_IDLE) {
		spin_unlock(&smc->lock);
		return -EBUSY;
	}
	memcpy(m->req, arg->message, sizeof(m->req));
	m->kernel = false;
	m->deadline = jiffies + smc_user_timeout();
	m->state = TT_SMC_MSG_QUEUED;
	list_add_tail(&m->node, &smc->queued);
	spin_unlock(&smc->lock);

	/* Submit right away if the pump is free; POLL drives it otherwise. */
	smc_pump_try(dev);
	return 0;
}

static long smc_poll(struct chardev_private *priv, struct tenstorrent_smc_msg __user *uarg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_smc_dev_state *smc = &dev->smc;
	struct tt_smc_msg *m = &priv->smc.msg;
	u32 resp[TT_SMC_MSG_WORDS];
	u8 state;
	int ret;

	spin_lock(&smc->lock);
	state = m->state;
	spin_unlock(&smc->lock);
	if (state == TT_SMC_MSG_IDLE)
		return -ESRCH;

	if (state != TT_SMC_MSG_DONE)
		smc_pump_try(dev);

	spin_lock(&smc->lock);
	switch (m->state) {
	case TT_SMC_MSG_IDLE:		/* another thread consumed or abandoned it */
		ret = -ESRCH;
		break;
	case TT_SMC_MSG_DONE:
		memcpy(resp, m->resp, sizeof(resp));
		ret = m->result;
		m->state = TT_SMC_MSG_IDLE;
		spin_unlock(&smc->lock);
		if (!ret && copy_to_user(uarg->message, resp, sizeof(resp)))
			ret = -EFAULT;
		return ret;
	default:
		ret = -EAGAIN;
		break;
	}
	spin_unlock(&smc->lock);
	return ret;
}

long tt_smc_ioctl_msg(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_smc_msg arg;

	/* Copy the whole struct first: a partially readable one is EFAULT. */
	if (copy_from_user(&arg, uarg, sizeof(arg)))
		return -EFAULT;

	if (arg.argsz != sizeof(arg))
		return -EINVAL;
	if (arg.queue_index != 0 || arg.reserved0 != 0)
		return -EINVAL;

	switch (arg.flags) {
	case TENSTORRENT_SMC_MSG_POST:
		return smc_post(priv, &arg);
	case TENSTORRENT_SMC_MSG_POLL:
		return smc_poll(priv, uarg);
	case TENSTORRENT_SMC_MSG_ABANDON:
		smc_abandon(&priv->dev->smc, &priv->smc.msg);
		return 0;
	default:	/* none, several, or unknown bits */
		return -EINVAL;
	}
}

/* ---- per fd ---- */

void tt_smc_fd_init(struct chardev_private *priv)
{
	INIT_LIST_HEAD(&priv->smc.msg.node);
	priv->smc.msg.state = TT_SMC_MSG_IDLE;
}

int tt_smc_fd_open(struct chardev_private *priv)
{
	return 0;
}

/* Software only: valid before and after detach / device_fini. */
void tt_smc_fd_release(struct chardev_private *priv)
{
	smc_abandon(&priv->dev->smc, &priv->smc.msg);
}

void tt_smc_fd_detach(struct chardev_private *priv)
{
	smc_abandon(&priv->dev->smc, &priv->smc.msg);
}

/* ---- device ---- */

void tt_smc_handle_irq(struct tenstorrent_device *dev)
{
	tt_fwlog_irq(dev);
}

/*
 * Telemetry, hwmon and the firmware log need a fully booted firmware
 * (telemetry is published after the queue, before boot "done") and the log
 * SETUP is a firmware round trip: done here, off the probe path, retried
 * until BOOT_STATUS0 says the boot finished (done or error).
 */
static void smc_init_work(struct work_struct *work)
{
	struct tt_smc_dev_state *smc = container_of(to_delayed_work(work),
						    struct tt_smc_dev_state, init_work);
	struct tenstorrent_device *dev = container_of(smc, struct tenstorrent_device, smc);
	u32 status = 0;

	if (tt_noc_read32(dev, 0, BH_ARC_X, BH_ARC_Y, BH_ARC_BOOT_STATUS0, &status) ||
	    status == 0xffffffff || SMC_BOOT_HW_INIT_STATUS(status) < 2) {
		if (++smc->init_tries < SMC_INIT_MAX_TRIES)
			schedule_delayed_work(&smc->init_work, SMC_INIT_RETRY_DELAY);
		else
			dev_info(&dev->pdev->dev,
				 "firmware boot not complete (BOOT_STATUS0 %#x): no telemetry, hwmon or fw log\n",
				 status);
		return;
	}

	smc_queue_check(dev, true);	/* the queue may have come up late */
	tt_telemetry_init(dev);
	tt_hwmon_init(dev);
	tt_fwlog_start(dev);
}

int tt_smc_device_init(struct tenstorrent_device *dev)
{
	struct tt_smc_dev_state *smc = &dev->smc;
	int ret = -ENODEV;

	mutex_init(&smc->pump_lock);
	spin_lock_init(&smc->lock);
	INIT_LIST_HEAD(&smc->queued);
	smc->last_doorbell = jiffies;
	smc->last_progress = jiffies;
	INIT_DELAYED_WORK(&smc->init_work, smc_init_work);
	tt_hwmon_setup(dev);
	tt_fwlog_setup(dev);

	if (dev->fw_ready) {
		mutex_lock(&smc->pump_lock);
		ret = smc_discover(dev);
		smc->next_discover = ret ? jiffies + SMC_DISCOVER_INTERVAL : 0;
		mutex_unlock(&smc->pump_lock);
	}
	if (ret)
		dev_info(&dev->pdev->dev, "no firmware message queue (%d)\n", ret);

	schedule_delayed_work(&smc->init_work, 0);
	return 0;
}

void tt_smc_device_fini(struct tenstorrent_device *dev)
{
	struct tt_smc_dev_state *smc = &dev->smc;

	cancel_delayed_work_sync(&smc->init_work);
	tt_hwmon_fini(dev);
	tt_fwlog_fini(dev);	/* needs the queue: RELEASE */

	mutex_lock(&smc->pump_lock);
	WRITE_ONCE(smc->shutdown, true);
	smc_pump(dev);		/* fails whatever is left with -ENODEV */
	mutex_unlock(&smc->pump_lock);
}
