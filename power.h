/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SET_POWER_STATE aggregation and open/close power behaviour.
 *
 * Every open fd has a request (validity, flags, settings) on dev->power.fds.
 * The aggregate: flags-valid count = max over fds, flags = OR of each fd's
 * flags masked to its own valid count, settings-valid count = max, setting
 * j = max over the fds that mark j valid. Only the 4 defined flags count.
 * With no fd left (power_policy on) the target is the safe state (AICLK not
 * busy; GDDR PHY, Tensix and L2CPU on), or all four flags off with
 * power_lowpower_on_close (see power.c). Device removal always sends the
 * safe state (all 4 flags valid), whatever the acknowledged state is.
 *
 * The driver tracks the state the firmware last acknowledged and sends
 * POWER_SETTING (0x21) through the kernel SMC API only for a change, with
 * the valid-flag count cut to the highest changed flag + 1 (0x21 covering the
 * PHY or L2CPU flags is slow in firmware, and opens/closes are frequent). A
 * user SMC_MSG POWER_SETTING that applies something makes the state unknown.
 * A failed send is retried once; open() still succeeds (the next change
 * retries), SET_POWER_STATE returns the error. dev->power.lock serializes
 * aggregation and the send.
 *
 * Not handled (documented): an fd opened with O_APPEND that never calls
 * SET_POWER_STATE contributes nothing; the waits at open/close are not
 * interruptible (bounded by the kernel message timeout).
 */
#ifndef TT_POWER_H
#define TT_POWER_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/mutex.h>

struct tenstorrent_device;
struct chardev_private;

#define TT_POWER_NUM_SETTINGS	14

struct tt_power_dev_state {
	struct mutex lock;
	struct list_head fds;		/* of tt_power_fd_state.node */
	/* Last state the firmware acknowledged (the 4 known flags only). */
	u8 acked_flags;
	u8 acked_known;			/* mask of flags whose state is known */
	bool settings_known;
	u8 acked_settings_valid;
	u16 acked_settings[TT_POWER_NUM_SETTINGS];
	bool fw_changed;		/* a user POWER_SETTING was posted (READ/WRITE_ONCE) */
};

struct tt_power_fd_state {
	struct list_head node;		/* in dev->power.fds while on_list */
	bool on_list;
	u8 validity;
	u16 flags;
	u16 settings[TT_POWER_NUM_SETTINGS];
};

int tt_power_device_init(struct tenstorrent_device *dev);
void tt_power_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_power_fd_init(struct chardev_private *priv);

/*
 * Called at the end of open(), after the fd has been admitted and added to
 * dev->open_fds, inside a detach SRCU section with the device attached.
 * @o_append: open() had O_APPEND (start at power state 0, no aggregation);
 * otherwise legacy high-power + aggregate. Never fails: a firmware error is
 * not a reason to refuse the open.
 */
int tt_power_fd_open(struct chardev_private *priv, bool o_append);

/*
 * Is a user message with word 0 @req0 a POWER_SETTING that applies something?
 * The SMC pump then sets dev->power.fw_changed (WRITE_ONCE; consumed under
 * power.lock) when the message's slot completes, expires or is lost.
 */
bool tt_power_msg_applies(u32 req0);

/* Called last among the release hooks: drop this fd's contribution and, if
 * the power_policy module parameter is set and the device is attached,
 * re-aggregate. */
void tt_power_fd_release(struct chardev_private *priv);

/* Nothing device-side to release; the contribution goes at fd release. */
void tt_power_fd_detach(struct chardev_private *priv);

long tt_power_ioctl_set_state(struct chardev_private *priv, void __user *arg);

#endif
