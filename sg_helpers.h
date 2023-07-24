#ifndef TTDRIVER_SG_HELPERS_H_INCLUDED
#define TTDRIVER_SG_HELPERS_H_INCLUDED

#include <linux/scatterlist.h>

bool alloc_chained_sgt_for_pages(struct sg_table *table, struct page **pages, unsigned int n_pages);
void free_chained_sgt(struct sg_table *table); // Safe to pass zero-intialized sg_table.

void debug_print_sgtable(struct sg_table *table);

#endif
