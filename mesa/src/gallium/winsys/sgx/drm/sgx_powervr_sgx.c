/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * sgxws over the powervr_sgx DRM driver (docs/UAPI.md).
 *
 * The kernel owns every structure the microkernel reads: the HW render
 * context and PB (CREATE_CONTEXT), the RT data and render details
 * (CREATE_RT_DATASET), the TA command.  The client keeps what the hardware
 * reads as data, in its own VM: VDM streams, TA state, PDS and USSE programs,
 * vertices, textures, surfaces.  A frame is one SUBMIT_JOBS render job whose
 * client parts are the render and TA kick command structs of the uAPI header
 * (docs/UAPI.md section 3), with the SGX545 extension for the ZLS words.
 *
 * Addresses: the client picks every mapping's address in the heaps
 * DEV_QUERY returns.  USSE code lives in code objects, which have no CPU
 * mapping: the rings keep a copy, and what was written since the last kick
 * is uploaded (UPLOAD_CODE) before it, only ever past what was uploaded
 * before (D12).
 *
 * Completion: every job signals the next point of one timeline syncobj per
 * device.  A context's kick waits on the point its job got; a buffer's CPU
 * wait on the point of the last job that wrote it.  The kernel does no
 * implicit sync: for a shared (imported or exported) buffer the job waits on
 * its dma-buf's fences (DMA_BUF_IOCTL_EXPORT_SYNC_FILE), and its own fence is
 * added to them after the submit (DMA_BUF_IOCTL_IMPORT_SYNC_FILE), so the
 * buffer's other users (X, the display, other clients) order against it.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <xf86drm.h>

#include "drm-uapi/dma-buf.h"
#include "drm-uapi/powervr_sgx_drm.h"
#include "util/libsync.h"
#include "util/macros.h"
#include "util/simple_mtx.h"
#include "util/u_math.h"
#include "util/vma.h"

#include "sgx_frame.h"

/* A scene as two TA kicks: the frame's stream, then one with only the
 * terminate words (what the powervr_sgx tests run: drm/tests) */
#define TA_KICKS 2

/* client heaps, by the uAPI's heap index */
static const enum sgx_heap heap_ids[DRM_POWERVR_SGX_HEAP_COUNT] = {
   [DRM_POWERVR_SGX_HEAP_GENERAL] = SGX_HEAP_GENERAL,
   [DRM_POWERVR_SGX_HEAP_PDS_PIXEL] = SGX_HEAP_PDSPIXEL,
   [DRM_POWERVR_SGX_HEAP_PDS_VERTEX] = SGX_HEAP_PDSVERTEX,
   [DRM_POWERVR_SGX_HEAP_USSE_PIXEL] = SGX_HEAP_PIXSHADER,
   [DRM_POWERVR_SGX_HEAP_USSE_VERTEX] = SGX_HEAP_VTXSHADER,
   [DRM_POWERVR_SGX_HEAP_TEXTURE] = SGX_HEAP_TEXTURE,
};

/* a buffer freed while jobs submitted before may still use it: unmapped
 * once the timeline passes point */
struct zombie {
   uint32_t handle;
   uint64_t va, size;
   enum sgx_heap heap;
   uint64_t point;
};

/* Freed buffers kept for reuse.  glamor makes and frees pixmaps all the
 * time, and a new buffer's first map allocates its pages and makes them
 * write-combined: a cache flush, and a TLB flush on every CPU.  Never a buffer
 * another device or process has seen; handed out again only once the
 * timeline is past every job submitted before it was freed. */
#define PSGX_CACHE_BYTES (32u << 20)
#define PSGX_CACHE_NS 1000000000ull /* idle for longer: released */

struct cached_bo {
   struct psgx_bo *bo;
   uint64_t point;    /* the last job that may still use it */
   uint64_t freed_ns;
};

/* the uAPI this backend speaks: the driver's DRM major version */
#define PSGX_UAPI_MAJOR 1

struct psgx_dev {
   struct sgxws_dev base;
   int fd;
   uint64_t render_exts; /* the render extensions this core accepts */
   bool lost;         /* -EIO: the GPU couldn't be recovered; nothing is sent */
   unsigned pb_pages; /* the parameter buffer's size (0: the kernel's) */
   uint32_t vm;       /* VM context */
   uint32_t timeline; /* syncobj: every job signals the next point */
   /* binary syncobjs: a job's wait on its shared buffers' dma-buf fences,
    * and its fence for them */
   uint32_t in_sync, out_sync;
   simple_mtx_t lock; /* the address space, the timeline's points, the zombies */
   struct util_vma_heap vma[16]; /* by heap id */
   bool has_heap[16];
   uint64_t point; /* the last point a job was given */
   struct zombie *zombies;
   unsigned n_zombies, max_zombies;
   struct psgx_bo **imports; /* imported buffers, to share one GEM handle */
   unsigned n_imports, max_imports;
   struct cached_bo *cache; /* oldest first */
   unsigned n_cache, max_cache;
   uint64_t cache_bytes;
   uint64_t done; /* the timeline's value when last read */
};

struct psgx_bo {
   struct sgxws_bo_impl base;
   struct psgx_dev *dev;
   uint32_t handle;
   uint32_t va;
   enum sgx_heap heap;
   bool code;     /* a code object: map is a copy */
   bool shared;   /* imported or exported: synced through its dma-buf */
   bool seen;     /* its handle went outside the winsys: never cached */
   int dmabuf_fd; /* shared: the dma-buf, for its fences; else -1 */
   unsigned refs; /* imports of the same dma-buf */
   void *map;
   uint64_t write_point; /* the last job that writes it */
};

struct psgx_ctx {
   uint32_t handle;
   uint32_t gen;                    /* kernel contexts made after resets */
   uint64_t point[SGXWS_MAX_SLOTS]; /* by kick number % c->slots */
};

static const struct sgxws_kif psgx_kif;

static struct psgx_dev *
ddev(struct sgxws_dev *d)
{
   return (struct psgx_dev *)d;
}

static struct psgx_bo *
dbo(const struct sgxws_bo *bo)
{
   return (struct psgx_bo *)bo->impl;
}

static int
xioctl(int fd, unsigned long req, void *arg)
{
   return drmIoctl(fd, req, arg) ? -errno : 0;
}

/* ------------------------------------------------------------------ timeline */

/* 0 once the timeline has reached point; timeout 0 polls */
static int
timeline_wait(struct psgx_dev *dd, uint64_t point, unsigned timeout_ms)
{
   int64_t abs = timeout_ms
                    ? (int64_t)(sgxws_now_ns() + (uint64_t)timeout_ms * 1000000)
                    : 0;
   int e;

   if (!point)
      return 0;
   e = drmSyncobjTimelineWait(dd->fd, &dd->timeline, &point, 1, abs, 0, NULL);
   return e == -ETIME ? -ETIMEDOUT : e;
}

