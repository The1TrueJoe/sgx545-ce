// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Kernel-owned objects at device-wide addresses: render contexts' and RT
 * datasets' firmware structures, parameter buffers, free lists, region headers.
 * The 3D parameters range (PB, free list, scratch, background block) is per VM
 * instead: the DPM numbers PB pages from the heap base, so a second context's PB
 * at a higher device-wide address hangs the GPU.
 *
 * Releasing one does not free it at once: a render already handed to the
 * microkernel may still read it. Released objects are freed once every render
 * submitted before the release has completed. A PB descriptor waits for the
 * next restart instead: the microkernel remembers the last PB's address to
 * skip reloading its free list, so a new PB at a freed one's address must not
 * appear while it runs.
 *
 * Unmapping is not the end either: the BIF may hold a translation of the
 * object's pages until it is invalidated, and the microkernel invalidates only
 * for the cache control word of a command it takes, before it moves the read
 * offset on. So a reaped object is unmapped, the invalidate is queued for the
 * next command, and its pages and address are freed once the kernel CCB has
 * consumed that command. A hardware unit's late write then faults instead of
 * landing in a page the kernel has handed out again.
 */

#include <linux/mm.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_vm.h"

struct krange {
	u32 start, end, pte;
};

/* Each range in the core's layout (pvrsgx_core.c). */
static struct krange krange(const struct pvrsgx_device *sdev, enum pvrsgx_krange r)
{
	const struct pvrsgx_heap_layout *h = pvrsgx_heaps(sdev);

	switch (r) {
	case PVRSGX_KRANGE_KDATA:	/* past the firmware's own arenas (pvrsgx_fw.c) */
		/* EDM-protected where the core allows it, as the firmware's data arena. */
		return (struct krange){ h->kernel_data.base + 0x10000, h->kernel_data.end,
					SGX_MMU_PTE_CACHECONSISTENT |
					(sdev->family->data_no_edmprotect ? 0 :
					 SGX_MMU_PTE_EDMPROTECT) };
	case PVRSGX_KRANGE_KCODE:
		return (struct krange){ h->kernel_code.base + 0x10000, h->kernel_code.end,
					SGX_MMU_PTE_CACHECONSISTENT };
	case PVRSGX_KRANGE_SYNC:
		return (struct krange){ h->sync.base, h->sync.end, SGX_MMU_PTE_CACHECONSISTENT };
	case PVRSGX_KRANGE_3DPARAMS:
		return (struct krange){ h->params_3d.base, h->params_3d.end, 0 };
	case PVRSGX_KRANGE_TADATA:
	default:
		return (struct krange){ h->ta_data.base, h->ta_data.end, 0 };
	}
}

int pvrsgx_kobj_init(struct pvrsgx_device *sdev)
{
	mutex_init(&sdev->kobj_lock);
	INIT_LIST_HEAD(&sdev->graveyard);
	INIT_LIST_HEAD(&sdev->kobj_unmapped);
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++) {
		struct krange kr = krange(sdev, r);

		drm_mm_init(&sdev->krange[r], kr.start, kr.end - kr.start);
	}
	return 0;
}

