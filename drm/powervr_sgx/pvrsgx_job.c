// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Contexts, jobs and the scheduler. One drm_sched scheduler with a credit limit
 * of one: a single job on the GPU at a time. Each job becomes one kernel CCB
 * command and is complete when the microkernel has
 * finished it; completion is checked from the SW_EVENT interrupt and, since the
 * firmware does not interrupt for every command, from a short poll while jobs
 * are out.
 */

#include <drm/drm_auth.h>
#include <drm/drm_drv.h>
#include <drm/drm_exec.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>

#include <linux/capability.h>
#include <linux/dma-fence-chain.h>
#include <linux/dma-resv.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_ccb.h"
#include "pvrsgx_device.h"
#include "pvrsgx_pm.h"
#include "pvrsgx_drv.h"
#include "pvrsgx_dump.h"
#include "pvrsgx_fw.h"
#include "pvrsgx_job.h"
#include "pvrsgx_prime.h"
#include "pvrsgx_kobj.h"
#include "pvrsgx_trace.h"
#include "pvrsgx_validate.h"
#include "pvrsgx_vm.h"

#define PVRSGX_MAX_JOBS		64
#define PVRSGX_MAX_SYNC_OPS	64
#define PVRSGX_MAX_BO_REFS	256
static unsigned int job_timeout_ms = 2000;
module_param(job_timeout_ms, uint, 0444);
MODULE_PARM_DESC(job_timeout_ms, "How long a job may run before the GPU is reset (default 2000 ms)");

/*
 * Two jobs on the GPU at once, so one render's TA runs while another's 3D
 * finishes. Off until proven: it needs the microkernel to take a TA while a 3D
 * is in flight. A reset then cancels both, and both contexts are refused.
 */

static struct pvrsgx_job *to_pvrsgx_job(struct drm_sched_job *sched_job)
{
	return container_of(sched_job, struct pvrsgx_job, base);
}

static const char *fence_driver_name(struct dma_fence *fence)
{
	return "powervr_sgx";
}

static const char *fence_timeline_name(struct dma_fence *fence)
{
	return "sgx";
}

static const struct dma_fence_ops pvrsgx_fence_ops = {
	.get_driver_name = fence_driver_name,
	.get_timeline_name = fence_timeline_name,
};

static void context_release(struct kref *ref)
{
	struct pvrsgx_context *ctx = container_of(ref, struct pvrsgx_context, ref);
	struct pvrsgx_device *sdev = ctx->sdev;

	pvrsgx_render_ctx_fini(sdev, ctx);
	pvrsgx_transfer_ctx_fini(sdev, ctx);
	/*
	 * As the DDK after a context's cleanup: the next command flushes the GPU's
	 * data caches, or a later client at the same addresses reads this one's.
	 */
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_DATA;
	mutex_unlock(&sdev->ccb_lock);
	pvrsgx_vm_put(ctx->vm);
	kfree(ctx);
}

void pvrsgx_context_put(struct pvrsgx_context *ctx)
{
	kref_put(&ctx->ref, context_release);
}

static void context_put(struct pvrsgx_context *ctx)
{
	pvrsgx_context_put(ctx);
}

static bool job_done(struct pvrsgx_job *job, u64 consumed)
{
	if (job->done_cpu)
		return (s32)(READ_ONCE(*job->done_cpu) - job->done_val) >= 0;
	return job->ccb_seq <= consumed;
}

/* Whether the oldest job on the GPU is done; for the hard IRQ handler. */
bool pvrsgx_job_oldest_done(struct pvrsgx_device *sdev)
{
	struct pvrsgx_job *job;
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	job = list_first_entry_or_null(&sdev->active, struct pvrsgx_job, node);
	done = job && job_done(job, pvrsgx_ccb_consumed(sdev));
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	return done;
}

/*
 * A page fault stalls the faulting requestor, and the SGX545 raises no host
 * event for it: check the BIF here, and end the job at once rather than at its
 * timeout. Only while jobs are out, so the core is powered.
 */
static bool pvrsgx_job_fault_check(struct pvrsgx_device *sdev)
{
	bool fault;
	int idx;

	if (READ_ONCE(sdev->faulted) || READ_ONCE(sdev->lost) || !READ_ONCE(sdev->powered) ||
	    !drm_dev_enter(&sdev->base, &idx))
		return false;
	fault = sgx_rd(sdev, SGX_CR_BIF_INT_STAT) & sdev->cr->bif_int_stat_fault_req;
	drm_dev_exit(idx);
	if (!fault)
		return false;
	WRITE_ONCE(sdev->faulted, true);
	drm_sched_fault(&sdev->sched);
	return true;
}

