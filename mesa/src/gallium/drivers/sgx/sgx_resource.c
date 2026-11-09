/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "drm-uapi/drm_fourcc.h"
#include "frontend/winsys_handle.h"
#include "renderonly/renderonly.h"
#include "util/format/u_format.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"

#include "sgx_context.h"
#include "sgx_resource.h"
#include "sgx_screen.h"
#include "sgx_texture.h"

/* What a window or pbuffer is, and the colour buffers in a format that isn't
 * a texture format: rows of a multiple of 32 pixels (STATE.md section 4), read
 * in place.  Anything in a texture format is laid out as a texture even when it
 * is rendered to: the PBE writes that layout (TEX.md sections 4, 11). */
static bool
is_render_target(const struct pipe_resource *templ)
{
   if (templ->target == PIPE_BUFFER)
      return false;
   if (templ->bind &
       (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED))
      return true;
   return (templ->bind & PIPE_BIND_RENDER_TARGET) &&
          !sgx_tex_format(templ->format);
}

/* A shared buffer is a linear render target as is_render_target makes them:
 * BGRA/RGBA8, rows of a multiple of 32 pixels (STATE.md section 4), one level. */
static bool
shareable(const struct pipe_resource *templ)
{
   return templ->target == PIPE_TEXTURE_2D && templ->last_level == 0 &&
          templ->array_size == 1 &&
          util_format_get_blocksize(templ->format) == 4 &&
          templ->nr_samples <= 1;
}

static size_t
texture_size(struct pipe_resource *prsc, uint32_t stride)
{
   size_t size = 0;

   for (unsigned l = 0; l <= prsc->last_level; l++) {
      unsigned w = u_minify(prsc->width0, l), h = u_minify(prsc->height0, l);

      size += (size_t)(l ? util_format_get_stride(prsc->format, w) : stride) *
              util_format_get_nblocksy(prsc->format, h) * prsc->depth0 *
              prsc->array_size;
   }
   return size;
}

static size_t
level_offset(struct pipe_resource *prsc, uint32_t stride, unsigned level)
{
   size_t off = 0;

   for (unsigned l = 0; l < level; l++) {
      unsigned w = u_minify(prsc->width0, l), h = u_minify(prsc->height0, l);

      off += (size_t)(l ? util_format_get_stride(prsc->format, w) : stride) *
             util_format_get_nblocksy(prsc->format, h) * prsc->depth0 *
             prsc->array_size;
   }
   return off;
}

/* kmsro with a display device whose buffers the CPU reads (simpledrm copies
 * each frame out) */
static bool
dumb_display(const struct sgx_screen *screen)
{
   return screen->ro && screen->ro->create_for_resource ==
                           renderonly_create_kms_dumb_buffer_for_resource;
}

/* A scanout buffer on such a display (dumb_later) when a KMS handle is asked
 * for: the display device's own dumb buffer, cached, imported, with the
 * contents so far (the kernel flushes the CPU caches around every job that
 * lists it, which the winsys does for the frame's target).  A buffer already
 * exported keeps its pages: the display device imports it instead. */
static void
make_scanout(struct sgx_screen *screen, struct sgx_resource *res)
{
   struct pipe_resource tmpl = res->base;
   struct winsys_handle handle = {0};
   struct sgxws_bo bo = {0};
   int e;

   res->dumb_later = false;
   if (res->shared) {
      res->scanout = renderonly_create_gpu_import_for_resource(
         &res->base, screen->ro, NULL);
      return;
   }
   tmpl.width0 = align(tmpl.width0, 32); /* rows of whole tiles */
   res->scanout = renderonly_scanout_for_resource(&tmpl, screen->ro, &handle);
   if (!res->scanout)
      return;
   simple_mtx_lock(&screen->lock);
   e = handle.stride != res->stride
          ? -EINVAL
          : sgxws_bo_import(screen->dev, &bo, (int)handle.handle,
                            handle.stride * res->base.height0);
   if (!e) {
      /* the frame recording into it goes, and its renders are waited for */
      if (screen->hw && sgxws_target_bo(screen->hw) == &res->bo)
         sgxws_flush(screen->hw, 0);
      if (screen->hw)
         sgxws_wait_idle(screen->hw, 2000);
      memcpy(bo.map, res->bo.map, (size_t)res->stride * res->base.height0);
      sgxws_bo_prepare_target(screen->dev, &bo);
      sgxws_bo_free(screen->dev, &res->bo);
      res->bo = bo;
      res->shared = true;
   }
   simple_mtx_unlock(&screen->lock);
   close(handle.handle);
   if (e) {
      renderonly_scanout_destroy(res->scanout, screen->ro);
      res->scanout = renderonly_create_gpu_import_for_resource(
         &res->base, screen->ro, NULL);
   }
}

