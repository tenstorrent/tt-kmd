// SPDX-License-Identifier: GPL-2.0-only
/*
 * User TLB windows: ALLOCATE/FREE/CONFIGURE_TLB, mmap of a window and
 * EXPORT_TLB_DMABUF. See tlb.h for the window lifetime and locking rules.
 */
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/log2.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci-p2pdma.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "device.h"
#include "chardev.h"
#include "blackhole.h"
#include "tlb.h"

MODULE_IMPORT_NS(DMA_BUF);

struct tt_tlb_window {
	unsigned int index;
	struct chardev_private *owner;	/* NULL once freed by / closed by the owner */
	struct list_head fd_node;	/* in owner->tlb.windows */
	unsigned int map_count;		/* tt_tlb_mapping objects */
	unsigned int export_count;	/* live dma-buf exports */
	bool configured;
	struct tenstorrent_noc_tlb_config cfg;	/* last config written (debugfs) */
};

/* A user mmap of (part of) a window. */
struct tt_tlb_mapping {
	struct tt_mmio_mapping mmio;
	struct tt_tlb_window *w;
};

/*
 * A dma-buf export of [offset, offset + size) of a window, i.e. of the CPU
 * physical BAR range [phys, phys + size). Importers get it through
 * dma_map_resource() on their own device: with an IOMMU that maps the
 * aperture into the importer's DMA space; without one (dma-direct) the
 * address is the CPU physical address, which equals the PCI bus address on
 * the platforms this driver supports (no host bridge offset).
 */
struct tt_tlb_export {
	struct tenstorrent_device *dev;	/* reference held */
	struct tt_tlb_window *w;	/* export_count held */
	struct dma_buf *dmabuf;
	struct list_head node;		/* in dev->tlb.exports */
	struct list_head revoke_node;	/* tt_tlb_device_fini() private list */
	phys_addr_t phys;
	u64 offset;
	u64 size;
	bool revoked;			/* device removed; under dmabuf->resv */
};

/* Per-attachment exporter state (attach->priv). */
struct tt_tlb_attach {
	unsigned int pin_count;		/* under dmabuf->resv */
};

/* Bound on how long PCI remove waits for importers' DMA on a revoked export. */
#define TT_TLB_REVOKE_TIMEOUT_MS	2000

/* ---- helpers ---- */

static phys_addr_t tt_tlb_window_phys(struct tenstorrent_device *dev, unsigned int index)
{
	resource_size_t base = bh_tlb_window_bar(index) == 0 ? dev->bar0_start : dev->bar4_start;

	return base + bh_tlb_window_bar_offset(index);
}

/* Is @id a window user space may name (allocated or not)? */
static bool tt_tlb_id_valid(struct tenstorrent_device *dev, u32 id)
{
	return id < bh_tlb_window_count(dev) && id != BH_KERNEL_TLB_INDEX &&
	       id < TENSTORRENT_MAX_INBOUND_TLBS;
}

/*
 * Look up window @id for an operation only its owner may do: -EINVAL if the
 * window is not allocated or no longer owned (freed, held only by an
 * export), -EPERM if another fd owns it. Caller holds dev->tlb.lock.
 */
static struct tt_tlb_window *tt_tlb_get_owned(struct chardev_private *priv, u32 id)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_tlb_window *w;

	lockdep_assert_held(&dev->tlb.lock);

	w = dev->tlb.windows[id];
	if (!w || !w->owner)
		return ERR_PTR(-EINVAL);
	if (w->owner != priv)
		return ERR_PTR(-EPERM);
	return w;
}

/* Return @w to the pool if nothing holds it any more. Caller holds the lock. */
static void tt_tlb_window_put_locked(struct tenstorrent_device *dev, struct tt_tlb_window *w)
{
	lockdep_assert_held(&dev->tlb.lock);

	if (w->owner || w->map_count || w->export_count)
		return;
	dev->tlb.windows[w->index] = NULL;
	kfree(w);
}