/* Signal every job the firmware has finished. Runs in the IRQ thread and the poll work. */
void pvrsgx_job_process_done(struct pvrsgx_device *sdev)
{
	struct pvrsgx_job *job, *tmp;
	unsigned long flags;
	bool more, reap = false;
	LIST_HEAD(finished);
	bool cookie;
	u64 consumed;

	mutex_lock(&sdev->done_lock);
	/* Under the lock a reset fails the old jobs under: no count from before it. */
	consumed = pvrsgx_ccb_consumed(sdev);
	cookie = dma_fence_begin_signalling();
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(job, tmp, &sdev->active, node) {
		if (!job_done(job, consumed))
			break;
		list_move_tail(&job->node, &finished);
		if (job->type == DRM_POWERVR_SGX_JOB_TYPE_RENDER) {
			sdev->renders_done++;
			reap = true;
		}
	}
	more = !list_empty(&sdev->active);
	spin_unlock_irqrestore(&sdev->fence_lock, flags);

	list_for_each_entry(job, &finished, node)
		pvrsgx_prime_flush(job, true);
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(job, tmp, &finished, node) {
		list_del_init(&job->node);
		trace_pvrsgx_job_done(job->type, job->hw_fence->seqno);
		dma_fence_signal_locked(job->hw_fence);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	dma_fence_end_signalling(cookie);
	mutex_unlock(&sdev->done_lock);

	if (reap)
		queue_work(system_dfl_wq, &sdev->reap_work);
	if (more && pvrsgx_job_fault_check(sdev))
		return;
	if (more)
		mod_delayed_work(system_dfl_wq, &sdev->poll_work, 1);
}

static void reap_work_fn(struct work_struct *work)
{
	struct pvrsgx_device *sdev = container_of(work, struct pvrsgx_device, reap_work);
	unsigned long flags;
	u64 done;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	done = sdev->renders_done;
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	pvrsgx_kobj_reap(sdev, done);
}

static void poll_work_fn(struct work_struct *work)
{
	struct pvrsgx_device *sdev = container_of(to_delayed_work(work), struct pvrsgx_device,
						  poll_work);

	pvrsgx_job_process_done(sdev);
}

static struct dma_fence *pvrsgx_run_job(struct drm_sched_job *sched_job)
{
	struct pvrsgx_job *job = to_pvrsgx_job(sched_job);
	struct pvrsgx_device *sdev = job->ctx->sdev;
	u32 data[6] = {};
	struct dma_fence *fence;
	unsigned long flags;
	int err, idx;

	if (sched_job->s_fence->finished.error)
		return NULL;
	/* A reset context's queued jobs build on work that never finished. */
	if (READ_ONCE(job->ctx->state) != DRM_POWERVR_SGX_CONTEXT_STATE_OK)
		return ERR_PTR(-ECANCELED);

	/* Allocated at submit: nothing here may wait for memory (dma-fence signalling). */
	fence = job->hw_fence_mem;
	job->hw_fence_mem = NULL;
	spin_lock_irqsave(&sdev->fence_lock, flags);
	dma_fence_init(fence, &pvrsgx_fence_ops, &sdev->fence_lock, sdev->fence_context,
		       ++sdev->fence_seqno);
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	job->hw_fence = dma_fence_get(fence);

	/* After unbind the doorbell is gone (pvrsgx_job_unplug). */
	if (!drm_dev_enter(&sdev->base, &idx)) {
		err = -ENODEV;
		goto fail;
	}
	/* The job's runtime-PM reference, taken at submit, keeps the GPU up. */
	if (sdev->lost) {
		err = -EIO;
		goto fail_exit;
	}

	pvrsgx_prime_flush(job, false);
	if (job->type == DRM_POWERVR_SGX_JOB_TYPE_RENDER) {
		mutex_lock(&sdev->kobj_lock);
		sdev->renders_submitted++;
		mutex_unlock(&sdev->kobj_lock);
		err = 0;
		for (u32 k = 0; k < job->render.nkicks && !err; k++) {
			pvrsgx_render_job_build(job, k, data);
			err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_TA, data, &job->ccb_seq);
		}
	} else if (job->type == DRM_POWERVR_SGX_JOB_TYPE_TRANSFER) {
		pvrsgx_transfer_job_build(job, data);
		err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_TRANSFER, data, &job->ccb_seq);
	} else {
		/* A NULL job is a no-op command: done when the microkernel has taken it. */
		err = pvrsgx_ccb_send(sdev, PVRSGX_CMD_PROCESS_QUEUES, data, &job->ccb_seq);
	}
	if (err) {
		drm_err_ratelimited(&sdev->base, "job not sent to the microkernel (%d)\n", err);
		goto fail_exit;
	}

	trace_pvrsgx_kick(job->type, job->ccb_seq, fence->seqno);
	dma_fence_put(sdev->last_fence);
	sdev->last_fence = dma_fence_get(fence);
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_add_tail(&job->node, &sdev->active);
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	mod_delayed_work(system_dfl_wq, &sdev->poll_work, 0);
	drm_dev_exit(idx);
	return fence;

