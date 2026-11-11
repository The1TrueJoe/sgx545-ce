// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * VM contexts on drm_gpuvm. The client picks every address inside the heaps
 * DEV_QUERY reports. VM_BIND maps and unmaps through drm_gpuvm's split and
 * merge steps; an unmap takes the page tables down at once and lets go of the
 * objects once every job submitted before it is done, from a fence callback.
 */

#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_gem.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>

#include <linux/device.h>
#include <linux/dma-resv.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_gem.h"
#include "pvrsgx_job.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_render.h"
#include "pvrsgx_validate.h"
#include "pvrsgx_prime.h"
#include "pvrsgx_vm.h"

/* The client heaps, the same in every VM. */
static const struct drm_powervr_sgx_heap heaps[DRM_POWERVR_SGX_HEAP_COUNT] = {
	[DRM_POWERVR_SGX_HEAP_GENERAL] = {
		.base = PVRSGX_HEAP_GENERAL,
		.size = PVRSGX_HEAP_TEXTURE - PVRSGX_HEAP_GENERAL,
		.page_size_log2 = SGX_MMU_PAGE_SHIFT,
	},
	[DRM_POWERVR_SGX_HEAP_PDS_PIXEL] = {
		.base = PVRSGX_HEAP_PDS_PIXEL,
		.size = PVRSGX_HEAP_KERNEL_CODE - PVRSGX_HEAP_PDS_PIXEL,
		.page_size_log2 = SGX_MMU_PAGE_SHIFT,
	},
	[DRM_POWERVR_SGX_HEAP_PDS_VERTEX] = {
		.base = PVRSGX_HEAP_PDS_VERTEX,
		.size = PVRSGX_HEAP_KERNEL_DATA - PVRSGX_HEAP_PDS_VERTEX,
		.page_size_log2 = SGX_MMU_PAGE_SHIFT,
	},
	[DRM_POWERVR_SGX_HEAP_USSE_PIXEL] = {
		.base = PVRSGX_HEAP_USSE_PIXEL,
		.size = PVRSGX_HEAP_USSE_PIXEL_SIZE,
		.page_size_log2 = SGX_MMU_PAGE_SHIFT,
		.usse_base_index = PVRSGX_USSE_PIXEL_BASE_INDEX,
		.usse_window_log2 = PVRSGX_USSE_WINDOW_LOG2,
	},
	[DRM_POWERVR_SGX_HEAP_USSE_VERTEX] = {
		.base = PVRSGX_HEAP_USSE_VERTEX,
		.size = PVRSGX_HEAP_USSE_VERTEX_SIZE,
		.page_size_log2 = SGX_MMU_PAGE_SHIFT,
		.usse_base_index = PVRSGX_USSE_VERTEX_BASE_INDEX,
		.usse_window_log2 = PVRSGX_USSE_WINDOW_LOG2,
	},
	[DRM_POWERVR_SGX_HEAP_TEXTURE] = {
		.base = PVRSGX_HEAP_TEXTURE,
		.size = SZ_4G - PVRSGX_HEAP_TEXTURE,
		.page_size_log2 = SGX_MMU_PAGE_SHIFT,
	},
};

/* The heap holding all of [addr, addr + size), or -1. */
int pvrsgx_heap_find(u64 addr, u64 size)
{
	u64 end;

	if (check_add_overflow(addr, size, &end))
		return -1;
	for (int i = 0; i < ARRAY_SIZE(heaps); i++) {
		if (addr >= heaps[i].base && end <= heaps[i].base + heaps[i].size)
			return i;
	}
	return -1;
}

int pvrsgx_query_heap_info(struct pvrsgx_device *sdev, void __user *ptr, u32 *size)
{
	struct drm_powervr_sgx_dev_query_heap_info info = {};
	int err;

	if (!ptr) {
		*size = sizeof(info);
		return 0;
	}
	err = copy_struct_from_user(&info, sizeof(info), ptr, *size);
	if (err)
		return err;

	if (info.heaps.array) {
		err = pvrsgx_obj_array_put(&info.heaps, heaps, sizeof(heaps[0]), ARRAY_SIZE(heaps));
		if (err)
			return err;
	}
	info.heaps.stride = sizeof(heaps[0]);
	info.heaps.count = ARRAY_SIZE(heaps);

	*size = min_t(u32, *size, sizeof(info));
	return copy_to_user(ptr, &info, *size) ? -EFAULT : 0;
}

