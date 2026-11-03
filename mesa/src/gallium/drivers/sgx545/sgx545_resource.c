/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

#include "util/os_time.h"
#include "util/format/u_format.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include "util/u_transfer.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"
#include "sgx545_texture.h"

/* What a window or pbuffer is, and the colour buffers in a format that isn't
 * a texture format: rows of a multiple of 32 pixels (STATE.md section 4), read
 * in place.  Anything in a texture format is laid out as a texture even when it
 * is rendered to: the PBE writes that layout (TEX.md sections 4, 11). */
static bool
is_render_target(const struct pipe_resource *templ)
{
   if (templ->target == PIPE_BUFFER)
      return false;
   if (templ->bind & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED))
      return true;
   return (templ->bind & PIPE_BIND_RENDER_TARGET) && !sgx545_tex_format(templ->format);
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
      e = sgxws_bo_alloc(screen->dev, &res->bo, SGXWS_MEM_TEXTURE,
                         align(MAX2(sgx545_tex_size(prsc), 1), 4096), 0x1000);
      simple_mtx_unlock(&screen->lock);
      if (e)
         goto fail;
      res->texture = true;
      /* the X byte of a format without alpha is read as alpha: 1 from the
       * start, for a texture rendered into before anything is uploaded */
      if (!util_format_has_alpha(templ->format) && util_format_get_blocksize(templ->format) == 4)
         memset(res->bo.map, 0xff, sgx545_tex_size(prsc));
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
   e = sgxws_bo_wrap(screen->dev, &res->bo, user_memory, size);
   simple_mtx_unlock(&screen->lock);
   if (e) {
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
   if (res->data)
      align_free(res->data);
   FREE(res);
}

bool
sgx545_resource_target(struct sgx545_screen *screen, struct sgx545_resource *res, unsigned level,
                       unsigned layer, struct sgxws_target *t)
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
      t->pbe_format = 1;
      t->t1_format = 0x0d;         /* swaps R and B against 0x0c: the PBE's order back */
      break;
   default:
      return false;
   }
   if (res->texture) {
      unsigned w = u_minify(res->base.width0, level), h = u_minify(res->base.height0, level);
      bool twiddled = util_is_power_of_two_nonzero(w) && util_is_power_of_two_nonzero(h);

      t->va = res->bo.va + sgx545_tex_offset(&res->base, level, layer);
      t->stride = twiddled ? w : align(w, 8);
      t->t1_layout = twiddled ? 0 : 3;
      sgxws_bo_prepare_target(screen->dev, &res->bo);    /* (the caller holds the lock) */
      res->rendered |= 1u << level;
      if (!level)
         res->level0_gen++;
      return true;
   }
   if (!res->bo.va || !res->pitch || level || layer || util_format_get_blocksize(res->base.format) != 4)
      return false;
   t->va = res->bo.va;
   t->stride = res->pitch;
   t->t1_layout = 6;
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
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr, "sgx545: a render into a mapped buffer did not finish\n");
   }
   /* A texture the GPU rendered into: the CPU copy catches up first */
   if (res->texture && res->rendered && !(usage & PIPE_MAP_UNSYNCHRONIZED)) {
      ctx->stats.fl_read += ctx->recording == res;
      sgx545_flush_resource_writes(ctx, res);
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr, "sgx545: a render into a mapped texture did not finish\n");
      u_foreach_bit(l, res->rendered)
         sgx545_tex_decode_level(res, l);
      res->rendered = 0;
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
         res->gen++;            /* the vertex cache's copies of it are stale */
         res->written_ns = os_time_get_nano();
      }
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
alt_dirty_add(struct sgx545_resource *res, unsigned level, unsigned layer, unsigned x0, unsigned y0,
              unsigned x1, unsigned y1)
{
   if (!res->alt.va || res->n_alt_dirty == SGX545_ALT_ALL)
      return;
   if (res->n_alt_dirty == ARRAY_SIZE(res->alt_dirty)) {
      res->n_alt_dirty = SGX545_ALT_ALL;
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
 * copy.  Minecraft animates sprites of its block atlas every tick (water,
 * lava, fire) while the frames before still sample the atlas: writing under
 * a render in flight would show half-updated sprites, and waiting for the GPU
 * at every tick threw the CPU/GPU overlap away.  So a busy copy is left to
 * its renders and the other copy takes over, brought up to date from the
 * CPU copy first.
 */
static void
tex_write(struct sgx545_context *ctx, struct sgx545_resource *res, unsigned level, unsigned layer,
          unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   uint32_t done = ctx->hw ? sgxws_done_seq(ctx->hw) : 0;

   if (ctx->hw && sgxws_seq_after(res->use_seq, done)) {
      struct sgxws_bo b;
      uint32_t u;

      if (!res->alt.va) {
         simple_mtx_lock(&screen->lock);
         if (sgxws_bo_alloc(screen->dev, &res->alt, SGXWS_MEM_TEXTURE,
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
               sgx545_tex_encode_box(res, res->alt_dirty[i].level, res->alt_dirty[i].layer, res->alt_dirty[i].x0,
                                     res->alt_dirty[i].y0, res->alt_dirty[i].x1, res->alt_dirty[i].y1);
         }
         res->n_alt_dirty = 0;
      }
   }
   sgx545_tex_encode_box(res, level, layer, x0, y0, x1, y1);
   alt_dirty_add(res, level, layer, x0, y0, x1, y1);
   if (!level)
      res->level0_gen++;
}

bool
sgx545_resource_cube0(struct sgx545_context *ctx, struct sgx545_resource *res)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);

   if (!res->cube0.va) {
      simple_mtx_lock(&screen->lock);
      if (sgxws_bo_alloc(screen->dev, &res->cube0, SGXWS_MEM_TEXTURE,
                         align(sgx545_tex_cube0_size(&res->base), 4096), 0x1000)) {
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
      sgx545_flush_resource_writes(ctx, res);
      if (sgxws_bo_wait(&res->bo, 2000))
         fprintf(stderr, "sgx545: a render into a sampled cube map did not finish\n");
      sgx545_tex_decode_level(res, 0);
      res->rendered &= ~1u;
   }
   /* (rare: a render in flight may still sample the old copy) */
   if (ctx->hw && sgxws_seq_after(res->use_seq, sgxws_done_seq(ctx->hw)))
      sgx545_finish(ctx);
   sgx545_tex_encode_cube0(res);
   res->cube0_gen = res->level0_gen;
   return true;
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

      for (int z = ptrans->box.z; z < ptrans->box.z + ptrans->box.depth; z++)
         tex_write(ctx, res, ptrans->level, z, ptrans->box.x, ptrans->box.y,
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