struct pvrsgx_kobj *pvrsgx_kobj_create(struct pvrsgx_device *sdev, enum pvrsgx_krange range,
				       u32 size, u32 min_addr, struct pvrsgx_vm *vm)
{
	struct pvrsgx_kobj *o;
	struct krange kr = krange(sdev, range);
	u32 start = max(min_addr, kr.start);
	/* A 3D-params object without a VM is the device's: the PB (pvrsgx_render.c). */
	bool per_vm = range == PVRSGX_KRANGE_3DPARAMS && vm;
	int err;

	/* Clients' ioctls create these: charge them to the caller. */
	o = kzalloc_obj(*o, GFP_KERNEL_ACCOUNT);
	if (!o)
		return ERR_PTR(-ENOMEM);
	o->range = range;
	o->mm = per_vm ? &vm->params : &sdev->krange[range];
	mutex_lock(&sdev->kobj_lock);
	err = drm_mm_insert_node_in_range(o->mm, &o->node, PAGE_ALIGN(size), PAGE_SIZE,
					  0, start, kr.end, DRM_MM_INSERT_LOW);
	mutex_unlock(&sdev->kobj_lock);
	if (err)
		goto err_free;

	if (per_vm)
		o->km.mmu = vm->mmu;
	err = pvrsgx_kmem_alloc(sdev, &o->km, o->node.start, size, kr.pte,
				__GFP_ACCOUNT);
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

static void kobj_unmap(struct pvrsgx_kobj *o)
{
	struct pvrsgx_vm *vm = o->vm;

	if (vm && o->km.mmu != vm->mmu)
		pvrsgx_mmu_unmap(vm->mmu, kobj_va(o), o->km.size);
	pvrsgx_mmu_unmap(o->km.mmu, o->km.dev_addr, o->km.size);
}

/* An unmapped object, after the invalidate: nothing can reach it now. */
static void kobj_free(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
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
 * An object no command has named: the GPU never walked to it, so it goes now,
 * without the graveyard's wait for a render and an invalidate.
 */
void pvrsgx_kobj_free_unused(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o)
{
	if (IS_ERR_OR_NULL(*o))
		return;
	kobj_unmap(*o);
	kobj_free(sdev, *o);
	*o = NULL;
}

void pvrsgx_kobj_release(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	if (IS_ERR_OR_NULL(o))
		return;
	mutex_lock(&sdev->kobj_lock);
	o->grave_seq = o->transfer ? sdev->transfers_submitted : sdev->renders_submitted;
	o->grave_restarts = READ_ONCE(sdev->restarts);
	list_add_tail(&o->grave, &sdev->graveyard);
	mutex_unlock(&sdev->kobj_lock);
	/*
	 * No render will finish to free it (unbind, or a device lost): free it
	 * now. Once the device's release has drained its queues, the release's
	 * own kobj_fini() does it.
	 */
	if (READ_ONCE(sdev->lost) && sdev->free_wq)
		queue_work(sdev->free_wq, &sdev->reap_work);
}

/*
 * Unmap what was released before a render that has now finished, and free
 * what was unmapped before a command the microkernel has since taken.
 * renders_done U64_MAX (unbind, the GPU stopped) frees everything at once.
 */
void pvrsgx_kobj_reap(struct pvrsgx_device *sdev, u64 renders_done, u64 transfers_done)
{
	struct pvrsgx_kobj *o, *tmp;
	bool all = renders_done == U64_MAX;
	u32 restarts = READ_ONCE(sdev->restarts);
	u64 seq = 0, consumed;
	LIST_HEAD(dead);

	mutex_lock(&sdev->kobj_lock);
	list_for_each_entry_safe(o, tmp, &sdev->graveyard, grave) {
		/* A restart forgets everything the old run pointed at. */
		if (all || o->grave_restarts != restarts ||
		    (o->grave_seq < (o->transfer ? transfers_done : renders_done) &&
		     !o->until_restart))
			list_move_tail(&o->grave, &dead);
	}
	mutex_unlock(&sdev->kobj_lock);

	if (!list_empty(&dead)) {
		list_for_each_entry(o, &dead, grave)
			kobj_unmap(o);
		/* The next command carries the invalidate. */
		mutex_lock(&sdev->ccb_lock);
		sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD;
		seq = sdev->ccb_issued + 1;
		mutex_unlock(&sdev->ccb_lock);
	}

	mutex_lock(&sdev->kobj_lock);
	list_for_each_entry(o, &dead, grave)
		o->flush_seq = seq;
	list_splice_tail_init(&dead, &sdev->kobj_unmapped);
	/*
	 * After a restart the ring starts over empty, so everything sent before it
	 * counts as consumed: the GPU was reset, and the first command after the
	 * microkernel's start carries an invalidate of its own.
	 */
	consumed = all ? U64_MAX : pvrsgx_ccb_consumed(sdev);
	list_for_each_entry_safe(o, tmp, &sdev->kobj_unmapped, grave) {
		if (o->flush_seq <= consumed)
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
		empty = list_empty(&sdev->graveyard) && list_empty(&sdev->kobj_unmapped);
		mutex_unlock(&sdev->kobj_lock);
		if (empty)
			break;
		pvrsgx_kobj_reap(sdev, U64_MAX, U64_MAX);
	}
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++)
		drm_mm_takedown(&sdev->krange[r]);
	mutex_destroy(&sdev->kobj_lock);
}

/* For status dumps: what waits to be freed. Never sleeps: false if busy. */
bool pvrsgx_kobj_pending(struct pvrsgx_device *sdev, struct pvrsgx_kobj_counts *c)
{
	struct pvrsgx_kobj *o;

	if (!mutex_trylock(&sdev->kobj_lock))
		return false;
	memset(c, 0, sizeof(*c));
	list_for_each_entry(o, &sdev->graveyard, grave) {
		c->graveyard++;
		c->graveyard_pages += o->km.npages;
	}
	list_for_each_entry(o, &sdev->kobj_unmapped, grave) {
		c->unmapped++;
		c->unmapped_pages += o->km.npages;
	}
	mutex_unlock(&sdev->kobj_lock);
	return true;
}

void pvrsgx_kobj_drop(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o)
{
	pvrsgx_kobj_release(sdev, *o);
	*o = NULL;
}

/* Dropped if a command may have named it, freed now if none did. */
void pvrsgx_kobj_drop_sent(struct pvrsgx_device *sdev, struct pvrsgx_kobj **o, bool sent)
{
	if (sent)
		pvrsgx_kobj_drop(sdev, o);
	else
		pvrsgx_kobj_free_unused(sdev, o);
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

