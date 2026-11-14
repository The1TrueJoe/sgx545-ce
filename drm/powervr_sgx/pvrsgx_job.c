// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Contexts, jobs and the scheduler. One drm_sched scheduler with a credit limit
 * of one: a single job on the GPU at a time. Each job becomes one kernel CCB
 * command and is complete when the microkernel has finished it. A microkernel
 * that counts finished jobs (PVRSGX_UKFW_DONE_SEQ) interrupts for each one, and
 * a slow poll is only a backstop; with an older one, completion is also polled
 * every jiffy while jobs are out.
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
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

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

struct pvrsgx_usage *pvrsgx_usage_new(void)
{
	struct pvrsgx_usage *u = kzalloc_obj(*u);

	if (u)
		kref_init(&u->ref);
	return u;
}

struct pvrsgx_usage *pvrsgx_usage_get(struct pvrsgx_usage *u)
{
	kref_get(&u->ref);
	return u;
}

static void usage_release(struct kref *ref)
{
	kfree(container_of(ref, struct pvrsgx_usage, ref));
}

void pvrsgx_usage_put(struct pvrsgx_usage *u)
{
	if (u)
		kref_put(&u->ref, usage_release);
}

static void context_release(struct kref *ref)
{
	struct pvrsgx_context *ctx = container_of(ref, struct pvrsgx_context, ref);
	struct pvrsgx_device *sdev = ctx->sdev;

	pvrsgx_render_ctx_fini(sdev, ctx);
	pvrsgx_transfer_ctx_fini(sdev, ctx);
	/*
	 * The next command flushes the GPU's data caches, or a later client at
	 * the same addresses reads this one's.
	 */
	mutex_lock(&sdev->ccb_lock);
	sdev->fw.cache_control |= PVRSGX_CC_INVAL_DATA;
	mutex_unlock(&sdev->ccb_lock);
	pvrsgx_vm_put(ctx->vm);
	pvrsgx_usage_put(ctx->usage);
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

/*
 * How many completions the microkernel reports beyond those accounted, when it
 * counts them (PVRSGX_UKFW_DONE_SEQ); otherwise -1. It counts jobs, in order,
 * and the no-ops the kernel sends that are not jobs (job_completions()); not
 * POWER.
 */
static s64 jobs_finished(struct pvrsgx_device *sdev)
{
	const struct pvrsgx_fw_ta3d_ctl *t = sdev->fw.cpu[PVRSGX_FW_TA3DCTL];

	if (!(sdev->fw.features & PVRSGX_UKFW_DONE_SEQ))
		return -1;
	return (u32)(READ_ONCE(t->done_seq) - sdev->done_seq);
}

/*
 * The completions a counting microkernel reports before @job's: one per no-op
 * the kernel sent ahead of it since the last job signalled, and its own.
 */
static u32 job_completions(struct pvrsgx_device *sdev, struct pvrsgx_job *job)
{
	return job->noops_before - sdev->noops_seen + 1;
}

/* A job off the GPU, done or ended by a reset: its time on it, for fdinfo. */
static void job_account(struct pvrsgx_job *job, u64 now)
{
	if (job->type == DRM_POWERVR_SGX_JOB_TYPE_NULL || now < job->start_ns)
		return;
	atomic64_add(now - job->start_ns, &job->ctx->usage->busy_ns);
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
	done = job && (sdev->fw.features & PVRSGX_UKFW_DONE_SEQ ?
		       jobs_finished(sdev) >= job_completions(sdev, job) :
		       job_done(job, pvrsgx_ccb_consumed(sdev)));
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
	bool fault = false;
	int idx;

	/* Not during a reset, which gates the clocks and clears the fault. */
	if (!mutex_trylock(&sdev->reset_lock))
		return false;
	if (!sdev->faulted && !sdev->lost && READ_ONCE(sdev->powered) &&
	    drm_dev_enter(&sdev->base, &idx)) {
		fault = sgx_rd(sdev, SGX_CR_BIF_INT_STAT) & sdev->cr->bif_int_stat_fault_req;
		drm_dev_exit(idx);
		if (fault)
			WRITE_ONCE(sdev->faulted, true);
	}
	mutex_unlock(&sdev->reset_lock);
	if (fault)
		drm_sched_fault(&sdev->sched);
	return fault;
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
	s64 n;

	mutex_lock(&sdev->done_lock);
	/* Under the lock a reset fails the old jobs under: no count from before it. */
	consumed = pvrsgx_ccb_consumed(sdev);
	cookie = dma_fence_begin_signalling();
	spin_lock_irqsave(&sdev->fence_lock, flags);
	n = jobs_finished(sdev);
	list_for_each_entry_safe(job, tmp, &sdev->active, node) {
		if (n >= 0) {
			u32 need = job_completions(sdev, job);

			if (n < need)
				break;
			n -= need;
			sdev->done_seq += need;
			sdev->noops_seen = job->noops_before;
		} else if (!job_done(job, consumed)) {
			break;
		}
		list_move_tail(&job->node, &finished);
		if (job->type == DRM_POWERVR_SGX_JOB_TYPE_RENDER) {
			sdev->renders_done++;
			reap = true;
		} else if (job->type == DRM_POWERVR_SGX_JOB_TYPE_TRANSFER) {
			sdev->transfers_done++;
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
		job_account(job, ktime_get_ns());
		trace_pvrsgx_job_done(job->type, job->hw_fence->seqno);
		dma_fence_signal_locked(job->hw_fence);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	dma_fence_end_signalling(cookie);
	/* Queued under the lock, so a waiter that takes it can flush it. */
	if (reap || !list_empty_careful(&sdev->inval_waits))
		queue_work(sdev->free_wq, &sdev->reap_work);
	mutex_unlock(&sdev->done_lock);

	if (more && pvrsgx_job_fault_check(sdev))
		return;
	/* With a count the interrupt does it, and the poll is only a backstop. */
	if (more)
		mod_delayed_work(sdev->wq, &sdev->poll_work,
				 sdev->fw.features & PVRSGX_UKFW_DONE_SEQ ? HZ / 10 : 1);
}

static void reap_work_fn(struct work_struct *work)
{
	struct pvrsgx_device *sdev = container_of(work, struct pvrsgx_device, reap_work);
	unsigned long flags;
	u64 done, xdone;

	spin_lock_irqsave(&sdev->fence_lock, flags);
	done = sdev->renders_done;
	xdone = sdev->transfers_done;
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	/* Lost, the GPU is stopped for good: everything can go. */
	if (READ_ONCE(sdev->lost)) {
		done = U64_MAX;
		xdone = U64_MAX;
	}
	pvrsgx_kobj_reap(sdev, done, xdone);
	pvrsgx_ccb_inval_reap(sdev, false);
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
	u32 sent = 0;
	bool cookie;
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
	WRITE_ONCE(job->ctx->vm->ran_gen, sdev->fw_gen + 1);
	if (job->type == DRM_POWERVR_SGX_JOB_TYPE_RENDER) {
		mutex_lock(&sdev->kobj_lock);
		sdev->renders_submitted++;
		mutex_unlock(&sdev->kobj_lock);
		err = 0;
		for (u32 k = 0; k < job->render.nkicks && !err; k++) {
			pvrsgx_render_job_build(job, k, data);
			err = pvrsgx_ccb_send(sdev, PVRSGX_KCMD_TA, data, &job->ccb_seq);
			if (!err)
				sent++;
		}
		if (err && !sent) {
			mutex_lock(&sdev->kobj_lock);
			sdev->renders_submitted--;
			mutex_unlock(&sdev->kobj_lock);
		}
	} else if (job->type == DRM_POWERVR_SGX_JOB_TYPE_TRANSFER) {
		mutex_lock(&sdev->kobj_lock);
		sdev->transfers_submitted++;
		mutex_unlock(&sdev->kobj_lock);
		pvrsgx_transfer_job_build(job, data);
		err = pvrsgx_ccb_send(sdev, PVRSGX_KCMD_TRANSFER, data, &job->ccb_seq);
		if (err) {
			mutex_lock(&sdev->kobj_lock);
			sdev->transfers_submitted--;
			mutex_unlock(&sdev->kobj_lock);
		}
	} else {
		/* A NULL job is a no-op command: done when the microkernel has taken it. */
		err = pvrsgx_ccb_send(sdev, PVRSGX_KCMD_PROCESS_QUEUES, data, &job->ccb_seq);
	}
	if (err && !sent) {
		drm_err_ratelimited(&sdev->base, "job not sent to the microkernel (%d)\n", err);
		goto fail_exit;
	}

	job->start_ns = ktime_get_ns();
	/* After the send, so every no-op ahead of it counts; one just after only delays it. */
	job->noops_before = READ_ONCE(sdev->noops_sent);
	trace_pvrsgx_kick(job->type, job->ccb_seq, fence->seqno);
	dma_fence_put(sdev->last_fence);
	sdev->last_fence = dma_fence_get(fence);
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_add_tail(&job->node, &sdev->active);
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	if (err) {
		/*
		 * Part of the render is on the GPU and the rest never will be: it
		 * ends at its reset, now, not at a fence that would free its memory
		 * under the kicks that run.
		 */
		drm_err_ratelimited(&sdev->base, "render sent in part (%u of %u kicks, %d); resetting\n",
				    sent, job->render.nkicks, err);
		drm_sched_fault(&sdev->sched);
	} else {
		mod_delayed_work(sdev->wq, &sdev->poll_work, 0);
	}
	drm_dev_exit(idx);
	return fence;

fail_exit:
	drm_dev_exit(idx);
fail:
	cookie = dma_fence_begin_signalling();
	dma_fence_set_error(fence, err);
	dma_fence_signal(fence);
	dma_fence_end_signalling(cookie);
	return fence;
}

/*
 * A job ran too long. Blame its context, fail what was on the GPU, and start
 * the microkernel again from a clean control block and an empty CCB, rather
 * than rescan the old one.
 */
static enum drm_gpu_sched_stat pvrsgx_timedout_job(struct drm_sched_job *sched_job)
{
	struct pvrsgx_job *bad = to_pvrsgx_job(sched_job), *job, *tmp;
	struct pvrsgx_device *sdev = bad->ctx->sdev;
	bool fault, lost, cookie, wedged = false;
	unsigned long flags;
	int idx;

	mutex_lock(&sdev->reset_lock);
	/* Finished but not yet freed, or held by system sleep: not a hang. */
	if (sdev->quiesced || (bad->hw_fence && dma_fence_is_signaled(bad->hw_fence))) {
		mutex_unlock(&sdev->reset_lock);
		return DRM_GPU_SCHED_STAT_NO_HANG;
	}
	drm_sched_wqueue_stop(&sdev->sched);
	fault = sdev->faulted;
	/* Every fence on the GPU and in the queue waits on what follows. */
	cookie = dma_fence_begin_signalling();
	/* After unbind (pvrsgx_job_unplug) the hardware is gone: fail, touch nothing. */
	lost = sdev->lost || !drm_dev_enter(&sdev->base, &idx);
	if (lost)
		goto fail;
	if (fault)
		drm_err(&sdev->base, "job page fault at 0x%08x (BIF status 0x%08x); resetting\n",
			sgx_rd(sdev, SGX_CR_BIF_FAULT) & sdev->cr->bif_fault_addr,
			sgx_rd(sdev, SGX_CR_BIF_INT_STAT));
	else
		drm_err(&sdev->base, "job timed out (ccb %llu, consumed %llu); resetting\n",
			bad->ccb_seq, pvrsgx_ccb_consumed(sdev));
	WRITE_ONCE(bad->ctx->state, DRM_POWERVR_SGX_CONTEXT_STATE_GUILTY);
	/* Counted before any fence says so: a client woken by one sees the reset. */
	WRITE_ONCE(sdev->reset_count, sdev->reset_count + 1);
	pvrsgx_coredump(sdev, bad);
	/* Stopped before any fence signals: userspace may free the jobs' memory then. */
	pvrsgx_fw_halt(sdev);

fail:
	mutex_lock(&sdev->done_lock);
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(job, tmp, &sdev->active, node) {
		list_del_init(&job->node);
		job_account(job, ktime_get_ns());
		if (job->ctx != bad->ctx)
			WRITE_ONCE(job->ctx->state, DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT);
		dma_fence_set_error(job->hw_fence, lost ? -ENODEV : job != bad ? -ECANCELED :
				    fault ? -EFAULT : -ETIME);
		dma_fence_signal_locked(job->hw_fence);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	mutex_unlock(&sdev->done_lock);
	dma_fence_end_signalling(cookie);
	/*
	 * Their callbacks still attached, the jobs' finished fences carry these
	 * errors; the scheduler, stopped after, only takes the jobs off its list.
	 */
	drm_sched_stop(&sdev->sched, sched_job);
	if (lost)
		goto out;

	/*
	 * No power-off handshake with a hung microkernel, which cannot answer:
	 * straight into the full start (clocks, scripts, a reset that drains
	 * pending faults, the first kick).
	 */
	cookie = dma_fence_begin_signalling();
	WRITE_ONCE(sdev->fw_running, false);
	/* The restart's reset clears the BIF's fault state. */
	WRITE_ONCE(sdev->faulted, false);
	if (pvrsgx_fw_start(sdev)) {
		drm_err(&sdev->base, "microkernel did not restart: device lost\n");
		/* Lost: the cores stay in reset, and run_job fails every job. */
		pvrsgx_fw_halt(sdev);
		WRITE_ONCE(sdev->lost, true);
		wedged = true;
	}
	dma_fence_end_signalling(cookie);
	drm_dev_exit(idx);
out:
	drm_sched_start(&sdev->sched, 0);
	mutex_unlock(&sdev->reset_lock);
	/* DMA is off and the fences are signaled: tell userspace to rebind. It allocates. */
	if (wedged)
		drm_dev_wedged_event(&sdev->base, DRM_WEDGE_RECOVERY_REBIND, NULL);
	return DRM_GPU_SCHED_STAT_RESET;
}

/* The puts that can reach a VM's release, which takes reservations. */
/*
 * A CPU latency limit, for a core whose display needs one while the GPU works
 * (pvrsgx_core_info): taken at a job's submit, released CPU_LATENCY_RELEASE_MS
 * after the last job in flight is freed, so an idle desktop gets the deep
 * C-states back between frames. Independent of runtime PM.
 */
#define CPU_LATENCY_RELEASE_MS	100

static void cpu_latency_release(struct work_struct *work)
{
	struct pvrsgx_cpu_latency *cl = container_of(to_delayed_work(work),
						     struct pvrsgx_cpu_latency, release);

	mutex_lock(&cl->lock);
	if (!atomic_read(&cl->jobs) && cpu_latency_qos_request_active(&cl->req))
		cpu_latency_qos_remove_request(&cl->req);
	mutex_unlock(&cl->lock);
}

static void cpu_latency_init(struct pvrsgx_cpu_latency *cl, u32 us)
{
	cl->us = us;
	atomic_set(&cl->jobs, 0);
	mutex_init(&cl->lock);
	INIT_DELAYED_WORK(&cl->release, cpu_latency_release);
}

/* At submit, where sleeping is allowed. */
static void cpu_latency_get(struct pvrsgx_cpu_latency *cl)
{
	if (!cl->us)
		return;
	atomic_inc(&cl->jobs);
	mutex_lock(&cl->lock);
	if (!cpu_latency_qos_request_active(&cl->req))
		cpu_latency_qos_add_request(&cl->req, cl->us);
	mutex_unlock(&cl->lock);
}

/* From free_job or a failed submit: n jobs gone. */
static void cpu_latency_put(struct pvrsgx_cpu_latency *cl, u32 n)
{
	if (cl->us && n && atomic_sub_and_test(n, &cl->jobs))
		mod_delayed_work(system_dfl_wq, &cl->release,
				 msecs_to_jiffies(CPU_LATENCY_RELEASE_MS));
}

/* With every job freed. */
static void cpu_latency_fini(struct pvrsgx_cpu_latency *cl)
{
	cancel_delayed_work_sync(&cl->release);
	if (cpu_latency_qos_request_active(&cl->req))
		cpu_latency_qos_remove_request(&cl->req);
	mutex_destroy(&cl->lock);
}

/* What a job holds from submit to free_job: runtime PM and the latency limit. */
static int job_hold(struct pvrsgx_device *sdev)
{
	int err = pvrsgx_power_get(sdev);

	if (!err)
		cpu_latency_get(&sdev->cpu_latency);
	return err;
}

static void job_unhold(struct pvrsgx_device *sdev, u32 n)
{
	cpu_latency_put(&sdev->cpu_latency, n);
	pvrsgx_power_put(sdev, n);
}

static void free_job_work(struct work_struct *work)
{
	struct pvrsgx_job *job = container_of(work, struct pvrsgx_job, free_work);

	pvrsgx_prime_put(job);
	pvrsgx_render_job_fini(job);
	context_put(job->ctx);
	kfree(job);
}

static void pvrsgx_free_job(struct drm_sched_job *sched_job)
{
	struct pvrsgx_job *job = to_pvrsgx_job(sched_job);
	struct pvrsgx_device *sdev = job->ctx->sdev;

	/* Taken at submit, for this job: the GPU may suspend once none are left. */
	job_unhold(sdev, 1);

	drm_sched_job_cleanup(sched_job);
	dma_fence_put(job->hw_fence);
	kfree(job->hw_fence_mem);	/* never run */
	INIT_WORK(&job->free_work, free_job_work);
	queue_work(sdev->free_wq, &job->free_work);
}

static const struct drm_sched_backend_ops pvrsgx_sched_ops = {
	.run_job = pvrsgx_run_job,
	.timedout_job = pvrsgx_timedout_job,
	.free_job = pvrsgx_free_job,
};

/*
 * Teach lockdep that the locks run_job, the completion and the timeout take
 * are taken while fences are pending, which reclaim can wait for: an
 * allocation that can reclaim, made under any of them, is then reported at
 * once instead of when memory runs low.
 */
static void lockdep_prime(struct pvrsgx_device *sdev)
{
	if (!IS_ENABLED(CONFIG_LOCKDEP))
		return;
	fs_reclaim_acquire(GFP_KERNEL);
	might_lock(&sdev->reset_lock);
	might_lock(&sdev->done_lock);
	might_lock(&sdev->ccb_lock);
	might_lock(&sdev->kobj_lock);
	fs_reclaim_release(GFP_KERNEL);
}

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

	int err;

	spin_lock_init(&sdev->fence_lock);
	mutex_init(&sdev->reset_lock);
	INIT_LIST_HEAD(&sdev->active);
	cpu_latency_init(&sdev->cpu_latency, sdev->core->cpu_latency_us);
	INIT_DELAYED_WORK(&sdev->poll_work, poll_work_fn);
	INIT_WORK(&sdev->reap_work, reap_work_fn);
	sdev->wq = alloc_workqueue("powervr_sgx", WQ_MEM_RECLAIM | WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!sdev->wq)
		return -ENOMEM;
	sdev->free_wq = alloc_workqueue("powervr_sgx_free", WQ_UNBOUND, 0);
	if (!sdev->free_wq) {
		destroy_workqueue(sdev->wq);
		return -ENOMEM;
	}
	lockdep_prime(sdev);
	pvrsgx_coredump_init(sdev);
	sdev->fence_context = dma_fence_context_alloc(1);
	err = drm_sched_init(&sdev->sched, &args);
	if (err) {
		pvrsgx_coredump_fini(sdev);
		destroy_workqueue(sdev->free_wq);
		destroy_workqueue(sdev->wq);
	}
	return err;
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
	bool cookie;

	/* A reset under way finishes first, scheduler restart included; one after sees @lost. */
	mutex_lock(&sdev->reset_lock);
	drm_sched_wqueue_stop(&sdev->sched);
	WRITE_ONCE(sdev->lost, true);
	pvrsgx_fw_stop(sdev);

	mutex_lock(&sdev->done_lock);
	cookie = dma_fence_begin_signalling();
	spin_lock_irqsave(&sdev->fence_lock, flags);
	list_for_each_entry_safe(job, tmp, &sdev->active, node) {
		list_del_init(&job->node);
		job_account(job, ktime_get_ns());
		dma_fence_set_error(job->hw_fence, -ENODEV);
		dma_fence_signal_locked(job->hw_fence);
	}
	spin_unlock_irqrestore(&sdev->fence_lock, flags);
	dma_fence_end_signalling(cookie);
	mutex_unlock(&sdev->done_lock);
	/* Nothing on the GPU now: a done check after this finds none and stays off. */
	cancel_delayed_work_sync(&sdev->poll_work);
	drm_sched_wqueue_start(&sdev->sched);
	mutex_unlock(&sdev->reset_lock);

	/*
	 * Stopped: nothing translates any more, so unmapped memory can go, and
	 * every kernel object in the graveyard, whose VMs hold the DRM device.
	 */
	pvrsgx_ccb_inval_reap(sdev, true);
	pvrsgx_kobj_reap(sdev, U64_MAX, U64_MAX);
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

		/* Between resets: none is under way, and none restarts the scheduler after. */
		mutex_lock(&sdev->reset_lock);
		drm_sched_wqueue_stop(&sdev->sched);
		/* Set only by run_job, which is stopped. Jobs finish in order. */
		last = dma_fence_get(sdev->last_fence);
		if (!last || dma_fence_is_signaled(last)) {
			sdev->quiesced = true;
			mutex_unlock(&sdev->reset_lock);
			dma_fence_put(last);
			/* The done check that signaled it has queued its reap. */
			mutex_lock(&sdev->done_lock);
			mutex_unlock(&sdev->done_lock);
			flush_work(&sdev->reap_work);
			return 0;
		}
		mutex_unlock(&sdev->reset_lock);
		left = dma_fence_wait_timeout(last, false, left);
		dma_fence_put(last);
		if (left <= 0)
			break;
	}
	drm_err(&sdev->base, "GPU still busy: not suspending\n");
	drm_sched_wqueue_start(&sdev->sched);
	return -EBUSY;
}

void pvrsgx_job_unquiesce(struct pvrsgx_device *sdev)
{
	mutex_lock(&sdev->reset_lock);
	sdev->quiesced = false;
	drm_sched_wqueue_start(&sdev->sched);
	mutex_unlock(&sdev->reset_lock);
}

void pvrsgx_job_fini(struct pvrsgx_device *sdev)
{
	drm_sched_fini(&sdev->sched);
	cancel_delayed_work_sync(&sdev->poll_work);
	cancel_work_sync(&sdev->reap_work);
	/* Drains the jobs' final puts, which drm_sched_fini may have queued. */
	destroy_workqueue(sdev->free_wq);
	sdev->free_wq = NULL;
	destroy_workqueue(sdev->wq);
	pvrsgx_coredump_fini(sdev);
	dma_fence_put(sdev->last_fence);
	sdev->last_fence = NULL;
	cpu_latency_fini(&sdev->cpu_latency);
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
	    (args->param_buffer_pages < 0x100 ||
	     args->param_buffer_pages > pvrsgx_heaps(to_pvrsgx_device(drm))->pb_max_pages))
		return -EINVAL;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;
	ctx->vm = pvrsgx_vm_lookup_user(sfile, args->vm_context_handle);
	if (!ctx->vm) {
		kfree(ctx);
		return -ENOENT;
	}
	ctx->sdev = sdev;
	ctx->type = args->type;
	ctx->usage = pvrsgx_usage_get(sfile->usage);
	kref_init(&ctx->ref);
	err = ctx->type == DRM_POWERVR_SGX_CTX_TYPE_RENDER ?
	      pvrsgx_render_ctx_init(sdev, ctx, args->param_buffer_pages) :
	      pvrsgx_transfer_ctx_init(sdev, ctx);
	if (err)
		goto err_put;
	err = drm_sched_entity_init(&ctx->entity, sched_priority(args->priority), &sched, 1, NULL);
	if (err)
		goto err_put;
	err = xa_alloc(&sfile->contexts, &args->handle, ctx,
		       XA_LIMIT(1, PVRSGX_MAX_FILE_CONTEXTS), GFP_KERNEL);
	if (err) {
		err = err == -EBUSY ? -ENOSPC : err;
		goto err_entity;
	}
	return 0;

err_entity:
	drm_sched_entity_destroy(&ctx->entity);
err_put:
	pvrsgx_vm_user_put(ctx->vm);
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
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
	struct pvrsgx_context *ctx;

	if (args->_padding_4)
		return -EINVAL;
	/* A submit that found it has pushed its jobs before the entity goes. */
	down_write(&sfile->submit_lock);
	ctx = xa_erase(&sfile->contexts, args->handle);
	up_write(&sfile->submit_lock);
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
/*
 * Whether earlier jobs of this submit signal @handle at @point or past it
 * (point 0: at all); if so, every one of them that signals @handle joins the
 * job's waits. That waits for more than a timeline point needs, never less.
 */
static bool intra_waits(struct job_args *ja, const struct job_args *prev, u32 nprev,
			u32 handle, u64 point)
{
	bool covered = false;

	for (u32 k = 0; k < nprev; k++) {
		for (u32 s = 0; s < prev[k].nsignals; s++) {
			if (prev[k].signals[s].handle == handle &&
			    (!point || prev[k].signals[s].point >= point))
				covered = true;
		}
	}
	if (!covered)
		return false;
	for (u32 k = 0; k < nprev; k++) {
		bool signals = false, listed = false;

		for (u32 s = 0; s < prev[k].nsignals; s++)
			signals |= prev[k].signals[s].handle == handle;
		for (u32 d = 0; d < ja->nintra; d++)
			listed |= ja->intra[d] == k;
		if (signals && !listed)
			ja->intra[ja->nintra++] = k;
	}
	return true;
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
	ja->intra = kvcalloc(max(nprev, 1u), sizeof(*ja->intra), GFP_KERNEL);
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
			/* Their fences exist once those jobs are armed: added then. */
			if (intra_waits(ja, prev, nprev, ops[i].handle, ops[i].value)) {
				/* And whatever the syncobj holds already, earlier points. */
				if (drm_syncobj_find_fence(file, ops[i].handle, 0, 0, &fence))
					continue;
				err = drm_sched_job_add_dependency(&ja->job->base, fence);
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

static int job_create(struct drm_file *file, const struct drm_powervr_sgx_job *in,
		      struct job_args *ja, const struct job_args *prev, u32 nprev)
{
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
	struct pvrsgx_device *sdev = sfile->sdev;
	struct pvrsgx_context *ctx;
	struct pvrsgx_job *job;
	int err;

	if (in->_padding_c || in->_padding_4c)
		return -EINVAL;

	switch (in->type) {
	case DRM_POWERVR_SGX_JOB_TYPE_NULL:
		if (in->flags || in->cmd || in->ta_kicks.count ||
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
	err = pvrsgx_prime_ranges(file, &in->cache_ranges, job);
	if (err)
		return err;
	return pvrsgx_validate_job(sdev, job);
}

int pvrsgx_ioctl_submit_jobs(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_powervr_sgx_ioctl_submit_jobs_args *args = data;
	struct pvrsgx_file *sfile = to_pvrsgx_file(file);
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

	err = down_read_interruptible(&sfile->submit_lock);
	if (err)
		goto out_free;
	for (u32 i = 0; i < n; i++) {
		err = job_create(file, &in[i], &ja[i], ja, i);
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
		err = job_hold(to_pvrsgx_device(drm));
		if (err) {
			job_unhold(to_pvrsgx_device(drm), i);
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
		}
	}

	/*
	 * All or nothing. First each job's waits on earlier jobs of the submit,
	 * then arm it; a wait that can't be added fails the call, and the armed
	 * jobs, never published, are cleaned up unpushed (job_args_fini). Then
	 * publish and push every job, which can't fail. The VM's reservation,
	 * held throughout, keeps the order.
	 */
	for (u32 i = 0; i < n; i++) {
		struct pvrsgx_job *job = ja[i].job;

		for (u32 d = 0; d < ja[i].nintra; d++) {
			err = drm_sched_job_add_dependency(&job->base,
							   dma_fence_get(ja[ja[i].intra[d]].done));
			if (err) {
				args->failed_index = i;
				goto out_exec;
			}
		}
		drm_sched_job_arm(&job->base);
		ja[i].done = dma_fence_get(&job->base.s_fence->finished);
	}

	for (u32 i = 0; i < n; i++) {
		struct pvrsgx_job *job = ja[i].job;
		struct dma_fence *done = ja[i].done;

		dma_resv_add_fence(drm_gpuvm_resv(&job->ctx->vm->gpuvm), done,
				   DMA_RESV_USAGE_BOOKKEEP);
		for (u32 s = 0; s < ja[i].nsignals; s++) {
			struct pvrsgx_signal *sig = &ja[i].signals[s];

			if (sig->chain) {
				drm_syncobj_add_point(sig->syncobj, sig->chain, done, sig->point);
				sig->chain = NULL;
			} else {
				drm_syncobj_replace_fence(sig->syncobj, done);
			}
		}
		drm_sched_entity_push_job(&job->base);
		ja[i].job = NULL;	/* the scheduler owns it now */
		pushed++;
	}

out_exec:
	drm_exec_fini(&exec);
	/* The references of the jobs not pushed. */
	job_unhold(to_pvrsgx_device(drm), n - pushed);
out:
	/* Armed jobs not pushed are cleaned up while their entity still exists. */
	for (u32 i = 0; i < n; i++)
		job_args_fini(&ja[i]);
	up_read(&sfile->submit_lock);
out_free:
	kvfree(ja);
	kvfree(in);
	return err;
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_job_test.c"
#endif
