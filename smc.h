/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Firmware (SMC) message queue: SMC_MSG POST/POLL/ABANDON and kernel-internal
 * synchronous messages, multiplexed over firmware queue 0.
 *
 * The device state also carries the other firmware interfaces that hang off
 * the queue (telemetry + hwmon, firmware log): tt_smc_device_init() brings
 * them up after the queue, tt_smc_device_fini() takes them down first.
 *
 * Message lifecycle
 * -----------------
 * A struct tt_smc_msg is IDLE, QUEUED (on smc.queued, not yet in the firmware
 * ring), SUBMITTED (written to request slot smc.inflight[ptr]) or DONE
 * (response or error result waiting for its owner). The "pump"
 * (tt_smc_pump(), under smc.pump_lock) pops firmware responses and matches
 * them to smc.inflight[] by ring pointer, submits queued messages while the
 * firmware has free slots, rings the doorbell, and expires messages past
 * their deadline. Abandoning a SUBMITTED message only clears its inflight[]
 * entry: the slot stays accounted for and its response is discarded when it
 * arrives, so the owner's struct may be reused or freed at once.
 *
 * Locking: smc.pump_lock (mutex; the queue hardware and the host pointer
 * copies) -> dev->kernel_tlb_mutex (inside tt_noc_*) and smc.lock (spinlock;
 * smc.queued, smc.inflight[], every message's state/result/resp). Nobody
 * holds pump_lock while waiting for the firmware: callers pump, drop the
 * lock, and check (or sleep) outside it. User POLL/POST only trylock the
 * pump, so they never wait for another pump pass.
 */
#ifndef TT_SMC_H
#define TT_SMC_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/bits.h>
#include <linux/workqueue.h>

#include "telemetry.h"
#include "fwlog_drv.h"

struct tenstorrent_device;
struct chardev_private;

#define TT_SMC_MSG_WORDS	8
#define TT_SMC_QUEUE_SLOTS	4			/* request/response slots per queue */
#define TT_SMC_PTR_WRAP		(2 * TT_SMC_QUEUE_SLOTS)	/* pointers count mod 8 */

enum tt_smc_msg_state {
	TT_SMC_MSG_IDLE = 0,
	TT_SMC_MSG_QUEUED,
	TT_SMC_MSG_SUBMITTED,
	TT_SMC_MSG_DONE,
};

struct tt_smc_msg {
	struct list_head node;		/* in smc.queued while QUEUED */
	u32 req[TT_SMC_MSG_WORDS];
	u32 resp[TT_SMC_MSG_WORDS];
	int result;			/* DONE: 0 (resp valid) or -errno */
	u8 state;			/* enum tt_smc_msg_state */
	u8 ptr;				/* SUBMITTED: request ring pointer */
	bool kernel;			/* tt_smc_send_sync() message */
	unsigned long deadline;		/* jiffies */
};

/* Telemetry values cached for hwmon (sysfs readers poll). */
#define TT_HWMON_CACHE_TAGS	9

struct tt_hwmon_cache {
	struct mutex lock;
	u32 val[TT_HWMON_CACHE_TAGS];
	unsigned long stamp[TT_HWMON_CACHE_TAGS];	/* jiffies; 0 = empty */
};

struct tt_smc_dev_state {
	struct mutex pump_lock;
	spinlock_t lock;

	/*
	 * Queue state, written under pump_lock (READ_ONCE outside):
	 * available: a queue 0 was found at least once (else POST is
	 * EOPNOTSUPP); queue_addr == 0 while available: the queue is lost
	 * (firmware reset, dead card) and messages fail with -EIO until a
	 * rate-limited rediscovery succeeds.
	 */
	bool available;
	bool shutdown;			/* device_fini: no more hardware access */
	u32 queue_addr;			/* ARC address of queue 0; 0 = not usable now */
	unsigned long next_discover;	/* jiffies; 0 = (re)discover at once */

	/* Host-side pointer copies, under pump_lock. */
	u32 req_wptr;
	u32 resp_rptr;
	unsigned long last_doorbell;	/* jiffies */
	unsigned long last_progress;	/* jiffies: last response, or submit into an empty ring */
	bool kernel_expired;		/* a submitted kernel message expired; cleared by a response */
	bool slot_power[TT_SMC_PTR_WRAP];	/* user POWER_SETTING submitted in this slot */
	unsigned long last_resync;	/* jiffies of the last resync (0 = none) */
	bool stalled;			/* kernel_expired or no progress for a while (READ_ONCE) */

	/* Under lock. */
	struct list_head queued;
	struct tt_smc_msg *inflight[TT_SMC_PTR_WRAP];

	/* Late init: telemetry, hwmon, fwlog once the firmware is booted. */
	struct delayed_work init_work;
	unsigned int init_tries;

	/* Other firmware interfaces (set up by init_work only). */
	struct tt_telemetry_state telemetry;
	struct device *hwmon;
	struct tt_hwmon_cache hwmon_cache;
	struct tt_fwlog_state fwlog;
};

struct tt_smc_fd_state {
	struct tt_smc_msg msg;		/* the fd's one outstanding message */
};

int tt_smc_device_init(struct tenstorrent_device *dev);
void tt_smc_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_smc_fd_init(struct chardev_private *priv);

int tt_smc_fd_open(struct chardev_private *priv);
/* Abandon any outstanding message of this fd. */
void tt_smc_fd_release(struct chardev_private *priv);

/*
 * PCI remove, for every fd still open, after dev->detached is set and with
 * dev->open_lock held: release everything that uses the device (DMA
 * mappings, iATU regions, exports' device side); the PCI device is gone once
 * remove returns. May run concurrently with this fd's release hook: both
 * take the subsystem's per-fd lock; release afterwards only frees software
 * state (and must not call the DMA API once detached).
 */
void tt_smc_fd_detach(struct chardev_private *priv);

long tt_smc_ioctl_msg(struct chardev_private *priv, void __user *arg);

/*
 * Kernel-internal synchronous message on queue 0, sharing the queue with user
 * traffic. Process context; sleeps; bounded by the smc_kernel_timeout_ms
 * module parameter, cut to a short timeout while the firmware looks stalled
 * unless @flags has TT_SMC_SYNC_FULL_TIMEOUT. Returns 0 with the response in
 * @resp (firmware status in resp[0] not interpreted), -EOPNOTSUPP if no
 * queue was ever found, -EIO (queue lost / unreachable), -ETIMEDOUT or
 * -ENODEV (shutting down). The caller must keep the device attached (detach
 * section) or be in probe / device_fini before the MMIO teardown.
 */
#define TT_SMC_SYNC_FULL_TIMEOUT	BIT(0)

int tt_smc_send_sync(struct tenstorrent_device *dev, const u32 req[TT_SMC_MSG_WORDS],
		     u32 resp[TT_SMC_MSG_WORDS], unsigned int flags);

/* MSI handler hook (hard IRQ context). */
void tt_smc_handle_irq(struct tenstorrent_device *dev);

#endif
