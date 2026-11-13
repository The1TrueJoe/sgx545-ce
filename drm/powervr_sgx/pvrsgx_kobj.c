// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Kernel-owned objects at device-wide addresses: render contexts' and RT
 * datasets' firmware structures, parameter buffers, free lists, region headers.
 * The 3D parameters range (PB, free list, scratch, background block) is per VM
 * instead: the DPM numbers PB pages from the heap base, so a second context's PB
 * at a higher device-wide address hung its third render (CE5310 board, 2026-11-03).
 *
 * Releasing one does not free it at once: a render already handed to the
 * microkernel may still read it. Released objects are freed once every render
 * submitted before the release has completed. A PB descriptor waits for the
 * next restart instead: the microkernel remembers the last PB's address to
 * skip reloading its free list, so a new PB at a freed one's address must not
 * appear while it runs.
 */

#include <linux/mm.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>

#include "pvrsgx_ccb.h"
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
	struct pvrsgx_kobj *o;
	u32 start = max(min_addr, ranges[range].start);
	/* A 3D-params object without a VM is the device's: the PB (pvrsgx_render.c). */
	bool per_vm = range == PVRSGX_KRANGE_3DPARAMS && vm;
	int err;

	o = kzalloc_obj(*o);
	if (!o)
		return ERR_PTR(-ENOMEM);
	o->range = range;
	o->mm = per_vm ? &vm->params : &sdev->krange[range];
	mutex_lock(&sdev->kobj_lock);
	err = drm_mm_insert_node_in_range(o->mm, &o->node, PAGE_ALIGN(size), PAGE_SIZE,
					  0, start, ranges[range].end, DRM_MM_INSERT_LOW);
	mutex_unlock(&sdev->kobj_lock);
	if (err)
		goto err_free;

	if (per_vm)
		o->km.mmu = vm->mmu;
	err = pvrsgx_kmem_alloc(sdev, &o->km, o->node.start, size, ranges[range].pte);
	if (err)
		goto err_node;
	memset(o->km.cpu, 0, o->km.size);

	if (per_vm) {
		o->vm = pvrsgx_vm_get(vm);
	} else if (vm) {
		err = pvrsgx_mmu_map_pages(vm->mmu, o->node.start, o->km.pages, o->km.npages, 0);
		if (err)
			goto err_kmem;
		o->vm = pvrsgx_vm_get(vm);
	}
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD;
	mutex_unlock(&sdev->ccb_lock);
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

/* The invalidate has run: nothing can reach the object's pages any more. */
static void kobj_free_done(struct pvrsgx_inval_wait *w)
{
	struct pvrsgx_kobj *o = container_of(w, struct pvrsgx_kobj, inval);
	struct pvrsgx_device *sdev = o->sdev;
	struct pvrsgx_vm *vm = o->vm;

	pvrsgx_kmem_release(&o->km);
	mutex_lock(&sdev->kobj_lock);
	drm_mm_remove_node(&o->node);
	mutex_unlock(&sdev->kobj_lock);
	kfree(o);
	/* Last: the node may belong to the VM's own allocator. */
	pvrsgx_vm_put(vm);
}

/*
 * Unmapping is not the end: the BIF may hold a translation of the pages until
 * the microkernel invalidates it, which it does for the next command. The
 * pages, the address and the VM reference wait for that.
 */
static void kobj_free(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	struct pvrsgx_vm *vm = o->vm;

	if (vm && o->km.mmu != vm->mmu)
		pvrsgx_mmu_unmap(vm->mmu, kobj_va(o), o->km.size);
	pvrsgx_mmu_unmap(o->km.mmu, o->km.dev_addr, o->km.size);
	o->sdev = sdev;
	o->inval.done = kobj_free_done;
	pvrsgx_ccb_after_invalidate(sdev, &o->inval,
				    PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD);
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
	bool restart = renders_done == U64_MAX;
	LIST_HEAD(dead);

	mutex_lock(&sdev->kobj_lock);
	list_for_each_entry_safe(o, tmp, &sdev->graveyard, grave) {
		if (o->grave_seq < renders_done && (restart || !o->until_restart))
			list_move_tail(&o->grave, &dead);
	}
	mutex_unlock(&sdev->kobj_lock);
	list_for_each_entry_safe(o, tmp, &dead, grave)
		kobj_free(sdev, o);
}

void pvrsgx_kobj_fini(struct pvrsgx_device *sdev)
{
	/*
	 * Freeing can release more: a VM's last object frees the VM, which
	 * drops its PB reference, and the PB's objects join the graveyard.
	 */
	for (;;) {
		bool empty;

		mutex_lock(&sdev->kobj_lock);
		empty = list_empty(&sdev->graveyard);
		mutex_unlock(&sdev->kobj_lock);
		/* The GPU is stopped: what waits for an invalidate can go too. */
		empty = empty && list_empty(&sdev->inval_waits);
		if (empty)
			break;
		pvrsgx_kobj_reap(sdev, U64_MAX);
		pvrsgx_ccb_inval_reap(sdev, true);
	}
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++)
		drm_mm_takedown(&sdev->krange[r]);
	mutex_destroy(&sdev->kobj_lock);
}

void pvrsgx_kobj_drop(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o)
{
	pvrsgx_kobj_release(sdev, *o);
	*o = NULL;
}

/* A kernel object into *dst, which stays NULL on failure. */
int pvrsgx_kobj_new(struct pvrsgx_device *sdev, struct pvrsgx_kobj **dst,
		    enum pvrsgx_krange range, u32 size, u32 min, struct pvrsgx_vm *vm)
{
	struct pvrsgx_kobj *o = pvrsgx_kobj_create(sdev, range, size, min, vm);

	if (IS_ERR(o))
		return PTR_ERR(o);
	*dst = o;
	return 0;
}

void pvrsgx_kobj_dump(struct seq_buf *sb, const char *name, struct pvrsgx_kobj *o, u32 off,
		      u32 bytes)
{
	const u32 *w = (const u32 *)((const u8 *)kobj_cpu(o) + off);

	seq_buf_printf(sb, "%s @ 0x%08x:", name, kobj_va(o) + off);
	for (u32 i = 0; i < bytes / 4; i++)
		seq_buf_printf(sb, "%s%08x", i % 8 ? " " : "\n  ", READ_ONCE(w[i]));
	seq_buf_puts(sb, "\n");
}