static uint64_t
timeline_value(struct psgx_dev *dd)
{
   uint64_t v = 0;

   drmSyncobjQuery(dd->fd, &dd->timeline, &v, 1);
   return v;
}

/* ------------------------------------------------------------------ memory */

/* VM_BIND: ops applied in order, all or nothing; synchronous */
static int
vm_bind(struct psgx_dev *dd, struct drm_powervr_sgx_vm_bind_op *ops, unsigned n)
{
   struct drm_powervr_sgx_ioctl_vm_bind_args a = {
      .vm_context_handle = dd->vm,
      .flags = DRM_POWERVR_SGX_VM_BIND_FLAG_SYNC,
      .ops = {.stride = sizeof(*ops), .count = n, .array = (uintptr_t)ops},
   };

   return xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_VM_BIND, &a);
}

static struct drm_powervr_sgx_vm_bind_op
unmap_op(uint64_t va, uint64_t size)
{
   return (struct drm_powervr_sgx_vm_bind_op){
      .op = DRM_POWERVR_SGX_VM_BIND_OP_UNMAP,
      .device_addr = va,
      .size = size,
   };
}

static void
vm_unmap(struct psgx_dev *dd, uint64_t va, uint64_t size)
{
   struct drm_powervr_sgx_vm_bind_op op = unmap_op(va, size);
   int e = vm_bind(dd, &op, 1);

   if (e)
      mesa_loge("sgx: VM unmap %#llx +%#llx failed: %d", (unsigned long long)va,
                (unsigned long long)size, e);
}

static void
gem_close(struct psgx_dev *dd, uint32_t handle)
{
   struct drm_gem_close a = {.handle = handle};

   xioctl(dd->fd, DRM_IOCTL_GEM_CLOSE, &a);
}

/* unmap and close the zombies the GPU is done with; with the lock held */
static void
reap(struct psgx_dev *dd, bool all)
{
   uint64_t done;
   unsigned n = 0;

   if (!dd->n_zombies)
      return;
   if (all)
      timeline_wait(dd, dd->point, 5000);
   done = timeline_value(dd);
   /* the done ones' unmaps in as few VM_BINDs as the kernel takes */
   {
      struct drm_powervr_sgx_vm_bind_op ops[256];
      unsigned nops = 0;

      for (unsigned i = 0; i < dd->n_zombies; i++) {
         struct zombie *z = &dd->zombies[i];

         if (z->point > done && !all)
            continue;
         ops[nops++] = unmap_op(z->va, z->size);
         if (nops == ARRAY_SIZE(ops)) {
            if (vm_bind(dd, ops, nops))
               mesa_loge("sgx: VM unmap of %u buffers failed", nops);
            nops = 0;
         }
      }
      if (nops && vm_bind(dd, ops, nops))
         mesa_loge("sgx: VM unmap of %u buffers failed", nops);
   }
   for (unsigned i = 0; i < dd->n_zombies; i++) {
      struct zombie *z = &dd->zombies[i];

      if (z->point > done && !all) {
         dd->zombies[n++] = *z;
         continue;
      }
      gem_close(dd, z->handle);
      util_vma_heap_free(&dd->vma[z->heap], z->va, z->size);
   }
   dd->n_zombies = n;
}

/* a buffer's handle, address and mapping go; with the lock held.  Its unmap
 * and close wait for the jobs up to point. */
static void
bo_release(struct psgx_dev *dd, struct psgx_bo *impl, uint64_t point)
{
   if (dd->n_zombies == dd->max_zombies) {
      unsigned max = dd->max_zombies ? 2 * dd->max_zombies : 64;
      struct zombie *z = realloc(dd->zombies, max * sizeof(*z));

      if (!z) {
         /* nowhere to keep it: wait for the GPU instead */
         reap(dd, true);
         timeline_wait(dd, dd->point, 5000);
      } else {
         dd->zombies = z;
         dd->max_zombies = max;
      }
   }
   if (dd->n_zombies < dd->max_zombies) {
      dd->zombies[dd->n_zombies++] = (struct zombie){
         impl->handle, impl->va, impl->base.size, impl->heap, point,
      };
   } else {
      vm_unmap(dd, impl->va, impl->base.size);
      gem_close(dd, impl->handle);
      util_vma_heap_free(&dd->vma[impl->heap], impl->va, impl->base.size);
   }
   if (impl->code)
      free(impl->map);
   else if (impl->map)
      munmap(impl->map, impl->base.size);
   if (impl->dmabuf_fd >= 0)
      close(impl->dmabuf_fd);
   free(impl);
}

/* the cached buffers idle for too long go, and the oldest while over keep
 * bytes; with the lock held */
static void
cache_trim(struct psgx_dev *dd, uint64_t keep)
{
   uint64_t now = sgxws_now_ns();
   unsigned n = 0;

   while (n < dd->n_cache &&
          (dd->cache_bytes > keep ||
           now - dd->cache[n].freed_ns > PSGX_CACHE_NS)) {
      dd->cache_bytes -= dd->cache[n].bo->base.size;
      bo_release(dd, dd->cache[n].bo, dd->cache[n].point);
      n++;
   }
   if (n) {
      memmove(dd->cache, dd->cache + n, (dd->n_cache - n) * sizeof(*dd->cache));
      dd->n_cache -= n;
   }
}

/* allocation sizes above 16 KiB in quarter steps of a power of two, so a
 * freed buffer fits the next one of about its size (25% at most to spare) */
static uint64_t
bucket_size(uint64_t bytes)
{
   if (bytes <= 0x4000)
      return bytes;
   return ALIGN_POT(bytes, 1ull << (util_logbase2_64(bytes) - 2));
}

/* a cached buffer of exactly this heap, size and alignment that no job uses
 * any more, newest first; with the lock held */
static struct psgx_bo *
cache_take(struct psgx_dev *dd, enum sgx_heap heap, uint64_t bytes,
           uint64_t align)
{
   bool read = false;

   cache_trim(dd, PSGX_CACHE_BYTES);
   for (unsigned i = dd->n_cache; i-- > 0;) {
      struct cached_bo *c = &dd->cache[i];
      struct psgx_bo *impl = c->bo;

      if (impl->heap != heap || impl->base.size != bytes ||
          (impl->va & (align - 1)))
         continue;
      /* idle by the timeline, not by assumption */
      if (c->point > dd->done && !read) {
         dd->done = timeline_value(dd);
         read = true;
      }
      if (c->point > dd->done) {
         dd->base.bo_busy++;
         continue;
      }
      dd->cache_bytes -= bytes;
      memmove(c, c + 1, (dd->n_cache - i - 1) * sizeof(*c));
      dd->n_cache--;
      return impl;
   }
   return NULL;
}

