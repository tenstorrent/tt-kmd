// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA buffers (ALLOCATE_DMA_BUF + mmap), pinned user pages (PIN/UNPIN_PAGES)
 * and the outbound iATU that gives either of them a NOC address.
 *
 * Outbound iATU
 * -------------
 * Device code reaches host memory through the PCIe tile's NoC-to-host
 * windows (NoC address bits 63:58 select the window). The firmware sets
 * window 4 (NoC 0x1000_0000_0000_0000 + 2^58) to "through the outbound
 * iATU". The iATU is a Synopsys DesignWare one. Its registers are in BAR2:
 * outbound region n at BAR2 0x1000 + n * 0x200, inbound region n at +0x100,
 * 16 regions, 4 KiB granule. We found that layout on the card (the specs do
 * not document BH BAR2). Registers of a region (DW "unrolled" layout):
 * CTRL1 +0x00, CTRL2 +0x04 (bit 31 enable), LOWER/UPPER_BASE +0x08/+0x0C,
 * LIMIT +0x10, LOWER/UPPER_TARGET +0x14/+0x18, UPPER_LIMIT +0x20. The low
 * 12 bits of base and target are hard-wired to 0 and those of the limit to
 * 1 (4 KiB granule); probe uses that to tell real registers from an
 * all-ones BAR2.
 *
 * At probe, outbound regions that are disabled, of type MEM and pass that
 * sanity check become ours. A region found enabled is left alone: we cannot
 * tell whether someone else owns it. After a crash of a previous driver
 * instance, the regions it left enabled stay lost until the card is reset.
 * We write only our regions (CTRL1 = 0, CTRL2, bases, limits, targets) and
 * never inbound regions. Disabling a region also clears its address
 * registers.
 *
 * A region maps [noc_offset, +size) of window 4 to the buffer's IOVA (or
 * physical address). The iATU is assumed to see the window offset (NoC
 * address bits 63:58 are replaced by the window's NOC_TLB_DATA.addr field,
 * which the firmware sets to 0 for window 4). The data path cannot be checked
 * from the host (every host-side path is a loop back through the PCIe tile),
 * so programming is checked by register read-back only.
 *
 * NOC address space: offsets in window 4 are given out bottom-up from 0 by
 * default, or top-down from 2^58 with PIN_PAGES_NOC_TOP_DOWN. Each mapping is
 * aligned to its size rounded up to a power of two, so a mapping (at most
 * 4 GiB) never crosses a 4 GiB boundary, and the upper limit register is
 * never needed.
 *
 * Lifetime
 * --------
 * All per-fd objects are torn down, under the per-fd lock, by whichever of
 * fd_release and fd_detach comes first. Release may find objects left only
 * if detach has not processed this fd yet. The fd stays on dev->open_fds
 * until after its release hooks, so PCI remove has then not passed
 * tt_chardev_detach_fds(), and the PCI device, BAR2 and the DMA API are
 * still usable. A release after detach finds nothing left and touches no
 * hardware.
 *
 * DMA buffer mmaps are populated lazily: ->mmap only validates and sets the
 * VMA up; the fault handler, inside a detach section and under the per-fd
 * lock, looks the buffer up and inserts the PFN. PCI remove sets detached,
 * waits for every section, zaps all user mappings and only then frees the
 * buffers (fd_detach), so no PTE can point at freed memory: later faults
 * SIGBUS. (Release cannot free a buffer while it is mapped: every VMA holds
 * a file reference.)
 */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/sizes.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "device.h"
#include "chardev.h"
#include "memory.h"

/* ---- Outbound iATU (BAR2) ---- */

#define TT_IATU_BAR2_BASE		0x1000
#define TT_IATU_REGION_STRIDE		0x200
#define TT_IATU_OB(n)			(TT_IATU_BAR2_BASE + (n) * TT_IATU_REGION_STRIDE)
#define TT_IATU_CTRL1			0x00
#define TT_IATU_CTRL2			0x04
#define TT_IATU_LOWER_BASE		0x08
#define TT_IATU_UPPER_BASE		0x0C
#define TT_IATU_LIMIT			0x10
#define TT_IATU_LOWER_TARGET		0x14
#define TT_IATU_UPPER_TARGET		0x18
#define TT_IATU_UPPER_LIMIT		0x20
#define TT_IATU_CTRL1_TYPE_MASK		0x1f	/* 0 = memory */
#define TT_IATU_CTRL2_REGION_EN		BIT(31)
#define TT_IATU_GRANULE			SZ_4K
#define TT_IATU_GRANULE_MASK		(TT_IATU_GRANULE - 1)

