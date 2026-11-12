/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_JOB_H
#define PVRSGX_JOB_H

#include <drm/gpu_scheduler.h>

#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>

#include "pvrsgx_render.h"
#include "pvrsgx_transfer.h"

struct drm_device;
struct drm_file;
struct drm_syncobj;
struct dma_fence_chain;
struct pvrsgx_device;
struct pvrsgx_file;
struct pvrsgx_vm;

/**
 * struct pvrsgx_usage - A file's GPU time, for fdinfo
 *
 * Its contexts hold it too: a job may finish after its file has closed.
 */
struct pvrsgx_usage {
	/** @ref: Held by the file and by each of its contexts. */
	struct kref ref;
	/** @render_ns: Time render jobs of the file spent on the GPU. */
	atomic64_t render_ns;
};

struct pvrsgx_usage *pvrsgx_usage_new(void);
struct pvrsgx_usage *pvrsgx_usage_get(struct pvrsgx_usage *u);
void pvrsgx_usage_put(struct pvrsgx_usage *u);

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
	/** @usage: Its file's GPU time. */
	struct pvrsgx_usage *usage;
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

/* Bytes of an import a job reads or writes: @rows rows of @size, @stride apart. */
struct pvrsgx_cache_range {
	struct drm_gem_object *obj;
	u64 offset;
	u64 size;
	u32 stride;
	u32 rows;
	bool write;
	bool import;
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
	/** @hw_fence_mem: @hw_fence's memory from submit until run_job initialises it. */
	struct dma_fence *hw_fence_mem;
	/** @ccb_seq: Kernel CCB command that carries the job. */
	/** @start_ns: When run_job sent it to the microkernel. */
	u64 start_ns;
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

	/** @ranges: The bytes of imports the job reads or writes, flushed around it. */
	struct pvrsgx_cache_range *ranges;
	/** @nranges: Number of @ranges. */
	u32 nranges;
	/** @signals: Syncobjs to signal with the job's fence. */
	struct pvrsgx_signal *signals;
	/** @nsignals: Number of @signals. */
	u32 nsignals;
};

int pvrsgx_job_init(struct pvrsgx_device *sdev);
void pvrsgx_job_fini(struct pvrsgx_device *sdev);
void pvrsgx_job_process_done(struct pvrsgx_device *sdev);
void pvrsgx_job_unplug(struct pvrsgx_device *sdev);
int pvrsgx_job_quiesce(struct pvrsgx_device *sdev);
void pvrsgx_job_unquiesce(struct pvrsgx_device *sdev);
bool pvrsgx_job_oldest_done(struct pvrsgx_device *sdev);
void pvrsgx_context_destroy_all(struct pvrsgx_file *sfile);
struct pvrsgx_context *pvrsgx_context_lookup(struct pvrsgx_file *sfile, u32 handle);
void pvrsgx_context_put(struct pvrsgx_context *ctx);

int pvrsgx_ioctl_create_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_destroy_context(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_get_context_state(struct drm_device *drm, void *data, struct drm_file *file);
int pvrsgx_ioctl_submit_jobs(struct drm_device *drm, void *data, struct drm_file *file);

#endif /* PVRSGX_JOB_H */
