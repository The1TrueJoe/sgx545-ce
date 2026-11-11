// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Kernel-owned objects at device-wide addresses: render contexts' and RT
 * datasets' firmware structures, parameter buffers, free lists, region headers.
 * The 3D parameters range (PB, free list, scratch, background block) is per VM
 * instead: the DPM numbers PB pages from the heap base, so a second context's PB
 * at a higher device-wide address hung its third render (EA1, 2026-11-03).
 *
 * Releasing one does not free it at once: a render already handed to the
 * microkernel may still read it. Released objects are freed once every render
 * submitted before the release has completed. A PB descriptor waits for the
 * next restart instead: the microkernel remembers the last PB's address to
 * skip reloading its free list, so a new PB at a freed one's address must not
 * appear while it runs.
 */

#include <drm/drm_print.h>

#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/set_memory.h>

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

/*
 * Debug: a freed object's pages are kept, unmapped from the GPU, filled with a
 * pattern, and checked for writes later (at reaps, a few pages at a time, and
 * in full at a restart and at unload). Its device-wide address stays allocated,
 * so a later access through the page tables faults. 1: as freeing does today,
 * the BIF TLB is not invalidated, so a stale translation still reaches the
 * kept page. 2: the TLB is invalidated with the next command, so a late access
 * faults instead, at the address the recovery message prints.
 */
static int quarantine;
module_param(quarantine, int, 0444);
MODULE_PARM_DESC(quarantine,
		 "Debug: keep freed kernel objects and report late GPU writes to them (0 off, 1 keep, 2 keep and invalidate the BIF TLB at free)");

static unsigned int quarantine_max_mb = 32;
module_param(quarantine_max_mb, uint, 0444);
MODULE_PARM_DESC(quarantine_max_mb, "Debug: quarantine size before the oldest entry is checked and freed");

/* Pages checked per reap; a restart and unload check everything. */
#define QUARANTINE_CHECK_PAGES	16
/* Changed dwords printed per object and check. */
#define QUARANTINE_PRINT	16

static u32 q_pattern(u32 dword)
{
	return 0x5afe0000 | (dword & 0xffff);
}

int pvrsgx_kobj_init(struct pvrsgx_device *sdev)
{
	mutex_init(&sdev->kobj_lock);
	INIT_LIST_HEAD(&sdev->graveyard);
	INIT_LIST_HEAD(&sdev->quarantine);
	if (quarantine)
		drm_warn(&sdev->base, "kernel object quarantine %d on (debug, up to %u MiB)\n",
			 quarantine, quarantine_max_mb);
	for (int r = 0; r < PVRSGX_KRANGE_COUNT; r++)
		drm_mm_init(&sdev->krange[r], ranges[r].start, ranges[r].end - ranges[r].start);
	return 0;
}

static struct pvrsgx_kobj *kobj_create(struct pvrsgx_device *sdev, const char *name,
				       enum pvrsgx_krange range, u32 size, u32 min_addr,
				       struct pvrsgx_vm *vm)
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
	o->name = name;
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

struct pvrsgx_kobj *pvrsgx_kobj_create(struct pvrsgx_device *sdev, enum pvrsgx_krange range,
				       u32 size, u32 min_addr, struct pvrsgx_vm *vm)
{
	return kobj_create(sdev, "kobj", range, size, min_addr, vm);
}

