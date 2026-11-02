/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_JOB_H
#define PVRSGX_JOB_H

#include <drm/gpu_scheduler.h>

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>

struct drm_device;
struct drm_file;
struct drm_syncobj;
struct dma_fence_chain;
struct pvrsgx_device;
struct pvrsgx_file;
struct pvrsgx_vm;

/**
 * struct pvrsgx_context - A queue of jobs in one VM
 */
struct pvrsgx_context {
	struct kref ref;
	struct pvrsgx_device *sdev;
	struct pvrsgx_vm *vm;
	struct drm_sched_entity entity;
	u32 type;
	/** @state: enum drm_powervr_sgx_context_state. */
	u32 state;
};

struct pvrsgx_signal {
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	u64 point;
};

/**
 * struct pvrsgx_job - One submitted job
 */
struct pvrsgx_job {
	struct drm_sched_job base;
	struct pvrsgx_context *ctx;
	u32 type;

	/** @hw_fence: Signalled when the firmware has finished the job. */
	struct dma_fence *hw_fence;
	/** @ccb_seq: Kernel CCB command that carries the job. */
	u64 ccb_seq;
	/** @node: In pvrsgx_device.active while on the GPU. */
	struct list_head node;

	struct drm_gem_object **bos;
	u32 *bo_flags;
	u32 nbos;
	struct pvrsgx_signal *signals;
	u32 nsignals;
};

int pvrsgx_job_init(struct pvrsgx_device *sdev);
void pvrsgx_job_fini(struct pvrsgx_device *sdev);
void pvrsgx_job_process_done(struct pvrsgx_device *sdev);
void pvrsgx_context_destroy_all(struct pvrsgx_file *sfile);

int pvrsgx_ioctl_create_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_destroy_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_get_context_state(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_submit_jobs(struct drm_device *drm, void *data, struct drm_file *file);

#endif /* PVRSGX_JOB_H */