/* NoC-to-host window 4: through the outbound iATU. */
#define TT_NOC_DMA_WINDOW_BASE		0x1000000000000000ULL
#define TT_NOC_DMA_WINDOW_SIZE		(1ULL << 58)
#define TT_NOC_DMA_MAX_SIZE		SZ_4G

#define TT_PIN_FLAGS_ALL	(TENSTORRENT_PIN_PAGES_CONTIGUOUS | TENSTORRENT_PIN_PAGES_NOC_DMA | \
				 TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN | TENSTORRENT_PIN_PAGES_READ_ONLY)

/* pin_user_pages_fast() takes an int; also bounds the page array. */
#define TT_PIN_MAX_PAGES	(INT_MAX / sizeof(struct page *))

struct tt_dma_buf {
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
	int iatu;		/* region index or -1 */
	u64 noc_addr;
	u8 index;
};

struct tt_pin {
	struct rb_node node;	/* in priv->memory.pins */
	struct list_head free_node;	/* teardown: freed after the lock is dropped */
	u64 va;
	u64 size;
	u32 flags;
	struct mm_struct *mm;	/* mmgrab()ed; locked_vm charged when accounted */
	bool accounted;
	struct page **pages;
	unsigned long npages;
	struct sg_table sgt;
	bool dma_mapped;
	dma_addr_t dma_addr;
	int iatu;		/* region index or -1 */
	u64 noc_addr;
};

static void __iomem *tt_iatu_reg(struct tenstorrent_device *dev, unsigned int region,
				 unsigned int reg)
{
	return dev->bar2 + TT_IATU_OB(region) + reg;
}

/* Do the region's registers look real (hard-wired bits as expected)? */
static bool tt_iatu_region_sane(struct tenstorrent_device *dev, unsigned int region)
{
	u32 ctrl2 = ioread32(tt_iatu_reg(dev, region, TT_IATU_CTRL2));
	u32 base = ioread32(tt_iatu_reg(dev, region, TT_IATU_LOWER_BASE));
	u32 limit = ioread32(tt_iatu_reg(dev, region, TT_IATU_LIMIT));
	u32 target = ioread32(tt_iatu_reg(dev, region, TT_IATU_LOWER_TARGET));

	return ctrl2 != 0xffffffff && !(base & TT_IATU_GRANULE_MASK) &&
	       !(target & TT_IATU_GRANULE_MASK) &&
	       (limit & TT_IATU_GRANULE_MASK) == TT_IATU_GRANULE_MASK;
}

static void tt_iatu_disable_hw(struct tenstorrent_device *dev, unsigned int region)
{
	static const unsigned int regs[] = {
		TT_IATU_LOWER_BASE, TT_IATU_UPPER_BASE, TT_IATU_LIMIT, TT_IATU_UPPER_LIMIT,
		TT_IATU_LOWER_TARGET, TT_IATU_UPPER_TARGET,
	};
	unsigned int i;

	iowrite32(0, tt_iatu_reg(dev, region, TT_IATU_CTRL2));
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		iowrite32(0, tt_iatu_reg(dev, region, regs[i]));
	ioread32(tt_iatu_reg(dev, region, TT_IATU_CTRL2));	/* flush */
}

