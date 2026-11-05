// SPDX-FileCopyrightText: © 2026 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

// Kernel-mediated bulk NOC transfers (TENSTORRENT_IOCTL_DEV_NOC_{READ,WRITE}_BLOCK).
//
// This file owns the userspace side: validation, pinning, progress, signals.
// The device class owns the hardware side through four ops (see device.h):
// validate the target range, lock an aperture, hand out iomem windows into it,
// and unlock it again after draining. Today that is a CPU copy through a
// kernel-owned write-combining TLB window; DMA can replace it underneath the
// same ABI.

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "device.h"
#include "ioctl.h"
#include "noc_block_io.h"

// A chunk is the set of user pages pinned at once. It bounds how much memory a
// request holds down, and is the granularity at which the aperture lock is
// dropped and signals are checked.
#define PIN_PAGES_MAX	64

// Pins last one chunk, so they are short-term: no FOLL_LONGTERM.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static int block_pin_pages(unsigned long start, int nr_pages, bool writable, struct page **pages)
{
	return pin_user_pages_fast(start, nr_pages, writable ? FOLL_WRITE : 0, pages);
}

static void block_unpin_pages(struct page **pages, unsigned long nr_pages, bool make_dirty)
{
	unpin_user_pages_dirty_lock(pages, nr_pages, make_dirty);
}
#else
static int block_pin_pages(unsigned long start, int nr_pages, bool writable, struct page **pages)
{
	return get_user_pages_fast(start, nr_pages, writable ? FOLL_WRITE : 0, pages);
}

