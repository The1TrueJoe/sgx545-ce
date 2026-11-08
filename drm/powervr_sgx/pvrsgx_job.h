/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_JOB_H
#define PVRSGX_JOB_H

#include <drm/gpu_scheduler.h>

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>

#include "pvrsgx_render.h"

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
	/** @ref: Held by the handle and by each job. */
	struct kref ref;
	/** @sdev: The device. */
	struct pvrsgx_device *sdev;
	/** @vm: The VM its jobs run in. */
	struct pvrsgx_vm *vm;
	/** @entity: Its scheduler queue. */
	struct drm_sched_entity entity;
	/** @type: enum drm_powervr_sgx_ctx_type. */
	u32 type;
	/** @state: enum drm_powervr_sgx_context_state. */
	u32 state;
	/** @render: The firmware half of a render context, else NULL. */
	struct pvrsgx_render_ctx *render;
	/** @transfer: The firmware half of a transfer context, else NULL. */
	struct pvrsgx_transfer_ctx *transfer;
};

struct pvrsgx_signal {
	u32 handle;
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	u64 point;
};

/**
 * struct pvrsgx_job - One submitted job
 */
struct pvrsgx_job {
	/** @base: The scheduler job. */
	struct drm_sched_job base;
	/** @ctx: The context it was submitted on. */
	struct pvrsgx_context *ctx;
	/** @type: enum drm_powervr_sgx_job_type. */
	u32 type;

	/** @hw_fence: Signalled when the firmware has finished the job. */
	struct dma_fence *hw_fence;
	/** @ccb_seq: Kernel CCB command that carries the job. */
	u64 ccb_seq;
	/** @node: In pvrsgx_device.active while on the GPU. */
	struct list_head node;
	/**
	 * @done_cpu: Word the firmware sets to @done_val when the job is done;
	 * NULL for a job that is done when the CCB has taken it.
	 */
	u32 *done_cpu;
	/** @done_val: Value of *@done_cpu once the job is done. */
	u32 done_val;
	/** @render: The render job, for a render context. */
	struct pvrsgx_render_job render;
	/** @transfer: The transfer job, for a transfer context. */
	struct pvrsgx_transfer_job transfer;

	/** @bos: Objects the job references, for implicit sync. */
	struct drm_gem_object **bos;
	/** @bo_flags: DRM_POWERVR_SGX_BO_REF_FLAG_* of each of @bos. */
	u32 *bo_flags;
	/** @nbos: Number of @bos. */
	u32 nbos;
	/** @imports: Imported objects mapped in the VM at submit, flushed around the job. */
	struct drm_gem_object **imports;
	/** @nimports: Number of @imports. */
	u32 nimports;
	/** @signals: Syncobjs to signal with the job's fence. */
	struct pvrsgx_signal *signals;
	/** @nsignals: Number of @signals. */
	u32 nsignals;
};

int pvrsgx_job_init(struct pvrsgx_device *sdev);
void pvrsgx_job_fini(struct pvrsgx_device *sdev);
void pvrsgx_job_process_done(struct pvrsgx_device *sdev);
bool pvrsgx_job_oldest_done(struct pvrsgx_device *sdev);
void pvrsgx_context_destroy_all(struct pvrsgx_file *sfile);
struct pvrsgx_context *pvrsgx_context_lookup(struct pvrsgx_file *sfile, u32 handle);
void pvrsgx_context_put(struct pvrsgx_context *ctx);

int pvrsgx_ioctl_create_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_destroy_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_get_context_state(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_submit_jobs(struct drm_device *drm, void *data, struct drm_file *file);

#endif /* PVRSGX_JOB_H */