static int tt_iatu_program_hw(struct tenstorrent_device *dev, unsigned int region,
			      u64 base, u64 size, u64 target)
{
	u64 limit = base + size - 1;

	iowrite32(0, tt_iatu_reg(dev, region, TT_IATU_CTRL2));
	iowrite32(0, tt_iatu_reg(dev, region, TT_IATU_CTRL1));	/* memory, no options */
	iowrite32(lower_32_bits(base), tt_iatu_reg(dev, region, TT_IATU_LOWER_BASE));
	iowrite32(upper_32_bits(base), tt_iatu_reg(dev, region, TT_IATU_UPPER_BASE));
	iowrite32(lower_32_bits(limit), tt_iatu_reg(dev, region, TT_IATU_LIMIT));
	iowrite32(upper_32_bits(limit), tt_iatu_reg(dev, region, TT_IATU_UPPER_LIMIT));
	iowrite32(lower_32_bits(target), tt_iatu_reg(dev, region, TT_IATU_LOWER_TARGET));
	iowrite32(upper_32_bits(target), tt_iatu_reg(dev, region, TT_IATU_UPPER_TARGET));
	iowrite32(TT_IATU_CTRL2_REGION_EN, tt_iatu_reg(dev, region, TT_IATU_CTRL2));

	/* Read back: the writes reached the controller and took effect. */
	if (ioread32(tt_iatu_reg(dev, region, TT_IATU_CTRL1)) != 0 ||
	    ioread32(tt_iatu_reg(dev, region, TT_IATU_CTRL2)) != TT_IATU_CTRL2_REGION_EN ||
	    ioread32(tt_iatu_reg(dev, region, TT_IATU_LOWER_BASE)) != lower_32_bits(base) ||
	    ioread32(tt_iatu_reg(dev, region, TT_IATU_UPPER_BASE)) != upper_32_bits(base) ||
	    ioread32(tt_iatu_reg(dev, region, TT_IATU_LIMIT)) != lower_32_bits(limit) ||
	    ioread32(tt_iatu_reg(dev, region, TT_IATU_LOWER_TARGET)) != lower_32_bits(target) ||
	    ioread32(tt_iatu_reg(dev, region, TT_IATU_UPPER_TARGET)) != upper_32_bits(target)) {
		tt_iatu_disable_hw(dev, region);
		dev_err_ratelimited(&dev->pdev->dev,
				    "outbound iATU region %u did not take its configuration\n",
				    region);
		return -EIO;
	}
	return 0;
}

static bool tt_noc_range_overlaps(u64 a, u64 a_size, u64 b, u64 b_size)
{
	return a < b + b_size && b < a + a_size;
}

/* Find a free, @align-aligned range of @size in window 4. iatu_lock held. */
static int tt_noc_range_find(struct tt_memory_dev_state *mem, u64 size, u64 align,
			     bool top_down, u64 *offset)
{
	u64 cand = top_down ? TT_NOC_DMA_WINDOW_SIZE - align : 0;
	bool moved;
	int i;

	do {
		moved = false;
		for (i = 0; i < TT_IATU_NUM_REGIONS; i++) {
			struct tt_iatu_region *r = &mem->iatu[i];

			if (!r->used || !tt_noc_range_overlaps(cand, size, r->noc_offset, r->size))
				continue;
			if (top_down) {
				if (r->noc_offset < size)
					return -ENOSPC;
				cand = ALIGN_DOWN(r->noc_offset - size, align);
			} else {
				cand = ALIGN(r->noc_offset + r->size, align);
				if (cand + size > TT_NOC_DMA_WINDOW_SIZE)
					return -ENOSPC;
			}
			moved = true;
		}
	} while (moved);

	*offset = cand;
	return 0;
}

/*
 * Map [@dma_addr, +@size) at a free NOC address of window 4 through a free
 * outbound region. Device attached (ioctl). Returns the region index.
 */
static int tt_iatu_map(struct tenstorrent_device *dev, dma_addr_t dma_addr, u64 size,
		       bool top_down, u64 *noc_addr)
{
	struct tt_memory_dev_state *mem = &dev->memory;
	u64 align, offset;
	int region, ret;

	if (!mem->iatu_present)
		return -ENODEV;
	if (!size || size > TT_NOC_DMA_MAX_SIZE || !IS_ALIGNED(size, TT_IATU_GRANULE) ||
	    !IS_ALIGNED(dma_addr, TT_IATU_GRANULE))
		return -EINVAL;
	/* Smallest power of two >= size (size <= 4 GiB, so no overflow). */
	align = max_t(u64, 1ULL << fls64(size - 1), TT_IATU_GRANULE);

	mutex_lock(&mem->iatu_lock);
	for (region = 0; region < TT_IATU_NUM_REGIONS; region++)
		if (mem->iatu[region].usable && !mem->iatu[region].used)
			break;
	if (region == TT_IATU_NUM_REGIONS) {
		ret = -ENOSPC;
		goto out;
	}

	ret = tt_noc_range_find(mem, size, align, top_down, &offset);
	if (ret)
		goto out;

	ret = tt_iatu_program_hw(dev, region, offset, size, dma_addr);
	if (ret)
		goto out;

	mem->iatu[region].used = true;
	mem->iatu[region].noc_offset = offset;
	mem->iatu[region].size = size;
	*noc_addr = TT_NOC_DMA_WINDOW_BASE + offset;
	ret = region;
out:
	mutex_unlock(&mem->iatu_lock);
	return ret;
}

