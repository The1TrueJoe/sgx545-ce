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
#include "pvrsgx_validate.h"

enum pvrsgx_usse_kind {
	USSE_OK,
	USSE_REFUSE,
	USSE_BR,
	USSE_PHAS,
};

enum pvrsgx_usse_slot_kind {
	USSE_SLOT_NONE,
	USSE_SLOT_DST,
	USSE_SLOT_DST1,
	USSE_SLOT_SRC,
	USSE_SLOT_SRC0,
};

#define USSE_MAX_SLOTS	4
#define USSE_MAX_PIECES	3
#define USSE_ALWAYS	0xff

/* An operand slot: its value is the pieces' bits, each shifted into place. */
struct pvrsgx_usse_slot {
	u8 kind;
	/* Under a half or C10 format, number bit 6 is a format flag. */
	bool mode;
	/* The slot exists only while this bit of the word is set. */
	u8 when;
	struct {
		u8 lo, width, shift;
	} p[USSE_MAX_PIECES];
};

struct pvrsgx_usse_form {
	u64 mask, match;
	u8 kind;
	u8 slots;
};

#include "pvrsgx_usse_table.h"

/* The forms of each instruction set (pvrsgx_core.h). */
static const struct {
	const struct pvrsgx_usse_form *forms;
	u32 n;
} isas[] = {
	[PVRSGX_ISA_SGX545] = { pvrsgx_usse_forms, ARRAY_SIZE(pvrsgx_usse_forms) },
};

/* The register files an operand's bank and extension bits select. */
enum usse_file {
	USSE_R,		/* temporaries */
	USSE_O,		/* outputs */
	USSE_PA,	/* primary attributes */
	USSE_SA,	/* secondary attributes */
	USSE_INDEXED,	/* through idx1 or idx2 */
	USSE_CG,	/* constants c0-63, then globals g0- */
	USSE_IDX,	/* the index registers */
	USSE_IMM,	/* an immediate */
	USSE_I,		/* internal registers */
};

/* By bank | extension << 2 (dst, src) or << 1 (src0, dst1). */
static const u8 dst_files[8] = {
	USSE_R, USSE_O, USSE_PA, USSE_INDEXED, USSE_SA, USSE_CG, USSE_IDX, USSE_I,
};

static const u8 src_files[8] = {
	USSE_R, USSE_O, USSE_PA, USSE_SA, USSE_INDEXED, USSE_CG, USSE_IMM, USSE_I,
};

static const u8 src0_files[4] = { USSE_R, USSE_PA, USSE_O, USSE_SA };

/* g46 on (number 110) is undocumented; i0-i3 and idx1-idx2 are what exists. */
#define USSE_CG_END		110
#define USSE_I_LAST		3

static bool usse_slot_ok(const struct pvrsgx_usse_slot *s, u64 w)
{
	u32 v = 0, n, file;

	if (s->when != USSE_ALWAYS && !(w & BIT_ULL(s->when)))
		return true;
	for (int i = 0; i < USSE_MAX_PIECES && s->p[i].width; i++)
		v |= ((w >> s->p[i].lo) & GENMASK_ULL(s->p[i].width - 1, 0)) << s->p[i].shift;
	n = v & 0x7f;
	switch (s->kind) {
	case USSE_SLOT_DST:
		file = dst_files[(v >> 7) & 7];
		break;
	case USSE_SLOT_SRC:
		file = src_files[(v >> 7) & 7];
		break;
	case USSE_SLOT_SRC0:
		file = src0_files[(v >> 7) & 3];
		break;
	case USSE_SLOT_DST1:
		file = (v >> 7) & 1 ? USSE_PA : USSE_R;
		break;
	default:
		return false;
	}
	if (s->mode)
		n &= 0x3f;
	switch (file) {
	case USSE_CG:
		/* Read only: what writing c or g does isn't known. */
		return s->kind != USSE_SLOT_DST && (v & 0x7f) < USSE_CG_END;
	case USSE_I:
		return n <= USSE_I_LAST;
	case USSE_IDX:
		return n == 1 || n == 2;
	default:
		return true;
	}
}

/* br's and phas.i's targets: l0-17, in instructions. */
#define USSE_TARGET		GENMASK_ULL(17, 0)

/* Not mapped, or mapped at more than one place. */
#define CODE_PLACE_NONE		S64_MIN

/*
 * Check n instructions that will sit at instruction index first of an object
 * of obj_insts instructions. place is the object's start, in instructions from
 * the base of the USSE heap it is mapped in, or CODE_PLACE_NONE: an
 * absolute target (phas.i) needs it, and *abs says whether there was one.
 * Returns the index (from w) of the first bad instruction, or -1. Linear in n,
 * no allocation.
 */
static s64 usse_scan(enum pvrsgx_isa isa, const u64 *w, u32 n, u32 first, u32 obj_insts,
		     s64 place, bool *abs)
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
		for (int k = 0; k < USSE_MAX_SLOTS; k++) {
			const struct pvrsgx_usse_slot *s = &pvrsgx_usse_slots[f->slots][k];

			if (s->kind != USSE_SLOT_NONE && !usse_slot_ok(s, w[i]))
				return i;
		}
		switch (f->kind) {
		case USSE_OK:
			break;
		case USSE_BR:
			target = (s64)first + i + sign_extend64(w[i] & USSE_TARGET, 17);
			if (target < 0 || target >= obj_insts)
				return i;
			break;
		case USSE_PHAS:
			/* 0: no next phase. Otherwise from the heap base. */
			target = w[i] & USSE_TARGET;
			if (!target)
				break;
			if (place == CODE_PLACE_NONE || target < place ||
			    target >= place + obj_insts)
				return i;
			*abs = true;
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

/*
 * Where a mapping at addr of the object from offset puts the object's start,
 * in instructions from its USSE heap's base: what phas.i's targets count from.
 */
static s64 code_place(u64 addr, u64 offset)
{
	u64 base = addr >= PVRSGX_HEAP_USSE_VERTEX ? PVRSGX_HEAP_USSE_VERTEX :
						    PVRSGX_HEAP_USSE_PIXEL;

	return ((s64)(addr - base) - (s64)offset) / 8;
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

/* The one place every mapping of obj puts it, or CODE_PLACE_NONE. */
static s64 code_place_mapped(struct drm_gem_object *obj)
{
	struct drm_gpuvm_bo *vm_bo;
	struct drm_gpuva *va;
	s64 place = CODE_PLACE_NONE;

	drm_gem_for_each_gpuvm_bo(vm_bo, obj) {
		drm_gpuvm_bo_for_each_va(va, vm_bo) {
			s64 p = code_place(va->va.addr, va->gem.offset);

			if (place != CODE_PLACE_NONE && p != place)
				return CODE_PLACE_NONE;
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
		err = -EBUSY;
		goto out_unlock;
	}
	place = code_place_mapped(obj);
	bad = usse_scan(sdev->family->isa, buf, args->size / 8, args->offset / 8, obj->size / 8,
			place, &abs);
	if (bad >= 0) {
		args->bad_offset = args->offset + bad * 8;
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