struct pvrsgx_vm *pvrsgx_vm_get(struct pvrsgx_vm *vm)
{
	kref_get(&vm->ref);
	return vm;
}

struct dma_resv *pvrsgx_vm_resv(struct pvrsgx_vm *vm)
{
	return drm_gpuvm_resv(&vm->gpuvm);
}

static const struct drm_gpuvm_ops pvrsgx_gpuvm_ops;

/* A mapping. Once unmapped, it keeps its object until the jobs before the unmap are done. */
struct pvrsgx_va {
	struct drm_gpuva base;
	struct drm_gem_object *obj;
	struct list_head free_link;
};

static struct pvrsgx_va *to_pvrsgx_va(struct drm_gpuva *va)
{
	return container_of(va, struct pvrsgx_va, base);
}

/*
 * What an unmap or a teardown lets go of once every job on the VM so far is
 * done. A teardown (vm set) also takes the mappings down then.
 */
struct pvrsgx_vm_free {
	struct dma_fence_cb cb;
	struct work_struct work;
	struct pvrsgx_inval_wait inval;
	struct drm_device *drm;
	struct dma_fence *fence;
	struct list_head vas;
	struct pvrsgx_vm *vm;
};

/* The BIF may hold walks of what changed: the next command drops them. */
static void vm_invalidate(struct pvrsgx_device *sdev, bool data)
{
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD |
				  (data ? PVRSGX_CC_INVAL_DATA : 0);
	mutex_unlock(&sdev->ccb_lock);
}

/* Unlink a removed mapping from its object, keeping a reference on freed. */
static void va_unlink_locked(struct drm_gpuva *va, struct list_head *freed)
{
	struct pvrsgx_va *v = to_pvrsgx_va(va);

	v->obj = va->gem.obj;
	drm_gem_object_get(v->obj);
	drm_gpuva_unlink(va);
	list_add_tail(&v->free_link, freed);
}

static void vas_free(struct list_head *list)
{
	struct pvrsgx_va *v, *tmp;

	list_for_each_entry_safe(v, tmp, list, free_link) {
		list_del(&v->free_link);
		drm_gem_object_put(v->obj);
		kfree(v);
	}
}

/* Every mapping down, onto freed. Nothing may submit to the VM any more. */
static void vm_unmap_all(struct pvrsgx_vm *vm, struct list_head *freed)
{
	struct drm_gpuva *va, *next;

	drm_gpuvm_for_each_va_safe(va, next, &vm->gpuvm) {
		struct drm_gem_object *obj = va->gem.obj;

		pvrsgx_mmu_unmap(vm->mmu, va->va.addr, va->va.range);
		drm_gpuva_remove(va);
		dma_resv_lock(obj->resv, NULL);
		va_unlink_locked(va, freed);
		dma_resv_unlock(obj->resv);
	}
}

/* The invalidate has run: nothing can reach the unmapped pages any more. */
static void vm_free_done(struct pvrsgx_inval_wait *w)
{
	struct pvrsgx_vm_free *f = container_of(w, struct pvrsgx_vm_free, inval);

	vas_free(&f->vas);
	pvrsgx_vm_put(f->vm);
	dma_fence_put(f->fence);
	drm_dev_put(f->drm);
	kfree(f);
}

static void vm_free_work(struct work_struct *work)
{
	struct pvrsgx_vm_free *f = container_of(work, struct pvrsgx_vm_free, work);

	if (f->vm) {
		mutex_lock(&f->vm->lock);
		vm_unmap_all(f->vm, &f->vas);
		mutex_unlock(&f->vm->lock);
	}
	f->inval.done = vm_free_done;
	pvrsgx_ccb_after_invalidate(to_pvrsgx_device(f->drm), &f->inval,
				    PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD |
				    PVRSGX_CC_INVAL_DATA);
}

static void vm_free_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct pvrsgx_vm_free *f = container_of(cb, struct pvrsgx_vm_free, cb);

	queue_work(system_dfl_wq, &f->work);
}

