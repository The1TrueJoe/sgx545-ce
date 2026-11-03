/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_VALIDATE_H
#define PVRSGX_VALIDATE_H

#include <linux/types.h>

struct pvrsgx_bo;
struct pvrsgx_device;
struct pvrsgx_job;
struct pvrsgx_vm;

struct drm_device;
struct drm_file;

/* The shader validator (decision D9, docs/DRM-DRIVER.md section 6). */
int pvrsgx_validate_vm_map(struct pvrsgx_device *sdev, struct pvrsgx_vm *vm,
			   unsigned int heap, struct pvrsgx_bo *bo, u64 offset, u64 size);
u32 pvrsgx_validate_pte_flags(struct pvrsgx_bo *bo);
int pvrsgx_validate_job(struct pvrsgx_device *sdev, struct pvrsgx_job *job);
int pvrsgx_ioctl_upload_code(struct drm_device *drm, void *data, struct drm_file *file);

#endif /* PVRSGX_VALIDATE_H */