/* Compare a quarantined object's pages [first, last) with the pattern. */
static void q_check(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o, unsigned int first,
		    unsigned int last)
{
	u32 *w = o->km.cpu;
	u32 printed = 0, hits = 0;

	for (u32 i = first * (PAGE_SIZE / 4); i < last * (PAGE_SIZE / 4); i++) {
		u32 v = READ_ONCE(w[i]);

		if (v == q_pattern(i))
			continue;
		if (!hits++)
			WARN(1, "powervr_sgx: quarantined %s (0x%08x, %u pages) written %llu renders, %u ms after it was freed\n",
			     o->name, o->km.dev_addr, o->km.npages,
			     sdev->renders_done - o->q_done,
			     jiffies_to_msecs(jiffies - o->q_jiffies));
		if (printed++ < QUARANTINE_PRINT)
			drm_err(&sdev->base,
				"quarantine: %s +0x%x (0x%08x, page %u): 0x%08x -> 0x%08x\n",
				o->name, i * 4, o->km.dev_addr + i * 4, (u32)(i / (PAGE_SIZE / 4)),
				q_pattern(i), v);
		/* Re-arm: a later write is reported again. */
		WRITE_ONCE(w[i], q_pattern(i));
	}
	if (hits) {
		o->q_hits += hits;
		drm_err(&sdev->base, "quarantine: %s: %u dwords changed (%u in all)\n", o->name,
			hits, o->q_hits);
		wmb();
	}
}

/* The last of a quarantined object: its pages and, if kept, its address. */
static void q_release(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	q_check(sdev, o, 0, o->km.npages);
	vunmap(o->km.cpu);
	set_pages_array_wb(o->km.pages, o->km.npages);
	for (unsigned int i = 0; i < o->km.npages; i++)
		__free_page(o->km.pages[i]);
	kvfree(o->km.pages);
	if (o->q_node) {
		mutex_lock(&sdev->kobj_lock);
		drm_mm_remove_node(&o->node);
		mutex_unlock(&sdev->kobj_lock);
	}
	kfree(o);
}

/* Is any page of o still mapped in mmu? A PTE left behind would be a bug of its own. */
static void q_check_unmapped(struct pvrsgx_device *sdev, struct pvrsgx_mmu *mmu,
			     struct pvrsgx_kobj *o, const char *dir)
{
	for (unsigned int i = 0; i < o->km.npages; i++) {
		u32 addr = o->km.dev_addr + i * PAGE_SIZE, pde, pte;

		if (pvrsgx_mmu_lookup(mmu, addr, &pde, &pte) && (pte & SGX_MMU_PTE_VALID))
			drm_err(&sdev->base,
				"quarantine: %s page %u (0x%08x) still mapped in the %s directory: pde 0x%08x pte 0x%08x\n",
				o->name, i, addr, dir, pde, pte);
	}
}

static void kobj_quarantine(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	struct pvrsgx_vm *vm = o->vm;
	bool per_vm = vm && o->km.mmu == vm->mmu;
	u32 *w = o->km.cpu;
	LIST_HEAD(old);

	if (vm && !per_vm) {
		pvrsgx_mmu_unmap(vm->mmu, kobj_va(o), o->km.size);
		q_check_unmapped(sdev, vm->mmu, o, "VM");
	}
	pvrsgx_mmu_unmap(o->km.mmu, o->km.dev_addr, o->km.size);
	q_check_unmapped(sdev, o->km.mmu, o, per_vm ? "VM" : "firmware");
	o->km.mmu = NULL;

	/* A per-VM address goes with its VM; a device-wide one is kept. */
	o->q_node = !per_vm;
	if (per_vm) {
		mutex_lock(&sdev->kobj_lock);
		drm_mm_remove_node(&o->node);
		mutex_unlock(&sdev->kobj_lock);
	}
	o->vm = NULL;
	pvrsgx_vm_put(vm);

	for (u32 i = 0; i < o->km.size / 4; i++)
		WRITE_ONCE(w[i], q_pattern(i));
	wmb();
	if (quarantine >= 2) {
		mutex_lock(&sdev->ccb_lock);
		sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD;
		mutex_unlock(&sdev->ccb_lock);
	}

	mutex_lock(&sdev->kobj_lock);
	o->q_done = sdev->renders_done;
	o->q_jiffies = jiffies;
	list_add_tail(&o->grave, &sdev->quarantine);
	sdev->quarantine_pages += o->km.npages;
	/* Over the cap: the oldest go, checked one last time. */
	while (sdev->quarantine_pages > (unsigned long)quarantine_max_mb << (20 - PAGE_SHIFT)) {
		struct pvrsgx_kobj *q = list_first_entry(&sdev->quarantine, struct pvrsgx_kobj,
							 grave);

		if (q == o)
			break;
		sdev->quarantine_pages -= q->km.npages;
		list_move_tail(&q->grave, &old);
	}
	mutex_unlock(&sdev->kobj_lock);
	while (!list_empty(&old)) {
		struct pvrsgx_kobj *q = list_first_entry(&old, struct pvrsgx_kobj, grave);

		list_del(&q->grave);
		q_release(sdev, q);
	}
}

