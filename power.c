// SPDX-License-Identifier: GPL-2.0-only
/* SET_POWER_STATE and the open/close power policy (see power.h). */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/uaccess.h>

#include "device.h"
#include "power.h"
#include "smc.h"

static bool power_policy = true;
module_param(power_policy, bool, 0644);
MODULE_PARM_DESC(power_policy,
		 "Remove an fd's power request when it is closed and re-aggregate (default on)");

/*
 * Last close (with power_policy on): by default the driver sends the safe
 * state (POWER_SAFE_FLAGS: AICLK not busy, GDDR PHY awake, Tensix and L2CPU
 * on), as far as it differs from the acknowledged state. The "full" low power state of
 * ioctl.h (all four flags off) is behind power_lowpower_on_close, default
 * off, because it:
 *  - clock-gates every Tensix tile after putting its RISCs in soft reset, so
 *    the next user must reload and release them, and a NOC access to a gated
 *    tile has undocumented behaviour (possible hang);
 *  - powers the GDDR PHYs down (self-refresh): whether DRAM contents survive
 *    and what a NOC access to a powered-down channel does are not documented,
 *    and the MRISC handshake can stall the firmware queue for up to 1 s;
 *  - stops the L2CPU clocks with four PLL4 relocks per message.
 * Every later legacy open then has to undo all of it, slowly.
 */
static bool power_lowpower_on_close;
module_param(power_lowpower_on_close, bool, 0644);
MODULE_PARM_DESC(power_lowpower_on_close,
		 "On last close, send the full low-power state (Tensix/L2CPU gated, GDDR PHY down) instead of the safe state (default off)");

#define POWER_MSG_POWER_SETTING		0x21
#define POWER_KNOWN_FLAGS		4	/* TT_POWER_FLAG_* defined */
#define POWER_KNOWN_MASK		((1u << POWER_KNOWN_FLAGS) - 1)
#define POWER_LEGACY_FLAGS		(TT_POWER_FLAG_MAX_AI_CLK | TT_POWER_FLAG_MRISC_PHY_WAKEUP | \
					 TT_POWER_FLAG_TENSIX_ENABLE | TT_POWER_FLAG_L2CPU_ENABLE)
/*
 * Safe state, sent when the last fd closes and at device removal: everything
 * powered and ungated (GDDR PHY awake, Tensix and L2CPU clocked) but AICLK
 * not held busy, so the next user (or driver load) finds working DRAM and
 * tiles even if an O_APPEND-only aggregate had powered them down.
 */
#define POWER_SAFE_FLAGS		(TT_POWER_FLAG_MRISC_PHY_WAKEUP | TT_POWER_FLAG_TENSIX_ENABLE | \
					 TT_POWER_FLAG_L2CPU_ENABLE)

/* What the open fds ask for, normalised to the known flags. */
struct power_want {
	u8 mask;			/* flags that are specified */
	u8 flags;
	u8 settings_valid;
	u16 settings[TT_POWER_NUM_SETTINGS];
};

static void power_aggregate(struct tt_power_dev_state *p, struct power_want *w)
{
	struct tt_power_fd_state *f;
	unsigned int j, fv = 0;

	memset(w, 0, sizeof(*w));

	if (list_empty(&p->fds)) {
		/* Nobody left: full low power if asked, else the safe state. */
		w->mask = POWER_KNOWN_MASK;
		w->flags = READ_ONCE(power_lowpower_on_close) ? 0 : POWER_SAFE_FLAGS;
		return;
	}

	list_for_each_entry(f, &p->fds, node) {
		unsigned int nf = f->validity & 0xf;
		unsigned int ns = (f->validity >> 4) & 0xf;

		fv = max(fv, nf);
		w->settings_valid = max_t(u8, w->settings_valid, ns);
		w->flags |= f->flags & ((1u << nf) - 1);
		for (j = 0; j < ns && j < TT_POWER_NUM_SETTINGS; j++)
			w->settings[j] = max(w->settings[j], f->settings[j]);
	}
	/* Flags 4-15 are reserved and ignored by the firmware. */
	w->mask = ((1u << fv) - 1) & POWER_KNOWN_MASK;
	w->flags &= w->mask;
}

static int power_send(struct tenstorrent_device *dev, const u32 req[TT_SMC_MSG_WORDS],
		      unsigned int flags)
{
	u32 resp[TT_SMC_MSG_WORDS];

	return tt_smc_send_sync(dev, req, resp, flags);
}

/*
 * Bring the firmware to what the fds want: send POWER_SETTING covering flags
 * 0 .. n-1 with n = highest flag that differs from (or is unknown in) the
 * last acknowledged state + 1, so unchanged high flags (slow PHY / PLL work
 * in firmware) are not re-applied; nothing at all if nothing changed.
 * p->lock held; device attached.
 */
