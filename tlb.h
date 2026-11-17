/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * User TLB windows: ALLOCATE/FREE/CONFIGURE_TLB, TLB mmap, EXPORT_TLB_DMABUF
 * (batch B3). See memory.h for the hook conventions.
 *
 * Window geometry and the config register encoder live in blackhole.h
 * (bh_tlb_window_*(), bh_tlb_configure()); window BH_KERNEL_TLB_INDEX is
 * owned by the kernel and must never be handed out.
 */
#ifndef TT_TLB_H
#define TT_TLB_H

#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "ioctl.h"

struct tenstorrent_device;
struct chardev_private;
struct vm_area_struct;
struct seq_file;
struct tt_tlb_window;

/*
 * Window lifetime
 * ---------------
 * ALLOCATE_TLB creates a struct tt_tlb_window, puts it in
 * dev->tlb.windows[id] (a non-NULL slot means "out of the pool") and resets
 * the window's hardware config to all zeros. The window is held by up to
 * three kinds of users:
 *  - its owner fd (w->owner, cleared by FREE_TLB or fd release);
 *  - user mappings (w->map_count, one per tt_mmio_mapping object, i.e. per
 *    original mmap; VMA splits/dups/mremaps share it). FREE_TLB fails with
 *    -EBUSY while non-zero, and the fd cannot be released while a VMA
 *    exists (VMAs hold the file), so a mapping implies a live owner;
 *  - dma-buf exports (w->export_count). FREE_TLB and fd close succeed but the
 *    window stays out of the pool, unowned and so not reconfigurable, until
 *    the last export is released. Every export holds a device reference.
 * The slot is cleared and the window freed when all three are gone.
 *
 * PCI remove revokes live exports in tt_tlb_device_fini() (see there).
 *
 * Locking: dev->tlb.lock (mutex) protects the slots, all window fields, the
 * per-fd window lists and the export list. It is taken from vm_ops->close
 * (mmap_lock held), so it must never be held across user copies. Lock order:
 * mmap_lock -> dev->tlb.lock, dev->open_lock -> dev->tlb.lock. dev->tlb.lock
 * and the exports' dma_resv locks are never nested: the dma-buf callbacks
 * that run with the reservation held (map/unmap/pin/unpin) never take
 * dev->tlb.lock, and device_fini takes the reservations after dropping it.
 */
struct tt_tlb_dev_state {
	struct mutex lock;
	struct tt_tlb_window *windows[TENSTORRENT_MAX_INBOUND_TLBS];
	struct list_head exports;	/* of struct tt_tlb_export */
	unsigned int num_exports;
};

struct tt_tlb_fd_state {
	struct list_head windows;	/* owned windows, under dev->tlb.lock */
};

int tt_tlb_device_init(struct tenstorrent_device *dev);
void tt_tlb_device_fini(struct tenstorrent_device *dev);

/* Right after the fd's kzalloc, before it is visible anywhere: initialise
 * the per-fd state (lists, locks). Software only, cannot fail. */
void tt_tlb_fd_init(struct chardev_private *priv);

int tt_tlb_fd_open(struct chardev_private *priv);
void tt_tlb_fd_release(struct chardev_private *priv);

/*
 * PCI remove, for every fd still open, after dev->detached is set and with
 * dev->open_lock held. TLB windows hold no DMA mappings, so this does
 * nothing: user mappings were already zapped by the core, the windows are
 * freed (software only) by the fd's release hook, and exports, which may
 * outlive their owner fd, are revoked in tt_tlb_device_fini().
 */
void tt_tlb_fd_detach(struct chardev_private *priv);

long tt_tlb_ioctl_allocate(struct chardev_private *priv, void __user *arg);
long tt_tlb_ioctl_free(struct chardev_private *priv, void __user *arg);
long tt_tlb_ioctl_configure(struct chardev_private *priv, void __user *arg);
long tt_tlb_ioctl_export_dmabuf(struct chardev_private *priv, void __user *arg);

/* mmap of an offset in [TT_MMAP_TLB_BASE, TT_MMAP_TLB_END). Use
 * tt_mmio_mmap() (chardev.h) with a struct tt_mmio_mapping embedded in the
 * window's mapping record: lazy faulting, no_split for "munmap of a single
 * page fails", ops->release to drop the window's mapping refcount. */
int tt_tlb_mmap(struct chardev_private *priv, struct vm_area_struct *vma);

void tt_tlb_debugfs_show(struct seq_file *s, struct chardev_private *priv);

/*
 * Called by RESET_DEVICE before any real reset. Return -EBUSY while a TLB
 * dma-buf export is live, 0 otherwise. Called with the device attached.
 */
int tt_tlb_reset_check(struct tenstorrent_device *dev);

#endif