/* Check the quarantine: the next few pages, round robin, or all of it. */
static void q_scan(struct pvrsgx_device *sdev, bool all)
{
	unsigned long pos = 0, budget = QUARANTINE_CHECK_PAGES;
	struct pvrsgx_kobj *o;

	mutex_lock(&sdev->kobj_lock);
	list_for_each_entry(o, &sdev->quarantine, grave) {
		unsigned long end = pos + o->km.npages;

		if (all) {
			q_check(sdev, o, 0, o->km.npages);
		} else if (end > sdev->quarantine_cursor && budget) {
			unsigned int first = sdev->quarantine_cursor > pos ?
					     sdev->quarantine_cursor - pos : 0;
			unsigned int last = min_t(unsigned long, o->km.npages, first + budget);

			q_check(sdev, o, first, last);
			budget -= last - first;
			sdev->quarantine_cursor = pos + last;
		}
		pos = end;
	}
	if (all || sdev->quarantine_cursor >= pos)
		sdev->quarantine_cursor = 0;
	mutex_unlock(&sdev->kobj_lock);
}

/* For a fault address: the quarantined object it falls in, if any. */
const char *pvrsgx_kobj_quarantined(struct pvrsgx_device *sdev, u32 dev_addr, u64 *renders_ago)
{
	struct pvrsgx_kobj *o;

	/* Called from the reset path: never wait for the lock. */
	if (!quarantine || !mutex_trylock(&sdev->kobj_lock))
		return NULL;
	list_for_each_entry(o, &sdev->quarantine, grave) {
		if (dev_addr - o->km.dev_addr < o->km.size) {
			*renders_ago = sdev->renders_done - o->q_done;
			mutex_unlock(&sdev->kobj_lock);
			return o->name;
		}
	}
	mutex_unlock(&sdev->kobj_lock);
	return NULL;
}

static void kobj_free(struct pvrsgx_device *sdev, struct pvrsgx_kobj *o)
{
	struct pvrsgx_vm *vm = o->vm;

	if (quarantine) {
		kobj_quarantine(sdev, o);
		return;
	}

	if (vm && o->km.mmu != vm->mmu)
		pvrsgx_mmu_unmap(vm->mmu, kobj_va(o), o->km.size);
	pvrsgx_kmem_free(sdev, &o->km);
	mutex_lock(&sdev->kobj_lock);
	drm_mm_remove_node(&o->node);
	mutex_unlock(&sdev->kobj_lock);
	kfree(o);
	/* Last: the node may belong to the VM's own allocator. */
	pvrsgx_vm_put(vm);
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
	if (quarantine)
		q_scan(sdev, restart);
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
		if (empty)
			break;
		pvrsgx_kobj_reap(sdev, U64_MAX);
	}
	while (!list_empty(&sdev->quarantine)) {
		struct pvrsgx_kobj *o = list_first_entry(&sdev->quarantine, struct pvrsgx_kobj,
							 grave);

		list_del(&o->grave);
		sdev->quarantine_pages -= o->km.npages;
		q_release(sdev, o);
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
int __pvrsgx_kobj_new(struct pvrsgx_device *sdev, struct pvrsgx_kobj **dst, const char *name,
		      enum pvrsgx_krange range, u32 size, u32 min, struct pvrsgx_vm *vm)
{
	struct pvrsgx_kobj *o = kobj_create(sdev, name, range, size, min, vm);

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