static void tt_iatu_unmap(struct tenstorrent_device *dev, int region)
{
	struct tt_memory_dev_state *mem = &dev->memory;

	if (region < 0)
		return;

	mutex_lock(&mem->iatu_lock);
	if (mem->iatu[region].used) {
		tt_iatu_disable_hw(dev, region);
		mem->iatu[region].used = false;
	}
	mutex_unlock(&mem->iatu_lock);
}

int tt_memory_device_init(struct tenstorrent_device *dev)
{
	struct tt_memory_dev_state *mem = &dev->memory;
	unsigned int i, usable = 0, busy = 0;

	mutex_init(&mem->iatu_lock);

	for (i = 0; i < TT_IATU_NUM_REGIONS; i++) {
		u32 ctrl1, ctrl2;

		if (!tt_iatu_region_sane(dev, i))
			continue;
		ctrl1 = ioread32(tt_iatu_reg(dev, i, TT_IATU_CTRL1));
		ctrl2 = ioread32(tt_iatu_reg(dev, i, TT_IATU_CTRL2));
		/* Leave regions someone else set up alone. */
		if ((ctrl2 & TT_IATU_CTRL2_REGION_EN) || (ctrl1 & TT_IATU_CTRL1_TYPE_MASK)) {
			busy++;
			continue;
		}
		mem->iatu[i].usable = true;
		usable++;
	}
	mem->iatu_present = usable > 0;
	if (!usable)
		dev_warn(&dev->pdev->dev, "no usable outbound iATU region: no NOC DMA\n");
	else if (usable < TT_IATU_NUM_REGIONS)
		dev_info(&dev->pdev->dev,
			 "%u of %u outbound iATU regions available (%u already enabled)\n",
			 usable, TT_IATU_NUM_REGIONS, busy);
	return 0;
}

void tt_memory_device_fini(struct tenstorrent_device *dev)
{
	struct tt_memory_dev_state *mem = &dev->memory;
	unsigned int i;

	/* Every fd's mappings are gone (fd_detach); disable any leftover. */
	mutex_lock(&mem->iatu_lock);
	for (i = 0; i < TT_IATU_NUM_REGIONS; i++) {
		if (!mem->iatu[i].used)
			continue;
		tt_iatu_disable_hw(dev, i);
		mem->iatu[i].used = false;
	}
	mutex_unlock(&mem->iatu_lock);
	mutex_destroy(&mem->iatu_lock);
}

/* ---- DMA buffers ---- */

static void tt_dma_buf_free(struct tenstorrent_device *dev, struct tt_dma_buf *buf)
{
	tt_iatu_unmap(dev, buf->iatu);
	dma_free_coherent(&dev->pdev->dev, buf->size, buf->cpu_addr, buf->dma_addr);
	kfree(buf);
}

long tt_memory_ioctl_allocate_dma_buf(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_memory_fd_state *fm = &priv->memory;
	struct tenstorrent_allocate_dma_buf_in in;
	struct tenstorrent_allocate_dma_buf_out out = {};
	struct tt_dma_buf *buf;
	unsigned int idx;
	size_t size;
	long ret;

	if (tt_copy_in(&in, arg, sizeof(in)))
		return -EFAULT;

	if (in.flags & ~TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA)
		return -EINVAL;
	if (in.reserved0[0] || in.reserved0[1] || in.reserved1[0] || in.reserved1[1])
		return -EINVAL;
	if (!in.requested_size || in.requested_size > (1ULL << TT_MAX_DMA_BUF_SIZE_LOG2))
		return -EINVAL;
	/* buf_index is a u8: always < TENSTORRENT_MAX_DMA_BUFS. */
	idx = in.buf_index;
	size = PAGE_ALIGN((size_t)in.requested_size);

	mutex_lock(&fm->lock);
	if (test_bit(idx, fm->dma_buf_busy)) {
		mutex_unlock(&fm->lock);
		return -EINVAL;
	}
	__set_bit(idx, fm->dma_buf_busy);
	mutex_unlock(&fm->lock);

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_unreserve;
	}
	buf->index = idx;
	buf->size = size;
	buf->iatu = -1;

	/* Large requests may fail; the caller retries smaller. No warning.
	 * The memory comes back zeroed (it is mapped to user space). */
	buf->cpu_addr = dma_alloc_coherent(&dev->pdev->dev, size, &buf->dma_addr,
					   GFP_KERNEL | __GFP_NOWARN);
	if (!buf->cpu_addr) {
		kfree(buf);
		ret = -ENOMEM;
		goto err_unreserve;
	}

	if (in.flags & TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA) {
		ret = tt_iatu_map(dev, buf->dma_addr, size, false, &buf->noc_addr);
		if (ret < 0)
			goto err_free;
		buf->iatu = ret;
	}

	out.physical_address = buf->dma_addr;
	out.mapping_offset = TT_MMAP_DMA_BUF_BASE + (u64)idx * TT_MMAP_DMA_BUF_STRIDE;
	out.size = size;
	out.noc_address = buf->noc_addr;
	if (copy_to_user(TT_UOUT(arg, struct tenstorrent_allocate_dma_buf), &out, sizeof(out))) {
		ret = -EFAULT;
		goto err_free;
	}

	/* The index stays reserved for us: install. */
	mutex_lock(&fm->lock);
	fm->dma_bufs[idx] = buf;
	mutex_unlock(&fm->lock);
	return 0;