/* Run f once every job submitted to vm so far is done, without waiting for them. */
static void vm_free_after_jobs(struct pvrsgx_vm *vm, struct pvrsgx_vm_free *f)
{
	struct dma_fence *fence = NULL;

	f->drm = &vm->sdev->base;
	drm_dev_get(f->drm);
	INIT_WORK(&f->work, vm_free_work);
	if (dma_resv_get_singleton(pvrsgx_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP, &fence)) {
		/* No memory for a fence array: wait here instead. */
		dma_resv_wait_timeout(pvrsgx_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP, false,
				      MAX_SCHEDULE_TIMEOUT);
		fence = NULL;
	}
	f->fence = fence;
	if (!fence || dma_fence_add_callback(fence, &f->cb, vm_free_cb))
		queue_work(system_dfl_wq, &f->work);
}

static struct pvrsgx_vm_free *vm_free_alloc(void)
{
	struct pvrsgx_vm_free *f = kzalloc_obj(*f);

	if (f)
		INIT_LIST_HEAD(&f->vas);
	return f;
}

static void vm_release(struct kref *ref)
{
	struct pvrsgx_vm *vm = container_of(ref, struct pvrsgx_vm, ref);
	LIST_HEAD(freed);

	/* No job holds this VM any more, so nothing on the GPU uses it. */
	vm_unmap_all(vm, &freed);
	vas_free(&freed);
	if (vm->pb)
		pvrsgx_pb_unmap(vm);
	drm_mm_takedown(&vm->params);
	pvrsgx_mmu_destroy(vm->mmu);
	/* The directory and its pages may come back as another VM's. */
	mutex_lock(&vm->sdev->ccb_lock);
	vm->sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD |
				      PVRSGX_CC_INVAL_DATA;
	mutex_unlock(&vm->sdev->ccb_lock);
	mutex_destroy(&vm->lock);
	drm_gpuvm_put(&vm->gpuvm);
}

void pvrsgx_vm_put(struct pvrsgx_vm *vm)
{
	if (vm)
		kref_put(&vm->ref, vm_release);
}

void pvrsgx_vm_user_get(struct pvrsgx_vm *vm)
{
	atomic_inc(&vm->users);
}

/*
 * The handle or a context is gone. With neither left, nothing can submit to
 * the VM again: once its jobs are done, unmap and release every object. The
 * VM itself can live on much longer, held by its contexts' kernel objects
 * until a later render (pvrsgx_kobj.c), but the client's memory goes then.
 */
void pvrsgx_vm_user_put(struct pvrsgx_vm *vm)
{
	struct pvrsgx_vm_free *f;
	LIST_HEAD(freed);

	if (!atomic_dec_and_test(&vm->users))
		return;

	f = vm_free_alloc();
	if (f) {
		f->vm = pvrsgx_vm_get(vm);
		vm_free_after_jobs(vm, f);
		return;
	}
	/*
	 * No memory to defer it: wait for the jobs here. The pages then go before
	 * the invalidate has run; nothing of this VM can run again, and the next
	 * scene's directory switch invalidates before any translation.
	 */
	dma_resv_wait_timeout(pvrsgx_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP, false,
			      MAX_SCHEDULE_TIMEOUT);
	mutex_lock(&vm->lock);
	vm_unmap_all(vm, &freed);
	mutex_unlock(&vm->lock);
	vm_invalidate(vm->sdev, true);
	vas_free(&freed);
}

struct pvrsgx_vm *pvrsgx_vm_create(struct pvrsgx_device *sdev)
{
	struct drm_gem_object *r_obj;
	struct pvrsgx_vm *vm;

