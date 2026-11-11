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
 * client parts are CMD_RENDER and CMD_TA_KICK streams (docs/UAPI.md section
 * 3), written by the packers generated from powervr_sgx_kmd_stream.xml.
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
#include "util/vma.h"

#include "powervr_sgx_kmd_stream.h"
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

struct psgx_dev {
   struct sgxws_dev base;
   int fd;
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
};

struct psgx_bo {
   struct sgxws_bo_impl base;
   struct psgx_dev *dev;
   uint32_t handle;
   uint32_t va;
   enum sgx_heap heap;
   bool code;     /* a code object: map is a copy */
   bool shared;   /* imported or exported: synced through its dma-buf */
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

static void
vm_unmap(struct psgx_dev *dd, uint64_t va, uint64_t size)
{
   struct drm_powervr_sgx_ioctl_vm_unmap_args a = {
      .vm_context_handle = dd->vm,
      .device_addr = va,
      .size = size,
   };
   int e = xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_VM_UNMAP, &a);

   if (e)
      mesa_loge("sgx: VM_UNMAP %#llx +%#llx failed: %d", (unsigned long long)va,
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
   for (unsigned i = 0; i < dd->n_zombies; i++) {
      struct zombie *z = &dd->zombies[i];

      if (z->point > done && !all) {
         dd->zombies[n++] = *z;
         continue;
      }
      vm_unmap(dd, z->va, z->size);
      gem_close(dd, z->handle);
      util_vma_heap_free(&dd->vma[z->heap], z->va, z->size);
   }
   dd->n_zombies = n;
}

/* an address for size bytes in a heap; with the lock held */
static uint64_t
va_alloc(struct psgx_dev *dd, enum sgx_heap heap, uint64_t size, uint64_t align)
{
   uint64_t va;

   reap(dd, false);
   va = util_vma_heap_alloc(&dd->vma[heap], size, align);
   if (!va && dd->n_zombies) {
      reap(dd, true);
      va = util_vma_heap_alloc(&dd->vma[heap], size, align);
   }
   return va;
}

static int
vm_map(struct psgx_dev *dd, uint32_t handle, uint64_t va, uint64_t size)
{
   struct drm_powervr_sgx_ioctl_vm_map_args a = {
      .vm_context_handle = dd->vm,
      .device_addr = va,
      .handle = handle,
      .size = size,
   };

   return xioctl(dd->fd, DRM_IOCTL_POWERVR_SGX_VM_MAP, &a);
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
   if (!(impl = calloc(1, sizeof(*impl))))
      return -ENOMEM;
   impl->base.kif = &psgx_kif;
   impl->base.size = (uint32_t)bytes;
   impl->dev = dd;
   impl->heap = heap;
   impl->code = code;
   impl->dmabuf_fd = -1;
   impl->refs = 1;
   if ((e = create_bo(dd, bytes, code, &impl->handle))) {
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
      mesa_loge("sgx: heap %u has no room for %#llx bytes", heap,
                (unsigned long long)bytes);
      e = -ENOMEM;
      goto fail_map;
   }
   if ((e = vm_map(dd, impl->handle, va, bytes))) {
      mesa_loge("sgx: VM_MAP heap %u at %#llx +%#llx failed: %d", heap,
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
         impl->handle, impl->va, impl->base.size, impl->heap, dd->point,
      };
   } else {
      vm_unmap(dd, impl->va, impl->base.size);
      gem_close(dd, impl->handle);
      util_vma_heap_free(&dd->vma[impl->heap], impl->va, impl->base.size);
   }
   reap(dd, false);
   simple_mtx_unlock(&dd->lock);

   if (impl->code)
      free(impl->map);
   else if (impl->map)
      munmap(impl->map, impl->base.size);
   if (impl->dmabuf_fd >= 0)
      close(impl->dmabuf_fd);
   free(impl);
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
      mesa_loge("sgx: VM_MAP of an imported dma-buf failed: %d", e);
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
   *handle = dbo(bo)->handle;
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

static int
psgx_kick(struct sgxws_ctx *c, uint32_t seq)
{
   struct psgx_dev *dd = ddev(c->dev);
   struct psgx_ctx *k = c->k;
   uint32_t pds = c->dev->heap_base[SGX_HEAP_PDSPIXEL];
   struct SGX_KMD_STREAM_CMD_RENDER render = {
      .pixel_event_pds_addr = c->pixev,
      .pixel_event_pds_rows = c->pixev_rows,
      .bg_pds_secondary_addr = pds + ((c->bg_pds[0] & 0x3ffffff) << 4),
      .bg_pds_secondary_rows = c->bg_pds[0] >> 26,
      .bg_pds_primary_addr = pds + ((c->bg_pds[2] & 0x3ffffff) << 4),
      .bg_pds_primary_rows = c->bg_pds[2] >> 26,
      .bg_depth = sgxws_f32(1.0f),
      .depth_bias_units = c->bias_units,
      .depth_bias_factor = sgxws_f32(c->bias_factor),
   };
   uint32_t pad = sgxws_zls_pixels(c->rt->w, c->rt->h);
   uint64_t render_buf[SGX_KMD_STREAM_CMD_RENDER_length / 8];
   uint64_t kick_buf[TA_KICKS][(SGX_KMD_STREAM_CMD_TA_KICK_length + 7) / 8];
   struct drm_powervr_sgx_ta_kick kicks[TA_KICKS];
   struct shared_use sh[ARRAY_SIZE(c->use) + 1];
   struct drm_powervr_sgx_bo_ref refs[ARRAY_SIZE(sh)];
   struct drm_powervr_sgx_sync_op ops[3];
   struct drm_powervr_sgx_job job;
   struct drm_powervr_sgx_ioctl_submit_jobs_args sj = {
      .jobs = {.stride = sizeof(job), .count = 1, .array = (uintptr_t)&job},
   };
   unsigned n_sh = 0, n_ops = 0;
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

   /* the scene's stream (it ends with the terminate words), then a stream
    * of the terminate words alone */
   for (unsigned i = 0; i < TA_KICKS; i++) {
      struct SGX_KMD_STREAM_CMD_TA_KICK ta = {.vdm_base = c->vdm_start};

      if (i) {
         ta.vdm_base = c->vdm.bo.va + ((c->vdm.used + 3) & ~3u);
         sgxws_vdm_end(c);
      }
      kicks[i] = (struct drm_powervr_sgx_ta_kick){
         .cmd_stream = (uintptr_t)kick_buf[i],
         .cmd_stream_len = SGX_KMD_STREAM_CMD_TA_KICK_pack(kick_buf[i], &ta),
      };
   }
   if (c->target.zls_va) {
      /* the kernel adds the tile count and the ZLS base */
      render.depth_addr = c->target.zls_va;
      render.stencil_addr = c->target.zls_va + pad * 4;
      render.zls_control = SGXWS_ZLS_BASE | SGXWS_ZLS_STORE |
                           (c->target.zls_load ? SGXWS_ZLS_LOAD : 0);
      /* the 3D flags of a render with a depth buffer (kick b19-20) [D] */
      render.flags_3d = 0x180;
   }
   SGX_KMD_STREAM_CMD_RENDER_pack(render_buf, &render);

   if (c->target.bo)
      add_shared(sh, &n_sh, c->target.bo, true);
   for (unsigned i = 0; i < c->n_use; i++)
      add_shared(sh, &n_sh, c->use[i].bo, c->use[i].write);

   /* until the kernel flushes imports without it: the shared buffers'
    * list, which is what makes it flush them around the job */
   for (unsigned i = 0; i < n_sh; i++)
      refs[i] = (struct drm_powervr_sgx_bo_ref){
         .handle = sh[i].bo->handle,
         .flags = sh[i].write ? DRM_POWERVR_SGX_BO_REF_FLAG_WRITE : 0,
      };

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
      .cmd_stream = (uintptr_t)render_buf,
      .cmd_stream_len = SGX_KMD_STREAM_CMD_RENDER_length,
      .sync_ops = {.stride = sizeof(ops[0]),
                   .count = n_ops,
                   .array = (uintptr_t)ops},
      .bo_refs = {.stride = sizeof(refs[0]),
                  .count = n_sh,
                  .array = (uintptr_t)refs},
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
   struct drm_powervr_sgx_ioctl_create_vm_context_args vm = {0};
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
                      sizeof(hi)))) {
      mesa_loge("sgx: DEV_QUERY failed: %d", e);
      goto fail;
   }
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