static struct pipe_resource *
sgx_resource_create(struct pipe_screen *pscreen,
                    const struct pipe_resource *templ)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_resource *res = CALLOC_STRUCT(sgx_resource);
   struct pipe_resource *prsc;

   if (!res)
      return NULL;
   prsc = &res->base;
   *prsc = *templ;
   pipe_reference_init(&prsc->reference, 1);
   prsc->screen = pscreen;

   if (templ->target == PIPE_BUFFER) {
      static uint32_t next_uid;

      res->uid = p_atomic_inc_return(&next_uid);
      res->stride = templ->width0;
      res->data = align_malloc(MAX2(templ->width0, 1), 64);
      if (!res->data)
         goto fail;
      return prsc;
   }

   if (is_render_target(templ)) {
      int e;

      res->pitch = align(templ->width0, 32);
      res->stride = res->pitch * util_format_get_blocksize(templ->format);
      simple_mtx_lock(&screen->lock);
      e = sgxws_bo_alloc(screen->dev, &res->bo, SGXWS_MEM_RENDER_TARGET,
                         align(res->stride * templ->height0, 4096), 0x1000);
      simple_mtx_unlock(&screen->lock);
      if (e)
         goto fail;
      SGX_DBG(SYNC, "render target %ux%u %s at %#x\n", templ->width0,
              templ->height0, util_format_short_name(templ->format),
              res->bo.va);
      /* kmsro with a display device whose buffers the CPU reads (simpledrm
       * copies each frame out): ours until a KMS handle is asked for
       * (sgx_resource_get_handle).  Every DRI3 back buffer is asked for as
       * scanout and X only copies from it; a cached dumb buffer would be
       * flushed around every render into it */
      if (screen->ro && (templ->bind & PIPE_BIND_SCANOUT) && shareable(templ) &&
          dumb_display(screen)) {
         res->dumb_later = true;
         return prsc;
      }
      /* kmsro otherwise: exported to the display device, which scans it out */
      if (screen->ro && (templ->bind & PIPE_BIND_SCANOUT) && shareable(templ)) {
         res->scanout = renderonly_scanout_for_resource(prsc, screen->ro, NULL);
         if (!res->scanout) {
            simple_mtx_lock(&screen->lock);
            sgxws_bo_free(screen->dev, &res->bo);
            simple_mtx_unlock(&screen->lock);
            goto fail;
         }
      }
      return prsc;
   }

   /* Everything else has a linear CPU copy; a texture the GPU can sample
    * also has its device copy in the texture heap, laid out as docs/TEX.md
    * says, rewritten from the CPU copy whenever a level is written. */
   res->stride = util_format_get_stride(templ->format, templ->width0);
   for (unsigned l = 0;
        l <= templ->last_level && l < ARRAY_SIZE(res->level_offset); l++)
      res->level_offset[l] = level_offset(prsc, res->stride, l);
   res->data = align_malloc(MAX2(texture_size(prsc, res->stride), 1), 64);
   if (!res->data)
      goto fail;
   if (sgx_tex_format(templ->format) && templ->target != PIPE_TEXTURE_3D &&
       templ->last_level < ARRAY_SIZE(res->level_offset)) {
      int e;

      simple_mtx_lock(&screen->lock);
      e = sgxws_bo_alloc(screen->dev, &res->bo, SGXWS_MEM_TEXTURE,
                         align(MAX2(sgx_tex_size(prsc), 1), 4096), 0x1000);
      simple_mtx_unlock(&screen->lock);
      if (e)
         goto fail;
      res->texture = true;
      res->tail_from = templ->last_level;
      /* the X byte of a format without alpha is read as alpha: 1 from the
       * start, for a texture rendered into before anything is uploaded */
      if (!util_format_has_alpha(templ->format) &&
          util_format_get_blocksize(templ->format) == 4)
         memset(res->bo.map, 0xff, sgx_tex_size(prsc));
      /* R8 and RG8 as RGBA8 (sgx_tex_bpp): (0, 0, 0, 255) until written, the
       * B and A a render into them leaves alone */
      if (sgx_tex_bpp(templ->format) !=
          util_format_get_blocksize(templ->format))
         for (size_t i = 0; i < sgx_tex_size(prsc) / 4; i++)
            res->bo.map[i] = 0xff000000;
   }
   return prsc;