/* a freed buffer into the cache, or false; with the lock held */
static bool
cache_put(struct psgx_dev *dd, struct psgx_bo *impl)
{
   if (impl->seen || impl->shared || impl->dmabuf_fd >= 0 || impl->code ||
       impl->base.size > PSGX_CACHE_BYTES / 4)
      return false;
   if (dd->n_cache == dd->max_cache) {
      unsigned max = dd->max_cache ? 2 * dd->max_cache : 64;
      struct cached_bo *c = realloc(dd->cache, max * sizeof(*c));

      if (!c)
         return false;
      dd->cache = c;
      dd->max_cache = max;
   }
   dd->cache[dd->n_cache++] = (struct cached_bo){
      impl, dd->point, sgxws_now_ns(),
   };
   dd->cache_bytes += impl->base.size;
   cache_trim(dd, PSGX_CACHE_BYTES);
   return true;
}

/* an address for size bytes in a heap; with the lock held */
static uint64_t
va_alloc(struct psgx_dev *dd, enum sgx_heap heap, uint64_t size, uint64_t align)
{
   uint64_t va;

   reap(dd, false);
   va = util_vma_heap_alloc(&dd->vma[heap], size, align);
   if (!va && (dd->n_zombies || dd->n_cache)) {
      cache_trim(dd, 0); /* memory pressure: the cache goes */
      reap(dd, true);
      va = util_vma_heap_alloc(&dd->vma[heap], size, align);
   }
   return va;
}

static int
vm_map(struct psgx_dev *dd, uint32_t handle, uint64_t va, uint64_t size)
{
   struct drm_powervr_sgx_vm_bind_op op = {
      .op = DRM_POWERVR_SGX_VM_BIND_OP_MAP,
      .handle = handle,
      .device_addr = va,
      .size = size,
   };

   return vm_bind(dd, &op, 1);
}

static int
create_bo(struct psgx_dev *dd, uint64_t size, bool code, uint32_t *handle)
{
   struct drm_powervr_sgx_ioctl_create_bo_args a = {
      .size = size,
      .flags =
         code ? DRM_POWERVR_SGX_BO_CODE : DRM_POWERVR_SGX_BO_ALLOW_CPU_ACCESS,
   };
   int e = xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_CREATE_BO, &a);

   *handle = a.handle;
   return e;
}

static int
psgx_bo_alloc(struct sgxws_dev *d, struct sgxws_bo *bo, enum sgx_heap heap,
              uint32_t attribs, uint32_t size, uint32_t align)
{
   struct psgx_dev *dd = ddev(d);
   struct psgx_bo *impl;
   uint64_t bytes = ((uint64_t)size + 0xfff) & ~0xfffull, va;
   bool code = heap == SGX_HEAP_PIXSHADER || heap == SGX_HEAP_VTXSHADER;
   int e;

   (void)attribs;
   /* textures go to the general heap when there is no texture heap (TEX.md:
    * convention only) */
   if (heap == SGX_HEAP_TEXTURE && !dd->has_heap[heap])
      heap = SGX_HEAP_GENERAL;
   if (heap >= 16 || !dd->has_heap[heap])
      return -EINVAL;
   d->bo_allocs++;
   if (!code) {
      bytes = bucket_size(bytes);
      simple_mtx_lock(&dd->lock);
      impl = cache_take(dd, heap, bytes, align > 0x1000 ? align : 0x1000);
      simple_mtx_unlock(&dd->lock);
      if (impl) {
         d->bo_cached++;
         impl->write_point = 0;
         impl->base.cpu_dirty = false;
         /* zero as new pages are, but textures: their contents are
          * undefined until written, and they are the big ones */
         if (heap != SGX_HEAP_TEXTURE)
            memset(impl->map, 0, bytes);
         bo->impl = &impl->base;
         bo->va = impl->va;
         bo->map = impl->map;
         return 0;
      }
   }
   if (!(impl = calloc(1, sizeof(*impl))))
      return -ENOMEM;
   impl->base.kif = &psgx_kif;
   impl->base.size = (uint32_t)bytes;
   impl->dev = dd;
   impl->heap = heap;
   impl->code = code;
   impl->dmabuf_fd = -1;
   impl->refs = 1;
   e = create_bo(dd, bytes, code, &impl->handle);
   if (e && dd->n_cache) {
      /* memory pressure: the cache goes */
      simple_mtx_lock(&dd->lock);
      cache_trim(dd, 0);
      reap(dd, true);
      simple_mtx_unlock(&dd->lock);
      e = create_bo(dd, bytes, code, &impl->handle);
   }
   if (e) {
      mesa_loge("sgx: CREATE_BO %#llx failed: %d", (unsigned long long)bytes,
                e);
      free(impl);
      return e;
   }
   if (code) {
      impl->map = calloc(1, bytes);
      e = impl->map ? 0 : -ENOMEM;
   } else {
      struct drm_powervr_sgx_ioctl_get_bo_mmap_offset_args mo = {
         .handle = impl->handle};

      if (!(e =
               xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_GET_BO_MMAP_OFFSET, &mo))) {
         impl->map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                          dd->fd, mo.offset);
         if (impl->map == MAP_FAILED) {
            impl->map = NULL;
            e = -errno;
         }
      }
   }
   if (e)
      goto fail_bo;

   simple_mtx_lock(&dd->lock);
   va = va_alloc(dd, heap, bytes, align > 0x1000 ? align : 0x1000);
   simple_mtx_unlock(&dd->lock);
   if (!va) {
      /* (a full texture heap is the caller's to fall back from) */
      if (heap != SGX_HEAP_TEXTURE)
         mesa_loge("sgx: heap %u has no room for %#llx bytes", heap,
                   (unsigned long long)bytes);
      e = -ENOMEM;
      goto fail_map;
   }
   if ((e = vm_map(dd, impl->handle, va, bytes))) {
      mesa_loge("sgx: VM_BIND map of heap %u at %#llx +%#llx failed: %d", heap,
                (unsigned long long)va, (unsigned long long)bytes, e);
      simple_mtx_lock(&dd->lock);
      util_vma_heap_free(&dd->vma[heap], va, bytes);
      simple_mtx_unlock(&dd->lock);
      goto fail_map;
   }
   impl->va = (uint32_t)va;
   bo->impl = &impl->base;
   bo->va = impl->va;
   bo->map = impl->map;
   if (d->verbose)
      mesa_logi("sgx: heap %2u %#010x +%#x%s", heap, bo->va, size,
                code ? " (code)" : "");
   return 0;

