/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include "util/os_time.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include "util/u_transfer.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"
#include "sgx545_texture.h"

/* render targets: the PBE writes rows of a multiple of 32 pixels (STATE.md
 * section 4).  A sampled texture in a format the TPU reads twiddled is laid
 * out as a texture even when it may also be rendered to. */
static bool
is_render_target(const struct pipe_resource *templ)
{
   if (templ->target == PIPE_BUFFER)
      return false;
   if (templ->bind & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED))
      return true;
   return (templ->bind & PIPE_BIND_RENDER_TARGET) &&
          !((templ->bind & PIPE_BIND_SAMPLER_VIEW) && sgx545_tex_format(templ->format));
}

static size_t
texture_size(struct pipe_resource *prsc, uint32_t stride)
{
   size_t size = 0;

   for (unsigned l = 0; l <= prsc->last_level; l++) {
      unsigned w = u_minify(prsc->width0, l), h = u_minify(prsc->height0, l);

      size += (size_t)(l ? util_format_get_stride(prsc->format, w) : stride) *
              util_format_get_nblocksy(prsc->format, h) * prsc->depth0 * prsc->array_size;
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
             util_format_get_nblocksy(prsc->format, h) * prsc->depth0 * prsc->array_size;
   }
   return off;
}

static struct pipe_resource *
sgx545_resource_create(struct pipe_screen *pscreen, const struct pipe_resource *templ)
{
   struct sgx545_screen *screen = sgx545_screen(pscreen);
   struct sgx545_resource *res = CALLOC_STRUCT(sgx545_resource);
   struct pipe_resource *prsc;

   if (!res)
      return NULL;
   prsc = &res->base;
   *prsc = *templ;
   pipe_reference_init(&prsc->reference, 1);
   prsc->screen = pscreen;

   if (templ->target == PIPE_BUFFER) {
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
      e = sgxws_bo_alloc(&screen->dev, &res->bo, SGX545_HEAP_GENERAL, SGXWS_ATTRIBS_COLOR,
                         align(res->stride * templ->height0, 4096), 0x1000);
      if (!e)
         res->sync_data = pvr_sync_data(&screen->dev.pvr, res->bo.b.sync, &res->sync_map);
      simple_mtx_unlock(&screen->lock);
      if (e)
         goto fail;
      SGX545_DBG(SYNC, "render target %ux%u %s at %#x\n", templ->width0, templ->height0,
                 util_format_short_name(templ->format), res->bo.va);
      return prsc;
   }

   /* Everything else has a linear CPU copy; a texture the GPU can sample
    * also has its device copy in the texture heap, laid out as docs/TEX.md
    * says, rewritten from the CPU copy whenever a level is written. */
   res->stride = util_format_get_stride(templ->format, templ->width0);
   for (unsigned l = 0; l <= templ->last_level && l < ARRAY_SIZE(res->level_offset); l++)
      res->level_offset[l] = level_offset(prsc, res->stride, l);
   res->data = align_malloc(MAX2(texture_size(prsc, res->stride), 1), 64);
   if (!res->data)
      goto fail;
   if (sgx545_tex_format(templ->format) && templ->target != PIPE_TEXTURE_3D &&
       templ->last_level < ARRAY_SIZE(res->level_offset)) {
      int e;

      simple_mtx_lock(&screen->lock);
      e = sgxws_bo_alloc(&screen->dev, &res->bo, SGX545_HEAP_TEXTURE, 0x3,
                         align(MAX2(sgx545_tex_size(prsc), 1), 4096), 0x1000);
      simple_mtx_unlock(&screen->lock);
      if (e)
         goto fail;
      res->texture = true;
   }
   return prsc;

fail:
   FREE(res);
   return NULL;
}

static struct pipe_resource *
sgx545_resource_from_user_memory(struct pipe_screen *pscreen, const struct pipe_resource *templ,
                                 void *user_memory)
{
   struct sgx545_screen *screen = sgx545_screen(pscreen);
   struct sgx545_resource *res;
   uint32_t size;
   int e;

   /* only what the fbdev EGL platform hands in: a page-aligned BGRA mapping */
   if (templ->target != PIPE_TEXTURE_2D || ((uintptr_t)user_memory & 4095) ||
       util_format_get_blocksize(templ->format) != 4)
      return NULL;
   res = CALLOC_STRUCT(sgx545_resource);
   if (!res)
      return NULL;
   res->base = *templ;
   pipe_reference_init(&res->base.reference, 1);
   res->base.screen = pscreen;
   res->pitch = align(templ->width0, 32);
   res->stride = res->pitch * 4;
   size = align(res->stride * templ->height0, 4096);

   simple_mtx_lock(&screen->lock);
   e = pvr_wrap(&screen->dev.pvr, &res->bo.b, user_memory, size);
   if (!e) {
      res->bo.va = res->bo.b.devva;
      res->bo.map = res->bo.b.cpu;
      res->sync_data = pvr_sync_data(&screen->dev.pvr, res->bo.b.sync, &res->sync_map);
   }
   simple_mtx_unlock(&screen->lock);
   if (e) {
      fprintf(stderr, "sgx545: WRAP_EXT_MEMORY of %u bytes failed: %d\n", size, e);
      FREE(res);
      return NULL;
   }
   res->wrapped = true;
   return &res->base;
}