fail:
   FREE(res);
   return NULL;
}

static struct pipe_resource *
sgx_resource_from_user_memory(struct pipe_screen *pscreen,
                              const struct pipe_resource *templ,
                              void *user_memory)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_resource *res;
   uint32_t size;
   int e;

   /* only what the fbdev EGL platform hands in: a page-aligned BGRA mapping */
   if (templ->target != PIPE_TEXTURE_2D || ((uintptr_t)user_memory & 4095) ||
       util_format_get_blocksize(templ->format) != 4)
      return NULL;
   res = CALLOC_STRUCT(sgx_resource);
   if (!res)
      return NULL;
   res->base = *templ;
   pipe_reference_init(&res->base.reference, 1);
   res->base.screen = pscreen;
   res->pitch = align(templ->width0, 32);
   res->stride = res->pitch * 4;
   size = align(res->stride * templ->height0, 4096);

   simple_mtx_lock(&screen->lock);
   e = sgxws_bo_wrap(screen->dev, &res->bo, user_memory, size);
   simple_mtx_unlock(&screen->lock);
   if (e) {
      FREE(res);
      return NULL;
   }
   res->wrapped = true;
   return &res->base;
}

static struct pipe_resource *
sgx_resource_from_handle(struct pipe_screen *pscreen,
                         const struct pipe_resource *templ,
                         struct winsys_handle *whandle, unsigned usage)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_resource *res;
   int e;

   if (whandle->type != WINSYS_HANDLE_TYPE_FD || !shareable(templ) ||
       (templ->bind & PIPE_BIND_DEPTH_STENCIL) || whandle->offset ||
       (whandle->modifier != DRM_FORMAT_MOD_LINEAR &&
        whandle->modifier != DRM_FORMAT_MOD_INVALID) ||
       whandle->stride % 128 || whandle->stride < templ->width0 * 4)
      return NULL;
   res = CALLOC_STRUCT(sgx_resource);
   if (!res)
      return NULL;
   res->base = *templ;
   pipe_reference_init(&res->base.reference, 1);
   res->base.screen = pscreen;
   res->stride = whandle->stride;
   res->pitch = whandle->stride / 4;
   res->shared = true;

   simple_mtx_lock(&screen->lock);
   e = sgxws_bo_import(screen->dev, &res->bo, (int)whandle->handle,
                       res->stride * templ->height0);
   if (!e)
      sgxws_bo_prepare_target(screen->dev, &res->bo);
   simple_mtx_unlock(&screen->lock);
   if (e) {
      SGX_DBG(SYNC, "dma-buf import of %ux%u failed: %d\n", templ->width0,
              templ->height0, e);
      FREE(res);
      return NULL;
   }
   /* kmsro: a handle of it on the display device, for renderonly_get_handle */
   if (screen->ro)
      res->scanout = renderonly_create_gpu_import_for_resource(
         &res->base, screen->ro, NULL);
   return &res->base;
}

static bool
sgx_resource_get_handle(struct pipe_screen *pscreen, struct pipe_context *pctx,
                        struct pipe_resource *prsc,
                        struct winsys_handle *whandle, unsigned usage)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_resource *res = sgx_resource(prsc);
   int e, fd;

   /* only linear render targets: a texture's device copy is twiddled or
    * re-encoded from its CPU copy */
   if (!sgxws_bo_valid(&res->bo) || res->texture || res->wrapped ||
       !res->pitch || !shareable(prsc))
      return false;
   whandle->stride = res->stride;
   whandle->offset = 0;
   whandle->modifier = DRM_FORMAT_MOD_LINEAR;
   switch (whandle->type) {
   case WINSYS_HANDLE_TYPE_KMS:
      /* kmsro: the display device's handle */
      if (screen->ro) {
         if (res->dumb_later)
            make_scanout(screen, res);
         return res->scanout && renderonly_get_handle(res->scanout, whandle);
      }
      if (sgxws_bo_kms_handle(&res->bo, &whandle->handle))
         return false;
      res->shared = true;
      return true;
   case WINSYS_HANDLE_TYPE_FD:
      simple_mtx_lock(&screen->lock);
      e = sgxws_bo_export(screen->dev, &res->bo, &fd);
      simple_mtx_unlock(&screen->lock);
      if (e)
         return false;
      whandle->handle = (unsigned)fd;
      res->shared = true;
      return true;
   default:
      return false;
   }
}