err_free:
	tt_dma_buf_free(dev, buf);
err_unreserve:
	mutex_lock(&fm->lock);
	__clear_bit(idx, fm->dma_buf_busy);
	mutex_unlock(&fm->lock);
	return ret;
}

/*
 * FREE_DMA_BUF has an empty argument (no index), so there is nothing to
 * identify a buffer by: always -EINVAL. Buffers are freed when the fd is
 * closed.
 */
long tt_memory_ioctl_free_dma_buf(struct chardev_private *priv, void __user *arg)
{
	return -EINVAL;
}

/* Split a DMA-buffer mmap offset into buffer index and offset inside it. */
static void tt_dma_buf_offset(unsigned long pgoff, unsigned int *idx, u64 *rel)
{
	u64 off = ((u64)pgoff << PAGE_SHIFT) - TT_MMAP_DMA_BUF_BASE;

	*idx = off / TT_MMAP_DMA_BUF_STRIDE;
	*rel = off & (TT_MMAP_DMA_BUF_STRIDE - 1);
}

/* PFN of byte @rel of @buf (coherent memory: linear map or vmap()ed). */
static unsigned long tt_dma_buf_pfn(struct tt_dma_buf *buf, u64 rel)
{
	void *p = buf->cpu_addr + rel;

	return is_vmalloc_addr(p) ? vmalloc_to_pfn(p) : page_to_pfn(virt_to_page(p));
}

static vm_fault_t tt_dma_buf_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct chardev_private *priv = vma->vm_private_data;
	struct tenstorrent_device *dev = priv->dev;
	struct tt_memory_fd_state *fm = &priv->memory;
	struct tt_dma_buf *buf;
	unsigned int idx;
	vm_fault_t ret;
	u64 rel;
	int srcu;

	/* No PTE may appear once remove has started (it zaps, then frees). */
	if (tt_dev_enter(dev, &srcu))
		return VM_FAULT_SIGBUS;

	tt_dma_buf_offset(vmf->pgoff, &idx, &rel);
	mutex_lock(&fm->lock);
	buf = idx < TENSTORRENT_MAX_DMA_BUFS ? fm->dma_bufs[idx] : NULL;
	if (!buf || rel >= buf->size)
		ret = VM_FAULT_SIGBUS;
	else
		ret = vmf_insert_pfn(vma, vmf->address, tt_dma_buf_pfn(buf, rel));
	mutex_unlock(&fm->lock);

	tt_dev_exit(dev, srcu);
	return ret;
}

static const struct vm_operations_struct tt_dma_buf_vm_ops = {
	.fault = tt_dma_buf_fault,
};

int tt_memory_mmap(struct chardev_private *priv, struct vm_area_struct *vma)
{
	struct tt_memory_fd_state *fm = &priv->memory;
	u64 size = vma->vm_end - vma->vm_start;
	struct tt_dma_buf *buf;
	unsigned int idx;
	int ret = 0;
	u64 rel;

	/* Shared only: PFN mappings cannot be copied on write. */
	if (!(vma->vm_flags & VM_MAYSHARE))
		return -EINVAL;
	tt_dma_buf_offset(vma->vm_pgoff, &idx, &rel);
	if (idx >= TENSTORRENT_MAX_DMA_BUFS)
		return -EINVAL;

	mutex_lock(&fm->lock);
	buf = fm->dma_bufs[idx];
	if (!buf || rel >= buf->size || size > buf->size - rel)
		ret = -EINVAL;
	mutex_unlock(&fm->lock);
	if (ret)
		return ret;

	/* Coherent DMA memory is cacheable on the architectures we support;
	 * vm_page_prot stays as the VMA has it. */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_private_data = priv;
	vma->vm_ops = &tt_dma_buf_vm_ops;
	return 0;
}