fail_map:
   if (code)
      free(impl->map);
   else if (impl->map)
      munmap(impl->map, bytes);
fail_bo:
   gem_close(dd, impl->handle);
   free(impl);
   return e;
}

static void
psgx_bo_free(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   struct psgx_dev *dd = ddev(d);
   struct psgx_bo *impl = dbo(bo);

   simple_mtx_lock(&dd->lock);
   if (--impl->refs) {
      simple_mtx_unlock(&dd->lock);
      return;
   }
   if (impl->dmabuf_fd >= 0) {
      for (unsigned i = 0; i < dd->n_imports; i++) {
         if (dd->imports[i] == impl) {
            dd->imports[i] = dd->imports[--dd->n_imports];
            break;
         }
      }
   }
   if (!cache_put(dd, impl))
      bo_release(dd, impl, dd->point);
   reap(dd, false);
   simple_mtx_unlock(&dd->lock);
}

static int
psgx_bo_import(struct sgxws_dev *d, struct sgxws_bo *bo, int fd, uint32_t size)
{
   struct psgx_dev *dd = ddev(d);
   struct psgx_bo *impl;
   uint32_t handle;
   off_t bytes = lseek(fd, 0, SEEK_END);
   uint64_t va;
   int e;

   if (bytes <= 0 || (uint64_t)size > (uint64_t)bytes || bytes > 0x40000000)
      return -EINVAL;
   if (drmPrimeFDToHandle(dd->fd, fd, &handle))
      return -errno;

   simple_mtx_lock(&dd->lock);
   /* the same dma-buf again: the same GEM handle, so the same buffer */
   for (unsigned i = 0; i < dd->n_imports; i++) {
      if (dd->imports[i]->handle == handle) {
         impl = dd->imports[i];
         impl->refs++;
         simple_mtx_unlock(&dd->lock);
         bo->impl = &impl->base;
         bo->va = impl->va;
         bo->map = impl->map;
         return 0;
      }
   }
   simple_mtx_unlock(&dd->lock);

   if (!(impl = calloc(1, sizeof(*impl)))) {
      gem_close(dd, handle);
      return -ENOMEM;
   }
   impl->base.kif = &psgx_kif;
   impl->base.size = (uint32_t)bytes;
   impl->dev = dd;
   impl->handle = handle;
   /* an imported buffer may be sampled (a pixmap): the texture heap, as
    * sgxws_bo_alloc's buffers */
   impl->heap =
      dd->has_heap[SGX_HEAP_TEXTURE] ? SGX_HEAP_TEXTURE : SGX_HEAP_GENERAL;
   impl->shared = true;
   impl->refs = 1;
   impl->base.cpu_dirty = true; /* the CPU's lines before it was ours */
   impl->dmabuf_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
   impl->map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (impl->map == MAP_FAILED) {
      e = -errno;
      mesa_loge("sgx: mmap of an imported dma-buf failed: %d", e);
      goto fail;
   }
   simple_mtx_lock(&dd->lock);
   va = va_alloc(dd, impl->heap, bytes, 0x1000);
   if (!va && impl->heap != SGX_HEAP_GENERAL) {
      impl->heap = SGX_HEAP_GENERAL;
      va = va_alloc(dd, impl->heap, bytes, 0x1000);
   }
   simple_mtx_unlock(&dd->lock);
   if (!va) {
      e = -ENOMEM;
      goto fail_map;
   }
   if ((e = vm_map(dd, handle, va, bytes))) {
      mesa_loge("sgx: VM_BIND map of an imported dma-buf failed: %d", e);
      simple_mtx_lock(&dd->lock);
      util_vma_heap_free(&dd->vma[impl->heap], va, bytes);
      simple_mtx_unlock(&dd->lock);
      goto fail_map;
   }
   impl->va = (uint32_t)va;
   simple_mtx_lock(&dd->lock);
   if (dd->n_imports == dd->max_imports) {
      unsigned max = dd->max_imports ? 2 * dd->max_imports : 16;
      struct psgx_bo **p = realloc(dd->imports, max * sizeof(*p));

      if (p) {
         dd->imports = p;
         dd->max_imports = max;
      }
   }
   if (dd->n_imports < dd->max_imports)
      dd->imports[dd->n_imports++] = impl;
   simple_mtx_unlock(&dd->lock);
   bo->impl = &impl->base;
   bo->va = impl->va;
   bo->map = impl->map;
   return 0;

fail_map:
   munmap(impl->map, bytes);
fail:
   if (impl->dmabuf_fd >= 0)
      close(impl->dmabuf_fd);
   gem_close(dd, handle);
   free(impl);
   return e;
}

static int
psgx_bo_export(struct sgxws_dev *d, const struct sgxws_bo *bo, int *fd)
{
   struct psgx_bo *impl = dbo(bo);

   if (impl->code)
      return -EINVAL;
   simple_mtx_lock(&ddev(d)->lock);
   impl->seen = true;
   simple_mtx_unlock(&ddev(d)->lock);
   if (drmPrimeHandleToFD(ddev(d)->fd, impl->handle, DRM_CLOEXEC | DRM_RDWR,
                          fd))
      return -errno;
   simple_mtx_lock(&ddev(d)->lock);
   if (impl->dmabuf_fd < 0)
      impl->dmabuf_fd = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
   impl->shared = impl->dmabuf_fd >= 0;
   simple_mtx_unlock(&ddev(d)->lock);
   return impl->shared ? 0 : -errno;
}

static int
psgx_bo_kms_handle(const struct sgxws_bo *bo, uint32_t *handle)
{
   struct psgx_bo *impl = dbo(bo);

   /* the display device holds it now: never cached */
   simple_mtx_lock(&impl->dev->lock);
   impl->seen = true;
   simple_mtx_unlock(&impl->dev->lock);
   *handle = impl->handle;
   return 0;
}

static int
psgx_bo_wait(const struct sgxws_bo *bo, unsigned timeout_ms)
{
   struct psgx_bo *impl = dbo(bo);
   int e = timeline_wait(impl->dev, impl->write_point, timeout_ms);

   /* and whoever else writes an imported buffer: its dma-buf's write fences */
   if (!e && impl->dmabuf_fd >= 0) {
      struct pollfd p = {.fd = impl->dmabuf_fd, .events = POLLIN};

      if (poll(&p, 1, (int)timeout_ms) == 0)
         e = -ETIMEDOUT;
   }
   return e;
}