static int tt_tlb_free(struct chardev_private *priv, u32 id)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_tlb_window *w;
	int ret = 0;

	mutex_lock(&dev->tlb.lock);
	w = tt_tlb_get_owned(priv, id);
	if (IS_ERR(w)) {
		ret = PTR_ERR(w);
	} else if (w->map_count) {
		ret = -EBUSY;
	} else {
		w->owner = NULL;
		list_del_init(&w->fd_node);
		tt_tlb_window_put_locked(dev, w);
	}
	mutex_unlock(&dev->tlb.lock);
	return ret;
}

/* ---- device / fd hooks ---- */

int tt_tlb_device_init(struct tenstorrent_device *dev)
{
	mutex_init(&dev->tlb.lock);
	INIT_LIST_HEAD(&dev->tlb.exports);
	dev->tlb.num_exports = 0;
	return 0;
}

/*
 * PCI remove (detached, after fd_detach): the BAR apertures behind live
 * exports are going away. Revoke each export: its next map fails, dynamic
 * importers that have not pinned get move_notify, and we wait (bounded) for
 * the fences importers attached to the reservation, i.e. their in-flight
 * DMA. Pin-only importers and pinned dynamic attachments cannot be revoked:
 * their mappings stay until they detach (which is why RESET_DEVICE is
 * refused while exports exist); move_notify is not sent to them because a
 * pinned importer cannot honour it. The exports themselves stay until their
 * dma-bufs are released; they hold device references.
 */
void tt_tlb_device_fini(struct tenstorrent_device *dev)
{
	struct tt_tlb_export *e, *tmp;
	LIST_HEAD(revoke);

	/* Take a file reference on every export not already being released
	 * (a dying dma-buf has no attachments left), then work unlocked. Its
	 * ->release blocks on tlb.lock, so the file is still valid here. */
	mutex_lock(&dev->tlb.lock);
	list_for_each_entry(e, &dev->tlb.exports, node) {
		struct file *f;

		rcu_read_lock();
		f = get_file_active(&e->dmabuf->file);
		rcu_read_unlock();
		if (f)
			list_add_tail(&e->revoke_node, &revoke);
	}
	mutex_unlock(&dev->tlb.lock);

	list_for_each_entry_safe(e, tmp, &revoke, revoke_node) {
		struct dma_buf *dmabuf = e->dmabuf;
		struct dma_buf_attachment *attach;
		long t;

		dma_resv_lock(dmabuf->resv, NULL);
		e->revoked = true;
		list_for_each_entry(attach, &dmabuf->attachments, node) {
			struct tt_tlb_attach *ta = attach->priv;

			if (attach->importer_ops && !ta->pin_count)
				attach->importer_ops->move_notify(attach);
		}
		dma_resv_unlock(dmabuf->resv);

		t = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_BOOKKEEP, false,
					  msecs_to_jiffies(TT_TLB_REVOKE_TIMEOUT_MS));
		if (t <= 0)
			dev_info(&dev->pdev->dev,
				 "TLB window %u export: importer DMA still pending at removal\n",
				 e->w->index);

		list_del(&e->revoke_node);
		dma_buf_put(dmabuf);	/* may free e */
	}
}

void tt_tlb_fd_init(struct chardev_private *priv)
{
	INIT_LIST_HEAD(&priv->tlb.windows);
}

int tt_tlb_fd_open(struct chardev_private *priv)
{
	return 0;
}

/*
 * Drop the fd's ownership of all its windows. No user mapping exists any
 * more (every VMA holds a file reference). Exported windows stay allocated
 * until their exports are released. Software only, so the same after removal.
 */
void tt_tlb_fd_release(struct chardev_private *priv)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_tlb_window *w, *tmp;

	mutex_lock(&dev->tlb.lock);
	list_for_each_entry_safe(w, tmp, &priv->tlb.windows, fd_node) {
		list_del_init(&w->fd_node);
		w->owner = NULL;
		tt_tlb_window_put_locked(dev, w);
	}
	mutex_unlock(&dev->tlb.lock);
}

/* Windows hold no DMA mappings; exports are revoked in tt_tlb_device_fini. */
void tt_tlb_fd_detach(struct chardev_private *priv)
{
}

