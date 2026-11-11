// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Shader validation, decision D9: a client's USSE program can read registers
 * with ldr and must not be able to; client stores to registers are refused
 * too until it is known why the three tested ones were dropped
 * (docs/UAPI.md section 7.2). The design is in docs/DRM-DRIVER.md section 6.
 *
 * Client USSE code can run only from the two USSE heaps of its own VM (the
 * code-base registers cover exactly those), the USSE heaps take only code
 * objects, and a code object's contents arrive only through UPLOAD_CODE,
 * which scans every instruction against the form table generated from the
 * clean-room ISA description.
 */

#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_print.h>

#include <linux/dma-resv.h>
#include <linux/iosys-map.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_core.h"
#include "pvrsgx_device.h"
#include "pvrsgx_gem.h"
#include "pvrsgx_validate.h"

enum pvrsgx_usse_kind {
	PVRSGX_USSE_OK,
	PVRSGX_USSE_LDR,
	PVRSGX_USSE_STR,
	PVRSGX_USSE_BR,
	PVRSGX_USSE_SETL,
	PVRSGX_USSE_SAVL,
};

struct pvrsgx_usse_form {
	u64 mask, match;
	u8 kind;
};

#include "pvrsgx_usse_table.h"

/* The forms of each instruction set (pvrsgx_core.h). */
static const struct {
	const struct pvrsgx_usse_form *forms;
	u32 n;
} isas[] = {
	[PVRSGX_ISA_SGX545] = { pvrsgx_usse_forms, ARRAY_SIZE(pvrsgx_usse_forms) },
};

/* br's target: l0-17, signed, in instructions from the branch. */
#define USSE_BR_OFFSET		GENMASK_ULL(17, 0)

/*
 * Check n instructions that will sit at instruction index first of an object
 * of obj_insts instructions. Returns the index (from w) of the first bad one,
 * or -1. Linear in n, no allocation.
 */
static s64 usse_scan(enum pvrsgx_isa isa, const u64 *w, u32 n, u32 first, u32 obj_insts)
{
	const struct pvrsgx_usse_form *forms = isas[isa].forms;

	for (u32 i = 0; i < n; i++) {
		const struct pvrsgx_usse_form *f = NULL;
		s64 target;

		for (u32 k = 0; k < isas[isa].n; k++) {
			if ((w[i] & forms[k].mask) == forms[k].match) {
				f = &forms[k];
				break;
			}
		}
		if (!f)
			return i;
		switch (f->kind) {
		case PVRSGX_USSE_OK:
			break;
		case PVRSGX_USSE_BR:
			target = (s64)first + i + sign_extend64(w[i] & USSE_BR_OFFSET, 17);
			if (target < 0 || target >= obj_insts)
				return i;
			break;
		default:
			return i;
		}
	}
	return -1;
}

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
	s64 bad;
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
	bad = usse_scan(sdev->family->isa, buf, args->size / 8, args->offset / 8, obj->size / 8);
	if (bad >= 0) {
		args->bad_offset = args->offset + bad * 8;
		err = -EINVAL;
		goto out_buf;
	}

	/* VM_MAP links a mapping under the same lock. */
	err = dma_resv_lock_interruptible(obj->resv, NULL);
	if (err)
		goto out_buf;
	if (!list_empty(&obj->gpuva.list) && args->offset < bo->code_end) {
		err = -EBUSY;
		goto out_unlock;
	}
	err = drm_gem_shmem_vmap_locked(&bo->base, &map);
	if (err)
		goto out_unlock;
	iosys_map_memcpy_to(&map, args->offset, buf, args->size);
	drm_gem_shmem_vunmap_locked(&bo->base, &map);
	bo->code_end = max(bo->code_end, end);

out_unlock:
	dma_resv_unlock(obj->resv);
out_buf:
	kvfree(buf);
out_obj:
	drm_gem_object_put(obj);
	return err;
}