	vm = kzalloc_obj(*vm);
	if (!vm)
		return ERR_PTR(-ENOMEM);
	vm->mmu = pvrsgx_mmu_create(sdev);
	if (IS_ERR(vm->mmu)) {
		struct pvrsgx_mmu *mmu = vm->mmu;

		kfree(vm);
		return ERR_CAST(mmu);
	}
	r_obj = drm_gpuvm_resv_object_alloc(&sdev->base);
	if (!r_obj) {
		pvrsgx_mmu_destroy(vm->mmu);
		kfree(vm);
		return ERR_PTR(-ENOMEM);
	}
	drm_gpuvm_init(&vm->gpuvm, "powervr_sgx", 0, &sdev->base, r_obj,
		       0, SZ_4G, 0, 0, &pvrsgx_gpuvm_ops);
	drm_gem_object_put(r_obj);
	vm->sdev = sdev;
	drm_mm_init(&vm->params, PVRSGX_HEAP_3D_PARAMS,
		    PVRSGX_HEAP_TA_DATA - PVRSGX_HEAP_3D_PARAMS);
	mutex_init(&vm->lock);
	kref_init(&vm->ref);
	atomic_set(&vm->users, 1);
	return vm;
}

struct pvrsgx_vm *pvrsgx_vm_lookup(struct pvrsgx_file *sfile, u32 handle)
{
	struct pvrsgx_vm *vm;

	xa_lock(&sfile->vms);
	vm = xa_load(&sfile->vms, handle);
	if (vm)
		pvrsgx_vm_get(vm);
	xa_unlock(&sfile->vms);
	return vm;
}

void pvrsgx_vm_destroy_all(struct pvrsgx_file *sfile)
{
	struct pvrsgx_vm *vm;
	unsigned long handle;

	xa_for_each(&sfile->vms, handle, vm) {
		xa_erase(&sfile->vms, handle);
		pvrsgx_vm_user_put(vm);
		pvrsgx_vm_put(vm);
	}
}

int pvrsgx_ioctl_create_vm_context(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_create_vm_context_args *args = data;
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
	struct pvrsgx_vm *vm;
	int err;

	if (args->_padding_4)
		return -EINVAL;

	vm = pvrsgx_vm_create(to_pvrsgx_device(drm));
	if (IS_ERR(vm))
		return PTR_ERR(vm);
	err = xa_alloc(&sfile->vms, &args->handle, vm, xa_limit_32b, GFP_KERNEL);
	if (err)
		pvrsgx_vm_put(vm);
	return err;
}

int pvrsgx_ioctl_destroy_vm_context(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_destroy_vm_context_args *args = data;
	struct pvrsgx_vm *vm;

	if (args->_padding_4)
		return -EINVAL;

	vm = xa_erase(&to_pvrsgx_file(file)->vms, args->handle);
	if (!vm)
		return -ENOENT;
	/* Contexts and jobs that use it keep it until they are done. */
	pvrsgx_vm_user_put(vm);
	pvrsgx_vm_put(vm);
	return 0;
}

#define PVRSGX_MAX_BIND_OPS	256
#define PVRSGX_MAX_BIND_SYNCS	64

/* One op of a VM_BIND and what it needs, all gathered before anything changes. */
struct bind_op {
	struct drm_powervr_sgx_vm_bind_op in;
	/* MAP: the object, its pages, its link to the VM, the new mapping. */
	struct drm_gem_object *obj;
	struct sg_table *sgt;
	struct drm_gpuvm_bo *vm_bo;
	/* MAP: [0]. UNMAP: the pieces a split may leave at either end. */
	struct pvrsgx_va *va[2];
};

struct bind_ctx {
	struct pvrsgx_vm *vm;
	struct bind_op *op;
	int next_va;
	struct list_head *freed;
};

static struct pvrsgx_va *bind_take_va(struct bind_ctx *b)
{
	struct pvrsgx_va *v = b->op->va[b->next_va];

	b->op->va[b->next_va++] = NULL;
	return v;
}

/* The steps below can't fail: bind_prepare() checked and allocated everything. */
static int bind_step_map(struct drm_gpuva_op *op, void *priv)
{
	struct bind_ctx *b = priv;
	struct bind_op *o = b->op;
	struct pvrsgx_va *v = bind_take_va(b);

	WARN_ON(pvrsgx_mmu_map_sgt(b->vm->mmu, op->map.va.addr, o->sgt, op->map.gem.offset,
				   op->map.va.range,
				   pvrsgx_validate_pte_flags(to_pvrsgx_bo(o->obj))));
	drm_gpuva_map(&b->vm->gpuvm, &v->base, &op->map);
	drm_gpuva_link(&v->base, o->vm_bo);
	return 0;
}

