/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_DRV_H
#define PVRSGX_DRV_H

#include <linux/types.h>

#define PVRSGX_DRIVER_NAME	"powervr_sgx"
#define PVRSGX_DRIVER_DESC	"PowerVR SGX545"
#define PVRSGX_DRIVER_MAJOR	0
#define PVRSGX_DRIVER_MINOR	1

#define PVRSGX_BO_FLAGS_MASK		(DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS | DRM_POWERVR_SGX_BO_CODE)
#define PVRSGX_SYNC_OP_FLAGS_MASK	(DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK | \
					 DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL)

struct drm_powervr_sgx_obj_array;

void *pvrsgx_obj_array_get(const struct drm_powervr_sgx_obj_array *in, u32 obj_size,
			   u32 max_count);
int pvrsgx_obj_array_put(const struct drm_powervr_sgx_obj_array *out, const void *src,
			 u32 obj_size, u32 count);

#endif /* PVRSGX_DRV_H */