static int
psgx_code_upload(struct sgxws_dev *d, struct sgxws_bo *bo, uint32_t off,
                 uint32_t size)
{
   struct psgx_bo *impl = dbo(bo);
   struct drm_powervr_sgx_ioctl_upload_code_args a = {
      .handle = impl->handle,
      .offset = off,
      .size = size,
      .pointer = (uintptr_t)((uint8_t *)impl->map + off),
   };
   int e = xioctl(ddev(d)->fd, DRM_IOCTL_POWERVR_SGX_UPLOAD_CODE, &a);

   if (e)
      mesa_loge(
         "sgx: UPLOAD_CODE at %#x +%#x: %d, first refused instruction at %#x: %08x %08x",
         impl->va + off, size, e, impl->va + (uint32_t)a.bad_offset,
         ((uint32_t *)impl->map)[a.bad_offset / 4 + 1],
         ((uint32_t *)impl->map)[a.bad_offset / 4]);
   return e;
}

/* A new code object at the same address: the old one may only be appended to
 * while it is mapped.  The caller made sure no job runs code from it. */
static int
psgx_code_reset(struct sgxws_dev *d, struct sgxws_bo *bo)
{
   struct psgx_dev *dd = ddev(d);
   struct psgx_bo *impl = dbo(bo);
   uint32_t handle;
   int e;

   if ((e = create_bo(dd, impl->base.size, true, &handle)))
      return e;
   vm_unmap(dd, impl->va, impl->base.size);
   gem_close(dd, impl->handle);
   impl->handle = handle;
   return vm_map(dd, handle, impl->va, impl->base.size);
}

/* ------------------------------------------------------------------ context */

static int
create_context(struct psgx_dev *dd, int priority, uint32_t *handle)
{
   struct drm_powervr_sgx_ioctl_create_context_args a = {
      .type = DRM_POWERVR_SGX_CTX_TYPE_RENDER,
      .priority = priority > 0   ? DRM_POWERVR_SGX_CTX_PRIORITY_HIGH
                  : priority < 0 ? DRM_POWERVR_SGX_CTX_PRIORITY_LOW
                                 : DRM_POWERVR_SGX_CTX_PRIORITY_NORMAL,
      .vm_context_handle = dd->vm,
      .param_buffer_pages = dd->pb_pages,
   };
   int e = xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_CREATE_CONTEXT, &a);

   /* high priority needs CAP_SYS_NICE: without it, normal */
   if ((e == -EACCES || e == -EPERM) && priority > 0)
      return create_context(dd, 0, handle);
   if (e)
      mesa_loge("sgx: CREATE_CONTEXT failed: %d", e);
   *handle = a.handle;
   return e;
}

static int
psgx_ctx_init(struct sgxws_ctx *c)
{
   struct psgx_ctx *k = calloc(1, sizeof(*k));
   int e;

   if (!k)
      return -ENOMEM;
   if ((e = create_context(ddev(c->dev), c->priority, &k->handle))) {
      free(k);
      return e;
   }
   c->k = k;
   return 0;
}

/* A context a reset made unusable (its submissions fail with -ECANCELED):
 * what the reset was to it, then a new kernel context in its place, whose
 * RT datasets are made again at their next render (UAPI.md,
 * GET_CONTEXT_STATE).  Called with dd->lock held. */
static void
replace_context(struct sgxws_ctx *c)
{
   struct psgx_dev *dd = ddev(c->dev);
   struct psgx_ctx *k = c->k;
   struct drm_powervr_sgx_ioctl_get_context_state_args st = {.handle =
                                                                k->handle};
   struct drm_powervr_sgx_ioctl_destroy_context_args d = {.handle = k->handle};
   uint32_t handle;

   if (!xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_GET_CONTEXT_STATE, &st) &&
       st.state == DRM_POWERVR_SGX_CONTEXT_STATE_INNOCENT) {
      if (c->reset != SGXWS_RESET_GUILTY)
         c->reset = SGXWS_RESET_INNOCENT;
   } else {
      c->reset = SGXWS_RESET_GUILTY;
   }
   mesa_loge(
      "sgx: GPU reset (%s): a new context; the frames in flight are lost",
      c->reset == SGXWS_RESET_GUILTY ? "this context's job"
                                     : "another context's job");
   xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT, &d);
   if (create_context(dd, c->priority, &handle)) {
      dd->lost = true;
      c->reset = SGXWS_RESET_LOST;
      return;
   }
   k->handle = handle;
   k->gen++;
}

static void
psgx_ctx_fini(struct sgxws_ctx *c)
{
   struct psgx_ctx *k = c->k;
   struct drm_powervr_sgx_ioctl_destroy_context_args a = {.handle = k->handle};
   int e = xioctl(ddev(c->dev)->fd, DRM_IOCTL_POWERVR_SGX_DESTROY_CONTEXT, &a);

   if (e)
      mesa_loge("sgx: DESTROY_CONTEXT failed: %d", e);
   free(k);
}

static int
psgx_rt_init(struct sgxws_ctx *c, struct sgxws_rt *rt)
{
   struct psgx_ctx *k = c->k;
   struct drm_powervr_sgx_ioctl_create_rt_dataset_args a = {
      .context_handle = k->handle,
      .width = rt->w,
      .height = rt->h,
      .samples = 1,
   };
   int e =
      xioctl(ddev(c->dev)->fd, DRM_IOCTL_POWERVR_SGX_CREATE_RT_DATASET, &a);

   if (e) {
      mesa_loge("sgx: CREATE_RT_DATASET %ux%u failed: %d", rt->w, rt->h, e);
      return e;
   }
   rt->k = (void *)(uintptr_t)a.handle;
   rt->ctx_gen = k->gen;
   return 0;
}

static void
psgx_rt_fini(struct sgxws_ctx *c, struct sgxws_rt *rt)
{
   struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args a = {
      .handle = (uint32_t)(uintptr_t)rt->k};
   int e =
      xioctl(ddev(c->dev)->fd, DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET, &a);

   /* one of a context a reset replaced went with it */
   if (e && rt->ctx_gen == ((struct psgx_ctx *)c->k)->gen)
      mesa_loge("sgx: DESTROY_RT_DATASET failed: %d", e);
}

/* ------------------------------------------------------------------ submission
 */

struct shared_use {
   struct psgx_bo *bo;
   bool write;
};

