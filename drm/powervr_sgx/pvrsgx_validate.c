// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Shader validation, decision D9: a client's USSE program can read registers
 * with ldr and must not be able to; client stores to registers are refused
 * too until it is known why the three tested ones were dropped
 * (docs/UAPI.md section 7.2). The design is in docs/DRM-DRIVER.md section 6.
 *
 * Today these hooks accept everything. That is not safe on a shared GPU, so
 * the job hook says so once.
 */

#include <drm/drm_print.h>

#include "pvrsgx_device.h"
#include "pvrsgx_validate.h"

int pvrsgx_validate_vm_map(struct pvrsgx_device *sdev, struct pvrsgx_vm *vm,
			   unsigned int heap, struct pvrsgx_bo *bo, u64 offset, u64 size)
{
	/*
	 * To come: a client-mapped object in a USSE or PDS heap must be one the
	 * kernel filled through the upload path (and validated), never one the
	 * client can still write through a CPU mapping or another VM.
	 */
	return 0;
}

int pvrsgx_validate_job(struct pvrsgx_device *sdev, struct pvrsgx_job *job)
{
	/*
	 * To come: every PDS program the job names (stream fields, VDM words)
	 * lies in a validated code object of the job's VM.
	 */
	drm_warn_once(&sdev->base, "shader validation (D9) not implemented: clients are not isolated\n");
	return 0;
}
