/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_GEM_H
#define PVRSGX_GEM_H

#include <drm/drm_gem_shmem_helper.h>

#include <linux/types.h>

struct drm_device;
struct drm_file;

struct pvrsgx_bo {
	struct drm_gem_shmem_object base;
	u64 flags;
	/** @code_end: A code object's end of the furthest upload (under the resv lock). */
	u64 code_end;
	/**
	 * @code_abs: The code has absolute targets, checked for the object's start
	 * at @code_place instructions from its USSE heap's base (resv lock).
	 */
	bool code_abs;
	s64 code_place;
};

static inline struct pvrsgx_bo *to_pvrsgx_bo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct pvrsgx_bo, base);
}

struct drm_gem_object *pvrsgx_gem_create_object(struct drm_device *drm, size_t size);
int pvrsgx_ioctl_create_bo(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_get_bo_mmap_offset(struct drm_device *drm, void *data, struct drm_file *file);

#endif /* PVRSGX_GEM_H */