int tt_tlb_reset_check(struct tenstorrent_device *dev)
{
	int ret;

	mutex_lock(&dev->tlb.lock);
	ret = dev->tlb.num_exports ? -EBUSY : 0;
	mutex_unlock(&dev->tlb.lock);
	return ret;
}

/* ---- ioctls ---- */

static const struct tenstorrent_noc_tlb_config tt_tlb_reset_config;

long tt_tlb_ioctl_allocate(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_allocate_tlb_in in;
	struct tenstorrent_allocate_tlb_out out = {};
	struct tt_tlb_window *w;
	unsigned int first, end, i;

	if (tt_copy_in(&in, arg, sizeof(in)))
		return -EFAULT;
	if (in.reserved)
		return -EINVAL;

	if (in.size == BH_TLB_2M_SIZE) {
		first = 0;
		end = BH_TLB_2M_COUNT;
	} else if (in.size == BH_TLB_4G_SIZE && dev->num_4g_windows) {
		first = BH_TLB_4G_FIRST;
		end = bh_tlb_window_count(dev);
	} else {
		return -EINVAL;
	}

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	mutex_lock(&dev->tlb.lock);
	for (i = first; i < end; i++)
		if (i != BH_KERNEL_TLB_INDEX && !dev->tlb.windows[i])
			break;
	if (i >= end) {
		mutex_unlock(&dev->tlb.lock);
		kfree(w);
		return -ENOMEM;
	}
	w->index = i;
	w->owner = priv;
	list_add_tail(&w->fd_node, &priv->tlb.windows);
	dev->tlb.windows[i] = w;
	/* Don't hand out the previous owner's target: reset to the all-zero
	 * config (unicast, NOC 0, (0,0), address 0) until CONFIGURE_TLB. */
	bh_tlb_configure(dev, i, &tt_tlb_reset_config);
	mutex_unlock(&dev->tlb.lock);

	out.id = i;
	out.mmap_offset_uc = TT_MMAP_TLB_BASE + (u64)i * TT_MMAP_TLB_STRIDE;
	out.mmap_offset_wc = out.mmap_offset_uc + TT_MMAP_TLB_WC_OFFSET;
	if (copy_to_user(TT_UOUT(arg, struct tenstorrent_allocate_tlb), &out, sizeof(out))) {
		/* Fails only if another thread already mapped the id it could not
		 * have learned; then the window stays with the fd until close. */
		tt_tlb_free(priv, i);
		return -EFAULT;
	}
	return 0;
}

long tt_tlb_ioctl_free(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_free_tlb_in in;

	if (tt_copy_in(&in, arg, sizeof(in)))
		return -EFAULT;
	if (!tt_tlb_id_valid(priv->dev, in.id))
		return -EINVAL;

	return tt_tlb_free(priv, in.id);
}

long tt_tlb_ioctl_configure(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_configure_tlb_in in;
	const struct tenstorrent_noc_tlb_config *cfg = &in.config;
	struct tt_tlb_window *w;
	u32 regs[3];
	int ret;

	if (tt_copy_in(&in, arg, sizeof(in)))
		return -EFAULT;
	if (!tt_tlb_id_valid(dev, in.id))
		return -EINVAL;
	if (in.reserved || memchr_inv(cfg->reserved0, 0, sizeof(cfg->reserved0)) ||
	    memchr_inv(cfg->reserved1, 0, sizeof(cfg->reserved1)))
		return -EINVAL;
	/* Alignment, coordinates and field widths (all 64 address bits are
	 * valid on Blackhole). */
	ret = bh_tlb_encode(in.id, cfg, regs);
	if (ret)
		return ret;
	/* Linked transactions must be closed by an unlinked one; a window left
	 * linked can hang the NOC path, so user windows never are. */
	if (cfg->linked)
		return -EINVAL;
	/* Windows are aligned to their size and DBI to 4 GiB, so a window
	 * overlaps DBI iff its base lies inside it. */
	if (tt_noc_addr_in_dbi(cfg->addr))
		return -EPERM;

	mutex_lock(&dev->tlb.lock);
	w = tt_tlb_get_owned(priv, in.id);
	if (IS_ERR(w)) {
		ret = PTR_ERR(w);
		goto out;
	}
	/* Under the lock: the window cannot change hands while we program it. */
	ret = bh_tlb_configure(dev, in.id, cfg);
	if (ret)
		goto out;
	w->cfg = *cfg;
	w->configured = true;
out:
	mutex_unlock(&dev->tlb.lock);
	return ret;
}