static bool
sgx_resource_get_param(struct pipe_screen *pscreen, struct pipe_context *pctx,
                       struct pipe_resource *prsc, unsigned plane,
                       unsigned layer, unsigned level,
                       enum pipe_resource_param param, unsigned usage,
                       uint64_t *value)
{
   struct sgx_resource *res = sgx_resource(prsc);
   struct winsys_handle whandle = {0};

   switch (param) {
   case PIPE_RESOURCE_PARAM_NPLANES:
      *value = 1;
      return true;
   case PIPE_RESOURCE_PARAM_STRIDE:
      *value = res->stride;
      return true;
   case PIPE_RESOURCE_PARAM_OFFSET:
      *value = 0;
      return true;
   case PIPE_RESOURCE_PARAM_MODIFIER:
      *value = DRM_FORMAT_MOD_LINEAR;
      return true;
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS:
   case PIPE_RESOURCE_PARAM_HANDLE_TYPE_FD:
      whandle.type = param == PIPE_RESOURCE_PARAM_HANDLE_TYPE_KMS
                        ? WINSYS_HANDLE_TYPE_KMS
                        : WINSYS_HANDLE_TYPE_FD;
      if (!sgx_resource_get_handle(pscreen, pctx, prsc, &whandle, usage))
         return false;
      *value = whandle.handle;
      return true;
   default:
      return false;
   }
}

/* A buffer's device copy that renders may still read: freed once they are
 * done, the ones already done now (screen lock held).  A full list waits for
 * the GPU. */
static void
vbo_retire(struct sgx_screen *screen, struct sgxws_bo *bo, uint32_t seq)
{
   uint32_t done;

   if (!sgxws_bo_valid(bo))
      return;
   done = screen->hw ? sgxws_done_seq(screen->hw) : seq;
   for (unsigned i = 0; i < screen->n_retired;) {
      if (!sgxws_seq_after(screen->retired[i].seq, done)) {
         sgxws_bo_free(screen->dev, &screen->retired[i].bo);
         screen->retired[i] = screen->retired[--screen->n_retired];
      } else {
         i++;
      }
   }
   if (sgxws_seq_after(seq, done) &&
       screen->n_retired == ARRAY_SIZE(screen->retired)) {
      sgxws_flush(screen->hw, 0);
      sgxws_wait_idle(screen->hw, 2000);
      for (unsigned i = 0; i < screen->n_retired; i++)
         sgxws_bo_free(screen->dev, &screen->retired[i].bo);
      screen->n_retired = 0;
      done = seq;
   }
   if (sgxws_seq_after(seq, done)) {
      screen->retired[screen->n_retired].bo = *bo;
      screen->retired[screen->n_retired++].seq = seq;
      memset(bo, 0, sizeof(*bo));
   } else {
      sgxws_bo_free(screen->dev, bo);
   }
}

/* (screen lock held) */
bool
sgx_buffer_vbo(struct sgx_context *ctx, struct sgx_resource *res)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);

   /* Written since it was copied, or mapped for writes still (whose writes
    * reach the copy only at the unmap): copied again, into a new copy when a
    * render may still read this one */
   if (sgxws_bo_valid(&res->vbo) &&
       (res->vbo_gen != res->gen || res->write_maps)) {
      if (!screen->hw ||
          !sgxws_seq_after(res->vbo_seq, sgxws_done_seq(screen->hw)))
         goto copy;
      /* (mid-draw: a full retired list would flush the frame being
       * recorded, so the draw goes without the device copy instead) */
      if (screen->n_retired == ARRAY_SIZE(screen->retired))
         return false;
      vbo_retire(screen, &res->vbo, res->vbo_seq);
   }
   if (sgxws_bo_valid(&res->vbo))
      return true;
   if (sgxws_bo_alloc(screen->dev, &res->vbo, SGXWS_MEM_VERTEX,
                      align(MAX2(res->base.width0, 16), 4096), 0x1000)) {
      memset(&res->vbo, 0, sizeof(res->vbo));
      return false;
   }
copy:
   memcpy(res->vbo.map, res->data, res->base.width0);
   res->vbo_gen = res->gen;
   return true;
}