static void
sgx545_resource_destroy(struct pipe_screen *pscreen, struct pipe_resource *prsc)
{
   struct sgx545_screen *screen = sgx545_screen(pscreen);
   struct sgx545_resource *res = sgx545_resource(prsc);

   if (res->bo.b.mem) {
      simple_mtx_lock(&screen->lock);
      /* renders in flight may still use it (and a target's last render keeps
       * a pointer to its sync data) */
      if (screen->hw)
         sgxws_wait_idle(screen->hw, 2000);
      pvr_unmap(&screen->dev.pvr, &res->sync_map);
      sgxws_bo_free(&screen->dev, &res->bo);
      if (res->alt.b.mem)
         sgxws_bo_free(&screen->dev, &res->alt);
      simple_mtx_unlock(&screen->lock);
   }
   if (res->data)
      align_free(res->data);
   FREE(res);
}

bool
sgx545_resource_target(struct sgx545_screen *screen, struct sgx545_resource *res,
                       struct sgxws_target *t)
{
   if (!res->bo.va || !res->pitch || util_format_get_blocksize(res->base.format) != 4)
      return false;
   t->va = res->bo.va;
   t->stride = res->pitch;
   t->sync = res->bo.b.sync;
   t->sync_data = res->sync_data;
   return true;
}

/* ---------------------------------------------------------------- transfers */

static void *
sgx545_transfer_map(struct pipe_context *pctx, struct pipe_resource *prsc, unsigned level,
                    unsigned usage, const struct pipe_box *box, struct pipe_transfer **out)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_resource *res = sgx545_resource(prsc);
   struct sgx545_transfer *trans;
   struct pipe_transfer *ptrans;
   uint8_t *base;
   uint32_t stride;

   /* A render target the frame being recorded writes: the CPU sees it once
    * the render is done.  Everything else is either CPU memory or idle (the
    * driver waits for every render). */
   if (res->bo.va && !res->texture && !(usage & PIPE_MAP_UNSYNCHRONIZED)) {
      ctx->stats.fl_read += ctx->recording == res;
      sgx545_flush_resource_writes(ctx, res);
      /* and the renders already submitted into it */
      if (res->sync_data && sgxws_wait_sync(res->sync_data, 2000))
         fprintf(stderr, "sgx545: a render into a mapped buffer did not finish\n");
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
      ptrans->stride = 0;
      ptrans->layer_stride = 0;
      *out = ptrans;
      return (uint8_t *)res->data + box->x;
   }

   stride = level ? util_format_get_stride(prsc->format, u_minify(prsc->width0, level))
                  : res->stride;
   base = res->bo.map && !res->texture ? (uint8_t *)res->bo.map : (uint8_t *)res->data;
   base += level_offset(prsc, res->stride, level);
   ptrans->stride = stride;
   ptrans->layer_stride = (uintptr_t)stride *
                          util_format_get_nblocksy(prsc->format, u_minify(prsc->height0, level));
   *out = ptrans;
   return base + box->z * ptrans->layer_stride +
          util_format_get_nblocksy(prsc->format, box->y) * stride +
          util_format_get_stride(prsc->format, box->x);
}

static void
alt_dirty_add(struct sgx545_resource *res, unsigned level, unsigned x0, unsigned y0, unsigned x1,
              unsigned y1)
{
   if (!res->alt.va || res->n_alt_dirty == SGX545_ALT_ALL)
      return;
   if (res->n_alt_dirty == ARRAY_SIZE(res->alt_dirty)) {
      res->n_alt_dirty = SGX545_ALT_ALL;
      return;
   }
   res->alt_dirty[res->n_alt_dirty].level = level;
   res->alt_dirty[res->n_alt_dirty].x0 = x0;
   res->alt_dirty[res->n_alt_dirty].y0 = y0;
   res->alt_dirty[res->n_alt_dirty].x1 = MIN2(x1, 0xffff);
   res->alt_dirty[res->n_alt_dirty].y1 = MIN2(y1, 0xffff);
   res->n_alt_dirty++;
}