static int bind_step_remap(struct drm_gpuva_op *op, void *priv)
{
	struct bind_ctx *b = priv;
	struct drm_gpuva *va = op->remap.unmap->va;
	struct pvrsgx_va *prev = NULL, *next = NULL;
	u64 addr, range;

	drm_gpuva_op_remap_to_unmap_range(&op->remap, &addr, &range);
	pvrsgx_mmu_unmap(b->vm->mmu, addr, range);
	if (op->remap.prev)
		prev = bind_take_va(b);
	if (op->remap.next)
		next = bind_take_va(b);
	drm_gpuva_remap(prev ? &prev->base : NULL, next ? &next->base : NULL, &op->remap);
	if (prev)
		drm_gpuva_link(&prev->base, va->vm_bo);
	if (next)
		drm_gpuva_link(&next->base, va->vm_bo);
	va_unlink_locked(va, b->freed);
	return 0;
}

static int bind_step_unmap(struct drm_gpuva_op *op, void *priv)
{
	struct bind_ctx *b = priv;
	struct drm_gpuva *va = op->unmap.va;

	pvrsgx_mmu_unmap(b->vm->mmu, va->va.addr, va->va.range);
	drm_gpuva_unmap(&op->unmap);
	va_unlink_locked(va, b->freed);
	return 0;
}

static void pvrsgx_gpuvm_free(struct drm_gpuvm *gpuvm)
{
	kfree(container_of(gpuvm, struct pvrsgx_vm, gpuvm));
}

static const struct drm_gpuvm_ops pvrsgx_gpuvm_ops = {
	.vm_free = pvrsgx_gpuvm_free,
	.sm_step_map = bind_step_map,
	.sm_step_remap = bind_step_remap,
	.sm_step_unmap = bind_step_unmap,
};

static bool ranges_overlap(u64 a, u64 as, u64 b, u64 bs)
{
	return a < b + bs && b < a + as;
}

/* The op's own arguments, and its object for a map. */
static int bind_op_check(struct drm_file *file, struct pvrsgx_vm *vm, struct bind_op *o)
{
	const struct drm_powervr_sgx_vm_bind_op *in = &o->in;
	u64 end;
	int heap, err;

	if (in->flags || in->_padding_24 || !in->size || !PAGE_ALIGNED(in->size) ||
	    !PAGE_ALIGNED(in->device_addr) || check_add_overflow(in->device_addr, in->size, &end) ||
	    end > SZ_4G)
		return -EINVAL;
	if (in->op == DRM_POWERVR_SGX_VM_BIND_OP_UNMAP)
		return in->handle || in->bo_offset ? -EINVAL : 0;
	if (in->op != DRM_POWERVR_SGX_VM_BIND_OP_MAP || !PAGE_ALIGNED(in->bo_offset))
		return -EINVAL;
	heap = pvrsgx_heap_find(in->device_addr, in->size);
	if (heap < 0)
		return -EINVAL;

	o->obj = drm_gem_object_lookup(file, in->handle);
	if (!o->obj)
		return -ENOENT;
	if (check_add_overflow(in->bo_offset, in->size, &end) || end > o->obj->size)
		return -EINVAL;
	err = pvrsgx_validate_vm_map(vm->sdev, vm, heap, to_pvrsgx_bo(o->obj), in->bo_offset,
				     in->size);
	if (err)
		return err;
	o->sgt = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(o->obj));
	if (IS_ERR(o->sgt)) {
		err = PTR_ERR(o->sgt);
		o->sgt = NULL;
		return err;
	}
	if (drm_gem_is_imported(o->obj) && !pvrsgx_prime_reachable(o->sgt))
		return -EFAULT;
	return 0;
}

/*
 * With vm->lock and every object involved locked: check each op against the
 * VM as it is (the ops don't overlap, so none depends on another) and allocate
 * what applying it takes.
 */
