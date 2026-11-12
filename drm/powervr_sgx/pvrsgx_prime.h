/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_PRIME_H
#define PVRSGX_PRIME_H

#include <linux/types.h>

struct dma_buf_attachment;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_powervr_sgx_obj_array;
struct pvrsgx_job;
struct sg_table;

bool pvrsgx_prime_reachable(struct sg_table *sgt);
struct drm_gem_object *pvrsgx_prime_import_sg_table(struct drm_device *drm,
						    struct dma_buf_attachment *attach,
						    struct sg_table *sgt);
int pvrsgx_prime_ranges(struct drm_file *file, const struct drm_powervr_sgx_obj_array *arr,
			struct pvrsgx_job *job);
void pvrsgx_prime_flush(struct pvrsgx_job *job, bool after);
void pvrsgx_prime_put(struct pvrsgx_job *job);

#endif /* PVRSGX_PRIME_H */
