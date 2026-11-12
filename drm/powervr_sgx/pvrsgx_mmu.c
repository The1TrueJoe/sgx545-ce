// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * SGX MMU page tables. One page directory of 1024 entries, each naming a page
 * table of 1024 four-KiB pages, so 4 GiB of device address space. Entries are
 * the physical address with flags in the low 12 bits, so every table and every
 * page the GPU sees must sit below 4 GiB.
 *
 * Tables are write-combined: whether the BIF snoops the CPU caches is not
 * known. Callers order the writes before the GPU uses them by kicking the
 * firmware, which issues a write barrier first.
 */

#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "pvrsgx_device.h"
#include "pvrsgx_mmu.h"

struct pvrsgx_mmu_pt {
	struct page *page;
	u32 *cpu;
	unsigned int used;
};

struct pvrsgx_mmu {
	struct pvrsgx_device *sdev;
	/** @lock: Guards the directory, the page tables and their use counts. */
	struct mutex lock;
	struct page *pd_page;
	u32 *pd;
	struct pvrsgx_mmu_pt *pt[SGX_MMU_PD_ENTRIES];
};

static struct page *table_page_alloc(struct pvrsgx_device *sdev, u32 **cpu)
{
	struct page *p = alloc_page(GFP_KERNEL | __GFP_ZERO | __GFP_DMA32);

	if (!p)
		return NULL;
	if (pvrsgx_pages_wc(sdev->base.dev, &p, 1))
		goto err_free;
	*cpu = pvrsgx_table_map(p);
	if (!*cpu)
		goto err_wb;
	return p;

err_wb:
	pvrsgx_pages_wb(&p, 1);
err_free:
	__free_page(p);
	return NULL;
}

static void table_page_free(struct page *p, u32 *cpu)
{
	pvrsgx_table_unmap(cpu);
	pvrsgx_pages_wb(&p, 1);
	__free_page(p);
}

struct pvrsgx_mmu *pvrsgx_mmu_create(struct pvrsgx_device *sdev)
{
	struct pvrsgx_mmu *mmu = kzalloc_obj(*mmu);

	if (!mmu)
		return ERR_PTR(-ENOMEM);

	mmu->pd_page = table_page_alloc(sdev, &mmu->pd);
	if (!mmu->pd_page) {
		kfree(mmu);
		return ERR_PTR(-ENOMEM);
	}
	mmu->sdev = sdev;
	mutex_init(&mmu->lock);
	return mmu;
}

void pvrsgx_mmu_destroy(struct pvrsgx_mmu *mmu)
{
	if (IS_ERR_OR_NULL(mmu))
		return;

	for (unsigned int i = 0; i < SGX_MMU_PD_ENTRIES; i++) {
		if (!mmu->pt[i])
			continue;
		WARN_ON(mmu->pt[i]->used);
		table_page_free(mmu->pt[i]->page, mmu->pt[i]->cpu);
		kfree(mmu->pt[i]);
	}
	table_page_free(mmu->pd_page, mmu->pd);
	mutex_destroy(&mmu->lock);
	kfree(mmu);
}

/*
 * Every address of @mmu on @page: one table whose every entry is @page, named
 * by every directory entry. Nothing is mapped into @mmu after this, so using
 * it later allocates nothing.
 */
int pvrsgx_mmu_fill(struct pvrsgx_mmu *mmu, struct page *page, u32 flags)
{
	phys_addr_t pa = page_to_phys(page);
	struct pvrsgx_mmu_pt *pt;
	u32 pde;

	if (upper_32_bits(pa))
		return -EFAULT;
	pt = kzalloc_obj(*pt);
	if (!pt)
		return -ENOMEM;
	pt->page = table_page_alloc(mmu->sdev, &pt->cpu);
	if (!pt->page) {
		kfree(pt);
		return -ENOMEM;
	}
	for (unsigned int i = 0; i < SGX_MMU_PT_ENTRIES; i++)
		pt->cpu[i] = (lower_32_bits(pa) & SGX_MMU_ADDR_MASK) | flags | SGX_MMU_PTE_VALID;
	pde = (lower_32_bits(page_to_phys(pt->page)) & SGX_MMU_ADDR_MASK) |
	      SGX_MMU_PDE_PAGE_SIZE_4K | SGX_MMU_PDE_VALID;

	mutex_lock(&mmu->lock);
	/* Freed once, through entry 0; the others are shared entries. */
	mmu->pt[0] = pt;
	for (unsigned int i = 0; i < SGX_MMU_PD_ENTRIES; i++)
		mmu->pd[i] = pde;
	mutex_unlock(&mmu->lock);
	return 0;
}