static int bind_prepare(struct pvrsgx_vm *vm, struct bind_op *ops, u32 n)
{
	for (u32 i = 0; i < n; i++) {
		struct bind_op *o = &ops[i];
		u64 addr = o->in.device_addr, size = o->in.size;
		int nva = o->in.op == DRM_POWERVR_SGX_VM_BIND_OP_MAP ? 1 : 2;
		int err;

		if (o->in.op == DRM_POWERVR_SGX_VM_BIND_OP_MAP) {
			if (drm_gpuva_find_first(&vm->gpuvm, addr, size))
				return -EEXIST;
			err = pvrsgx_validate_vm_link(to_pvrsgx_bo(o->obj), addr, o->in.bo_offset);
			if (err)
				return err;
			err = pvrsgx_mmu_reserve(vm->mmu, addr, size);
			if (err)
				return err;
			if (!o->vm_bo) {
				o->vm_bo = drm_gpuvm_bo_obtain_locked(&vm->gpuvm, o->obj);
				if (IS_ERR(o->vm_bo)) {
					err = PTR_ERR(o->vm_bo);
					o->vm_bo = NULL;
					return err;
				}
			}
		}
		for (int k = 0; k < nva; k++) {
			if (!o->va[k])
				o->va[k] = kzalloc_obj(*o->va[k]);
			if (!o->va[k])
				return -ENOMEM;
		}
	}
	return 0;
}

static void bind_apply(struct pvrsgx_vm *vm, struct bind_op *ops, u32 n, struct list_head *freed)
{
	struct bind_ctx b = { .vm = vm, .freed = freed };

	for (u32 i = 0; i < n; i++) {
		struct bind_op *o = &ops[i];

		b.op = o;
		b.next_va = 0;
		if (o->in.op == DRM_POWERVR_SGX_VM_BIND_OP_MAP) {
			struct drm_gpuvm_map_req req = {
				.map.va.addr = o->in.device_addr,
				.map.va.range = o->in.size,
				.map.gem.obj = o->obj,
				.map.gem.offset = o->in.bo_offset,
			};

			WARN_ON(drm_gpuvm_sm_map(&vm->gpuvm, &b, &req));
		} else {
			WARN_ON(drm_gpuvm_sm_unmap(&vm->gpuvm, &b, o->in.device_addr, o->in.size));
		}
	}
}

/* Wait, interruptibly, for every wait op; find every signal op's syncobj. */
static int bind_syncs(struct drm_file *file, const struct drm_powervr_sgx_obj_array *arr,
		      struct pvrsgx_signal **signals, u32 *nsignals)
{
	struct drm_powervr_sgx_sync_op *ops;
	int err = 0;

	ops = pvrsgx_obj_array_get(arr, sizeof(*ops), PVRSGX_MAX_BIND_SYNCS);
	if (IS_ERR_OR_NULL(ops))
		return PTR_ERR_OR_ZERO(ops);
	*signals = kvcalloc(arr->count, sizeof(**signals), GFP_KERNEL);
	if (!*signals) {
		err = -ENOMEM;
		goto out;
	}
	for (u32 i = 0; i < arr->count && !err; i++) {
		u32 type = ops[i].flags & DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK;
		bool timeline = type == DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ;
		struct pvrsgx_signal *sig = &(*signals)[*nsignals];
		struct dma_fence *fence;

		if ((ops[i].flags & ~PVRSGX_SYNC_OP_FLAGS_MASK) ||
		    (type != DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ && !timeline) ||
		    (!timeline && ops[i].value)) {
			err = -EINVAL;
			break;
		}
		if (!(ops[i].flags & DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL)) {
			err = drm_syncobj_find_fence(file, ops[i].handle, ops[i].value, 0, &fence);
			if (!err) {
				long ret = dma_fence_wait(fence, true);

				err = ret < 0 ? ret : 0;
				dma_fence_put(fence);
			}
			continue;
		}
		sig->syncobj = drm_syncobj_find(file, ops[i].handle);
		if (!sig->syncobj) {
			err = -ENOENT;
			break;
		}
		sig->point = ops[i].value;
		if (timeline) {
			sig->chain = dma_fence_chain_alloc();
			if (!sig->chain)
				err = -ENOMEM;
		}
		(*nsignals)++;
	}
out:
	kvfree(ops);
	return err;
}

