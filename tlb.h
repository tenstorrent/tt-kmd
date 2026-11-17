/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * User TLB windows: ALLOCATE/FREE/CONFIGURE_TLB, TLB mmap, EXPORT_TLB_DMABUF
 * (batch B3). B1 provides stubs; see memory.h for the hook conventions.
 *
 * Window geometry and the config register encoder live in blackhole.h
 * (bh_tlb_window_*(), bh_tlb_configure()); window BH_KERNEL_TLB_INDEX is
 * owned by the kernel and must never be handed out.
 */
#ifndef TT_TLB_H
#define TT_TLB_H

#include <linux/types.h>
#include <linux/compiler.h>

struct tenstorrent_device;
struct chardev_private;
struct vm_area_struct;
struct seq_file;

struct tt_tlb_dev_state {
};

struct tt_tlb_fd_state {
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
 * dev->open_lock held: release everything that uses the device (DMA
 * mappings, iATU regions, exports' device side); the PCI device is gone once
 * remove returns. May run concurrently with this fd's release hook: both
 * take the subsystem's per-fd lock; release afterwards only frees software
 * state (and must not call the DMA API once detached).
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
