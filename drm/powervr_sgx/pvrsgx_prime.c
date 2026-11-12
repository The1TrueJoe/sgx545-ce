// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Buffers imported from other devices. The GPU doesn't snoop the CPU caches
 * and can only reach the low 4 GiB, so an import needs a reachability check
 * when it is mapped and cache flushes around every job that might touch it.
 */

#include <drm/drm_cache.h>
#include <drm/drm_gem_shmem_helper.h>

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "pvrsgx_gem.h"
#include "pvrsgx_job.h"
#include "pvrsgx_prime.h"
#include "pvrsgx_trace.h"

/*
 * The imported objects in a job's BO list, each once and referenced, for the
 * job to flush around itself. *out is NULL when there are none.
 */
int pvrsgx_prime_imports(struct drm_gem_object **bos, u32 nbos,
			 struct drm_gem_object ***out, u32 *n)
{
	struct drm_gem_object **objs;
	u32 count = 0;

	*out = NULL;
	*n = 0;
	for (u32 b = 0; b < nbos; b++)
		count += drm_gem_is_imported(bos[b]);
	if (!count)
		return 0;
	objs = kmalloc_array(count, sizeof(*objs), GFP_KERNEL);
	if (!objs)
		return -ENOMEM;
	count = 0;
	for (u32 b = 0; b < nbos; b++) {
		u32 i;

		if (!drm_gem_is_imported(bos[b]))
			continue;
		for (i = 0; i < count && objs[i] != bos[b]; i++)
			;
		if (i < count)
			continue;
		drm_gem_object_get(bos[b]);
		objs[count++] = bos[b];
	}
	*out = objs;
	*n = count;
	return 0;
}

/*
 * The GPU's MMU takes 32-bit addresses. An import was mapped for our device,
 * whose DMA mask is 32 bits, so the DMA API already keeps every address below
 * 4 GiB (bouncing if it must, synced through the dma-buf's CPU access calls);
 * check the addresses only, never the exporter's pages.
 */
bool pvrsgx_prime_reachable(struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_dma_sg(sgt, sg, i) {
		if (upper_32_bits(sg_dma_address(sg) + sg_dma_len(sg) - 1))
			return false;
	}
	return true;
}

/*
 * The GPU's accesses don't snoop the CPU caches, not even through
 * cache-consistent PTEs. Our own objects
 * are write-combined on the CPU; an import may be cached, so flush each import
 * the job lists before it runs (no dirty line lands on its output later) and
 * after (no stale line hides it). Userspace lists every import a job touches.
 * An importer may not touch the attachment's pages, so the flushes go through
 * the exporter's own CPU mapping, taken at import.
 */
static u64 import_flush(struct drm_gem_object *obj)
{
	struct iosys_map *map = &to_pvrsgx_bo(obj)->import_map;

	/* I/O memory isn't cached. */
	if (map->is_iomem)
		return 0;
	drm_clflush_virt_range(map->vaddr, obj->size);
	return obj->size;
}

void pvrsgx_prime_flush(struct pvrsgx_job *job, bool after)
{
	u64 t0 = trace_pvrsgx_flush_imports_enabled() ? ktime_get_ns() : 0, bytes = 0;

	for (u32 i = 0; i < job->nimports; i++)
		bytes += import_flush(job->imports[i]);
	if (job->nimports)
		trace_pvrsgx_flush_imports(job->hw_fence ? job->hw_fence->seqno : 0,
					   job->nimports, bytes,
					   t0 ? ktime_get_ns() - t0 : 0, after);
}

struct drm_gem_object *pvrsgx_prime_import_sg_table(struct drm_device *drm,
						    struct dma_buf_attachment *attach,
						    struct sg_table *sgt)
{
	struct drm_gem_object *obj;
	struct iosys_map map;
	int err;

	err = dma_buf_vmap_unlocked(attach->dmabuf, &map);
	if (err)
		return ERR_PTR(err);
	obj = drm_gem_shmem_prime_import_sg_table(drm, attach, sgt);
	if (IS_ERR(obj)) {
		dma_buf_vunmap_unlocked(attach->dmabuf, &map);
		return obj;
	}
	to_pvrsgx_bo(obj)->import_map = map;
	/*
	 * A listed import is flushed around each job, an unlisted one never: what
	 * the CPU still holds of it (its zeroing, say) reaches memory now, or the
	 * GPU could see the pages' earlier contents.
	 */
	import_flush(obj);
	return obj;
}

void pvrsgx_prime_put(struct pvrsgx_job *job)
{
	for (u32 i = 0; i < job->nimports; i++)
		drm_gem_object_put(job->imports[i]);
	kfree(job->imports);
}