/*
 * A write to a texture's level box (already in the CPU copy) onto the device
 * copy.  Minecraft animates sprites of its block atlas every tick (water,
 * lava, fire) while the frames before still sample the atlas: writing under
 * a render in flight would show half-updated sprites, and waiting for the GPU
 * at every tick threw the CPU/GPU overlap away.  So a busy copy is left to
 * its renders and the other copy takes over, brought up to date from the
 * CPU copy first.
 */
static void
tex_write(struct sgx545_context *ctx, struct sgx545_resource *res, unsigned level, unsigned x0,
          unsigned y0, unsigned x1, unsigned y1)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   uint32_t done = ctx->hw ? sgxws_done_seq(ctx->hw) : 0;

   if (ctx->hw && sgxws_seq_after(res->use_seq, done)) {
      struct sgxws_bo b;
      uint32_t u;

      if (!res->alt.va) {
         simple_mtx_lock(&screen->lock);
         if (sgxws_bo_alloc(&screen->dev, &res->alt, SGX545_HEAP_TEXTURE, 0x3,
                            align(MAX2(sgx545_tex_size(&res->base), 1), 4096), 0x1000))
            memset(&res->alt, 0, sizeof(res->alt));
         simple_mtx_unlock(&screen->lock);
         res->n_alt_dirty = SGX545_ALT_ALL;
         res->alt_use_seq = 0;
      }
      if (!res->alt.va || sgxws_seq_after(res->alt_use_seq, done)) {
         /* no second copy, or both busy: wait for the renders */
         ctx->stats.tex_flushes += ctx->stats.on;
         sgx545_finish(ctx);
      }
      if (res->alt.va) {
         b = res->bo, res->bo = res->alt, res->alt = b;
         u = res->use_seq, res->use_seq = res->alt_use_seq, res->alt_use_seq = u;
         if (res->n_alt_dirty == SGX545_ALT_ALL) {
            for (unsigned l = 0; l <= res->base.last_level; l++)
               sgx545_tex_encode_level(res, l);
         } else {
            for (unsigned i = 0; i < res->n_alt_dirty; i++)
               sgx545_tex_encode_box(res, res->alt_dirty[i].level, res->alt_dirty[i].x0,
                                     res->alt_dirty[i].y0, res->alt_dirty[i].x1, res->alt_dirty[i].y1);
         }
         res->n_alt_dirty = 0;
      }
   }
   sgx545_tex_encode_box(res, level, x0, y0, x1, y1);
   alt_dirty_add(res, level, x0, y0, x1, y1);
}

static void
sgx545_transfer_unmap(struct pipe_context *pctx, struct pipe_transfer *ptrans)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_resource *res = sgx545_resource(ptrans->resource);

   /* only what the transfer covers: the game updates 16x16 sprites of a
    * 1024x512 atlas, and the whole level each time took a minute to load */
   if (res->texture && (ptrans->usage & PIPE_MAP_WRITE)) {
      uint64_t t = ctx->stats.on ? os_time_get_nano() : 0;

      tex_write(ctx, res, ptrans->level, ptrans->box.x, ptrans->box.y,
                ptrans->box.x + ptrans->box.width, ptrans->box.y + ptrans->box.height);
      if (ctx->stats.on) {
         ctx->stats.tex_ns += os_time_get_nano() - t;
         ctx->stats.tex_writes++;
      }
   }

   pipe_resource_reference(&ptrans->resource, NULL);
   slab_free(&ctx->transfer_pool, ptrans);
}

void
sgx545_resource_screen_init(struct sgx545_screen *screen)
{
   screen->base.resource_create = sgx545_resource_create;
   screen->base.resource_from_user_memory = sgx545_resource_from_user_memory;
   screen->base.resource_destroy = sgx545_resource_destroy;
}

void
sgx545_resource_context_init(struct sgx545_context *ctx)
{
   ctx->base.buffer_map = sgx545_transfer_map;
   ctx->base.texture_map = sgx545_transfer_map;
   ctx->base.buffer_unmap = sgx545_transfer_unmap;
   ctx->base.texture_unmap = sgx545_transfer_unmap;
   ctx->base.transfer_flush_region = u_default_transfer_flush_region;
   ctx->base.buffer_subdata = u_default_buffer_subdata;
   ctx->base.texture_subdata = u_default_texture_subdata;
}