static void
sgx_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct sgx_screen *screen = sgx_screen(pscreen);
   struct sgx_resource *res = sgx_resource(prsc);

   if (sgxws_bo_valid(&res->bo)) {
      simple_mtx_lock(&screen->lock);
      /* a frame still being recorded into it or sampling it goes now (st
       * draws its bitmap cache at teardown and then frees the bitmap: the
       * frame went to the GPU after the texture was freed, a 3D lockup), and
       * renders in flight may still use it (and a target's last render keeps
       * a pointer to its sync data) */
      if (screen->hw && (sgxws_target_bo(screen->hw) == &res->bo ||
                         res->use_seq == sgxws_next_seq(screen->hw) ||
                         res->alt_use_seq == sgxws_next_seq(screen->hw)))
         sgxws_flush(screen->hw, 0);
      if (screen->hw)
         sgxws_wait_idle(screen->hw, 2000);
      sgxws_bo_free(screen->dev, &res->bo);
      sgxws_bo_free(screen->dev, &res->alt);
      sgxws_bo_free(screen->dev, &res->cube0);
      simple_mtx_unlock(&screen->lock);
   }
   if (sgxws_bo_valid(&res->zls)) {
      /* a depth/stencil buffer: the frame storing into it goes now, and the
       * renders that do are waited for */
      simple_mtx_lock(&screen->lock);
      if (screen->hw && res->use_seq == sgxws_next_seq(screen->hw))
         sgxws_flush(screen->hw, 0);
      if (screen->hw)
         sgxws_wait_idle(screen->hw, 2000);
      sgxws_bo_free(screen->dev, &res->zls);
      simple_mtx_unlock(&screen->lock);
   }
   if (sgxws_bo_valid(&res->vbo)) {
      simple_mtx_lock(&screen->lock);
      vbo_retire(screen, &res->vbo, res->vbo_seq);
      simple_mtx_unlock(&screen->lock);
   }
   if (res->scanout)
      renderonly_scanout_destroy(res->scanout, screen->ro);
   if (res->data)
      align_free(res->data);
   FREE(res);
}

bool
sgx_resource_target(struct sgx_screen *screen, struct sgx_resource *res,
                    unsigned level, unsigned layer, struct sgxws_target *t)
{
   memset(t, 0, sizeof(*t));
   t->bo = &res->bo;
   switch (res->base.format) {
   case PIPE_FORMAT_B8G8R8A8_UNORM:
   case PIPE_FORMAT_B8G8R8X8_UNORM:
      t->pbe_format = 0;
      t->t1_format = 0x0c;
      break;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
   case PIPE_FORMAT_R8G8B8X8_UNORM:
   case PIPE_FORMAT_R8_UNORM:   /* stored as RGBA8 (sgx_tex_bpp); the draws */
   case PIPE_FORMAT_R8G8_UNORM: /* write R (and G) alone (sgx_target_mask) */
      t->pbe_format = 1;
      t->t1_format =
         0x0d; /* swaps R and B against 0x0c: the PBE's order back */
      break;
   default:
      return false;
   }
   if (res->texture) {
      unsigned w = u_minify(res->base.width0, level),
               h = u_minify(res->base.height0, level);
      bool twiddled =
         util_is_power_of_two_nonzero(w) && util_is_power_of_two_nonzero(h);

      t->va = res->bo.va + sgx_tex_offset(&res->base, level, layer);
      t->stride = twiddled ? w : align(w, 8);
      t->t1_layout = twiddled ? 0 : 3;
      sgxws_bo_prepare_target(screen->dev,
                              &res->bo); /* (the caller holds the lock) */
      res->rendered |= 1u << level;
      res->n_alt_dirty = SGX_ALT_ALL; /* the render isn't in the second copy */
      if (!level)
         res->level0_gen++;
      return true;
   }
   if (!res->bo.va || !res->pitch || level || layer ||
       util_format_get_blocksize(res->base.format) != 4)
      return false;
   t->va = res->bo.va;
   t->stride = res->pitch;
   t->t1_layout = 6;
   return true;
}

/* ---------------------------------------------------------------- transfers */