/* ---- mmap ---- */

static void tt_tlb_mapping_release(struct tt_mmio_mapping *m)
{
	struct tt_tlb_mapping *tm = container_of(m, struct tt_tlb_mapping, mmio);
	struct tenstorrent_device *dev = m->priv->dev;

	mutex_lock(&dev->tlb.lock);
	tm->w->map_count--;
	tt_tlb_window_put_locked(dev, tm->w);
	mutex_unlock(&dev->tlb.lock);
	kfree(tm);
}

static const struct tt_mmio_ops tt_tlb_mmio_ops = {
	.release = tt_tlb_mapping_release,
};

int tt_tlb_mmap(struct chardev_private *priv, struct vm_area_struct *vma)
{
	struct tenstorrent_device *dev = priv->dev;
	u64 off = ((u64)vma->vm_pgoff << PAGE_SHIFT) - TT_MMAP_TLB_BASE;
	u64 size = vma->vm_end - vma->vm_start;
	u64 rel = off & (TT_MMAP_TLB_STRIDE - 1);
	u64 id64 = off >> ilog2(TT_MMAP_TLB_STRIDE);
	struct tt_tlb_mapping *tm;
	struct tt_tlb_window *w;
	bool wc = false;
	u64 wsize;
	int ret;

	if (rel >= TT_MMAP_TLB_WC_OFFSET) {
		wc = true;
		rel -= TT_MMAP_TLB_WC_OFFSET;
	}
	if (id64 >= TENSTORRENT_MAX_INBOUND_TLBS || !tt_tlb_id_valid(dev, id64))
		return -EINVAL;
	wsize = bh_tlb_window_size(id64);
	if (rel >= wsize || size > wsize - rel)
		return -EINVAL;

	tm = kzalloc(sizeof(*tm), GFP_KERNEL);
	if (!tm)
		return -ENOMEM;

	mutex_lock(&dev->tlb.lock);
	w = tt_tlb_get_owned(priv, id64);
	if (IS_ERR(w)) {
		mutex_unlock(&dev->tlb.lock);
		kfree(tm);
		return PTR_ERR(w);
	}
	w->map_count++;
	mutex_unlock(&dev->tlb.lock);

	tm->w = w;
	tm->mmio.priv = priv;
	tm->mmio.ops = &tt_tlb_mmio_ops;
	tm->mmio.phys = tt_tlb_window_phys(dev, id64) + rel;
	tm->mmio.wc = wc;
	tm->mmio.no_split = true;

	ret = tt_mmio_mmap(vma, &tm->mmio);
	if (ret) {
		mutex_lock(&dev->tlb.lock);
		w->map_count--;
		mutex_unlock(&dev->tlb.lock);
		kfree(tm);
	}
	return ret;
}

/* ---- dma-buf export ---- */

static int tt_tlb_dmabuf_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	struct tt_tlb_export *e = dmabuf->priv;

	/* A BAR aperture without struct pages: only P2P-capable importers. */
	if (!attach->peer2peer || !dev_is_pci(attach->dev))
		return -EOPNOTSUPP;
	if (READ_ONCE(e->dev->detached))
		return -ENODEV;
	if (pci_p2pdma_distance(e->dev->pdev, attach->dev, false) < 0)
		return -EOPNOTSUPP;

	attach->priv = kzalloc(sizeof(struct tt_tlb_attach), GFP_KERNEL);
	if (!attach->priv)
		return -ENOMEM;
	return 0;
}

static void tt_tlb_dmabuf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
	kfree(attach->priv);
	attach->priv = NULL;
}