static void
add_shared(struct shared_use *sh, unsigned *n, const struct sgxws_bo *bo,
           bool write)
{
   struct psgx_bo *impl = dbo(bo);

   if (!impl || !impl->shared)
      return;
   for (unsigned i = 0; i < *n; i++) {
      if (sh[i].bo == impl) {
         sh[i].write |= write;
         return;
      }
   }
   sh[(*n)++] = (struct shared_use){impl, write};
}

/* the fences a job must wait for on its shared buffers, in dd->in_sync:
 * writers for a read, everyone for a write.  False when there are none. */
static bool
shared_wait(struct psgx_dev *dd, const struct shared_use *sh, unsigned n)
{
   int fd = -1;

   for (unsigned i = 0; i < n; i++) {
      struct dma_buf_export_sync_file x = {
         .flags = sh[i].write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ,
         .fd = -1,
      };

      if (drmIoctl(sh[i].bo->dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &x))
         continue;
      sync_accumulate("sgx", &fd, x.fd);
      close(x.fd);
   }
   if (fd < 0)
      return false;
   if (drmSyncobjImportSyncFile(dd->fd, dd->in_sync, fd)) {
      close(fd);
      return false;
   }
   close(fd);
   return true;
}

/* the job's fence (dd->out_sync) onto its shared buffers */
static void
shared_signal(struct psgx_dev *dd, const struct shared_use *sh, unsigned n)
{
   int fd = -1;

   if (!n || drmSyncobjExportSyncFile(dd->fd, dd->out_sync, &fd))
      return;
   for (unsigned i = 0; i < n; i++) {
      struct dma_buf_import_sync_file x = {
         .flags = sh[i].write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ,
         .fd = fd,
      };

      if (drmIoctl(sh[i].bo->dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &x))
         mesa_loge("sgx: IMPORT_SYNC_FILE failed: %d", -errno);
   }
   close(fd);
}

/* The bytes of shared buffers the kernel keeps coherent with the CPU around
 * the job (UAPI.md D6): the frame's damage of a linear target, which it writes;
 * the whole of any other buffer it writes, or of one the CPU wrote since a job
 * last flushed it.  A buffer the job only reads, clean, needs nothing: the GPU
 * reads memory.  The render rewrites the target's other tiles as it loaded
 * them, which needs no flush either. */
static unsigned
cache_ranges(struct sgxws_ctx *c, const struct shared_use *sh, unsigned n_sh,
             struct drm_powervr_sgx_cache_range *r)
{
   const struct psgx_bo *target = c->target.bo ? dbo(c->target.bo) : NULL;
   const struct sgxws_rect *d = &c->damage;
   unsigned n = 0;

   for (unsigned i = 0; i < n_sh; i++) {
      struct psgx_bo *bo = sh[i].bo;
      uint32_t whole = 0;

      if (bo->base.cpu_dirty)
         whole |= DRM_POWERVR_SGX_CACHE_RANGE_FLAG_READ;
      if (bo == target && !sgxws_target_twiddled(&c->target)) {
         uint32_t pitch = c->target.stride * 4,
                  x1 = MIN2(d->x1, c->rt->w), y1 = MIN2(d->y1, c->rt->h);

         if (c->draws && x1 > d->x0 && y1 > d->y0)
            r[n++] = (struct drm_powervr_sgx_cache_range){
               .handle = bo->handle,
               .flags = DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE,
               .offset = (uint64_t)(c->target.va - bo->va) +
                         (uint64_t)d->y0 * pitch + d->x0 * 4,
               .size = (x1 - d->x0) * 4,
               .stride = pitch,
               .rows = y1 - d->y0,
            };
      } else if (sh[i].write) {
         whole |= DRM_POWERVR_SGX_CACHE_RANGE_FLAG_WRITE;
      }
      if (whole)
         r[n++] = (struct drm_powervr_sgx_cache_range){
            .handle = bo->handle,
            .flags = whole,
            .size = bo->base.size,
         };
      bo->base.cpu_dirty = false;
   }
   return n;
}

