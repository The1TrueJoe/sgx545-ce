// SPDX-License-Identifier: GPL-2.0-only
/* Copyright © 2026 Joseph Telaak */

/*
 * Buffers imported from other devices. The GPU doesn't snoop the CPU caches
 * and can only reach the low 4 GiB, so an import needs a reachability check
 * when it is mapped, and cache flushes of the bytes each job lists
 * (&drm_powervr_sgx_job.cache_ranges) around it.
 */

#include <drm/drm_cache.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include <uapi/drm/powervr_sgx_drm.h>

#include "pvrsgx_drv.h"
#include "pvrsgx_gem.h"
#include "pvrsgx_job.h"
#include "pvrsgx_prime.h"
#include "pvrsgx_trace.h"

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

#define PVRSGX_MAX_CACHE_RANGES	64

#define CACHE_RANGE_FLAGS	(DRM_POWERVR_SGX_CACHE_RANGE_FLAG_READ | \
				 DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE)

/* A range's flags, and its rows inside an object of obj_size bytes. */
static int cache_range_check(const struct drm_powervr_sgx_cache_range *r, u64 obj_size)
{
	u32 rows = max(r->rows, 1u);
	u64 span, end;

	if (!r->flags || (r->flags & ~CACHE_RANGE_FLAGS) || !r->size)
		return -EINVAL;
	if (rows > 1 && r->stride < r->size)
		return -EINVAL;
	if (check_mul_overflow((u64)(rows - 1), (u64)r->stride, &span) ||
	    check_add_overflow(span, r->size, &span) ||
	    check_add_overflow(r->offset, span, &end) || end > obj_size)
		return -EINVAL;
	return 0;
}

/* The job's cache ranges, checked, each holding a reference on its object. */
int pvrsgx_prime_ranges(struct drm_file *file, const struct drm_powervr_sgx_obj_array *arr,
			struct pvrsgx_job *job)
{
	struct drm_powervr_sgx_cache_range *in;
	int err = 0;

	in = pvrsgx_obj_array_get(arr, sizeof(*in), PVRSGX_MAX_CACHE_RANGES);
	if (IS_ERR_OR_NULL(in))
		return PTR_ERR_OR_ZERO(in);
	job->ranges = kvcalloc(arr->count, sizeof(*job->ranges), GFP_KERNEL);
	if (!job->ranges) {
		err = -ENOMEM;
		goto out;
	}
	for (u32 i = 0; i < arr->count; i++) {
		struct pvrsgx_cache_range *r = &job->ranges[i];

		r->obj = drm_gem_object_lookup(file, in[i].handle);
		if (!r->obj) {
			err = -ENOENT;
			break;
		}
		job->nranges++;
		err = cache_range_check(&in[i], r->obj->size);
		if (err)
			break;
		/*
		 * Our own objects are write-combined: nothing to flush. A re-imported
		 * dma-buf of ours comes back as such an object, so it isn't refused.
		 */
		r->import = drm_gem_is_imported(r->obj);
		r->offset = in[i].offset;
		r->size = in[i].size;
		r->stride = in[i].stride;
		r->rows = max(in[i].rows, 1u);
		r->write = in[i].flags & DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE;
	}
out:
	kvfree(in);
	return err;
}

/*
 * The GPU's accesses don't snoop the CPU caches, not even through
 * cache-consistent PTEs. Our own objects are write-combined on the CPU; an
 * import may be cached. Before a job every range it lists is written back and
 * dropped (the GPU reads what the CPU wrote, no dirty line lands on its output
 * later), and after it the ranges it wrote are dropped again (no stale line
 * hides its output). An importer may not touch the attachment's pages, so the
 * flushes go through the exporter's own CPU mapping, taken at import.
 */
static u64 range_flush(const struct pvrsgx_cache_range *r)
{
	const struct iosys_map *map = &to_pvrsgx_bo(r->obj)->import_map;

	/* I/O memory isn't cached. */
	if (!r->import || map->is_iomem)
		return 0;
	/*
	 * Rows less than a page apart: one flush of their span, which the range
	 * check keeps inside the object. Rows further apart are at most one per
	 * page of it, so the loop is bounded by the object's size.
	 */
	if (r->rows == 1 || r->stride - r->size < PAGE_SIZE) {
		u64 span = (u64)(r->rows - 1) * r->stride + r->size;

		drm_clflush_virt_range(map->vaddr + r->offset, span);
		return span;
	}
	for (u32 y = 0; y < r->rows; y++)
		drm_clflush_virt_range(map->vaddr + r->offset + (u64)y * r->stride, r->size);
	return (u64)r->size * r->rows;
}

/* The whole of an import, at import. */
static void import_flush(struct drm_gem_object *obj)
{
	const struct iosys_map *map = &to_pvrsgx_bo(obj)->import_map;

	if (!map->is_iomem)
		drm_clflush_virt_range(map->vaddr, obj->size);
}

void pvrsgx_prime_flush(struct pvrsgx_job *job, bool after)
{
	u64 t0 = trace_pvrsgx_flush_imports_enabled() ? ktime_get_ns() : 0, bytes = 0;

	for (u32 i = 0; i < job->nranges; i++) {
		if (!after || job->ranges[i].write)
			bytes += range_flush(&job->ranges[i]);
	}
	if (job->nranges)
		trace_pvrsgx_flush_imports(job->hw_fence ? job->hw_fence->seqno : 0,
					   job->nranges, bytes,
					   t0 ? ktime_get_ns() - t0 : 0, after);
}

struct drm_gem_object *pvrsgx_prime_import_sg_table(struct drm_device *drm,
						    struct dma_buf_attachment *attach,
						    struct sg_table *sgt)
{
	struct drm_gem_object *obj;
	struct iosys_map map;
	int err;

	/* An import's flushes are CPU cache flushes of its vmap, which only x86 has here. */
	if (!IS_ENABLED(CONFIG_X86))
		return ERR_PTR(-EOPNOTSUPP);
	/*
	 * The exporter's mapping is taken once here and kept for the import's
	 * life. A job's cache ranges are flushed through it in run_job and at
	 * completion, inside the fence-signaling path, where a vmap can't be
	 * taken: it may sleep and allocate. The importer never touches the
	 * attachment's pages.
	 */
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
	 * Only the bytes a job lists are flushed around it: what the CPU still
	 * holds of the rest (its zeroing, say) reaches memory now, or the GPU
	 * could see the pages' earlier contents.
	 */
	import_flush(obj);
	return obj;
}

void pvrsgx_prime_put(struct pvrsgx_job *job)
{
	for (u32 i = 0; i < job->nranges; i++)
		drm_gem_object_put(job->ranges[i].obj);
	kvfree(job->ranges);
}

#if IS_ENABLED(CONFIG_DRM_POWERVR_SGX_KUNIT_TEST)
#include "tests/pvrsgx_prime_test.c"
#endif
