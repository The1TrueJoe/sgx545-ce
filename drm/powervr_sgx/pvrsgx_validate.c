// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Shader validation: what a client's USSE code may contain. The threat model
 * is in Documentation/gpu/powervr_sgx.rst, "Shader validation".
 *
 * Client USSE code runs only from the two USSE heaps of its own VM (the code
 * base registers cover exactly those), the USSE heaps take only code objects,
 * and a code object's contents arrive only through UPLOAD_CODE, which checks
 * every instruction against the form table generated from the isaspec
 * description: its form, what the form does, and each operand's register file
 * and number.
 */

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gpuvm.h>
#include <drm/drm_print.h>

#include <linux/dma-resv.h>
#include <linux/iosys-map.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_core.h"
#include "pvrsgx_device.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_gem.h"
#include "pvrsgx_usse.h"
#include "pvrsgx_validate.h"

static bool bo_is_code(struct pvrsgx_bo *bo)
{
	return bo->flags & DRM_POWERVR_SGX_BO_CODE;
}

static bool heap_is_usse(unsigned int heap)
{
	return heap == DRM_POWERVR_SGX_HEAP_USSE_PIXEL || heap == DRM_POWERVR_SGX_HEAP_USSE_VERTEX;
}

/* Code objects, and only they, go into the USSE heaps, read-only. */
int pvrsgx_validate_vm_map(struct pvrsgx_device *sdev, struct pvrsgx_vm *vm,
			   unsigned int heap, struct pvrsgx_bo *bo, u64 offset, u64 size)
{
	return heap_is_usse(heap) == bo_is_code(bo) ? 0 : -EINVAL;
}

static_assert(PVRSGX_USSE_WINDOW_INSTS * 8 == 1u << PVRSGX_USSE_WINDOW_LOG2);

/*
 * Where a mapping at addr of the object from offset puts the object's start,
 * in instructions from the start of its code base window: what phas.i's
 * targets count from when the task's PDS program names the window its first
 * word is in. One window per object makes that base the same for every entry.
 */
static s64 code_place(u64 addr, u64 offset)
{
	u64 base = addr >= PVRSGX_HEAP_USSE_VERTEX ? PVRSGX_HEAP_USSE_VERTEX :
						    PVRSGX_HEAP_USSE_PIXEL;
	s64 start = (s64)(addr - base) - (s64)offset;

	if (start < 0)
		return PVRSGX_USSE_PLACE_NONE;
	return (start / 8) & (PVRSGX_USSE_WINDOW_INSTS - 1);
}

/*
 * Once code with absolute targets is in an object, every mapping of it puts it
 * where those targets were checked. Called with the object's reservation held.
 */
int pvrsgx_validate_vm_link(struct pvrsgx_bo *bo, u64 addr, u64 offset)
{
	if (!bo_is_code(bo) || !bo->code_abs)
		return 0;
	return code_place(addr, offset) == bo->code_place ? 0 : -EINVAL;
}

/* The one place every mapping of obj puts it, or PVRSGX_USSE_PLACE_NONE. */
static s64 code_place_mapped(struct drm_gem_object *obj)
{
	struct drm_gpuvm_bo *vm_bo;
	struct drm_gpuva *va;
	s64 place = PVRSGX_USSE_PLACE_NONE;

	drm_gem_for_each_gpuvm_bo(vm_bo, obj) {
		drm_gpuvm_bo_for_each_va(va, vm_bo) {
			s64 p = code_place(va->va.addr, va->gem.offset);

			if (place != PVRSGX_USSE_PLACE_NONE && p != place)
				return PVRSGX_USSE_PLACE_NONE;
			place = p;
		}
	}
	return place;
}

u32 pvrsgx_validate_pte_flags(struct pvrsgx_bo *bo)
{
	return bo_is_code(bo) ? SGX_MMU_PTE_READONLY : 0;
}

int pvrsgx_validate_job(struct pvrsgx_device *sdev, struct pvrsgx_job *job)
{
	/* A job can only reach validated USSE code: nothing to check per job. */
	return 0;
}

int pvrsgx_ioctl_upload_code(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_upload_code_args *args = data;
	struct pvrsgx_device *sdev = to_pvrsgx_device(drm);
	struct drm_gem_object *obj;
	struct pvrsgx_bo *bo;
	struct iosys_map map;
	u64 end, *buf;
	enum pvrsgx_usse_refusal why;
	s64 bad, place;
	bool abs = false;
	int err;

	args->bad_offset = ~0ull;
	if (args->flags || !args->size || !IS_ALIGNED(args->size, 8) ||
	    !IS_ALIGNED(args->offset, 8) || check_add_overflow(args->offset, args->size, &end))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	bo = to_pvrsgx_bo(obj);
	if (!bo_is_code(bo) || end > obj->size) {
		err = -EINVAL;
		goto out_obj;
	}

	buf = kvmalloc(args->size, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto out_obj;
	}
	if (copy_from_user(buf, u64_to_user_ptr(args->pointer), args->size)) {
		err = -EFAULT;
		goto out_buf;
	}
	/* VM_BIND links a mapping under the same lock. */
	err = dma_resv_lock_interruptible(obj->resv, NULL);
	if (err)
		goto out_buf;
	if (!list_empty(&obj->gpuva.list) && args->offset < bo->code_end) {
		drm_dbg_driver(drm, "UPLOAD_CODE: 0x%llx is below mapped code's end 0x%llx\n",
			       args->offset, bo->code_end);
		err = -EBUSY;
		goto out_unlock;
	}
	place = code_place_mapped(obj);
	bad = pvrsgx_usse_scan(sdev->family->isa, buf, args->size / 8, args->offset / 8,
			       obj->size / 8, place, &abs, &why);
	if (bad >= 0) {
		args->bad_offset = args->offset + bad * 8;
		drm_dbg_driver(drm, "UPLOAD_CODE: word at 0x%llx refused: %s\n",
			       args->bad_offset, pvrsgx_usse_refusal_name(why));
		err = -EINVAL;
		goto out_unlock;
	}
	err = drm_gem_shmem_vmap_locked(&bo->base, &map);
	if (err)
		goto out_unlock;
	iosys_map_memcpy_to(&map, args->offset, buf, args->size);
	drm_gem_shmem_vunmap_locked(&bo->base, &map);
	bo->code_end = max(bo->code_end, end);
	if (abs) {
		bo->code_abs = true;
		bo->code_place = place;
	}

out_unlock:
	dma_resv_unlock(obj->resv);
out_buf:
	kvfree(buf);
out_obj:
	drm_gem_object_put(obj);
	return err;
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_validate_test.c"
#endif