static void *
sgx_transfer_map(struct pipe_context *pctx, struct pipe_resource *prsc,
                 unsigned level, unsigned usage, const struct pipe_box *box,
                 struct pipe_transfer **out)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_resource *res = sgx_resource(prsc);
   struct sgx_transfer *trans;
   struct pipe_transfer *ptrans;
   uint8_t *base;
   uint32_t stride;

   /* A render target the frame being recorded writes: the CPU sees it once
    * the render is done.  Everything else is either CPU memory or idle (the
    * driver waits for every render). */
   if (res->bo.va && !res->texture && !(usage & PIPE_MAP_UNSYNCHRONIZED)) {
      ctx->stats.fl_read += ctx->recording == res;
      if (ctx->recording == res)
         SGX_DBG(
            SYNC,
            "readback ends the render into %s %ux%u (map usage %#x, box %d,%d %dx%d)\n",
            util_format_short_name(prsc->format), prsc->width0, prsc->height0,
            usage, box->x, box->y, box->width, box->height);
      sgx_flush_resource_writes(ctx, res);
      /* and the renders already submitted into it */
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr, "sgx: a render into a mapped buffer did not finish\n");
   }
   /* A texture the GPU rendered into: the CPU copy catches up first.  A read
    * (piglit's probes, glReadPixels) brings up only the box it covers and the
    * level stays rendered; a write brings up every level rendered into, as
    * the unmap and the second copy work from the CPU copy. */
   if (res->texture && res->rendered && !(usage & PIPE_MAP_UNSYNCHRONIZED) &&
       ((usage & PIPE_MAP_WRITE) || (res->rendered & 1u << level))) {
      uint64_t t;

      ctx->stats.fl_read += ctx->recording == res;
      if (ctx->recording == res)
         SGX_DBG(
            SYNC,
            "readback ends the render into %s %ux%u (map usage %#x, box %d,%d %dx%d)\n",
            util_format_short_name(prsc->format), prsc->width0, prsc->height0,
            usage, box->x, box->y, box->width, box->height);
      sgx_flush_resource_writes(ctx, res);
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr,
                 "sgx: a render into a mapped texture did not finish\n");
      t = ctx->stats.on ? os_time_get_nano() : 0;
      if (usage & PIPE_MAP_WRITE) {
         u_foreach_bit (l, res->rendered) {
            sgx_tex_decode_level(res, l);
            ctx->stats.rb_bytes += (uint64_t)util_format_get_stride(
                                      prsc->format, u_minify(prsc->width0, l)) *
                                   u_minify(prsc->height0, l) *
                                   prsc->array_size;
         }
         res->rendered = 0;
      } else {
         for (int z = box->z; z < box->z + box->depth; z++)
            sgx_tex_decode_box(res, level, z, box->x, box->y,
                               box->x + box->width, box->y + box->height);
         ctx->stats.rb_bytes +=
            (uint64_t)util_format_get_stride(prsc->format, box->width) *
            box->height * box->depth;
      }
      if (ctx->stats.on)
         ctx->stats.rb_ns += os_time_get_nano() - t;
   }
   /* A texture write lands in the CPU copy; the unmap puts it on the device
    * copy, or on a second one when a render may still sample the first
    * (tex_write) */

   trans = slab_zalloc(&ctx->transfer_pool);
   if (!trans)
      return NULL;
   ptrans = &trans->base;
   pipe_resource_reference(&ptrans->resource, prsc);
   ptrans->level = level;
   ptrans->usage = usage;
   ptrans->box = *box;

   if (prsc->target == PIPE_BUFFER) {
      if (usage & PIPE_MAP_WRITE) {
         res->gen++; /* the vertex cache's copies of it are stale */
         res->write_maps++;
         res->written_ns = os_time_get_nano();
         /* a device copy a render may still read is left to it; the next
          * draw makes another (an unsynchronized write promises to miss
          * what renders read, and goes in at the unmap) */
         if (sgxws_bo_valid(&res->vbo) && !(usage & PIPE_MAP_UNSYNCHRONIZED)) {
            struct sgx_screen *screen = sgx_screen(pctx->screen);

            simple_mtx_lock(&screen->lock);
            if (screen->hw &&
                sgxws_seq_after(res->vbo_seq, sgxws_done_seq(screen->hw)))
               vbo_retire(screen, &res->vbo, res->vbo_seq);
            simple_mtx_unlock(&screen->lock);
         }
      }
      ptrans->stride = 0;
      ptrans->layer_stride = 0;
      *out = ptrans;
      return (uint8_t *)res->data + box->x;
   }

   stride = level ? util_format_get_stride(prsc->format,
                                           u_minify(prsc->width0, level))
                  : res->stride;
   base = res->bo.map && !res->texture ? (uint8_t *)res->bo.map
                                       : (uint8_t *)res->data;
   base += level_offset(prsc, res->stride, level);
   ptrans->stride = stride;
   ptrans->layer_stride =
      (uintptr_t)stride *
      util_format_get_nblocksy(prsc->format, u_minify(prsc->height0, level));
   *out = ptrans;
   return base + box->z * ptrans->layer_stride +
          util_format_get_nblocksy(prsc->format, box->y) * stride +
          util_format_get_stride(prsc->format, box->x);
}

