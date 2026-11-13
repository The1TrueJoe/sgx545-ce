// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

#include <drm/drm_gem.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>

#include <linux/dma-buf.h>
#include <linux/pagemap.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_drv.h"
#include "pvrsgx_gem.h"

/* The GPU has 32-bit physical addresses and a 4 GiB address space. */
#define PVRSGX_BO_MAX_SIZE	SZ_1G

/* A code object's contents are validated: no other way to reach them. */
static struct dma_buf *pvrsgx_gem_export(struct drm_gem_object *obj, int flags)
{
	if (to_pvrsgx_bo(obj)->flags & DRM_POWERVR_SGX_BO_CODE)
		return ERR_PTR(-EPERM);
	return drm_gem_prime_export(obj, flags);
}

/* Every CPU mapping, the dma-buf's included, needs ALLOW_CPU_ACCESS. */
static int pvrsgx_gem_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	if (!(to_pvrsgx_bo(obj)->flags & DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS))
		return -EINVAL;
	return drm_gem_shmem_object_mmap(obj, vma);
}

static void pvrsgx_gem_free(struct drm_gem_object *obj)
{
	struct pvrsgx_bo *bo = to_pvrsgx_bo(obj);

	if (drm_gem_is_imported(obj) && !iosys_map_is_null(&bo->import_map))
		dma_buf_vunmap_unlocked(obj->import_attach->dmabuf, &bo->import_map);
	drm_gem_shmem_object_free(obj);
}

/*
 * For fdinfo's memory stats: resident once it has pages (an import, its
 * exporter's). Nothing is purgeable: the uAPI has no madvise. Active, the core
 * reads from the reservation, where jobs' fences are only bookkeeping.
 */
static enum drm_gem_object_status pvrsgx_gem_status(struct drm_gem_object *obj)
{
	if (drm_gem_is_imported(obj) || READ_ONCE(to_drm_gem_shmem_obj(obj)->pages))
		return DRM_GEM_OBJECT_RESIDENT;
	return 0;
}

static size_t pvrsgx_gem_rss(struct drm_gem_object *obj)
{
	return pvrsgx_gem_status(obj) & DRM_GEM_OBJECT_RESIDENT ? obj->size : 0;
}

static const struct drm_gem_object_funcs pvrsgx_gem_funcs = {
	.free = pvrsgx_gem_free,
	.export = pvrsgx_gem_export,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = pvrsgx_gem_mmap,
	.status = pvrsgx_gem_status,
	.rss = pvrsgx_gem_rss,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

struct drm_gem_object *pvrsgx_gem_create_object(struct drm_device *drm, size_t size)
{
	struct pvrsgx_bo *bo = kzalloc_obj(*bo);

	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.base.funcs = &pvrsgx_gem_funcs;
	/* The GPU doesn't snoop the CPU caches. */
	bo->base.map_wc = true;
	return &bo->base.base;
}

int pvrsgx_ioctl_create_bo(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_create_bo_args *args = data;
	struct drm_gem_shmem_object *shmem;
	struct address_space *m;
	int err;

	if (args->_padding_c || (args->flags & ~PVRSGX_BO_FLAGS_MASK) ||
	    !args->size || !PAGE_ALIGNED(args->size) || args->size > PVRSGX_BO_MAX_SIZE)
		return -EINVAL;
	if ((args->flags & DRM_POWERVR_SGX_BO_CODE) &&
	    (args->flags & DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS))
		return -EINVAL;

	shmem = drm_gem_shmem_create(drm, args->size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	to_pvrsgx_bo(&shmem->base)->flags = args->flags;
	/* Pages the GPU can address; shmem's RETRY_MAYFAIL and NOWARN stay. */
	m = shmem->base.filp->f_mapping;
	mapping_set_gfp_mask(m, (mapping_gfp_mask(m) & ~__GFP_HIGHMEM) | __GFP_DMA32);
	/*
	 * Without x86's write-combined linear map the zeroing's lines stay dirty:
	 * clean them now, before any write through a write-combined mapping.
	 */
	if (!IS_ENABLED(CONFIG_X86)) {
		struct sg_table *sgt = drm_gem_shmem_get_pages_sgt(shmem);

		if (IS_ERR(sgt)) {
			drm_gem_object_put(&shmem->base);
			return PTR_ERR(sgt);
		}
	}

	err = drm_gem_handle_create(file, &shmem->base, &args->handle);
	drm_gem_object_put(&shmem->base);
	return err;
}

int pvrsgx_ioctl_get_bo_mmap_offset(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args *args = data;
	struct drm_gem_object *obj;
	int err;

	if (args->_padding_4)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	if (!(to_pvrsgx_bo(obj)->flags & DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS)) {
		err = -EINVAL;
		goto out;
	}
	err = drm_gem_create_mmap_offset(obj);
	if (!err)
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
out:
	drm_gem_object_put(obj);
	return err;
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_gem_test.c"
#endif