/* ---- Pinned pages ---- */

static int tt_pin_cmp(u64 va, u64 size, const struct tt_pin *pin)
{
	if (va != pin->va)
		return va < pin->va ? -1 : 1;
	if (size != pin->size)
		return size < pin->size ? -1 : 1;
	return 0;
}

static int tt_pin_node_cmp(struct rb_node *a, const struct rb_node *b)
{
	const struct tt_pin *pa = rb_entry(a, struct tt_pin, node);

	return tt_pin_cmp(pa->va, pa->size, rb_entry(b, struct tt_pin, node));
}

struct tt_pin_key {
	u64 va, size;
};

static int tt_pin_key_cmp(const void *key, const struct rb_node *node)
{
	const struct tt_pin_key *k = key;

	return tt_pin_cmp(k->va, k->size, rb_entry(node, struct tt_pin, node));
}

static struct tt_pin *tt_pin_find(struct tt_memory_fd_state *fm, u64 va, u64 size)
{
	struct tt_pin_key key = { va, size };
	struct rb_node *node;

	lockdep_assert_held(&fm->lock);
	node = rb_find(&key, &fm->pins, tt_pin_key_cmp);
	return node ? rb_entry(node, struct tt_pin, node) : NULL;
}

/* Undo everything that involves the device and the pages. No mmap_lock:
 * may run under the fd memory lock. */
static void tt_pin_release(struct tenstorrent_device *dev, struct tt_pin *pin)
{
	bool ro = pin->flags & TENSTORRENT_PIN_PAGES_READ_ONLY;

	tt_iatu_unmap(dev, pin->iatu);
	pin->iatu = -1;
	if (pin->dma_mapped)
		dma_unmap_sgtable(&dev->pdev->dev, &pin->sgt,
				  ro ? DMA_TO_DEVICE : DMA_BIDIRECTIONAL, 0);
	pin->dma_mapped = false;
	sg_free_table(&pin->sgt);
	if (pin->npages)
		unpin_user_pages_dirty_lock(pin->pages, pin->npages, !ro);
}

/* Free the software state; takes the pinning mm's mmap_lock (locked_vm):
 * never call under the fd memory lock. */
static void tt_pin_destroy(struct tt_pin *pin)
{
	if (pin->accounted)
		account_locked_vm(pin->mm, pin->size >> PAGE_SHIFT, false);
	if (pin->mm)
		mmdrop(pin->mm);
	kvfree(pin->pages);
	kfree(pin);
}

static void tt_pin_free(struct tenstorrent_device *dev, struct tt_pin *pin)
{
	tt_pin_release(dev, pin);
	tt_pin_destroy(pin);
}

/* Does the IOMMU translate the device's DMA (vs. no IOMMU or identity)? */
static bool tt_iommu_translated(struct tenstorrent_device *dev)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(&dev->pdev->dev);

	return domain && (domain->type & __IOMMU_DOMAIN_PAGING);
}

/* Charge, pin, DMA-map and check that the mapping is one contiguous DMA
 * range. On error the caller frees @pin (tt_pin_free). */
static int tt_pin_map(struct tenstorrent_device *dev, struct tt_pin *pin)
{
	bool ro = pin->flags & TENSTORRENT_PIN_PAGES_READ_ONLY;
	unsigned int gup_flags = FOLL_LONGTERM | (ro ? 0 : FOLL_WRITE);
	unsigned long want = pin->size >> PAGE_SHIFT;
	struct scatterlist *sg;
	u64 total = 0;
	int ret, i;

	/* Pinned pages count against RLIMIT_MEMLOCK (unless CAP_IPC_LOCK). */
	mmgrab(current->mm);
	pin->mm = current->mm;
	ret = account_locked_vm(pin->mm, want, true);
	if (ret)
		return ret;
	pin->accounted = true;

	pin->pages = kvmalloc_array(want, sizeof(*pin->pages), GFP_KERNEL | __GFP_NOWARN);
	if (!pin->pages)
		return -ENOMEM;

	while (pin->npages < want) {
		ret = pin_user_pages_fast(pin->va + (pin->npages << PAGE_SHIFT),
					  want - pin->npages, gup_flags,
					  pin->pages + pin->npages);
		if (ret <= 0)
			return ret ? ret : -EFAULT;
		pin->npages += ret;
	}

	ret = sg_alloc_table_from_pages(&pin->sgt, pin->pages, pin->npages, 0, pin->size,
					GFP_KERNEL);
	if (ret)
		return ret;

	ret = dma_map_sgtable(&dev->pdev->dev, &pin->sgt,
			      ro ? DMA_TO_DEVICE : DMA_BIDIRECTIONAL, 0);
	if (ret)
		return ret;
	pin->dma_mapped = true;

	/* Without IOMMU translation this requires physically contiguous pages;
	 * with it, the IOMMU maps the whole list to one IOVA range (possibly
	 * reported as several adjacent segments). */
	for_each_sgtable_dma_sg(&pin->sgt, sg, i) {
		if (i == 0)
			pin->dma_addr = sg_dma_address(sg);
		else if (sg_dma_address(sg) != pin->dma_addr + total)
			return -EINVAL;
		total += sg_dma_len(sg);
	}
	if (total != pin->size)
		return -EINVAL;
	return 0;
}

