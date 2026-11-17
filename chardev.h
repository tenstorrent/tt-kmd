/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Character device: file operations and helpers shared by the ioctl
 * implementations of every subsystem.
 */
#ifndef TT_CHARDEV_H
#define TT_CHARDEV_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>

#include "device.h"

extern const struct file_operations tt_chardev_fops;

/* Create/destroy /dev/tenstorrent/<ordinal> (cdev + class device). */
int tt_chardev_add(struct tenstorrent_device *dev);
void tt_chardev_remove(struct tenstorrent_device *dev);

/* PCI remove, after the detach wait: zap all user mappings of the device's
 * fds, then call every subsystem's fd_detach hook for each open fd. */
void tt_chardev_zap_mappings(struct tenstorrent_device *dev);
void tt_chardev_detach_fds(struct tenstorrent_device *dev);

/* Returns the chardev_private of @file if it is one of our fds, else NULL. */
struct chardev_private *tt_file_priv(struct file *file);

/*
 * ioctl argument convention
 * -------------------------
 * The user argument is `struct X { struct X_in in; struct X_out out; }`.
 * Copy exactly sizeof(in) from the start of the argument; never read `out`.
 *
 * For ioctls whose `in` carries output_size_bytes: write
 * min(output_size_bytes, sizeof(out)) bytes of `out` at the offset of `out`
 * and zero-fill the rest of the user's output_size_bytes (clear_user).
 * With output_size_bytes == 0 nothing beyond `in` is touched.
 *
 * Ioctls without output_size_bytes write the full `out` on success only.
 *
 * output_size_bytes above TT_IOCTL_MAX_OUTPUT_SIZE is rejected with -EINVAL;
 * call tt_check_output_size() before any side effect (tt_copy_out_sized()
 * checks again, too late to undo anything).
 */
int tt_check_output_size(u32 output_size_bytes);

static inline int tt_copy_in(void *in, const void __user *uarg, size_t in_size)
{
	return copy_from_user(in, uarg, in_size) ? -EFAULT : 0;
}

int tt_copy_out_sized(void __user *uout, const void *out, size_t out_size,
		      u32 output_size_bytes);

/* Convenience: arg of ioctl struct type T -> user pointer to its out. */
#define TT_UOUT(uarg, type) ((void __user *)((u8 __user *)(uarg) + offsetof(type, out)))

/*
 * Lazily faulted MMIO mapping (struct tt_mmio_mapping, see device.h).
 * Call from an mmap handler (inside the detach section tt_cdev_mmap() holds)
 * after filling m->priv, m->ops, m->phys, m->wc, m->no_split. Requires a
 * MAP_SHARED VMA; maps the whole VMA (m->size = VMA size) starting at
 * m->phys. Sets VM_IO|VM_PFNMAP|VM_DONTEXPAND|VM_DONTDUMP, the pgprot, and
 * the vm_ops (open/close refcount, may_split, fault). PTEs are inserted on
 * fault, the 2 MiB-aligned chunk around the faulting page at once.
 * On error nothing is installed and @m still belongs to the caller; on
 * success @m belongs to the VMA(s) and m->ops->release() frees it.
 */
int tt_mmio_mmap(struct vm_area_struct *vma, struct tt_mmio_mapping *m);

/*
 * debugfs "mappings": print one row for @priv with @type
 * ("OPEN_FD", "BAR", "DMA_BUF", "DMA_BUF+IATU", "PIN_PAGES", "TLB", ...)
 * and the printf-style details. Adds the newline.
 *
 * The tt_X_debugfs_show() hooks are called only for live fds (fully opened,
 * release not started), with dev->open_lock held, concurrently with the
 * fd's ioctls: take the subsystem's own per-fd lock while walking its state.
 */
__printf(4, 5)
void tt_mappings_row(struct seq_file *s, struct chardev_private *priv,
		     const char *type, const char *fmt, ...);

#endif
