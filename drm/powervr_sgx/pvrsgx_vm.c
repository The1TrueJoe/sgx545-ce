// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * VM contexts on drm_gpuvm. The client picks every address inside the heaps
 * DEV_QUERY reports (docs/UAPI.md section 5); a mapping is one GEM range at one
 * address, never split or merged, so VM_UNMAP takes back exactly one VM_MAP.
 */

#include <drm/drm_gem.h>
#include <drm/drm_print.h>

#include <linux/dma-resv.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_gem.h"
#include "pvrsgx_mmu.h"
#include "pvrsgx_validate.h"
#include "pvrsgx_vm.h"

#define SZ_4G_ULL (1ULL << 32)

/*
 * The DDK microkernel handles a TA kick under the job's directory: it moves the
 * EDM's BIF_BANK0 index onto the list it loads with the context's directory,
 * then fetches its own code and reads its control blocks through it (EA1,
 * 2026-11-03, docs/DRM-DRIVER.md section 4). So with it, every VM must map the
 * firmware's heaps, which gives a client's shaders the reach into kernel memory
 * that P2 and P3 describe (docs/UAPI.md 7.2). The open microkernel keeps the
 * EDM on its own list and needs none of this.
 */
static int share_kernel_heaps = -1;
module_param(share_kernel_heaps, int, 0444);
MODULE_PARM_DESC(share_kernel_heaps,
		 "Map the firmware's heaps in every VM: -1 when the DDK microkernel runs (default), 0 never, 1 always");

bool pvrsgx_vm_shares_kernel_heaps(struct pvrsgx_device *sdev)
{
	return share_kernel_heaps < 0 ? sdev->fw.ddk : share_kernel_heaps;
}

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
		.size = SZ_4G_ULL - PVRSGX_HEAP_TEXTURE,
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

/* Drop one mapping: page tables first, then the object, once nothing can use it. */
static void va_release(struct pvrsgx_vm *vm, struct drm_gpuva *va)
{
	struct drm_gem_object *obj = va->gem.obj;

	drm_gem_object_get(obj);
	dma_resv_lock(obj->resv, NULL);
	drm_gpuva_unlink(va);
	dma_resv_unlock(obj->resv);
	drm_gem_object_put(obj);
	kfree(va);
}

static void vm_release(struct kref *ref)
{
	struct pvrsgx_vm *vm = container_of(ref, struct pvrsgx_vm, ref);
	struct drm_gpuva *va, *next;

	/* No job holds this VM any more, so nothing on the GPU uses it. */
	drm_gpuvm_for_each_va_safe(va, next, &vm->gpuvm) {
		pvrsgx_mmu_unmap(vm->mmu, va->va.addr, va->va.range);
		drm_gpuva_remove(va);
		va_release(vm, va);
	}
	drm_mm_takedown(&vm->params);
	pvrsgx_mmu_destroy(vm->mmu);
	mutex_destroy(&vm->lock);
	drm_gpuvm_put(&vm->gpuvm);
}

void pvrsgx_vm_put(struct pvrsgx_vm *vm)
{
	if (vm)
		kref_put(&vm->ref, vm_release);
}

static void pvrsgx_gpuvm_free(struct drm_gpuvm *gpuvm)
{
	kfree(container_of(gpuvm, struct pvrsgx_vm, gpuvm));
}

static const struct drm_gpuvm_ops pvrsgx_gpuvm_ops = {
	.vm_free = pvrsgx_gpuvm_free,
};

static struct pvrsgx_vm *vm_create(struct pvrsgx_device *sdev)
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
	if (pvrsgx_vm_shares_kernel_heaps(sdev)) {
		for (int i = 0; i < ARRAY_SIZE(pvrsgx_kernel_heaps); i++)
			pvrsgx_mmu_share(vm->mmu, sdev->kernel_mmu, pvrsgx_kernel_heaps[i].start,
					 pvrsgx_kernel_heaps[i].end);
	}
	drm_gpuvm_init(&vm->gpuvm, "powervr_sgx", 0, &sdev->base, r_obj,
		       0, SZ_4G_ULL, 0, 0, &pvrsgx_gpuvm_ops);
	drm_gem_object_put(r_obj);
	vm->sdev = sdev;
	drm_mm_init(&vm->params, PVRSGX_HEAP_3D_PARAMS, PVRSGX_HEAP_TA_DATA - PVRSGX_HEAP_3D_PARAMS);
	mutex_init(&vm->lock);
	kref_init(&vm->ref);
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

	vm = vm_create(to_pvrsgx_device(drm));
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
		return -EINVAL;
	/* Contexts and jobs that use it keep it until they are done. */
	pvrsgx_vm_put(vm);
	return 0;
}