long tt_memory_ioctl_pin_pages(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_memory_fd_state *fm = &priv->memory;
	struct tenstorrent_pin_pages_in in;
	struct tenstorrent_pin_pages_out_extended out = {};
	struct tt_pin *pin;
	bool dup;
	long ret;

	if (tt_copy_in(&in, arg, sizeof(in)))
		return -EFAULT;
	if (tt_check_output_size(in.output_size_bytes))
		return -EINVAL;

	if (in.flags & ~TT_PIN_FLAGS_ALL)
		return -EINVAL;
	if ((in.flags & TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN) &&
	    !(in.flags & TENSTORRENT_PIN_PAGES_NOC_DMA))
		return -EINVAL;
	if (!in.size || !PAGE_ALIGNED(in.size) || !PAGE_ALIGNED(in.virtual_address))
		return -EINVAL;
	if ((in.size >> PAGE_SHIFT) > TT_PIN_MAX_PAGES ||
	    in.virtual_address + in.size < in.virtual_address ||
	    in.virtual_address + in.size > TASK_SIZE)
		return -EINVAL;
	if ((in.flags & TENSTORRENT_PIN_PAGES_NOC_DMA) && in.size > TT_NOC_DMA_MAX_SIZE)
		return -EINVAL;
	/* Read-only can only be enforced by the IOMMU. */
	if ((in.flags & TENSTORRENT_PIN_PAGES_READ_ONLY) && !tt_iommu_translated(dev))
		return -EINVAL;
	if (!current->mm)
		return -EINVAL;

	/* Cheap early check; the authoritative one is at insertion. */
	mutex_lock(&fm->lock);
	dup = tt_pin_find(fm, in.virtual_address, in.size);
	mutex_unlock(&fm->lock);
	if (dup)
		return -EEXIST;

	pin = kzalloc(sizeof(*pin), GFP_KERNEL);
	if (!pin)
		return -ENOMEM;
	RB_CLEAR_NODE(&pin->node);
	INIT_LIST_HEAD(&pin->free_node);
	pin->va = in.virtual_address;
	pin->size = in.size;
	pin->flags = in.flags;
	pin->iatu = -1;

	ret = tt_pin_map(dev, pin);
	if (ret)
		goto err;

	if (in.flags & TENSTORRENT_PIN_PAGES_NOC_DMA) {
		ret = tt_iatu_map(dev, pin->dma_addr, pin->size,
				  in.flags & TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN, &pin->noc_addr);
		if (ret < 0)
			goto err;
		pin->iatu = ret;
	}

	/* tenstorrent_pin_pages_out is a prefix of the extended struct. */
	out.physical_address = pin->dma_addr;
	out.noc_address = pin->noc_addr;
	ret = tt_copy_out_sized(TT_UOUT(arg, struct tenstorrent_pin_pages), &out, sizeof(out),
				in.output_size_bytes);
	if (ret)
		goto err;

	/* Atomic check-and-insert: a concurrent pin of the same range may
	 * have won meanwhile. */
	mutex_lock(&fm->lock);
	dup = rb_find_add(&pin->node, &fm->pins, tt_pin_node_cmp);
	mutex_unlock(&fm->lock);
	if (dup) {
		ret = -EEXIST;
		goto err;
	}
	return 0;

err:
	tt_pin_free(dev, pin);
	return ret;
}