static int power_update_locked(struct tenstorrent_device *dev)
{
	struct tt_power_dev_state *p = &dev->power;
	u32 req[TT_SMC_MSG_WORDS] = { };
	unsigned int n, j, send_flags = 0;
	struct power_want w;
	bool settings_changed;
	u8 changed, mask;
	int ret;

	lockdep_assert_held(&p->lock);

	if (READ_ONCE(p->fw_changed)) {
		/* A user POWER_SETTING went to the firmware: state unknown. */
		WRITE_ONCE(p->fw_changed, false);
		p->acked_known = 0;
		p->settings_known = false;
	}

	power_aggregate(p, &w);

	changed = w.mask & ((w.flags ^ p->acked_flags) | ~p->acked_known);
	settings_changed = w.settings_valid &&
		(!p->settings_known || w.settings_valid != p->acked_settings_valid ||
		 memcmp(w.settings, p->acked_settings, w.settings_valid * sizeof(u16)));
	if (!changed && !settings_changed)
		return 0;

	n = fls(changed);
	mask = (1u << n) - 1;
	req[0] = POWER_MSG_POWER_SETTING | n << 8 | (u32)w.settings_valid << 12 |
		 (u32)(w.flags & mask) << 16;
	for (j = 0; j < w.settings_valid; j++)
		req[1 + j / 2] |= (u32)w.settings[j] << (16 * (j % 2));

	/* Turning something on is worth waiting for, even if fw looks slow. */
	if (changed & w.flags)
		send_flags = TT_SMC_SYNC_FULL_TIMEOUT;

	ret = power_send(dev, req, send_flags);
	if (ret && ret != -EOPNOTSUPP)
		ret = power_send(dev, req, 0);	/* one retry */
	if (ret) {
		/* May or may not have been applied: resend next time. */
		p->acked_known &= ~mask;
		if (w.settings_valid)
			p->settings_known = false;
		if (ret != -EOPNOTSUPP)
			dev_info_ratelimited(&dev->pdev->dev,
					     "POWER_SETTING %#x failed (%d), retrying on the next change\n",
					     req[0], ret);
		return ret;
	}

	/* 0x21 always reports success; any answer means it was applied. */
	p->acked_flags = (p->acked_flags & ~mask) | (w.flags & mask);
	p->acked_known |= mask;
	if (w.settings_valid) {
		p->acked_settings_valid = w.settings_valid;
		memcpy(p->acked_settings, w.settings, sizeof(p->acked_settings));
		p->settings_known = true;
	}
	return 0;
}

bool tt_power_msg_applies(u32 req0)
{
	/* A POWER_SETTING with a nonzero validity byte changes something. */
	return (req0 & 0xff) == POWER_MSG_POWER_SETTING && ((req0 >> 8) & 0xff);
}

long tt_power_ioctl_set_state(struct chardev_private *priv, void __user *uarg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_power_fd_state *f = &priv->power;
	struct tenstorrent_power_state ps;
	int ret;

	if (copy_from_user(&ps, uarg, sizeof(ps)))
		return -EFAULT;
	if (ps.argsz != sizeof(ps) || ps.flags != 0 || ps.reserved0 != 0)
		return -EINVAL;
	if (((ps.validity >> 4) & 0xf) > TT_POWER_NUM_SETTINGS)
		return -EINVAL;

	mutex_lock(&dev->power.lock);
	f->validity = ps.validity;
	f->flags = ps.power_flags;
	memcpy(f->settings, ps.power_settings, sizeof(f->settings));
	ret = power_update_locked(dev);
	mutex_unlock(&dev->power.lock);
	return ret;
}

void tt_power_fd_init(struct chardev_private *priv)
{
	INIT_LIST_HEAD(&priv->power.node);
}

int tt_power_fd_open(struct chardev_private *priv, bool o_append)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_power_fd_state *f = &priv->power;

	mutex_lock(&dev->power.lock);
	if (!o_append) {
		f->validity = TT_POWER_VALIDITY(POWER_KNOWN_FLAGS, 0);
		f->flags = POWER_LEGACY_FLAGS;
	}
	list_add_tail(&f->node, &dev->power.fds);
	f->on_list = true;
	if (!o_append)
		power_update_locked(dev);	/* retried + logged; open goes on */
	mutex_unlock(&dev->power.lock);
	return 0;
}

void tt_power_fd_release(struct chardev_private *priv)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_power_fd_state *f = &priv->power;

	mutex_lock(&dev->power.lock);
	if (f->on_list) {
		list_del_init(&f->node);
		f->on_list = false;
		if (READ_ONCE(power_policy) && !READ_ONCE(dev->detached))
			power_update_locked(dev);
	}
	mutex_unlock(&dev->power.lock);
}

void tt_power_fd_detach(struct chardev_private *priv)
{
}

int tt_power_device_init(struct tenstorrent_device *dev)
{
	mutex_init(&dev->power.lock);
	INIT_LIST_HEAD(&dev->power.fds);
	dev->power.acked_known = 0;	/* nothing known until the first ack */
	dev->power.settings_known = false;
	return 0;
}

/*
 * PCI remove (device detached, MMIO and the SMC queue still up: power fini
 * runs before smc fini): leave the card in the safe state for whoever loads
 * next, whatever we believe the firmware state is (an O_APPEND-only
 * aggregate may have gated Tensix / L2CPU and powered the GDDR PHYs down).
 */
void tt_power_device_fini(struct tenstorrent_device *dev)
{
	struct tt_power_dev_state *p = &dev->power;
	u32 req[TT_SMC_MSG_WORDS] = { };
	int ret;

	/* Only with a working queue (not e.g. on a probe error path without
	 * one); bounded by the kernel message timeout, cut short if stalled. */
	if (!READ_ONCE(dev->smc.queue_addr))
		return;

	req[0] = POWER_MSG_POWER_SETTING | POWER_KNOWN_FLAGS << 8 | POWER_SAFE_FLAGS << 16;

	mutex_lock(&p->lock);
	ret = power_send(dev, req, 0);
	if (ret && ret != -EOPNOTSUPP)
		dev_info(&dev->pdev->dev, "could not restore the safe power state at removal (%d)\n",
			 ret);
	p->acked_known = 0;
	mutex_unlock(&p->lock);
}