static void
alt_dirty_add(struct sgx_resource *res, unsigned level, unsigned layer,
              unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   if (!res->alt.va || res->n_alt_dirty == SGX_ALT_ALL)
      return;
   if (res->n_alt_dirty == ARRAY_SIZE(res->alt_dirty)) {
      res->n_alt_dirty = SGX_ALT_ALL;
      return;
   }
   res->alt_dirty[res->n_alt_dirty].level = level;
   res->alt_dirty[res->n_alt_dirty].layer = layer;
   res->alt_dirty[res->n_alt_dirty].x0 = x0;
   res->alt_dirty[res->n_alt_dirty].y0 = y0;
   res->alt_dirty[res->n_alt_dirty].x1 = MIN2(x1, 0xffff);
   res->alt_dirty[res->n_alt_dirty].y1 = MIN2(y1, 0xffff);
   res->n_alt_dirty++;
}

/*
 * A write to a texture's level box (already in the CPU copy) onto the device
 * copy.  An application that animates sprites of a texture atlas updates it
 * while the frames before still sample it: writing under a render in flight
 * would show half-updated sprites, and waiting for the GPU at every update
 * threw the CPU/GPU overlap away.  So a busy copy is left to
 * its renders and the other copy takes over, brought up to date from the
 * CPU copy first.
 */
static void
tex_write(struct sgx_context *ctx, struct sgx_resource *res, unsigned level,
          unsigned layer, unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   uint32_t done = ctx->hw ? sgxws_done_seq(ctx->hw) : 0;

   if (ctx->hw && sgxws_seq_after(res->use_seq, done)) {
      struct sgxws_bo b;
      uint32_t u;

      if (!res->alt.va) {
         simple_mtx_lock(&screen->lock);
         if (sgxws_bo_alloc(screen->dev, &res->alt, SGXWS_MEM_TEXTURE,
                            align(MAX2(sgx_tex_size(&res->base), 1), 4096),
                            0x1000))
            memset(&res->alt, 0, sizeof(res->alt));
         simple_mtx_unlock(&screen->lock);
         res->n_alt_dirty = SGX_ALT_ALL;
         res->alt_use_seq = 0;
      }
      if (!res->alt.va || sgxws_seq_after(res->alt_use_seq, done)) {
         /* no second copy, or both busy: wait for the renders */
         ctx->stats.tex_flushes += ctx->stats.on;
         sgx_finish(ctx);
      }
      if (res->alt.va) {
         b = res->bo, res->bo = res->alt, res->alt = b;
         u = res->use_seq, res->use_seq = res->alt_use_seq,
         res->alt_use_seq = u;
         if (res->n_alt_dirty == SGX_ALT_ALL) {
            for (unsigned l = 0; l <= res->base.last_level; l++)
               sgx_tex_encode_level(res, l);
         } else {
            for (unsigned i = 0; i < res->n_alt_dirty; i++)
               sgx_tex_encode_box(res, res->alt_dirty[i].level,
                                  res->alt_dirty[i].layer, res->alt_dirty[i].x0,
                                  res->alt_dirty[i].y0, res->alt_dirty[i].x1,
                                  res->alt_dirty[i].y1);
         }
         res->n_alt_dirty = 0;
      }
   }
   sgx_tex_encode_box(res, level, layer, x0, y0, x1, y1);
   alt_dirty_add(res, level, layer, x0, y0, x1, y1);
   if (!level)
      res->level0_gen++;
}

bool
sgx_resource_cube0(struct sgx_context *ctx, struct sgx_resource *res)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);

   if (!res->cube0.va) {
      simple_mtx_lock(&screen->lock);
      if (sgxws_bo_alloc(screen->dev, &res->cube0, SGXWS_MEM_TEXTURE,
                         align(sgx_tex_cube0_size(&res->base), 4096), 0x1000)) {
         memset(&res->cube0, 0, sizeof(res->cube0));
         simple_mtx_unlock(&screen->lock);
         return false;
      }
      simple_mtx_unlock(&screen->lock);
      res->cube0_gen = res->level0_gen - 1;
   }
   if (res->cube0_gen == res->level0_gen)
      return true;
   /* level 0 rendered into: the CPU copy catches up, as for a map */
   if (res->rendered & 1) {
      sgx_flush_resource_writes(ctx, res);
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr,
                 "sgx: a render into a sampled cube map did not finish\n");
      sgx_tex_decode_level(res, 0);
      res->rendered &= ~1u;
   }
   /* (rare: a render in flight may still sample the old copy) */
   if (ctx->hw && sgxws_seq_after(res->use_seq, sgxws_done_seq(ctx->hw)))
      sgx_finish(ctx);
   sgx_tex_encode_cube0(res);
   res->cube0_gen = res->level0_gen;
   return true;
}