fail_exit:
	drm_dev_exit(idx);
fail:
	dma_fence_set_error(fence, err);
	dma_fence_signal(fence);
	return fence;
}

/*
 * A job ran too long. Blame its context, fail what was on the GPU, and start
 * the microkernel again from a clean control block and an empty CCB, rather
 * than rescan the old one as the vendor driver does.
 */
static enum drm_gpu_sched_stat pvrsgx_timedout_job(struct drm_sched_job *sched_job)
{
	struct pvrsgx_job *bad = to_pvrsgx_job(sched_job), *job, *tmp;
	struct pvrsgx_device *sdev = bad->ctx->sdev;
	bool fault = READ_ONCE(sdev->faulted), lost;
	unsigned long flags;
	int idx;

	drm_sched_stop(&sdev->sched, sched_job);
	/* After unbind (pvrsgx_job_unplug) the hardware is gone: fail, touch nothing. */
	mutex_lock(&sdev->reset_lock);
	lost = READ_ONCE(sdev->lost) || !drm_dev_enter(&sdev->base, &idx);
	if (lost)
		goto fail;
	if (fault)
		drm_err(&sdev->base, "job page fault at 0x%08lx (BIF status 0x%08x); resetting\n",
			sgx_rd(sdev, SGX_CR_BIF_FAULT) & SGX_MMU_ADDR_MASK,
			sgx_rd(sdev, SGX_CR_BIF_INT_STAT));
	else
		drm_err(&sdev->base, "job timed out (ccb %llu, consumed %llu); resetting\n",
			bad->ccb_seq, pvrsgx_ccb_consumed(sdev));
	WRITE_ONCE(bad->ctx->state, DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY);
	pvrsgx_coredump(sdev, bad);
	/* Stopped before any fence signals: userspace may free the jobs' memory then. */
	pvrsgx_fw_halt(sdev);

fail:
	mutex_lock(&sdev->done_lock);
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(job, tmp, &sdev->active, node) {
		list_del_init(&job->node);
		if (job->ctx != bad->ctx)
			WRITE_ONCE(job->ctx->state, DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT);
		dma_fence_set_error(job->hw_fence, lost ? -ENODEV : job != bad ? -ECANCELED :
				    fault ? -EFAULT : -ETIME);
		dma_fence_signal_locked(job->hw_fence);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	mutex_unlock(&sdev->done_lock);
	if (lost)
		goto out;

	/*
	 * As the vendor driver's hardware recovery: no power-off handshake with a
	 * hung microkernel, straight into the full start (clocks, scripts, a reset
	 * that drains pending faults, the first kick).
	 */
	sdev->reset_count++;
	sdev->fw_running = false;
	/* The restart's reset clears the BIF's fault state. */
	WRITE_ONCE(sdev->faulted, false);
	if (pvrsgx_fw_start(sdev)) {
		drm_err(&sdev->base, "microkernel did not restart: device lost\n");
		/* Lost: the cores stay in reset, and run_job fails every job. */
		pvrsgx_fw_halt(sdev);
		WRITE_ONCE(sdev->lost, true);
	}
	drm_dev_exit(idx);
out:
	mutex_unlock(&sdev->reset_lock);
	drm_sched_start(&sdev->sched, 0);
	return DRM_GPU_SCHED_STAT_RESET;
}

static void pvrsgx_free_job(struct drm_sched_job *sched_job)
{
	struct pvrsgx_job *job = to_pvrsgx_job(sched_job);

	/* Taken at submit, for this job: the GPU may suspend once none are left. */
	pvrsgx_power_put(job->ctx->sdev, 1);

	drm_sched_job_cleanup(sched_job);
	dma_fence_put(job->hw_fence);
	kfree(job->hw_fence_mem);	/* never run */
	for (u32 b = 0; b < job->nbos; b++)
		drm_gem_object_put(job->bos[b]);
	kvfree(job->bos);
	kvfree(job->bo_flags);
	pvrsgx_prime_put(job);
	pvrsgx_render_job_fini(job);
	context_put(job->ctx);
	kfree(job);
}

static const struct drm_sched_backend_ops pvrsgx_sched_ops = {
	.run_job = pvrsgx_run_job,
	.timedout_job = pvrsgx_timedout_job,
	.free_job = pvrsgx_free_job,
};

int pvrsgx_job_init(struct pvrsgx_device *sdev)
{
	const struct drm_sched_init_args args = {
		.ops = &pvrsgx_sched_ops,
		.num_rqs = DRM_SCHED_PRIORITY_COUNT,
		.credit_limit = 1,
		.timeout = msecs_to_jiffies(job_timeout_ms),
		.name = "powervr_sgx",
		.dev = sdev->base.dev,
	};

	spin_lock_init(&sdev->fence_lock);
	mutex_init(&sdev->reset_lock);
	INIT_LIST_HEAD(&sdev->active);
	INIT_DELAYED_WORK(&sdev->poll_work, poll_work_fn);
	INIT_WORK(&sdev->reap_work, reap_work_fn);
	sdev->fence_context = dma_fence_context_alloc(1);
	return drm_sched_init(&sdev->sched, &args);
}

/*
 * Unbind. Nothing reaches the GPU again: the firmware stops, the jobs on it
 * fail with -ENODEV, and run_job fails the queued ones (-EIO, @lost) without
 * touching the hardware.
 */
void pvrsgx_job_unplug(struct pvrsgx_device *sdev)
{
	struct pvrsgx_job *job, *tmp;
	unsigned long flags;

	drm_sched_wqueue_stop(&sdev->sched);
	/* A reset under way finishes first; one after this sees @lost. */
	mutex_lock(&sdev->reset_lock);
	WRITE_ONCE(sdev->lost, true);
	pvrsgx_fw_stop(sdev);
	mutex_unlock(&sdev->reset_lock);
	cancel_delayed_work_sync(&sdev->poll_work);

	mutex_lock(&sdev->done_lock);
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(job, tmp, &sdev->active, node) {
		list_del_init(&job->node);
		dma_fence_set_error(job->hw_fence, -ENODEV);
		dma_fence_signal_locked(job->hw_fence);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	mutex_unlock(&sdev->done_lock);
	drm_sched_wqueue_start(&sdev->sched);
}

/*
 * System sleep: no new job reaches the GPU, and the jobs on it finish, or the
 * timeout's reset ends them, within a job timeout. -EBUSY aborts the suspend.
 */
int pvrsgx_job_quiesce(struct pvrsgx_device *sdev)
{
	long left = msecs_to_jiffies(job_timeout_ms) + HZ;

	/* A reset restarts submission; stop it again after one. */
	for (int i = 0; i < 2; i++) {
		struct dma_fence *last;

		drm_sched_wqueue_stop(&sdev->sched);
		/* Set only by run_job, which is stopped. Jobs finish in order. */
		last = sdev->last_fence;
		if (!last || dma_fence_is_signaled(last)) {
			flush_work(&sdev->reap_work);
			return 0;
		}
		left = dma_fence_wait_timeout(last, false, left);
		if (left <= 0)
			break;
	}
	drm_err(&sdev->base, "GPU still busy: not suspending\n");
	drm_sched_wqueue_start(&sdev->sched);
	return -EBUSY;
}

void pvrsgx_job_unquiesce(struct pvrsgx_device *sdev)
{
	drm_sched_wqueue_start(&sdev->sched);
}

void pvrsgx_job_fini(struct pvrsgx_device *sdev)
{
	drm_sched_fini(&sdev->sched);
	cancel_delayed_work_sync(&sdev->poll_work);
	cancel_work_sync(&sdev->reap_work);
	dma_fence_put(sdev->last_fence);
	sdev->last_fence = NULL;
}

static enum drm_sched_priority sched_priority(s32 p)
{
	switch (p) {
	case DRM_POWERVR_SGX_CTX_PRIORITY_LOW:
		return DRM_SCHED_PRIORITY_LOW;
	case DRM_POWERVR_SGX_CTX_PRIORITY_HIGH:
		return DRM_SCHED_PRIORITY_HIGH;
	default:
		return DRM_SCHED_PRIORITY_NORMAL;
	}
}

int pvrsgx_ioctl_create_context(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_create_context_args *args = data;
	struct pvrsgx_device *sdev = to_pvrsgx_device(drm);
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
	struct drm_gpu_scheduler *sched = &sdev->sched;
	struct pvrsgx_context *ctx;
	int err;

	if (args->flags)
		return -EINVAL;
	if (args->type != DRM_POWERVR_SGX_CTX_TYPE_RENDER &&
	    args->type != DRM_POWERVR_SGX_CTX_TYPE_TRANSFER)
		return -EINVAL;
	if (args->priority != DRM_POWERVR_SGX_CTX_PRIORITY_LOW &&
	    args->priority != DRM_POWERVR_SGX_CTX_PRIORITY_NORMAL &&
	    args->priority != DRM_POWERVR_SGX_CTX_PRIORITY_HIGH)
		return -EINVAL;
	if (args->priority == DRM_POWERVR_SGX_CTX_PRIORITY_HIGH &&
	    !capable(CAP_SYS_NICE) && !drm_is_current_master(file))
		return -EACCES;
	if (args->type == DRM_POWERVR_SGX_CTX_TYPE_TRANSFER && args->param_buffer_pages)
		return -EINVAL;
	if (args->param_buffer_pages &&
	    (args->param_buffer_pages < 0x100 || args->param_buffer_pages > 0x8000))
		return -EINVAL;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->vm = pvrsgx_vm_lookup(sfile, args->vm_context_handle);
	if (!ctx->vm) {
		kfree(ctx);
		return -ENOENT;
	}
	ctx->sdev = sdev;
	ctx->type = args->type;
	kref_init(&ctx->ref);
	err = ctx->type == DRM_POWERVR_SGX_CTX_TYPE_RENDER ?
	      pvrsgx_render_ctx_init(sdev, ctx, args->param_buffer_pages) :
	      pvrsgx_transfer_ctx_init(sdev, ctx);
	if (err)
		goto err_put;
	err = drm_sched_entity_init(&ctx->entity, sched_priority(args->priority), &sched, 1, NULL);
	if (err)
		goto err_put;
	pvrsgx_vm_user_get(ctx->vm);
	err = xa_alloc(&sfile->contexts, &args->handle, ctx, xa_limit_32b, GFP_KERNEL);
	if (err)
		goto err_entity;
	return 0;

err_entity:
	drm_sched_entity_destroy(&ctx->entity);
	pvrsgx_vm_user_put(ctx->vm);
err_put:
	context_put(ctx);
	return err;
}

static void context_destroy(struct pvrsgx_context *ctx)
{
	/* Its queued jobs are on the GPU or cancelled before its VM's mappings may go. */
	drm_sched_entity_destroy(&ctx->entity);
	pvrsgx_vm_user_put(ctx->vm);
	context_put(ctx);
}

void pvrsgx_context_destroy_all(struct pvrsgx_file *sfile)
{
	struct pvrsgx_context *ctx;
	unsigned long handle;

	xa_for_each(&sfile->contexts, handle, ctx) {
		xa_erase(&sfile->contexts, handle);
		context_destroy(ctx);
	}
}

int pvrsgx_ioctl_destroy_context(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_destroy_context_args *args = data;
	struct pvrsgx_context *ctx;

	if (args->_padding_4)
		return -EINVAL;
	ctx = xa_erase(&to_pvrsgx_file(file)->contexts, args->handle);
	if (!ctx)
		return -ENOENT;
	context_destroy(ctx);
	return 0;
}

struct pvrsgx_context *pvrsgx_context_lookup(struct pvrsgx_file *sfile, u32 handle)
{
	struct pvrsgx_context *ctx;

	xa_lock(&sfile->contexts);
	ctx = xa_load(&sfile->contexts, handle);
	if (ctx)
		kref_get(&ctx->ref);
	xa_unlock(&sfile->contexts);
	return ctx;
}

int pvrsgx_ioctl_get_context_state(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_get_context_state_args *args = data;
	struct pvrsgx_context *ctx;

	if (args->_padding_c)
		return -EINVAL;
	ctx = pvrsgx_context_lookup(to_pvrsgx_file(file), args->handle);
	if (!ctx)
		return -ENOENT;
	args->state = READ_ONCE(ctx->state);
	args->reset_count = to_pvrsgx_device(drm)->reset_count;
	context_put(ctx);
	return 0;
}

/* What one job needs from its submission, held until it is pushed. */
struct job_args {
	struct pvrsgx_job *job;
	struct drm_gem_object **bos;
	u32 *bo_flags;
	u32 nbos;
	struct pvrsgx_signal *signals;
	u32 nsignals;
	/* Earlier jobs of this submit whose signals this job waits on. */
	u32 *intra;
	u32 nintra;
	bool inited;
	/* Once pushed: the job's finished fence, for later jobs' waits. */
	struct dma_fence *done;
};

static void job_args_fini(struct job_args *ja)
{
	for (u32 i = 0; i < ja->nbos; i++)
		drm_gem_object_put(ja->bos[i]);
	kvfree(ja->bos);
	kvfree(ja->bo_flags);
	for (u32 i = 0; i < ja->nsignals; i++) {
		if (ja->signals[i].syncobj)
			drm_syncobj_put(ja->signals[i].syncobj);
		dma_fence_chain_free(ja->signals[i].chain);
	}
	kvfree(ja->signals);
	kvfree(ja->intra);
	dma_fence_put(ja->done);
	if (ja->job) {
		if (ja->inited)
			drm_sched_job_cleanup(&ja->job->base);
		pvrsgx_prime_put(ja->job);
		pvrsgx_render_job_fini(ja->job);
		context_put(ja->job->ctx);
		kfree(ja->job->hw_fence_mem);
		kfree(ja->job);
	}
}

/* The latest earlier job of this submit that signals handle at point, or -1. */
static int intra_signaller(const struct job_args *prev, u32 nprev, u32 handle, u64 point)
{
	for (int k = nprev - 1; k >= 0; k--) {
		for (u32 s = 0; s < prev[k].nsignals; s++) {
			if (prev[k].signals[s].handle == handle &&
			    prev[k].signals[s].point == point)
				return k;
		}
	}
	return -1;
}

static int job_sync_ops(struct drm_file *file, struct job_args *ja,
			const struct drm_powervr_sgx_obj_array *arr,
			const struct job_args *prev, u32 nprev)
{
	struct drm_powervr_sgx_sync_op *ops;
	int err = 0;

	ops = pvrsgx_obj_array_get(arr, sizeof(*ops), PVRSGX_MAX_SYNC_OPS);
	if (IS_ERR_OR_NULL(ops))
		return PTR_ERR_OR_ZERO(ops);

	ja->signals = kvcalloc(arr->count, sizeof(*ja->signals), GFP_KERNEL);
	ja->intra = kvcalloc(arr->count, sizeof(*ja->intra), GFP_KERNEL);
	if (!ja->signals || !ja->intra) {
		err = -ENOMEM;
		goto out;
	}
	for (u32 i = 0; i < arr->count && !err; i++) {
		u32 type = ops[i].flags & DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_MASK;
		bool timeline = type == DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ;
		struct dma_fence *fence;

		if ((ops[i].flags & ~PVRSGX_SYNC_OP_FLAGS_MASK) ||
		    (type != DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ && !timeline) ||
		    (!timeline && ops[i].value)) {
			err = -EINVAL;
			break;
		}

		if (!(ops[i].flags & DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL)) {
			int k = intra_signaller(prev, nprev, ops[i].handle, ops[i].value);

			/* Its fence exists once that job is armed: added then. */
			if (k >= 0) {
				ja->intra[ja->nintra++] = k;
				continue;
			}
			err = drm_syncobj_find_fence(file, ops[i].handle, ops[i].value, 0, &fence);
			if (!err)
				err = drm_sched_job_add_dependency(&ja->job->base, fence);
			continue;
		}

		ja->signals[ja->nsignals].syncobj = drm_syncobj_find(file, ops[i].handle);
		if (!ja->signals[ja->nsignals].syncobj) {
			err = -ENOENT;
			break;
		}
		ja->signals[ja->nsignals].handle = ops[i].handle;
		ja->signals[ja->nsignals].point = ops[i].value;
		if (timeline) {
			ja->signals[ja->nsignals].chain = dma_fence_chain_alloc();
			if (!ja->signals[ja->nsignals].chain)
				err = -ENOMEM;
		}
		ja->nsignals++;
	}
out:
	kvfree(ops);
	return err;
}

static int job_bo_refs(struct drm_file *file, struct job_args *ja,
		       const struct drm_powervr_sgx_obj_array *arr)
{
	struct drm_powervr_sgx_bo_ref *refs;
	int err = 0;

	refs = pvrsgx_obj_array_get(arr, sizeof(*refs), PVRSGX_MAX_BO_REFS);
	if (IS_ERR_OR_NULL(refs))
		return PTR_ERR_OR_ZERO(refs);

	ja->bos = kvcalloc(arr->count, sizeof(*ja->bos), GFP_KERNEL);
	ja->bo_flags = kvcalloc(arr->count, sizeof(*ja->bo_flags), GFP_KERNEL);
	if (!ja->bos || !ja->bo_flags) {
		err = -ENOMEM;
		goto out;
	}
	for (u32 i = 0; i < arr->count; i++) {
		if (refs[i].flags & ~DRM_POWERVR_SGX_BO_REF_FLAG_WRITE) {
			err = -EINVAL;
			break;
		}
		ja->bos[i] = drm_gem_object_lookup(file, refs[i].handle);
		if (!ja->bos[i]) {
			err = -ENOENT;
			break;
		}
		ja->bo_flags[i] = refs[i].flags;
		ja->nbos++;
	}
out:
	kvfree(refs);
	return err;
}

static int job_create(struct drm_file *file, const struct drm_powervr_sgx_job *in,
		      struct job_args *ja, const struct job_args *prev, u32 nprev)
{
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
	struct pvrsgx_device *sdev = sfile->sdev;
	struct pvrsgx_context *ctx;
	struct pvrsgx_job *job;
	int err;

	if (in->_padding_4c)
		return -EINVAL;

	switch (in->type) {
	case DRM_POWERVR_SGX_JOB_TYPE_NULL:
		if (in->flags || in->cmd_stream_len || in->cmd_stream || in->ta_kicks.count ||
		    in->rt_dataset_handle)
			return -EINVAL;
		break;
	case DRM_POWERVR_SGX_JOB_TYPE_RENDER:
	case DRM_POWERVR_SGX_JOB_TYPE_TRANSFER:
		break;
	default:
		return -EINVAL;
	}

	ctx = pvrsgx_context_lookup(sfile, in->context_handle);
	if (!ctx)
		return -ENOENT;
	if (READ_ONCE(ctx->state) != DRM_POWERVR_SGX_CONTEXT_STATE_OK) {
		context_put(ctx);
		return -ECANCELED;
	}
	if (sdev->lost) {
		context_put(ctx);
		return -EIO;
	}

	job = kzalloc_obj(*job);
	if (!job) {
		context_put(ctx);
		return -ENOMEM;
	}
	job->ctx = ctx;
	job->type = in->type;
	INIT_LIST_HEAD(&job->node);
	ja->job = job;
	job->hw_fence_mem = kzalloc_obj(*job->hw_fence_mem);
	if (!job->hw_fence_mem)
		return -ENOMEM;

	err = drm_sched_job_init(&job->base, &ctx->entity, 1, sfile, file->client_id);
	if (err)
		return err;
	ja->inited = true;

	if (job->type == DRM_POWERVR_SGX_JOB_TYPE_RENDER) {
		if (ctx->type != DRM_POWERVR_SGX_CTX_TYPE_RENDER)
			return -EINVAL;
		err = pvrsgx_render_job_init(file, job, in);
		if (err)
			return err;
	} else if (job->type == DRM_POWERVR_SGX_JOB_TYPE_TRANSFER) {
		if (ctx->type != DRM_POWERVR_SGX_CTX_TYPE_TRANSFER)
			return -EINVAL;
		err = pvrsgx_transfer_job_init(file, job, in);
		if (err)
			return err;
	}

	err = job_sync_ops(file, ja, &in->sync_ops, prev, nprev);
	if (err)
		return err;
	err = job_bo_refs(file, ja, &in->bo_refs);
	if (err)
		return err;
	return pvrsgx_validate_job(sdev, job);
}

int pvrsgx_ioctl_submit_jobs(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_submit_jobs_args *args = data;
	struct drm_powervr_sgx_job *in;
	struct job_args *ja;
	struct drm_exec exec;
	u32 n = args->jobs.count;
	u32 pushed = 0;
	int err = 0;

	args->failed_index = ~0u;
	if (args->_padding_14 || !n)
		return -EINVAL;
	if (drm_dev_is_unplugged(drm))
		return -ENODEV;

	in = pvrsgx_obj_array_get(&args->jobs, sizeof(*in), PVRSGX_MAX_JOBS);
	if (IS_ERR(in))
		return PTR_ERR(in);
	ja = kvcalloc(n, sizeof(*ja), GFP_KERNEL);
	if (!ja) {
		kvfree(in);
		return -ENOMEM;
	}

	for (u32 i = 0; i < n; i++) {
		err = job_create(file, &in[i], &ja[i], ja, i);
		err = err ?: pvrsgx_prime_imports(ja[i].bos, ja[i].nbos, &ja[i].job->imports,
						  &ja[i].job->nimports);
		if (err) {
			args->failed_index = i;
			goto out;
		}
	}

	/*
	 * One runtime-PM reference per job, from here, where resuming may sleep and
	 * no reservation is held, to free_job; never in run_job, which signals fences.
	 */
	for (u32 i = 0; i < n; i++) {
		err = pvrsgx_power_get(to_pvrsgx_device(drm));
		if (err) {
			pvrsgx_power_put(to_pvrsgx_device(drm), i);
			goto out;
		}
	}

	drm_exec_init(&exec, DRM_EXEC_INTERRUPTIBLE_WAIT | DRM_EXEC_IGNORE_DUPLICATES, 0);
	drm_exec_until_all_locked(&exec) {
		for (u32 i = 0; i < n; i++) {
			struct drm_gpuvm *gpuvm = &ja[i].job->ctx->vm->gpuvm;

			err = drm_exec_prepare_obj(&exec, drm_gpuvm_resv_obj(gpuvm), n);
			drm_exec_retry_on_contention(&exec);
			if (err)
				goto out_exec;
			for (u32 b = 0; b < ja[i].nbos; b++) {
				err = drm_exec_prepare_obj(&exec, ja[i].bos[b], n);
				drm_exec_retry_on_contention(&exec);
				if (err)
					goto out_exec;
			}
		}
	}

	for (u32 i = 0; i < n; i++) {
		for (u32 b = 0; b < ja[i].nbos; b++) {
			bool write = ja[i].bo_flags[b] & DRM_POWERVR_SGX_BO_REF_FLAG_WRITE;

			err = drm_sched_job_add_implicit_dependencies(&ja[i].job->base,
								      ja[i].bos[b], write);
			if (err) {
				args->failed_index = i;
				goto out_exec;
			}
		}
	}

	/*
	 * In order: a job's waits on earlier jobs of the submit, then arm, publish
	 * and push it. An armed job must be pushed, so a wait that can't be added
	 * fails this job and those after it; the earlier ones are already queued
	 * (@failed_index). The VM's reservation, held throughout, keeps the order.
	 */
	for (u32 i = 0; i < n; i++) {
		struct pvrsgx_job *job = ja[i].job;
		struct dma_fence *done;

		for (u32 d = 0; d < ja[i].nintra; d++) {
			err = drm_sched_job_add_dependency(&job->base,
							   dma_fence_get(ja[ja[i].intra[d]].done));
			if (err) {
				args->failed_index = i;
				goto out_exec;
			}
		}
		drm_sched_job_arm(&job->base);
		done = &job->base.s_fence->finished;
		ja[i].done = dma_fence_get(done);

		dma_resv_add_fence(drm_gpuvm_resv(&job->ctx->vm->gpuvm), done,
				   DMA_RESV_USAGE_BOOKKEEP);
		for (u32 b = 0; b < ja[i].nbos; b++)
			dma_resv_add_fence(ja[i].bos[b]->resv, done,
					   ja[i].bo_flags[b] & DRM_POWERVR_SGX_BO_REF_FLAG_WRITE ?
					   DMA_RESV_USAGE_WRITE : DMA_RESV_USAGE_READ);
		for (u32 s = 0; s < ja[i].nsignals; s++) {
			struct pvrsgx_signal *sig = &ja[i].signals[s];

			if (sig->chain) {
				drm_syncobj_add_point(sig->syncobj, sig->chain, done, sig->point);
				sig->chain = NULL;
			} else {
				drm_syncobj_replace_fence(sig->syncobj, done);
			}
		}
		/* The job keeps its list: run_job and completion flush the imports. */
		job->bos = ja[i].bos;
		job->bo_flags = ja[i].bo_flags;
		job->nbos = ja[i].nbos;
		ja[i].bos = NULL;
		ja[i].bo_flags = NULL;
		ja[i].nbos = 0;
		drm_sched_entity_push_job(&job->base);
		ja[i].job = NULL;	/* the scheduler owns it now */
		pushed++;
	}

out_exec:
	drm_exec_fini(&exec);
	/* The references of the jobs not pushed. */
	pvrsgx_power_put(to_pvrsgx_device(drm), n - pushed);
out:
	for (u32 i = 0; i < n; i++)
		job_args_fini(&ja[i]);
	kvfree(ja);
	kvfree(in);
	return err;
}
