#ifndef TTDRIVER_SG_HELPERS_H_INCLUDED
#define TTDRIVER_SG_HELPERS_H_INCLUDED

#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/version.h>

// Merged in 5.8 and backported to 5.4.233.
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 233)
#define NEED_SGTABLE
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define NEED_SGTABLE
#endif

#ifdef NEED_SGTABLE

static inline int dma_map_sgtable(struct device *dev, struct sg_table *dma_mapping, enum dma_data_direction dir, unsigned long attrs)
{
	int mapped_nents = dma_map_sg_attrs(dev, dma_mapping->sgl, dma_mapping->nents, dir, attrs);
	if (mapped_nents) {
		dma_mapping->orig_nents = dma_mapping->nents;
		dma_mapping->nents = mapped_nents;
	}
	return mapped_nents ? 0 : -ENOMEM;
}

static inline void dma_unmap_sgtable(struct device *dev, struct sg_table *dma_mapping, enum dma_data_direction dir, unsigned long attrs)
{
	dma_unmap_sg_attrs(dev, dma_mapping->sgl, dma_mapping->nents, dir, attrs);
}

#define for_each_sgtable_dma_sg(sgt, tmp_scl, tmp_idx) for_each_sg((sgt)->sgl, tmp_scl, (sgt)->nents, tmp_idx)

#endif

bool alloc_chained_sgt_for_pages(struct sg_table *table, struct page **pages, unsigned int n_pages);
void free_chained_sgt(struct sg_table *table); // Safe to pass zero-intialized sg_table.

void debug_print_sgtable(struct sg_table *table);

#endif