static void bind_signals_fini(struct pvrsgx_signal *signals, u32 n, bool fire)
{
	for (u32 i = 0; i < n; i++) {
		struct pvrsgx_signal *sig = &signals[i];

		if (fire) {
			struct dma_fence *stub = dma_fence_get_stub();

			if (sig->chain) {
				drm_syncobj_add_point(sig->syncobj, sig->chain, stub, sig->point);
				sig->chain = NULL;
			} else {
				drm_syncobj_replace_fence(sig->syncobj, stub);
			}
			dma_fence_put(stub);
		}
		dma_fence_chain_free(sig->chain);
		drm_syncobj_put(sig->syncobj);
	}
	kvfree(signals);
}

int pvrsgx_ioctl_vm_bind(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_vm_bind_args *args = data;
	struct pvrsgx_signal *signals = NULL;
	struct pvrsgx_vm_free *f = NULL;
	struct bind_op *ops = NULL;
	struct drm_powervr_sgx_vm_bind_op *in;
	struct pvrsgx_vm *vm;
	struct drm_exec exec;
	u32 n = args->ops.count, nsignals = 0;
	int err;

	if (args->flags != DRM_POWERVR_SGX_VM_BIND_FLAG_SYNC || !n)
		return -EINVAL;

	vm = pvrsgx_vm_lookup(to_pvrsgx_file(file), args->vm_context_handle);
	if (!vm)
		return -ENOENT;
	in = pvrsgx_obj_array_get(&args->ops, sizeof(*in), PVRSGX_MAX_BIND_OPS);
	if (IS_ERR(in)) {
		err = PTR_ERR(in);
		goto out_vm;
	}
	ops = kvcalloc(n, sizeof(*ops), GFP_KERNEL);
	f = vm_free_alloc();
	if (!ops || !f) {
		err = -ENOMEM;
		goto out;
	}
	for (u32 i = 0; i < n; i++) {
		ops[i].in = in[i];
		err = bind_op_check(file, vm, &ops[i]);
		if (err)
			goto out;
		for (u32 k = 0; k < i; k++) {
			if (ranges_overlap(in[i].device_addr, in[i].size, in[k].device_addr,
					   in[k].size)) {
				err = -EINVAL;
				goto out;
			}
		}
	}
	err = bind_syncs(file, &args->sync_ops, &signals, &nsignals);
	if (err)
		goto out;

	mutex_lock(&vm->lock);
	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT | DRM_EXEC_IGNORE_DUPLICATES, 0);
	drm_exec_until_all_locked(&exec) {
		err = 0;
		for (u32 i = 0; i < n && !err; i++) {
			struct drm_gpuva *va;

			if (ops[i].obj) {
				err = drm_exec_lock_obj(&exec, ops[i].obj);
				drm_exec_retry_on_contention(&exec);
				continue;
			}
			drm_gpuvm_for_each_va_range(va, &vm->gpuvm, in[i].device_addr,
						    in[i].device_addr + in[i].size) {
				err = drm_exec_lock_obj(&exec, va->gem.obj);
				drm_exec_retry_on_contention(&exec);
				if (err)
					break;
			}
		}
	}
	err = err ?: bind_prepare(vm, ops, n);
	if (!err)
		bind_apply(vm, ops, n, &f->vas);
	drm_exec_fini(&exec);
	mutex_unlock(&vm->lock);
	if (err)
		goto out;

	vm_invalidate(vm->sdev, false);
	/* The objects unmapped stay until every job submitted before now is done. */
	if (!list_empty(&f->vas)) {
		vm_free_after_jobs(vm, f);
		f = NULL;
	}

out:
	bind_signals_fini(signals, nsignals, !err);
	if (ops) {
		for (u32 i = 0; i < n; i++) {
			kfree(ops[i].va[0]);
			kfree(ops[i].va[1]);
			if (ops[i].vm_bo) {
				dma_resv_lock(ops[i].obj->resv, NULL);
				drm_gpuvm_bo_put(ops[i].vm_bo);
				dma_resv_unlock(ops[i].obj->resv);
			}
			if (ops[i].obj)
				drm_gem_object_put(ops[i].obj);
		}
		kvfree(ops);
	}
	kfree(f);
	kvfree(in);
out_vm:
	pvrsgx_vm_put(vm);
	return err;
}