u32 pvrsgx_mmu_pd_addr(struct pvrsgx_mmu *mmu)
{
	return lower_32_bits(page_to_phys(mmu->pd_page));
}

static u32 *pte_get(struct pvrsgx_mmu *mmu, u32 dev_addr, bool alloc)
{
	unsigned int pdi = dev_addr >> (SGX_MMU_PAGE_SHIFT + SGX_MMU_PT_SHIFT);
	unsigned int pti = (dev_addr >> SGX_MMU_PAGE_SHIFT) & (SGX_MMU_PT_ENTRIES - 1);
	struct pvrsgx_mmu_pt *pt = mmu->pt[pdi];

	if (!pt) {
		/* A directory entry with no table of ours is another directory's (shared). */
		if (!alloc || WARN_ON(mmu->pd[pdi]))
			return NULL;
		pt = kzalloc_obj(*pt);
		if (!pt)
			return NULL;
		pt->page = table_page_alloc(mmu->sdev, &pt->cpu);
		if (!pt->page) {
			kfree(pt);
			return NULL;
		}
		mmu->pt[pdi] = pt;
		mmu->pd[pdi] = (lower_32_bits(page_to_phys(pt->page)) & SGX_MMU_ADDR_MASK) |
			       SGX_MMU_PDE_PAGE_SIZE_4K | SGX_MMU_PDE_VALID;
	}
	return &pt->cpu[pti];
}

static int pte_set(struct pvrsgx_mmu *mmu, u32 dev_addr, phys_addr_t pa, u32 flags)
{
	u32 *pte;

	if (upper_32_bits(pa))
		return -EFAULT;
	pte = pte_get(mmu, dev_addr, true);
	if (!pte)
		return -ENOMEM;
	if (WARN_ON(*pte & SGX_MMU_PTE_VALID))
		return -EEXIST;
	*pte = (lower_32_bits(pa) & SGX_MMU_ADDR_MASK) | flags | SGX_MMU_PTE_VALID;
	mmu->pt[dev_addr >> (SGX_MMU_PAGE_SHIFT + SGX_MMU_PT_SHIFT)]->used++;
	return 0;
}

static void unmap_locked(struct pvrsgx_mmu *mmu, u32 dev_addr, u64 size)
{
	for (u64 off = 0; off < size; off += SGX_MMU_PAGE_SIZE) {
		u32 a = dev_addr + off;
		u32 *pte = pte_get(mmu, a, false);

		if (!pte || !(*pte & SGX_MMU_PTE_VALID))
			continue;
		*pte = 0;
		mmu->pt[a >> (SGX_MMU_PAGE_SHIFT + SGX_MMU_PT_SHIFT)]->used--;
	}
}

int pvrsgx_mmu_map_pages(struct pvrsgx_mmu *mmu, u32 dev_addr,
			 struct page **pages, unsigned int npages, u32 flags)
{
	int err = 0;
	unsigned int i;

	mutex_lock(&mmu->lock);
	for (i = 0; i < npages; i++) {
		err = pte_set(mmu, dev_addr + i * SGX_MMU_PAGE_SIZE,
			      page_to_phys(pages[i]), flags);
		if (err)
			break;
	}
	if (err)
		unmap_locked(mmu, dev_addr, (u64)i * SGX_MMU_PAGE_SIZE);
	mutex_unlock(&mmu->lock);
	return err;
}