static void block_unpin_pages(struct page **pages, unsigned long nr_pages, bool make_dirty)
{
	unsigned long i;

	for (i = 0; i < nr_pages; i++) {
		if (make_dirty)
			set_page_dirty_lock(pages[i]);
		put_page(pages[i]);
	}
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
static void *block_map_page(struct page *page)
{
	return kmap_local_page(page);
}

static void block_unmap_page(struct page *page, void *addr)
{
	kunmap_local(addr);
}
#else
static void *block_map_page(struct page *page)
{
	return kmap(page);
}

static void block_unmap_page(struct page *page, void *addr)
{
	kunmap(page);
}
#endif

// Copy with a fixed access width: 64-bit when everything is 8-byte aligned,
// 32-bit otherwise. memcpy_{to,from}io() promise no width and may emit byte
// accesses at the edges, which some targets refuse. Through a WC mapping the
// CPU combines writes into bursts anyway, so the store width mostly matters
// for reads (one PCIe read per load) and under emulation (one exit per access).
static void block_copy(void __iomem *win, void *buf, size_t len, bool write)
{
#ifdef CONFIG_64BIT
	if (((uintptr_t)win | (uintptr_t)buf | len) % 8 == 0) {
		if (write) {
			__iowrite64_copy(win, buf, len / 8);
		} else {
			const u64 __iomem *src = win;
			u64 *dst = buf;
			size_t i;

			for (i = 0; i < len / 8; i++)
				dst[i] = __raw_readq(src + i);
		}
		return;
	}
#endif
	if (write)
		__iowrite32_copy(win, buf, len / 4);
	else
		__ioread32_copy(buf, win, len / 4);
}

// Move one chunk of mapped user memory. The class's aperture lock is held.
static int block_copy_chunk(struct tenstorrent_device *tt_dev, const struct tt_block_io *req, u64 addr,
			    void *kaddr, size_t len)
{
	const struct tenstorrent_device_class *dc = tt_dev->dev_class;
	size_t done = 0;

	while (done < len) {
		size_t this_len = len - done;
		void __iomem *win = dc->block_io_window(tt_dev, req, addr + done, &this_len);

		if (!win)
			return -EIO;

		block_copy(win, (u8 *)kaddr + done, this_len, req->write);
		done += this_len;
	}

	return 0;
}

static int block_transfer(struct tenstorrent_device *tt_dev, const struct tt_block_io *req,
			  unsigned long user_addr, u64 len, u64 *bytes_transferred)
{
	const struct tenstorrent_device_class *dc = tt_dev->dev_class;
	struct page *pages[PIN_PAGES_MAX];
	u64 addr = req->addr;
	u64 remaining = len;
	int ret = 0;

	while (remaining > 0) {
		size_t offset = offset_in_page(user_addr);
		size_t chunk = min_t(u64, remaining, PIN_PAGES_MAX * PAGE_SIZE - offset);
		int nr_pages = DIV_ROUND_UP(offset + chunk, PAGE_SIZE);
		size_t done = 0;
		int pinned;
		int i;

		if (signal_pending(current)) {
			ret = -EINTR;
			break;
		}

		// Pin before taking the aperture lock: gup can fault and take
		// mmap_lock, and a future fault-backed device mapping will take
		// aperture locks under mmap_lock.
		//
		// A block read stores into the caller's pages, so it is the
		// direction that pins for write.
		pinned = block_pin_pages(user_addr - offset, nr_pages, !req->write, pages);
		if (pinned <= 0) {
			ret = pinned < 0 ? pinned : -EFAULT;
			break;
		}

		// A short pin still covers a usable prefix: move it, then fail.
		if (pinned < nr_pages) {
			chunk = min_t(size_t, chunk, (size_t)pinned * PAGE_SIZE - offset);
			ret = -EFAULT;
		}

		{
			int lret = dc->block_io_lock(tt_dev, req);

			if (lret) {
				block_unpin_pages(pages, pinned, false);
				ret = lret;
				break;
			}
		}

		for (i = 0; i < pinned && done < chunk; i++) {
			size_t page_off = i == 0 ? offset : 0;
			size_t page_len = min_t(size_t, chunk - done, PAGE_SIZE - page_off);
			void *kaddr = block_map_page(pages[i]);
			int cret = block_copy_chunk(tt_dev, req, addr + done, (u8 *)kaddr + page_off, page_len);

			block_unmap_page(pages[i], kaddr);
			if (cret) {
				ret = cret;
				break;
			}

			// A kernel store into a page userspace also maps needs a
			// flush on architectures with aliasing D-caches.
			if (!req->write)
				flush_dcache_page(pages[i]);

			done += page_len;
		}

		dc->block_io_unlock(tt_dev, req);

		block_unpin_pages(pages, pinned, !req->write && done);

		*bytes_transferred += done;
		addr += done;
		user_addr += done;
		remaining -= done;

		if (ret)
			break;

		cond_resched();
	}

	return ret;
}

static long validate_noc_block_io(const struct tenstorrent_noc_block_io *data)
{
	if (data->argsz != sizeof(*data))
		return -EINVAL;

	if ((data->flags & ~TENSTORRENT_NOC_FLAG_KLA) != 0 || data->bytes_transferred != 0)
		return -EINVAL;

	if (data->reserved0[0] != 0 || data->reserved0[1] != 0 || data->reserved0[2] != 0)
		return -EINVAL;

	if (data->x > 63 || data->y > 63 || data->noc > 1)
		return -EINVAL;

	if (data->data_len == 0 || data->data_len > TENSTORRENT_NOC_BLOCK_IO_MAX_LEN)
		return -EINVAL;

	// The kernel picks fixed 32- or 64-bit access widths, so the range and
	// buffer must be whole 32-bit units on 32-bit boundaries.
	if ((data->addr | data->data_len | data->data_ptr) & 3)
		return -EINVAL;

	if (data->addr + data->data_len < data->addr)
		return -EINVAL;

	// The whole buffer must be representable as a user pointer.
	if (data->data_ptr + data->data_len < data->data_ptr ||
	    data->data_ptr + data->data_len - 1 > (u64)ULONG_MAX)
		return -EFAULT;

	return 0;
}

// Caller holds reset_rwsem shared for the whole call, so a reset waits for the
// transfer; a signal ends it early with the completed prefix reported.
long tenstorrent_noc_block_io(struct tenstorrent_device *tt_dev, struct tenstorrent_noc_block_io __user *arg,
			      bool write)
{
	const struct tenstorrent_device_class *dc = tt_dev->dev_class;
	struct tenstorrent_noc_block_io data = {0};
	struct tt_block_io req;
	u64 bytes_transferred = 0;
	long ret;

	if (!dc->block_io_validate || !dc->block_io_lock || !dc->block_io_window || !dc->block_io_unlock)
		return -EOPNOTSUPP;

	if (copy_from_user(&data, arg, sizeof(data)) != 0)
		return -EFAULT;

	ret = validate_noc_block_io(&data);
	if (ret)
		return ret;

	// The request is written back on return; a buffer overlapping it would
	// have reported-complete read data overwritten by the structure.
	if (data.data_ptr < (u64)(uintptr_t)(arg + 1) &&
	    (u64)(uintptr_t)arg < data.data_ptr + data.data_len)
		return -EINVAL;

	req.addr = data.addr;
	req.x = data.x;
	req.y = data.y;
	req.noc = data.noc;
	req.kla = data.flags & TENSTORRENT_NOC_FLAG_KLA;
	req.write = write;

	ret = dc->block_io_validate(tt_dev, &req, data.data_len);
	if (ret)
		return ret;

	ret = block_transfer(tt_dev, &req, (unsigned long)data.data_ptr, data.data_len, &bytes_transferred);

	// Progress is part of the result, success or not.
	data.bytes_transferred = bytes_transferred;
	if (copy_to_user(arg, &data, sizeof(data)) != 0)
		return -EFAULT;

	return ret;
}
