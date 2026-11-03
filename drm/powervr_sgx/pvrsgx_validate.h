/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_VALIDATE_H
#define PVRSGX_VALIDATE_H

#include <linux/types.h>

struct pvrsgx_bo;
struct pvrsgx_device;
struct pvrsgx_job;
struct pvrsgx_vm;

/*
 * Hooks for the shader validator (decision D9, docs/DRM-DRIVER.md section 6).
 * Every path by which client code can reach a USSE or PDS heap calls one of
 * these. They accept everything until the validator exists.
 */
int pvrsgx_validate_vm_map(struct pvrsgx_device *sdev, struct pvrsgx_vm *vm,
			   unsigned int heap, struct pvrsgx_bo *bo, u64 offset, u64 size);
int pvrsgx_validate_job(struct pvrsgx_device *sdev, struct pvrsgx_job *job);

#endif /* PVRSGX_VALIDATE_H */
