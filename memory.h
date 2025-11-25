// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef TENSTORRENT_MEMORY_H_INCLUDED
#define TENSTORRENT_MEMORY_H_INCLUDED

#include <linux/compiler.h>
#include <linux/scatterlist.h>

#define MAX_DMA_BUF_SIZE_LOG2 28

struct chardev_private;
struct tenstorrent_device;
struct tenstorrent_query_mappings;
struct tenstorrent_allocate_dma_buf;
struct tenstorrent_free_dma_buf;
struct tenstorrent_pin_pages;
struct tenstorrent_map_peer_bar;
struct vm_area_struct;

struct pinned_page_range {
	struct list_head list;

	unsigned long page_count;
	struct page **pages;	// vmalloc/vfree

	struct sg_table dma_mapping;	// alloc_chained_sgt_for_pages / free_chained_sgt
	u64 virtual_address;

	int outbound_iatu_region;
};


long ioctl_query_mappings(struct chardev_private *priv,
			  struct tenstorrent_query_mappings __user *arg);
long ioctl_allocate_dma_buf(struct chardev_private *priv,
			    struct tenstorrent_allocate_dma_buf __user *arg);
long ioctl_free_dma_buf(struct chardev_private *priv,
			struct tenstorrent_free_dma_buf __user *arg);
long ioctl_pin_pages(struct chardev_private *priv,
		     struct tenstorrent_pin_pages __user *arg);
long ioctl_unpin_pages(struct chardev_private *priv,
		     struct tenstorrent_unpin_pages __user *arg);
long ioctl_map_peer_bar(struct chardev_private *priv,
			struct tenstorrent_map_peer_bar __user *arg);
long ioctl_allocate_tlb(struct chardev_private *priv,
			struct tenstorrent_allocate_tlb __user *arg);
long ioctl_free_tlb(struct chardev_private *priv,
			struct tenstorrent_free_tlb __user *arg);
long ioctl_configure_tlb(struct chardev_private *priv,
			struct tenstorrent_configure_tlb __user *arg);

int tenstorrent_mmap(struct chardev_private *priv, struct vm_area_struct *vma);
void tenstorrent_memory_cleanup(struct chardev_private *priv);
void tenstorrent_vma_zap(struct tenstorrent_device *tt_dev);
bool is_iommu_translated(struct device *dev);

#define TENSTORRENT_MAX_OUTBOUND_IATU_REGIONS 16
struct tenstorrent_outbound_iatu_region {
	struct chardev_private *priv;	// Owner of this region
	u64 base;
	u64 limit;
	u64 target;
};

#endif
