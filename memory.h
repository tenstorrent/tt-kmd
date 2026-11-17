/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DMA buffers, pinned user pages and the outbound iATU (batch B2).
 *
 * Per-fd state is protected by tt_memory_fd_state.lock (a mutex), the
 * device-wide iATU region table by tt_memory_dev_state.iatu_lock.
 * Lock order: dev->open_lock -> fd memory lock -> iatu_lock.
 * The fd memory lock is never held across pin_user_pages or a user copy, so
 * debugfs (which holds dev->open_lock) never waits behind a slow pin.
 * mmap and page faults of DMA buffers take it under mmap_lock (mmap_lock ->
 * fd memory lock); nothing takes mmap_lock under the fd memory lock.
 */
#ifndef TT_MEMORY_H
#define TT_MEMORY_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/bitmap.h>
#include <linux/rbtree.h>

#include "ioctl.h"

struct tenstorrent_device;
struct chardev_private;
struct vm_area_struct;
struct seq_file;

/* Reported by GET_DEVICE_INFO.max_dma_buf_size_log2; must stay <= 32
 * (each DMA buffer owns a 4 GiB slot of the mmap offset space, see device.h). */
#define TT_MAX_DMA_BUF_SIZE_LOG2 28

/* Outbound iATU regions the driver manages (all the controller has). */
#define TT_IATU_NUM_REGIONS	16

struct tt_iatu_region {
	bool usable;		/* disabled at probe: ours to use */
	bool used;		/* currently mapping something */
	u64 noc_offset;		/* offset inside NoC-to-host window 4 */
	u64 size;		/* bytes covered (multiple of 4 KiB) */
};

/* Device-wide memory state (embedded in struct tenstorrent_device as ->memory). */
struct tt_memory_dev_state {
	struct mutex iatu_lock;
	bool iatu_present;	/* BAR2 iATU block looks sane */
	struct tt_iatu_region iatu[TT_IATU_NUM_REGIONS];
};

struct tt_dma_buf;

/* Per-fd memory state (embedded in struct chardev_private as ->memory). */
struct tt_memory_fd_state {
	struct mutex lock;
	/* Index reserved (allocation in progress or done). */
	DECLARE_BITMAP(dma_buf_busy, TENSTORRENT_MAX_DMA_BUFS);
	struct tt_dma_buf *dma_bufs[TENSTORRENT_MAX_DMA_BUFS];	/* installed buffers */
	struct rb_root pins;		/* of struct tt_pin, keyed by (va, size) */
};

/* Probe/remove. device_init runs after the hardware is up and before the
 * char device is visible. device_fini runs in remove after dev->detached is
 * set and every ioctl/open/release that saw the device attached has finished;
 * BAR mappings are still valid. Fds may still be open: their fd_detach hooks
 * have run, and their release hooks may run later or concurrently with
 * device_fini (detached). */
int tt_memory_device_init(struct tenstorrent_device *dev);
void tt_memory_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_memory_fd_init(struct chardev_private *priv);

/* open()/release() of an fd, both inside a detach SRCU section. open runs
 * with the device attached; release may run after PCI remove: check
 * READ_ONCE(priv->dev->detached) before touching hardware (if it is false,
 * the hardware stays mapped until the hook returns). */
int tt_memory_fd_open(struct chardev_private *priv);
void tt_memory_fd_release(struct chardev_private *priv);

/*
 * PCI remove, for every fd still open, after dev->detached is set and with
 * dev->open_lock held: release everything that uses the device (DMA
 * mappings, iATU regions, exports' device side); the PCI device is gone once
 * remove returns. May run concurrently with this fd's release hook: both
 * take the subsystem's per-fd lock; release afterwards only frees software
 * state (and must not call the DMA API once detached).
 */
void tt_memory_fd_detach(struct chardev_private *priv);

/* ioctls; called inside a detach SRCU section with the device attached. */
long tt_memory_ioctl_allocate_dma_buf(struct chardev_private *priv, void __user *arg);
long tt_memory_ioctl_free_dma_buf(struct chardev_private *priv, void __user *arg);
long tt_memory_ioctl_pin_pages(struct chardev_private *priv, void __user *arg);
long tt_memory_ioctl_unpin_pages(struct chardev_private *priv, void __user *arg);

/* mmap of an offset in [TT_MMAP_DMA_BUF_BASE, TT_MMAP_DMA_BUF_END). The VMA
 * is guaranteed to lie entirely inside that range. */
int tt_memory_mmap(struct chardev_private *priv, struct vm_area_struct *vma);

/* Emit this fd's rows in debugfs "mappings" (use tt_mappings_row()).
 * Called for live fds with dev->open_lock held; take the per-fd lock. */
void tt_memory_debugfs_show(struct seq_file *s, struct chardev_private *priv);

#endif
