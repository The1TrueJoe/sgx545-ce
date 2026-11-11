/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_CACHE_H
#define PVRSGX_CACHE_H

/*
 * The SGX doesn't snoop the CPU caches. Memory the CPU writes for the GPU is
 * written through uncached (write-combined) mappings. On x86 the linear map
 * of those pages is made write-combined too. Elsewhere (ARM) the linear map
 * stays cacheable: the lines the page's zeroing left are written back once,
 * and the CPU only writes the page through its own write-combined mapping.
 */

#include <drm/drm_cache.h>

#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

#if IS_ENABLED(CONFIG_X86)
#include <asm/set_memory.h>

static inline int pvrsgx_pages_wc(struct device *dev, struct page **pages, unsigned int n)
{
	return set_pages_array_wc(pages, n);
}

static inline void pvrsgx_pages_wb(struct page **pages, unsigned int n)
{
	set_pages_array_wb(pages, n);
}

/* A page table's CPU mapping: its linear map is already write-combined. */
static inline u32 *pvrsgx_table_map(struct page *p)
{
	return page_address(p);
}

static inline void pvrsgx_table_unmap(u32 *cpu) { }

static inline void pvrsgx_import_sync(struct device *dev, struct sg_table *sgt, bool after)
{
	drm_clflush_sg(sgt);
}
#else
static inline int pvrsgx_pages_wc(struct device *dev, struct page **pages, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++) {
		/* Mapping for the device writes back the page's dirty lines. */
		dma_addr_t a = dma_map_page(dev, pages[i], 0, PAGE_SIZE, DMA_TO_DEVICE);

		if (dma_mapping_error(dev, a))
			return -ENOMEM;
		dma_unmap_page(dev, a, PAGE_SIZE, DMA_TO_DEVICE);
	}
	return 0;
}

static inline void pvrsgx_pages_wb(struct page **pages, unsigned int n) { }

static inline u32 *pvrsgx_table_map(struct page *p)
{
	return vmap(&p, 1, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
}

static inline void pvrsgx_table_unmap(u32 *cpu)
{
	vunmap(cpu);
}

/* Before a job, write back what the CPU left; after it, drop stale lines. */
static inline void pvrsgx_import_sync(struct device *dev, struct sg_table *sgt, bool after)
{
	if (after)
		dma_sync_sgtable_for_cpu(dev, sgt, DMA_BIDIRECTIONAL);
	else
		dma_sync_sgtable_for_device(dev, sgt, DMA_BIDIRECTIONAL);
}
#endif

#endif /* PVRSGX_CACHE_H */
