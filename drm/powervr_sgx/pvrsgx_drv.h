/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_DRV_H
#define PVRSGX_DRV_H

#include <linux/types.h>

#define PVRSGX_DRIVER_NAME	"powervr_sgx"
#define PVRSGX_DRIVER_DESC	"PowerVR SGX5xx"
#define PVRSGX_DRIVER_MAJOR	0
/* The uAPI revision: 3, per-command structs (docs/uapi/rev3-commands.md). */
#define PVRSGX_DRIVER_MINOR	3

#define PVRSGX_BO_FLAGS_MASK		(DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS | \
					 DRM_POWERVR_SGX_BO_CODE)
#define PVRSGX_SYNC_OP_FLAGS_MASK	(DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK | \
					 DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL)

/* The registers this driver touches all lie below this offset. */
#define PVRSGX_REGS_MIN			0x4000

struct device;
struct dev_pm_ops;
struct drm_powervr_sgx_obj_array;
struct pvrsgx_device;

struct pvrsgx_device *pvrsgx_alloc(struct device *dev);
int pvrsgx_probe_common(struct pvrsgx_device *sdev);
void pvrsgx_remove_common(struct pvrsgx_device *sdev);

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_GMA500)
int pvrsgx_aux_register(void);
void pvrsgx_aux_unregister(void);
#else
static inline int pvrsgx_aux_register(void) { return 0; }
static inline void pvrsgx_aux_unregister(void) { }
#endif

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_OF)
int pvrsgx_of_register(void);
void pvrsgx_of_unregister(void);
#else
static inline int pvrsgx_of_register(void) { return 0; }
static inline void pvrsgx_of_unregister(void) { }
#endif

void *pvrsgx_obj_array_get(const struct drm_powervr_sgx_obj_array *in, u32 obj_size,
			   u32 max_count);
int pvrsgx_obj_array_put(const struct drm_powervr_sgx_obj_array *out, const void *src,
			 u32 obj_size, u32 count);

#endif /* PVRSGX_DRV_H */