/* The device tail of a mipmapped 2D texture from the last level a sampler
 * view allows (sgx_tex_set_tail): rare, when GL_TEXTURE_MAX_LEVEL changes */
void
sgx_resource_tail(struct sgx_context *ctx, struct sgx_resource *res,
                  unsigned last)
{
   if (!res->texture || res->base.target != PIPE_TEXTURE_2D ||
       last == res->tail_from || last >= sgx_tex_dev_last(&res->base))
      return;
   /* levels rendered into: the CPU copy catches up, as for a map */
   if (res->rendered) {
      sgx_flush_resource_writes(ctx, res);
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr,
                 "sgx: a render into a sampled texture did not finish\n");
      for (unsigned l = 0; l <= res->base.last_level; l++)
         if (res->rendered & 1u << l)
            sgx_tex_decode_level(res, l);
      res->rendered = 0;
   }
   if (ctx->hw && sgxws_seq_after(res->use_seq, sgxws_done_seq(ctx->hw)))
      sgx_finish(ctx);
   sgx_tex_set_tail(res, last);
   res->n_alt_dirty = SGX_ALT_ALL; /* the second copy has the old tail */
}

static void
sgx_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_resource *res = sgx_resource(ptrans->resource);

   /* only what the transfer covers: 16x16 updates of a 1024x512 atlas
    * re-encoding the whole level each time were far too slow */
   if (res->texture && (ptrans->usage & PIPE_MAP_WRITE)) {
      uint64_t t = ctx->stats.on ? os_time_get_nano() : 0;

      for (int z = ptrans->box.z; z < ptrans->box.z + ptrans->box.depth; z++)
         tex_write(ctx, res, ptrans->level, z, ptrans->box.x, ptrans->box.y,
                   ptrans->box.x + ptrans->box.width,
                   ptrans->box.y + ptrans->box.height);
      if (ctx->stats.on) {
         ctx->stats.tex_ns += os_time_get_nano() - t;
         ctx->stats.tex_writes++;
      }
   }

   if (ptrans->resource->target == PIPE_BUFFER &&
       (ptrans->usage & PIPE_MAP_WRITE)) {
      if (res->vbo.map)
         memcpy((uint8_t *)res->vbo.map + ptrans->box.x,
                (const uint8_t *)res->data + ptrans->box.x, ptrans->box.width);
      if (res->write_maps)
         res->write_maps--;
   }
   pipe_resource_reference(&ptrans->resource, NULL);
   slab_free(&ctx->transfer_pool, ptrans);
}

void
sgx_resource_screen_init(struct sgx_screen *screen)
{
   screen->base.resource_create = sgx_resource_create;
   screen->base.resource_from_user_memory = sgx_resource_from_user_memory;
   screen->base.resource_destroy = sgx_resource_destroy;
   if (sgxws_dev_has_dmabuf(screen->dev)) {
      screen->base.resource_from_handle = sgx_resource_from_handle;
      screen->base.resource_get_handle = sgx_resource_get_handle;
      screen->base.resource_get_param = sgx_resource_get_param;
   }
}

/* clears of a surface outside glClear (st's copy and clear paths), on the
 * CPU through a map: rare, and the map already decodes what a render left */
static void
sgx_clear_render_target(struct pipe_context *pctx, struct pipe_surface *dst,
                        const union pipe_color_union *color, unsigned x,
                        unsigned y, unsigned w, unsigned h,
                        bool render_condition_enabled)
{
   util_clear_render_target(pctx, dst, color, x, y, w, h);
}

static void
sgx_clear_depth_stencil(struct pipe_context *pctx, struct pipe_surface *dst,
                        unsigned flags, double depth, unsigned stencil,
                        unsigned x, unsigned y, unsigned w, unsigned h,
                        bool render_condition_enabled)
{
   util_clear_depth_stencil(pctx, dst, flags, depth, stencil, x, y, w, h);
}

void
sgx_resource_context_init(struct sgx_context *ctx)
{
   ctx->base.buffer_map = sgx_transfer_map;
   ctx->base.texture_map = sgx_transfer_map;
   ctx->base.buffer_unmap = sgx_transfer_unmap;
   ctx->base.texture_unmap = sgx_transfer_unmap;
   ctx->base.transfer_flush_region = u_default_transfer_flush_region;
   ctx->base.buffer_subdata = u_default_buffer_subdata;
   ctx->base.texture_subdata = u_default_texture_subdata;
   ctx->base.clear_render_target = sgx_clear_render_target;
   ctx->base.clear_depth_stencil = sgx_clear_depth_stencil;
   ctx->base.clear_texture = u_default_clear_texture;
}
