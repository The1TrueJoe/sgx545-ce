// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Buffers imported from other devices. The GPU doesn't snoop the CPU caches
 * and can only reach the low 4 GiB, so an import needs a reachability check
 * when it is mapped and cache flushes around every job that might touch it.
 */

#include <drm/drm_cache.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_gpuvm.h>

#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "pvrsgx_job.h"
#include "pvrsgx_prime.h"
#include "pvrsgx_trace.h"
#include "pvrsgx_vm.h"

/*
 * The imported objects mapped in vm, each once and referenced, for a job to
 * flush around itself. *out is NULL when there are none.
 */
int pvrsgx_prime_imports(struct pvrsgx_vm *vm, struct drm_gem_object ***out, u32 *n)
{
	struct drm_gem_object **objs = NULL;
	struct drm_gpuva *va;
	u32 count = 0, cap = 0;
	int err = 0;

	mutex_lock(&vm->lock);
	drm_gpuvm_for_each_va(va, &vm->gpuvm) {
		struct drm_gem_object *obj = va->gem.obj;
		u32 i;

		if (!obj || !drm_gem_is_imported(obj))
			continue;
		for (i = 0; i < count && objs[i] != obj; i++)
			;
		if (i < count)
			continue;
		if (count == cap) {
			struct drm_gem_object **more;

			cap = cap ? 2 * cap : 4;
			more = krealloc_array(objs, cap, sizeof(*objs), GFP_KERNEL);
			if (!more) {
				err = -ENOMEM;
				break;
			}
			objs = more;
		}
		drm_gem_object_get(obj);
		objs[count++] = obj;
	}
	mutex_unlock(&vm->lock);

	if (err) {
		while (count)
			drm_gem_object_put(objs[--count]);
		kfree(objs);
		return err;
	}
	*out = objs;
	*n = count;
	return 0;
}

/*
 * An import's pages were mapped for this device with its 32-bit DMA mask.
 * Without an IOMMU a page above 4 GiB comes back as a swiotlb bounce buffer,
 * which the GPU would write behind the exporter's back: refuse those.
 */
bool pvrsgx_prime_reachable(struct device *dev, struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned int i;

	if (device_iommu_mapped(dev))
		return true;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (sg_page(sg) && sg_dma_address(sg) != sg_phys(sg))
			return false;
	}
	return true;
}

/*
 * The GPU's accesses don't snoop the CPU caches (log 2026-11-04-0100). Our own
 * objects are write-combined on the CPU; an import may be cached, so flush
 * every import mapped in the job's VM before it runs (no dirty line lands on
 * its output later) and after (no stale line hides it). Which of them the job
 * touches is not known: a BO list is optional, and explicit sync names none.
 */
void pvrsgx_prime_flush(struct pvrsgx_job *job, bool after)
{
	u64 t0 = trace_pvrsgx_flush_imports_enabled() ? ktime_get_ns() : 0, bytes = 0;

	for (u32 i = 0; i < job->nimports; i++) {
		struct drm_gem_shmem_object *shmem = to_drm_gem_shmem_obj(job->imports[i]);

		if (shmem->sgt) {
			drm_clflush_sg(shmem->sgt);
			bytes += shmem->base.size;
		}
	}
	if (job->nimports)
		trace_pvrsgx_flush_imports(job->hw_fence ? job->hw_fence->seqno : 0,
					   job->nimports, bytes,
					   t0 ? ktime_get_ns() - t0 : 0, after);
}

void pvrsgx_prime_put(struct pvrsgx_job *job)
{
	for (u32 i = 0; i < job->nimports; i++)
		drm_gem_object_put(job->imports[i]);
	kfree(job->imports);
}
