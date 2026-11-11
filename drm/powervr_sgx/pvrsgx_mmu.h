/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_MMU_H
#define PVRSGX_MMU_H

#include <linux/types.h>

struct page;
struct pvrsgx_device;
struct sg_table;

struct pvrsgx_mmu *pvrsgx_mmu_create(struct pvrsgx_device *sdev);
void pvrsgx_mmu_destroy(struct pvrsgx_mmu *mmu);
u32 pvrsgx_mmu_pd_addr(struct pvrsgx_mmu *mmu);

int pvrsgx_mmu_map_pages(struct pvrsgx_mmu *mmu, u32 dev_addr,
			 struct page **pages, unsigned int npages, u32 flags);
int pvrsgx_mmu_map_sgt(struct pvrsgx_mmu *mmu, u32 dev_addr,
		       struct sg_table *sgt, u64 offset, u64 size, u32 flags);
void pvrsgx_mmu_unmap(struct pvrsgx_mmu *mmu, u32 dev_addr, u64 size);
bool pvrsgx_mmu_range_empty(struct pvrsgx_mmu *mmu, u32 dev_addr, u64 size);
int pvrsgx_mmu_prealloc(struct pvrsgx_mmu *mmu, u32 start, u64 end);
void pvrsgx_mmu_share(struct pvrsgx_mmu *dst, struct pvrsgx_mmu *src, u32 start, u64 end);
bool pvrsgx_mmu_lookup(struct pvrsgx_mmu *mmu, u32 dev_addr, u32 *pde, u32 *pte);

#endif /* PVRSGX_MMU_H */