static int
psgx_kick(struct sgxws_ctx *c, uint32_t seq)
{
   struct psgx_dev *dd = ddev(c->dev);
   struct psgx_ctx *k = c->k;
   uint32_t pds = c->dev->heap_base[SGX_HEAP_PDSPIXEL];
   struct drm_powervr_sgx_cmd_render render = {
      .head.size = sizeof(render),
      .pixel_event_pds_addr = c->pixev,
      .pixel_event_pds_rows = c->pixev_rows,
      .bg_pds_secondary_addr = pds + ((c->bg_pds[0] & 0x3ffffff) << 4),
      .bg_pds_secondary_rows = c->bg_pds[0] >> 26,
      .bg_pds_primary_addr = pds + ((c->bg_pds[2] & 0x3ffffff) << 4),
      .bg_pds_primary_rows = c->bg_pds[2] >> 26,
      .bg_depth_f32 = sgxws_f32(1.0f),
      .depth_bias_units = c->bias_units,
      .depth_bias_factor_f32 = sgxws_f32(c->bias_factor),
   };
   struct drm_powervr_sgx_ext_sgx545_render zls = {
      .base.name = DRM_POWERVR_SGX_EXT_SGX545_RENDER,
   };
   uint32_t pad = sgxws_zls_pixels(c->rt->w, c->rt->h);
   struct drm_powervr_sgx_cmd_ta_kick kicks[TA_KICKS];
   struct shared_use sh[ARRAY_SIZE(c->use) + 1];
   struct drm_powervr_sgx_cache_range ranges[ARRAY_SIZE(sh) + 1];
   struct drm_powervr_sgx_sync_op ops[3];
   struct drm_powervr_sgx_job job;
   struct drm_powervr_sgx_ioctl_submit_jobs_args sj = {
      .jobs = {.stride = sizeof(job), .count = 1, .array = (uintptr_t)&job},
   };
   unsigned n_sh = 0, n_ops = 0, n_ranges;
   uint64_t point, t0;
   int e = 0;

   /* an RT dataset of the context a reset replaced: made again (the old one
    * went with its context) */
   if (c->rt->ctx_gen != k->gen && !dd->lost) {
      struct drm_powervr_sgx_ioctl_destroy_rt_dataset_args d = {
         .handle = (uint32_t)(uintptr_t)c->rt->k};

      xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_DESTROY_RT_DATASET, &d);
      if ((e = psgx_rt_init(c, c->rt)))
         dd->lost = true;
   }

   /* the scene's VDM stream (it ends with the terminate words), then one of
    * the terminate words alone */
   for (unsigned i = 0; i < TA_KICKS; i++) {
      kicks[i] = (struct drm_powervr_sgx_cmd_ta_kick){
         .head.size = sizeof(kicks[i]),
         .vdm_base = c->vdm_start,
      };
      if (i) {
         kicks[i].vdm_base = c->vdm.bo.va + ((c->vdm.used + 3) & ~3u);
         sgxws_vdm_end(c);
      }
   }
   if (c->target.zls_va &&
       (dd->render_exts & BITFIELD64_BIT(DRM_POWERVR_SGX_EXT_SGX545_RENDER))) {
      /* the kernel adds the tile count and the ZLS base */
      render.depth_addr = c->target.zls_va;
      render.stencil_addr = c->target.zls_va + pad * 4;
      zls.zls_control = SGXWS_ZLS_BASE | SGXWS_ZLS_STORE |
                        (c->target.zls_load ? SGXWS_ZLS_LOAD : 0);
      /* the 3D flags of a render with a depth buffer (kick b19-20) [D] */
      zls.flags_3d = 0x180;
      render.head.extensions = (uintptr_t)&zls;
   }

   if (c->target.bo)
      add_shared(sh, &n_sh, c->target.bo, true);
   for (unsigned i = 0; i < c->n_use; i++)
      add_shared(sh, &n_sh, c->use[i].bo, c->use[i].write);

   n_ranges = cache_ranges(c, sh, n_sh, ranges);

   simple_mtx_lock(&dd->lock);
   point = ++dd->point;
   ops[n_ops++] = (struct drm_powervr_sgx_sync_op){
      .handle = dd->timeline,
      .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ |
               DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL,
      .value = point,
   };
   if (n_sh) {
      if (shared_wait(dd, sh, n_sh))
         ops[n_ops++] = (struct drm_powervr_sgx_sync_op){
            .handle = dd->in_sync,
            .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ |
                     DRM_POWERVR_SGX_SYNC_OP_FLAG_WAIT,
         };
      ops[n_ops++] = (struct drm_powervr_sgx_sync_op){
         .handle = dd->out_sync,
         .flags = DRM_POWERVR_SGX_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ |
                  DRM_POWERVR_SGX_SYNC_OP_FLAG_SIGNAL,
      };
   }
   job = (struct drm_powervr_sgx_job){
      .type = DRM_POWERVR_SGX_JOB_TYPE_RENDER,
      .context_handle = k->handle,
      .cmd = (uintptr_t)&render,
      .sync_ops = {.stride = sizeof(ops[0]),
                   .count = n_ops,
                   .array = (uintptr_t)ops},
      .cache_ranges = {.stride = sizeof(ranges[0]),
                       .count = n_ranges,
                       .array = (uintptr_t)ranges},
      .ta_kicks = {.stride = sizeof(kicks[0]),
                   .count = TA_KICKS,
                   .array = (uintptr_t)kicks},
      .rt_dataset_handle = (uint32_t)(uintptr_t)c->rt->k,
   };
   t0 = sgxws_now_ns();
   if (dd->lost) {
      e = -EIO; /* nothing reaches a lost device */
   } else if (c->dev->dry_run >= 0 && c->frames >= (unsigned)c->dev->dry_run) {
      e = -ECANCELED; /* built, not sent */
   } else if ((e = xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_SUBMIT_JOBS, &sj))) {
      if (e == -ECANCELED) {
         replace_context(c);
      } else if (e == -EIO) {
         mesa_loge("sgx: the GPU is lost: nothing more is rendered");
         dd->lost = true;
         c->reset = SGXWS_RESET_LOST;
      } else {
         mesa_loge("sgx: SUBMIT_JOBS failed: %d", e);
      }
   }
   c->ns_kick += sgxws_now_ns() - t0;
   /* a job that never ran still gets its point, so waits on it end */
   if (e)
      drmSyncobjTimelineSignal(dd->fd, &dd->timeline, &point, 1);
   else
      shared_signal(dd, sh, n_sh);
   simple_mtx_unlock(&dd->lock);

   k->point[seq % c->slots] = point;
   if (c->target.bo && c->target.bo->impl)
      dbo(c->target.bo)->write_point = point;
   for (unsigned i = 0; i < c->n_use; i++)
      if (c->use[i].write)
         dbo(c->use[i].bo)->write_point = point;
   /* a lost frame isn't an error to the caller: the reset is reported
    * through sgxws_reset_status */
   return e == -ECANCELED || e == -EIO ? 0 : e;
}

static int
psgx_wait_seq(struct sgxws_ctx *c, uint32_t seq, unsigned timeout_ms)
{
   struct psgx_ctx *k = c->k;

   return timeline_wait(ddev(c->dev), k->point[seq % c->slots], timeout_ms);
}

/* ------------------------------------------------------------------ device */

static void
psgx_dev_destroy(struct sgxws_dev *d)
{
   struct psgx_dev *dd = ddev(d);
   struct drm_powervr_sgx_ioctl_destroy_vm_context_args a = {.handle = dd->vm};

   simple_mtx_lock(&dd->lock);
   cache_trim(dd, 0);
   reap(dd, true);
   simple_mtx_unlock(&dd->lock);
   xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &a);
   drmSyncobjDestroy(dd->fd, dd->timeline);
   drmSyncobjDestroy(dd->fd, dd->in_sync);
   drmSyncobjDestroy(dd->fd, dd->out_sync);
   for (unsigned i = 0; i < 16; i++)
      if (dd->has_heap[i])
         util_vma_heap_finish(&dd->vma[i]);
   simple_mtx_destroy(&dd->lock);
   free(dd->zombies);
   free(dd->imports);
   free(dd->cache);
   free(dd);
}

static const struct sgxws_kif psgx_kif = {
   .name = "powervr_sgx",
   .dev_destroy = psgx_dev_destroy,
   .bo_alloc = psgx_bo_alloc,
   .bo_free = psgx_bo_free,
   .bo_import = psgx_bo_import,
   .bo_export = psgx_bo_export,
   .bo_kms_handle = psgx_bo_kms_handle,
   .bo_wait = psgx_bo_wait,
   .code_upload = psgx_code_upload,
   .code_reset = psgx_code_reset,
   .ctx_init = psgx_ctx_init,
   .ctx_fini = psgx_ctx_fini,
   .rt_init = psgx_rt_init,
   .rt_fini = psgx_rt_fini,
   .kick = psgx_kick,
   .wait_seq = psgx_wait_seq,
};

static int
dev_query(int fd, uint32_t type, void *out, uint32_t size)
{
   struct drm_powervr_sgx_ioctl_dev_query_args q = {.type = type,
                                                    .size = size,
                                                    .pointer = (uintptr_t)out};

   return xioctl(fd, DRM_IOCTL_POWERVR_SGX_DEV_QUERY, &q);
}