int pvrsgx_ioctl_vm_map(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_vm_map_args *args = data;
	struct pvrsgx_device *sdev = to_pvrsgx_device(drm);
	struct drm_gpuva_op_map op = {};
	struct drm_gpuvm_bo *vm_bo;
	struct drm_gem_object *obj;
	struct pvrsgx_vm *vm;
	struct drm_gpuva *va;
	struct sg_table *sgt;
	u64 obj_end;
	int heap, err;

	if (args->flags || args->_padding_14)
		return -EINVAL;
	if (!args->size || !PAGE_ALIGNED(args->size) || !PAGE_ALIGNED(args->offset) ||
	    !PAGE_ALIGNED(args->device_addr))
		return -EINVAL;
	heap = pvrsgx_heap_find(args->device_addr, args->size);
	if (heap < 0)
		return -EINVAL;

	vm = pvrsgx_vm_lookup(to_pvrsgx_file(file), args->vm_context_handle);
	if (!vm)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		err = -ENOENT;
		goto out_vm;
	}
	if (check_add_overflow(args->offset, args->size, &obj_end) || obj_end > obj->size) {
		err = -EINVAL;
		goto out_obj;
	}
	err = pvrsgx_validate_vm_map(sdev, vm, heap, to_pvrsgx_bo(obj), args->offset, args->size);
	if (err)
		goto out_obj;

	va = kzalloc_obj(*va);
	if (!va) {
		err = -ENOMEM;
		goto out_obj;
	}

	sgt = drm_gem_shmem_get_pages_sgt(to_drm_gem_shmem_obj(obj));
	if (IS_ERR(sgt)) {
		err = PTR_ERR(sgt);
		goto out_va;
	}

	mutex_lock(&vm->lock);
	if (drm_gpuva_find_first(&vm->gpuvm, args->device_addr, args->size)) {
		err = -EEXIST;
		goto out_unlock;
	}
	err = pvrsgx_mmu_map_sgt(vm->mmu, args->device_addr, sgt, args->offset, args->size, 0);
	if (err)
		goto out_unlock;

	dma_resv_lock(obj->resv, NULL);
	vm_bo = drm_gpuvm_bo_obtain_locked(&vm->gpuvm, obj);
	if (IS_ERR(vm_bo)) {
		dma_resv_unlock(obj->resv);
		pvrsgx_mmu_unmap(vm->mmu, args->device_addr, args->size);
		err = PTR_ERR(vm_bo);
		goto out_unlock;
	}
	op.va.addr = args->device_addr;
	op.va.range = args->size;
	op.gem.obj = obj;
	op.gem.offset = args->offset;
	drm_gpuva_map(&vm->gpuvm, va, &op);
	drm_gpuva_link(va, vm_bo);
	dma_resv_unlock(obj->resv);
	drm_gpuvm_bo_put(vm_bo);
	va = NULL;

	/* A walk of this range while it was empty may be cached: drop it. */
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD;
	mutex_unlock(&sdev->ccb_lock);

out_unlock:
	mutex_unlock(&vm->lock);
out_va:
	kfree(va);
out_obj:
	drm_gem_object_put(obj);
out_vm:
	pvrsgx_vm_put(vm);
	return err;
}

int pvrsgx_ioctl_vm_unmap(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_vm_unmap_args *args = data;
	struct pvrsgx_device *sdev = to_pvrsgx_device(drm);
	struct pvrsgx_vm *vm;
	struct drm_gpuva *va;
	long ret;

	if (args->_padding_4)
		return -EINVAL;

	vm = pvrsgx_vm_lookup(to_pvrsgx_file(file), args->vm_context_handle);
	if (!vm)
		return -EINVAL;

	mutex_lock(&vm->lock);
	va = drm_gpuva_find(&vm->gpuvm, args->device_addr, args->size);
	if (!va) {
		mutex_unlock(&vm->lock);
		pvrsgx_vm_put(vm);
		return -ENOENT;
	}
	pvrsgx_mmu_unmap(vm->mmu, va->va.addr, va->va.range);
	drm_gpuva_remove(va);
	mutex_unlock(&vm->lock);

	/* The next command invalidates the BIF's cached translations. */
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_BIF_PT | PVRSGX_CC_INVAL_BIF_PD;
	mutex_unlock(&sdev->ccb_lock);

	/* Keep the pages until every job submitted before the unmap is done. */
	ret = dma_resv_wait_timeout(pvrsgx_vm_resv(vm), DMA_RESV_USAGE_BOOKKEEP,
				    false, MAX_SCHEDULE_TIMEOUT);
	if (ret <= 0)
		drm_warn(drm, "VM_UNMAP: wait for jobs failed (%ld)\n", ret);
	va_release(vm, va);
	pvrsgx_vm_put(vm);
	return 0;
}