int pvrsgx_mmu_map_sgt(struct pvrsgx_mmu *mmu, u32 dev_addr,
		       struct sg_table *sgt, u64 offset, u64 size, u32 flags)
{
	struct sg_dma_page_iter it;
	u64 skip = offset >> PAGE_SHIFT, done = 0;
	int err = 0;

	mutex_lock(&mmu->lock);
	for_each_sgtable_dma_page(sgt, &it, skip) {
		if (done >= size)
			break;
		err = pte_set(mmu, dev_addr + done, sg_page_iter_dma_address(&it), flags);
		if (err)
			break;
		done += SGX_MMU_PAGE_SIZE;
	}
	if (!err && done < size)
		err = -EINVAL;
	if (err)
		unmap_locked(mmu, dev_addr, done);
	mutex_unlock(&mmu->lock);
	return err;
}

/*
 * Allocate the page tables of [dev_addr, dev_addr + size), so that a later map
 * of the range can't fail for memory. Tables stay until the directory goes.
 */
int pvrsgx_mmu_reserve(struct pvrsgx_mmu *mmu, u32 dev_addr, u64 size)
{
	u64 span = SGX_MMU_PDE_SPAN, end = (u64)dev_addr + size;
	int err = 0;

	mutex_lock(&mmu->lock);
	for (u64 a = round_down(dev_addr, span); a < end && !err; a += span)
		if (!pte_get(mmu, a, true))
			err = -ENOMEM;
	mutex_unlock(&mmu->lock);
	return err;
}

void pvrsgx_mmu_unmap(struct pvrsgx_mmu *mmu, u32 dev_addr, u64 size)
{
	mutex_lock(&mmu->lock);
	unmap_locked(mmu, dev_addr, size);
	mutex_unlock(&mmu->lock);
}

bool pvrsgx_mmu_range_empty(struct pvrsgx_mmu *mmu, u32 dev_addr, u64 size)
{
	bool empty = true;

	mutex_lock(&mmu->lock);
	for (u64 off = 0; off < size && empty; off += SGX_MMU_PAGE_SIZE) {
		u32 *pte = pte_get(mmu, dev_addr + off, false);

		empty = !pte || !(*pte & SGX_MMU_PTE_VALID);
	}
	mutex_unlock(&mmu->lock);
	return empty;
}

/*
 * For a hang dump: the directory and table entries for dev_addr, or false if
 * the directory is busy. Never allocates.
 */
bool pvrsgx_mmu_lookup(struct pvrsgx_mmu *mmu, u32 dev_addr, u32 *pde, u32 *pte)
{
	u32 *p;

	if (!mutex_trylock(&mmu->lock))
		return false;
	*pde = mmu->pd[dev_addr >> (SGX_MMU_PAGE_SHIFT + SGX_MMU_PT_SHIFT)];
	p = pte_get(mmu, dev_addr, false);
	*pte = p ? *p : 0;
	mutex_unlock(&mmu->lock);
	return true;
}

/* Give [start, end) page tables of their own now, so their PDEs never change. */
int pvrsgx_mmu_prealloc(struct pvrsgx_mmu *mmu, u32 start, u64 end)
{
	int err = 0;

	mutex_lock(&mmu->lock);
	for (u64 a = start; a < end && !err; a += SGX_MMU_PDE_SPAN)
		err = pte_get(mmu, a, true) ? 0 : -ENOMEM;
	mutex_unlock(&mmu->lock);
	return err;
}

/*
 * Point dst's directory entries for [start, end) at src's page tables: src's
 * mappings there show in dst too, and dst never maps anything there itself.
 * Both ends 4 MiB aligned, src preallocated.
 */
void pvrsgx_mmu_share(struct pvrsgx_mmu *dst, struct pvrsgx_mmu *src, u32 start, u64 end)
{
	for (u64 a = start; a < end; a += SGX_MMU_PDE_SPAN) {
		unsigned int i = a >> (SGX_MMU_PAGE_SHIFT + SGX_MMU_PT_SHIFT);

		dst->pd[i] = src->pd[i];
	}
}