long tt_memory_ioctl_unpin_pages(struct chardev_private *priv, void __user *arg)
{
	struct tt_memory_fd_state *fm = &priv->memory;
	struct tenstorrent_unpin_pages_in in;
	struct tt_pin *pin;

	if (tt_copy_in(&in, arg, sizeof(in)))
		return -EFAULT;
	if (in.reserved || !in.size || !PAGE_ALIGNED(in.size))
		return -EINVAL;

	mutex_lock(&fm->lock);
	pin = tt_pin_find(fm, in.virtual_address, in.size);
	if (pin)
		rb_erase(&pin->node, &fm->pins);
	mutex_unlock(&fm->lock);
	if (!pin)
		return -EINVAL;

	tt_pin_free(priv->dev, pin);
	return 0;
}

/* ---- per-fd lifecycle ---- */

void tt_memory_fd_init(struct chardev_private *priv)
{
	struct tt_memory_fd_state *fm = &priv->memory;

	mutex_init(&fm->lock);
	fm->pins = RB_ROOT;
}

int tt_memory_fd_open(struct chardev_private *priv)
{
	return 0;
}

/* Free everything this fd holds; see "Lifetime" at the top for why this is
 * safe from both release and detach. Everything that touches the device
 * happens under the lock, so the other caller cannot return (and remove
 * cannot finish) while we still use it; only the locked_vm uncharge (which
 * takes mmap_lock) is done after dropping the lock. */
static void tt_memory_teardown(struct chardev_private *priv)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_memory_fd_state *fm = &priv->memory;
	struct tt_pin *pin, *tmp;
	struct rb_node *node;
	LIST_HEAD(dead);
	unsigned int i;

	mutex_lock(&fm->lock);
	while ((node = rb_first(&fm->pins))) {
		pin = rb_entry(node, struct tt_pin, node);
		rb_erase(node, &fm->pins);
		tt_pin_release(dev, pin);
		list_add_tail(&pin->free_node, &dead);
		cond_resched();
	}
	for (i = 0; i < TENSTORRENT_MAX_DMA_BUFS; i++) {
		if (!fm->dma_bufs[i])
			continue;
		tt_dma_buf_free(dev, fm->dma_bufs[i]);
		fm->dma_bufs[i] = NULL;
		__clear_bit(i, fm->dma_buf_busy);
	}
	mutex_unlock(&fm->lock);

	list_for_each_entry_safe(pin, tmp, &dead, free_node)
		tt_pin_destroy(pin);
}

void tt_memory_fd_release(struct chardev_private *priv)
{
	/* No mutex_destroy: remove's fd_detach may still take the lock until
	 * this fd leaves dev->open_fds. */
	tt_memory_teardown(priv);
}

void tt_memory_fd_detach(struct chardev_private *priv)
{
	tt_memory_teardown(priv);
}

/* ---- debugfs ---- */

void tt_memory_debugfs_show(struct seq_file *s, struct chardev_private *priv)
{
	struct tt_memory_fd_state *fm = &priv->memory;
	struct rb_node *node;
	unsigned int i;

	mutex_lock(&fm->lock);
	for (node = rb_first(&fm->pins); node; node = rb_next(node)) {
		struct tt_pin *pin = rb_entry(node, struct tt_pin, node);
		const char *ro = (pin->flags & TENSTORRENT_PIN_PAGES_READ_ONLY) ? " ro" : "";

		if (pin->iatu >= 0)
			tt_mappings_row(s, priv, "PIN_PAGES+IATU",
					"va 0x%llx size 0x%llx iova 0x%llx noc 0x%llx region %d%s",
					pin->va, pin->size, (u64)pin->dma_addr, pin->noc_addr,
					pin->iatu, ro);
		else
			tt_mappings_row(s, priv, "PIN_PAGES", "va 0x%llx size 0x%llx iova 0x%llx%s",
					pin->va, pin->size, (u64)pin->dma_addr, ro);
	}
	for (i = 0; i < TENSTORRENT_MAX_DMA_BUFS; i++) {
		struct tt_dma_buf *buf = fm->dma_bufs[i];

		if (!buf)
			continue;
		if (buf->iatu >= 0)
			tt_mappings_row(s, priv, "DMA_BUF+IATU",
					"ID: %u size 0x%zx iova 0x%llx noc 0x%llx region %d",
					buf->index, buf->size, (u64)buf->dma_addr, buf->noc_addr,
					buf->iatu);
		else
			tt_mappings_row(s, priv, "DMA_BUF", "ID: %u size 0x%zx iova 0x%llx",
					buf->index, buf->size, (u64)buf->dma_addr);
	}
	mutex_unlock(&fm->lock);
}