int
sgxws_powervr_sgx_dev_create(int fd, const struct sgxws_dev_options *opts,
                             struct sgxws_dev **out)
{
   struct drm_powervr_sgx_heap heaps[DRM_POWERVR_SGX_HEAP_COUNT] = {0};
   struct drm_powervr_sgx_dev_query_heap_info hi = {
      .heaps = {.stride = sizeof(heaps[0]),
                .count = DRM_POWERVR_SGX_HEAP_COUNT,
                .array = (uintptr_t)heaps},
   };
   struct drm_powervr_sgx_dev_query_gpu_info gi = {0};
   struct drm_powervr_sgx_dev_query_limits li = {0};
   struct drm_powervr_sgx_dev_query_commands cq = {0};
   struct drm_powervr_sgx_ioctl_create_vm_context_args vm = {0};
   drmVersionPtr ver;
   struct psgx_dev *dd = calloc(1, sizeof(*dd));
   struct sgxws_dev *d = &dd->base;
   uint64_t cap = 0;
   int e;

   if (!dd)
      return -ENOMEM;
   d->kif = &psgx_kif;
   dd->fd = fd;
   dd->pb_pages = opts->pb_pages;
   if (opts->trace)
      mesa_logw("sgx: SGX_TRACE is for the services driver; ignored");
   ver = drmGetVersion(fd);
   if (!ver || ver->version_major != PSGX_UAPI_MAJOR) {
      mesa_loge("sgx: powervr_sgx uAPI %d.%d, this Mesa speaks %d.x",
                ver ? ver->version_major : -1, ver ? ver->version_minor : -1,
                PSGX_UAPI_MAJOR);
      drmFreeVersion(ver);
      e = -ENODEV;
      goto fail;
   }
   drmFreeVersion(ver);
   if (drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) || !cap) {
      mesa_loge("sgx: powervr_sgx without timeline syncobjs");
      e = -ENODEV;
      goto fail;
   }
   if ((e = dev_query(fd, DRM_POWERVR_SGX_DEV_QUERY_GPU_INFO_GET, &gi,
                      sizeof(gi))) ||
       (e = dev_query(fd, DRM_POWERVR_SGX_DEV_QUERY_LIMITS_GET, &li,
                      sizeof(li))) ||
       (e = dev_query(fd, DRM_POWERVR_SGX_DEV_QUERY_HEAP_INFO_GET, &hi,
                      sizeof(hi))) ||
       (e = dev_query(fd, DRM_POWERVR_SGX_DEV_QUERY_COMMANDS_GET, &cq,
                      sizeof(cq)))) {
      mesa_loge("sgx: DEV_QUERY failed: %d", e);
      goto fail;
   }
   if (!(cq.job_types & BITFIELD_BIT(DRM_POWERVR_SGX_JOB_TYPE_RENDER))) {
      mesa_loge("sgx: powervr_sgx doesn't offer render jobs on core %#x yet",
                gi.core_id);
      e = -ENODEV;
      goto fail;
   }
   dd->render_exts = cq.render_exts;
   if (!(dd->render_exts & BITFIELD64_BIT(DRM_POWERVR_SGX_EXT_SGX545_RENDER)))
      mesa_logw("sgx: no SGX545 render extension: depth and stencil aren't stored");
   d->info.core_id = gi.core_id;
   d->info.core_revision = gi.core_revision;
   d->info.zero_copy = true;
   d->info.priorities = true;
   d->info.rt_max_width = li.rt_max_width;
   d->info.rt_max_height = li.rt_max_height;
   d->info.rt_max_samples = li.rt_max_samples;
   for (unsigned i = 0; i < DRM_POWERVR_SGX_HEAP_COUNT && i < hi.heaps.count;
        i++) {
      enum sgx_heap id = heap_ids[i];
      uint64_t lo = heaps[i].base, hi_ = heaps[i].base + heaps[i].size;

      if (!heaps[i].size || hi_ > 1ull << 32)
         continue;
      d->heap_base[id] = (uint32_t)lo;
      /* nothing at a heap's first page: a PDS state word or DOUTU word of 0
       * would name it */
      util_vma_heap_init(&dd->vma[id], lo + 0x1000, heaps[i].size - 0x1000);
      dd->vma[id].alloc_high = false;
      dd->has_heap[id] = true;
   }
   if (!dd->has_heap[SGX_HEAP_GENERAL] || !dd->has_heap[SGX_HEAP_PDSPIXEL] ||
       !dd->has_heap[SGX_HEAP_PDSVERTEX] || !dd->has_heap[SGX_HEAP_PIXSHADER] ||
       !dd->has_heap[SGX_HEAP_VTXSHADER]) {
      mesa_loge("sgx: powervr_sgx lacks a heap the winsys needs");
      e = -ENODEV;
      goto fail_heaps;
   }
   d->usse_base_index[0] =
      (uint8_t)heaps[DRM_POWERVR_SGX_HEAP_USSE_PIXEL].usse_base_index;
   d->usse_base_index[1] =
      (uint8_t)heaps[DRM_POWERVR_SGX_HEAP_USSE_VERTEX].usse_base_index;
   if ((e = xioctl(fd, DRM_IOCTL_POWERVR_SGX_CREATE_VM_CONTEXT, &vm))) {
      mesa_loge("sgx: CREATE_VM_CONTEXT failed: %d", e);
      goto fail_heaps;
   }
   dd->vm = vm.handle;
   if (drmSyncobjCreate(fd, 0, &dd->timeline) ||
       drmSyncobjCreate(fd, 0, &dd->in_sync) ||
       drmSyncobjCreate(fd, 0, &dd->out_sync)) {
      e = -errno;
      if (dd->timeline)
         drmSyncobjDestroy(fd, dd->timeline);
      if (dd->in_sync)
         drmSyncobjDestroy(fd, dd->in_sync);
      goto fail_vm;
   }
   simple_mtx_init(&dd->lock, mtx_plain);
   if (opts->verbose)
      mesa_logi("sgx: powervr_sgx: core %#x rev %#x, firmware interface %u",
                gi.core_id, gi.core_revision, gi.fw_interface_version);
   *out = d;
   return 0;

fail_vm: {
   struct drm_powervr_sgx_ioctl_destroy_vm_context_args a = {.handle = dd->vm};

   xioctl(fd, DRM_IOCTL_POWERVR_SGX_DESTROY_VM_CONTEXT, &a);
}
fail_heaps:
   for (unsigned i = 0; i < 16; i++)
      if (dd->has_heap[i])
         util_vma_heap_finish(&dd->vma[i]);
fail:
   free(dd);
   return e;
}