/* The aperture never moves: pinning is a no-op, but makes us a dynamic
 * exporter so that both move_notify and pin-only importers are accepted. */
static int tt_tlb_dmabuf_pin(struct dma_buf_attachment *attach)
{
	struct tt_tlb_attach *ta = attach->priv;

	dma_resv_assert_held(attach->dmabuf->resv);
	ta->pin_count++;
	return 0;
}

static void tt_tlb_dmabuf_unpin(struct dma_buf_attachment *attach)
{
	struct tt_tlb_attach *ta = attach->priv;

	dma_resv_assert_held(attach->dmabuf->resv);
	ta->pin_count--;
}

static void tt_tlb_dmabuf_unmap_sgt(struct dma_buf_attachment *attach, struct sg_table *sgt,
				    unsigned int mapped, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	unsigned int i;

	for_each_sg(sgt->sgl, sg, mapped, i)
		dma_unmap_resource(attach->dev, sg_dma_address(sg), sg_dma_len(sg), dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static struct sg_table *tt_tlb_dmabuf_map(struct dma_buf_attachment *attach,
					  enum dma_data_direction dir)
{
	struct tt_tlb_export *e = attach->dmabuf->priv;
	u64 chunk = min_t(u64, SZ_1G, dma_get_max_seg_size(attach->dev) & PAGE_MASK);
	struct scatterlist *sg;
	struct sg_table *sgt;
	unsigned int nents, i;
	u64 off = 0;
	int ret;

	dma_resv_assert_held(attach->dmabuf->resv);
	if (e->revoked)
		return ERR_PTR(-ENODEV);

	if (!chunk)
		chunk = PAGE_SIZE;
	nents = DIV_ROUND_UP_ULL(e->size, chunk);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	for_each_sg(sgt->sgl, sg, nents, i) {
		u64 len = min(chunk, e->size - off);
		dma_addr_t addr;

		addr = dma_map_resource(attach->dev, e->phys + off, len, dir,
					DMA_ATTR_SKIP_CPU_SYNC);
		if (dma_mapping_error(attach->dev, addr)) {
			tt_tlb_dmabuf_unmap_sgt(attach, sgt, i, dir);
			return ERR_PTR(-EIO);
		}
		sg_set_page(sg, NULL, len, 0);
		sg_dma_address(sg) = addr;
		sg_dma_len(sg) = len;
		off += len;
	}
	return sgt;
}

static void tt_tlb_dmabuf_unmap(struct dma_buf_attachment *attach, struct sg_table *sgt,
				enum dma_data_direction dir)
{
	tt_tlb_dmabuf_unmap_sgt(attach, sgt, sgt->orig_nents, dir);
}

/* Drop the export's hold on its window; the window returns to the pool if
 * its owner already freed it. May run long after PCI remove. */
static void tt_tlb_export_unlink(struct tt_tlb_export *e)
{
	struct tenstorrent_device *dev = e->dev;

	mutex_lock(&dev->tlb.lock);
	if (!list_empty(&e->node))
		list_del_init(&e->node);
	dev->tlb.num_exports--;
	e->w->export_count--;
	tt_tlb_window_put_locked(dev, e->w);
	mutex_unlock(&dev->tlb.lock);
}

static void tt_tlb_dmabuf_release(struct dma_buf *dmabuf)
{
	struct tt_tlb_export *e = dmabuf->priv;
	struct tenstorrent_device *dev = e->dev;

	tt_tlb_export_unlink(e);
	kfree(e);
	tt_device_put(dev);
}

static const struct dma_buf_ops tt_tlb_dmabuf_ops = {
	.attach = tt_tlb_dmabuf_attach,
	.detach = tt_tlb_dmabuf_detach,
	.pin = tt_tlb_dmabuf_pin,
	.unpin = tt_tlb_dmabuf_unpin,
	.map_dma_buf = tt_tlb_dmabuf_map,
	.unmap_dma_buf = tt_tlb_dmabuf_unmap,
	.release = tt_tlb_dmabuf_release,
	/* No mmap/vmap: CPU access goes through the TLB mmap of the owner fd. */
};

long tt_tlb_ioctl_export_dmabuf(struct chardev_private *priv, void __user *arg)
{
	struct tenstorrent_export_tlb_dmabuf __user *uexp = arg;
	struct tenstorrent_device *dev = priv->dev;
	struct tenstorrent_export_tlb_dmabuf exp;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct tt_tlb_export *e;
	struct tt_tlb_window *w;
	struct dma_buf *dmabuf;
	u64 wsize, size;
	u32 argsz;
	int fd, ret;

	if (get_user(argsz, &uexp->argsz))
		return -EFAULT;
	if (argsz != sizeof(exp))
		return -EINVAL;
	if (copy_from_user(&exp, uexp, sizeof(exp)))
		return -EFAULT;
	if (exp.flags || !tt_tlb_id_valid(dev, exp.tlb_id))
		return -EINVAL;

	wsize = bh_tlb_window_size(exp.tlb_id);
	if (!PAGE_ALIGNED(exp.offset) || !PAGE_ALIGNED(exp.size) || exp.offset >= wsize)
		return -EINVAL;
	size = exp.size ? exp.size : wsize - exp.offset;
	if (size > wsize - exp.offset)
		return -EINVAL;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return -ENOMEM;
	INIT_LIST_HEAD(&e->node);
	INIT_LIST_HEAD(&e->revoke_node);

	mutex_lock(&dev->tlb.lock);
	w = tt_tlb_get_owned(priv, exp.tlb_id);
	if (IS_ERR(w)) {
		mutex_unlock(&dev->tlb.lock);
		kfree(e);
		return PTR_ERR(w);
	}
	w->export_count++;
	dev->tlb.num_exports++;
	mutex_unlock(&dev->tlb.lock);

	tt_device_get(dev);
	e->dev = dev;
	e->w = w;
	e->phys = tt_tlb_window_phys(dev, exp.tlb_id) + exp.offset;
	e->offset = exp.offset;
	e->size = size;

	exp_info.ops = &tt_tlb_dmabuf_ops;
	exp_info.size = size;
	exp_info.flags = O_RDWR;
	exp_info.priv = e;
	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		tt_tlb_export_unlink(e);
		kfree(e);
		tt_device_put(dev);
		return PTR_ERR(dmabuf);
	}
	e->dmabuf = dmabuf;

	/* From here on dma_buf_put() undoes everything via ->release. */
	mutex_lock(&dev->tlb.lock);
	list_add_tail(&e->node, &dev->tlb.exports);
	mutex_unlock(&dev->tlb.lock);

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		ret = fd;
		goto err_put;
	}
	if (put_user(fd, &uexp->fd)) {
		put_unused_fd(fd);
		ret = -EFAULT;
		goto err_put;
	}
	fd_install(fd, dmabuf->file);
	return 0;

err_put:
	dma_buf_put(dmabuf);
	return ret;
}

/* ---- debugfs ---- */

void tt_tlb_debugfs_show(struct seq_file *s, struct chardev_private *priv)
{
	struct tenstorrent_device *dev = priv->dev;
	struct tt_tlb_window *w;

	mutex_lock(&dev->tlb.lock);
	list_for_each_entry(w, &priv->tlb.windows, fd_node) {
		const struct tenstorrent_noc_tlb_config *c = &w->cfg;

		if (!w->configured) {
			tt_mappings_row(s, priv, "TLB",
					"id %u size 0x%llx unconfigured maps %u exports %u",
					w->index, bh_tlb_window_size(w->index), w->map_count,
					w->export_count);
			continue;
		}
		tt_mappings_row(s, priv, "TLB",
				"id %u size 0x%llx noc %u x %u-%u y %u-%u addr 0x%llx%s ordering %u maps %u exports %u",
				w->index, bh_tlb_window_size(w->index), c->noc,
				c->x_start, c->x_end, c->y_start, c->y_end, c->addr,
				c->mcast ? " mcast" : "", c->ordering, w->map_count,
				w->export_count);
	}
	mutex_unlock(&dev->tlb.lock);
}
