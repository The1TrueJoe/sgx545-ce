// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Kernel-owned objects at device-wide addresses: render contexts' and RT
 * datasets' firmware structures, parameter buffers, free lists, region headers.
 *
 * Releasing one does not free it at once. The microkernel keeps pointers to
 * the last render's structures and reads them when the next TA starts
 * (docs/GEN.md, "Layout"; docs/UAPI.md 7.5), so a released object waits until
 * a render submitted after its release has finished.
 */

#include <linux/mm.h>
#include <linux/slab.h>

#include "pvrsgx_kobj.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_vm.h"

static const struct {
	u32 start, end, pte;
} ranges[PVRSGX_KRANGE_COUNT] = {
	/* past the firmware's own arenas (pvrsgx_fw.c) */
	[PVRSGX_KRANGE_KDATA] = { PVRSGX_HEAP_KERNEL_DATA + 0x10000, PVRSGX_HEAP_USSE_PIXEL,
				  SGX_MMU_PTE_CACHECONSISTENT | SGX_MMU_PTE_EDMPROTECT },
	[PVRSGX_KRANGE_KCODE] = { PVRSGX_HEAP_KERNEL_CODE + 0x10000, PVRSGX_HEAP_PDS_VERTEX,
				  SGX_MMU_PTE_CACHECONSISTENT },
	[PVRSGX_KRANGE_SYNC] = { PVRSGX_HEAP_SYNC, PVRSGX_HEAP_PDS_PIXEL,
				 SGX_MMU_PTE_CACHECONSISTENT },
	[PVRSGX_KRANGE_3DPARAMS] = { PVRSGX_HEAP_3D_PARAMS, PVRSGX_HEAP_TA_DATA, 0 },
	[PVRSGX_KRANGE_TADATA] = { PVRSGX_HEAP_TA_DATA, PVRSGX_HEAP_SYNC, 0 },
};

int pvrsgx_kobj_init(struct pvrsgx_device *sdev)
{
	mutex_init(&sdev->kobj_lock);
	INIT_LIST_HEAD(&sdev->graveyard);
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++)
		drm_mm_init(&sdev->krange[r], ranges[r].start, ranges[r].end - ranges[r].start);
	return 0;
}

struct pvrsgx_kobj *pvrsgx_kobj_create(struct pvrsgx_device *sdev, enum pvrsgx_krange range,
				       u32 size, u32 min_addr, struct pvrsgx_vm *vm)
{
	struct pvrsgx_kobj *o = kzalloc_obj(*o);
	u32 start = max(min_addr, ranges[range].start);
	int err;

	if (!o)
		return ERR_PTR(-ENOMEM);
	o->range = range;
	mutex_lock(&sdev->kobj_lock);
	err = drm_mm_insert_node_in_range(&sdev->krange[range], &o->node, PAGE_ALIGN(size), PAGE_SIZE,
					  0, start, ranges[range].end, DRM_MM_INSERT_LOW);
	mutex_unlock(&sdev->kobj_lock);
	if (err)
		goto err_free;

	err = pvrsgx_kmem_alloc(sdev, &o->km, o->node.start, size, ranges[range].pte);
	if (err)
		goto err_node;
	memset(o->km.cpu, 0, o->km.size);

	if (vm) {
		err = pvrsgx_mmu_map_pages(vm->mmu, o->node.start, o->km.pages, o->km.npages, 0);
		if (err)
			goto err_kmem;
		o->vm = pvrsgx_vm_get(vm);
	}
	return o;

err_kmem:
	pvrsgx_kmem_free(sdev, &o->km);
err_node:
	mutex_lock(&sdev->kobj_lock);
	drm_mm_remove_node(&o->node);
	mutex_unlock(&sdev->kobj_lock);
err_free:
	kfree(o);
	return ERR_PTR(err);
}

static void kobj_free(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	if (o->vm) {
		pvrsgx_mmu_unmap(o->vm->mmu, kobj_va(o), o->km.size);
		pvrsgx_vm_put(o->vm);
	}
	pvrsgx_kmem_free(sdev, &o->km);
	mutex_lock(&sdev->kobj_lock);
	drm_mm_remove_node(&o->node);
	mutex_unlock(&sdev->kobj_lock);
	kfree(o);
}

void pvrsgx_kobj_release(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	if (IS_ERR_OR_NULL(o))
		return;
	mutex_lock(&sdev->kobj_lock);
	o->grave_seq = sdev->renders_submitted;
	list_add_tail(&o->grave, &sdev->graveyard);
	mutex_unlock(&sdev->kobj_lock);
}

/* Free what was released before a render that has now finished. */
void pvrsgx_kobj_reap(struct pvrsgx_device *sdev, u64 renders_done)
{
	struct pvrsgx_kobj *o, *tmp;
	LIST_HEAD(dead);

	mutex_lock(&sdev->kobj_lock);
	list_for_each_entry_safe(o, tmp, &sdev->graveyard, grave) {
		if (o->grave_seq < renders_done)
			list_move_tail(&o->grave, &dead);
	}
	mutex_unlock(&sdev->kobj_lock);
	list_for_each_entry_safe(o, tmp, &dead, grave)
		kobj_free(sdev, o);
}

void pvrsgx_kobj_fini(struct pvrsgx_device *sdev)
{
	pvrsgx_kobj_reap(sdev, U64_MAX);
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++)
		drm_mm_takedown(&sdev->krange[r]);
	mutex_destroy(&sdev->kobj_lock);
}
