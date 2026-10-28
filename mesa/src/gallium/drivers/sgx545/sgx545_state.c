/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdlib.h>

#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/format/u_format.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"

/* A blend factor as a sop2 selector (sgxws_bsel), s1 the shader's colour and
 * s2 the tile's; -1 for the constant and dual-source factors (the selectors
 * k18/k19 are not known yet).  Without alpha in the colour buffer its alpha
 * reads as 1. */
static int
blend_sel(enum pipe_blendfactor f, bool alpha, bool dst_alpha)
{
   int s;

   switch (f) {
   case PIPE_BLENDFACTOR_ZERO: s = SGXWS_BS_ZERO; break;
   case PIPE_BLENDFACTOR_ONE: s = SGXWS_BS_ZERO | SGXWS_BS_INV; break;
   case PIPE_BLENDFACTOR_SRC_COLOR: s = alpha ? SGXWS_BS_S1A : SGXWS_BS_S1; break;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR: s = (alpha ? SGXWS_BS_S1A : SGXWS_BS_S1) | SGXWS_BS_INV; break;
   case PIPE_BLENDFACTOR_SRC_ALPHA: s = SGXWS_BS_S1A; break;
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA: s = SGXWS_BS_S1A | SGXWS_BS_INV; break;
   case PIPE_BLENDFACTOR_DST_COLOR: s = alpha ? SGXWS_BS_S2A : SGXWS_BS_S2; break;
   case PIPE_BLENDFACTOR_INV_DST_COLOR: s = (alpha ? SGXWS_BS_S2A : SGXWS_BS_S2) | SGXWS_BS_INV; break;
   case PIPE_BLENDFACTOR_DST_ALPHA: s = SGXWS_BS_S2A; break;
   case PIPE_BLENDFACTOR_INV_DST_ALPHA: s = SGXWS_BS_S2A | SGXWS_BS_INV; break;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
      if (alpha)
         return SGXWS_BS_ZERO | SGXWS_BS_INV;
      return dst_alpha ? SGXWS_BS_ASAT : SGXWS_BS_ZERO;   /* min(As, 1 - 1) */
   default:
      return -1;
   }
   if (!dst_alpha && (s & 7) == SGXWS_BS_S2A)
      s = (s & SGXWS_BS_INV) ^ SGXWS_BS_INV;   /* 1 -> one, 1 - 1 -> zero */
   return s;
}

/* the selector with the sources traded (sop2 o0, o0, pa<c>) */
static int
blend_swap_sel(int s)
{
   static const uint8_t swap[] = { SGXWS_BS_ZERO, SGXWS_BS_S2, SGXWS_BS_S1, SGXWS_BS_S2A, SGXWS_BS_S1A };

   if ((s & 7) >= ARRAY_SIZE(swap))
      return -1;            /* asat is min(s1.a, 1 - s2.a): not swappable */
   return swap[s & 7] | (s & SGXWS_BS_INV);
}

static bool
blend_op(enum pipe_blend_func func, uint8_t *op, bool *reverse)
{
   switch (func) {
   case PIPE_BLEND_ADD: *op = SGXWS_BOP_ADD; return true;
   case PIPE_BLEND_SUBTRACT: *op = SGXWS_BOP_SUB; return true;
   case PIPE_BLEND_REVERSE_SUBTRACT: *op = SGXWS_BOP_SUB; *reverse = true; return true;
   case PIPE_BLEND_MIN: *op = SGXWS_BOP_MIN; return true;
   case PIPE_BLEND_MAX: *op = SGXWS_BOP_MAX; return true;
   default: return false;
   }
}

/* render target 0's blend as the colour epilogue's sop2 (sgxws_blend); false
 * when sop2 can't express it */
static bool
blend_hw(const struct pipe_rt_blend_state *rt, bool dst_alpha, struct sgxws_blend *b)
{
   bool rev_c = false, rev_a = false, min_c, min_a;
   int c1, c2, a1, a2;

   memset(b, 0, sizeof(*b));
   if (!blend_op(rt->rgb_func, &b->cop, &rev_c) || !blend_op(rt->alpha_func, &b->aop, &rev_a))
      return false;
   /* MIN and MAX take no factors */
   min_c = b->cop >= SGXWS_BOP_MIN;
   min_a = b->aop >= SGXWS_BOP_MIN;
   c1 = min_c ? SGXWS_BS_ZERO | SGXWS_BS_INV : blend_sel(rt->rgb_src_factor, false, dst_alpha);
   c2 = min_c ? SGXWS_BS_ZERO | SGXWS_BS_INV : blend_sel(rt->rgb_dst_factor, false, dst_alpha);
   a1 = min_a ? SGXWS_BS_ZERO | SGXWS_BS_INV : blend_sel(rt->alpha_src_factor, true, dst_alpha);
   a2 = min_a ? SGXWS_BS_ZERO | SGXWS_BS_INV : blend_sel(rt->alpha_dst_factor, true, dst_alpha);
   if (c1 < 0 || c2 < 0 || a1 < 0 || a2 < 0)
      return false;
   if (rev_c || rev_a) {
      /* dst - src: the tile becomes s1.  A subtract on the other channel
       * would then run the wrong way; add, min and max don't care */
      if ((!rev_c && b->cop == SGXWS_BOP_SUB) || (!rev_a && b->aop == SGXWS_BOP_SUB))
         return false;
      int t = c1;

      c1 = blend_swap_sel(c2);
      c2 = blend_swap_sel(t);
      t = a1;
      a1 = blend_swap_sel(a2);
      a2 = blend_swap_sel(t);
      if (c1 < 0 || c2 < 0 || a1 < 0 || a2 < 0)
         return false;
      b->swap = 1;
   }
   b->blend = 1;
   b->c1 = c1;
   b->c2 = c2;
   b->a1 = a1;
   b->a2 = a2;
   return true;
}

static void *
sgx545_create_blend_state(struct pipe_context *pctx, const struct pipe_blend_state *cso)
{
   struct sgx545_blend *so = CALLOC_STRUCT(sgx545_blend);

   if (!so)
      return NULL;
   so->base = *cso;
   if (cso->rt[0].blend_enable && !cso->logicop_enable) {
      so->enable = blend_hw(&cso->rt[0], false, &so->hw[0]) && blend_hw(&cso->rt[0], true, &so->hw[1]);
      if (!so->enable)
         fprintf(stderr, "sgx545: blend (func %u/%u, factors %u %u %u %u) not drawn yet, drawing opaque\n",
                 cso->rt[0].rgb_func, cso->rt[0].alpha_func, cso->rt[0].rgb_src_factor,
                 cso->rt[0].rgb_dst_factor, cso->rt[0].alpha_src_factor, cso->rt[0].alpha_dst_factor);
   }
   return so;
}

static void
sgx545_bind_blend_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->blend = hwcso;
}

static void *
sgx545_create_rasterizer_state(struct pipe_context *pctx, const struct pipe_rasterizer_state *cso)
{
   struct sgx545_rasterizer *so = CALLOC_STRUCT(sgx545_rasterizer);

   if (so)
      so->base = *cso;
   return so;
}

static void
sgx545_bind_rasterizer_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->rasterizer = hwcso;
}

static void *
sgx545_create_dsa_state(struct pipe_context *pctx, const struct pipe_depth_stencil_alpha_state *cso)
{
   struct sgx545_dsa *so = CALLOC_STRUCT(sgx545_dsa);

   if (so)
      so->base = *cso;
   return so;
}

static void
sgx545_bind_dsa_state(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->dsa = hwcso;
}

static void
sgx545_delete_state(struct pipe_context *pctx, void *hwcso)
{
   FREE(hwcso);
}

static void *
sgx545_create_vertex_elements(struct pipe_context *pctx, unsigned num_elements,
                              const struct pipe_vertex_element *elements)
{
   struct sgx545_vertex_elements *so = CALLOC_STRUCT(sgx545_vertex_elements);

   if (!so)
      return NULL;
   so->num_elements = num_elements;
   memcpy(so->elements, elements, num_elements * sizeof(*elements));
   return so;
}

static void
sgx545_bind_vertex_elements(struct pipe_context *pctx, void *hwcso)
{
   sgx545_context(pctx)->velems = hwcso;
}

static void
sgx545_set_vertex_buffers(struct pipe_context *pctx, unsigned count,
                          const struct pipe_vertex_buffer *vb)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   util_set_vertex_buffers_count(ctx->vertex_buffers, &ctx->num_vertex_buffers, vb, count);
}

static void
sgx545_set_constant_buffer(struct pipe_context *pctx, mesa_shader_stage shader, uint index,
                           const struct pipe_constant_buffer *cb)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct pipe_constant_buffer *so = &ctx->constants[shader];

   if (index != 0)
      return;
   if (!cb) {
      pipe_resource_reference(&so->buffer, NULL);
      memset(so, 0, sizeof(*so));
      return;
   }
   pipe_resource_reference(&so->buffer, cb->buffer);
   so->buffer_offset = cb->buffer_offset;
   so->buffer_size = cb->buffer_size;
   so->user_buffer = cb->user_buffer;
}

static void
sgx545_set_framebuffer_state(struct pipe_context *pctx, const struct pipe_framebuffer_state *fb)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   util_copy_framebuffer_state(&ctx->framebuffer, fb);
}

static void
sgx545_set_viewport_states(struct pipe_context *pctx, unsigned start_slot, unsigned num_viewports,
                           const struct pipe_viewport_state *viewport)
{
   if (start_slot == 0 && num_viewports)
      sgx545_context(pctx)->viewport = viewport[0];
}

static void
sgx545_set_scissor_states(struct pipe_context *pctx, unsigned start_slot, unsigned num_scissors,
                          const struct pipe_scissor_state *scissor)
{
   /* pass-type-3 objects and region clips (STATE.md section 3): not yet */
}

static void
sgx545_set_blend_color(struct pipe_context *pctx, const struct pipe_blend_color *color)
{
}

static void
sgx545_set_stencil_ref(struct pipe_context *pctx, const struct pipe_stencil_ref ref)
{
}

static void
sgx545_set_sample_mask(struct pipe_context *pctx, unsigned sample_mask)
{
}

static void
sgx545_set_polygon_stipple(struct pipe_context *pctx, const struct pipe_poly_stipple *stipple)
{
}

static void
sgx545_set_clip_state(struct pipe_context *pctx, const struct pipe_clip_state *clip)
{
}

static void *
sgx545_create_sampler_state(struct pipe_context *pctx, const struct pipe_sampler_state *cso)
{
   struct pipe_sampler_state *so = CALLOC_STRUCT(pipe_sampler_state);

   if (so)
      *so = *cso;
   return so;
}

static void
sgx545_bind_sampler_states(struct pipe_context *pctx, mesa_shader_stage shader, unsigned start,
                           unsigned nr, void **hwcso)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < nr && start + i < PIPE_MAX_SAMPLERS; i++)
      ctx->samplers[start + i] = hwcso ? hwcso[i] : NULL;
}

static struct pipe_sampler_view *
sgx545_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *prsc,
                           const struct pipe_sampler_view *cso)
{
   struct pipe_sampler_view *so = CALLOC_STRUCT(pipe_sampler_view);

   if (!so)
      return NULL;
   *so = *cso;
   so->texture = NULL;
   pipe_resource_reference(&so->texture, prsc);
   so->reference.count = 1;
   so->context = pctx;
   return so;
}

static void
sgx545_sampler_view_destroy(struct pipe_context *pctx, struct pipe_sampler_view *view)
{
   pipe_resource_reference(&view->texture, NULL);
   FREE(view);
}

static void
sgx545_set_sampler_views(struct pipe_context *pctx, mesa_shader_stage shader, unsigned start,
                         unsigned nr, unsigned unbind_num_trailing_slots,
                         struct pipe_sampler_view **views)
{
   struct sgx545_context *ctx = sgx545_context(pctx);

   if (shader != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < nr + unbind_num_trailing_slots; i++) {
      unsigned s = start + i;

      if (s < PIPE_MAX_SHADER_SAMPLER_VIEWS)
         pipe_sampler_view_reference(&ctx->views[s], i < nr && views ? views[i] : NULL);
   }
}

static void
sgx545_texture_barrier(struct pipe_context *pctx, unsigned flags)
{
}

/* queries: none of GLES2's need the GPU yet */
struct sgx545_query {
   unsigned type;
};

static struct pipe_query *
sgx545_create_query(struct pipe_context *pctx, unsigned query_type, unsigned index)
{
   struct sgx545_query *q = CALLOC_STRUCT(sgx545_query);

   if (q)
      q->type = query_type;
   return (struct pipe_query *)q;
}

static void
sgx545_destroy_query(struct pipe_context *pctx, struct pipe_query *q)
{
   FREE(q);
}

static bool
sgx545_begin_end_query(struct pipe_context *pctx, struct pipe_query *q)
{
   return true;
}

static bool
sgx545_get_query_result(struct pipe_context *pctx, struct pipe_query *q, bool wait,
                        union pipe_query_result *result)
{
   memset(result, 0, sizeof(*result));
   return true;
}

static void
sgx545_set_active_query_state(struct pipe_context *pctx, bool enable)
{
}

/* copies on the CPU: textures and buffers are CPU-visible, render targets are
 * waited for by the map */
static void
sgx545_resource_copy_region(struct pipe_context *pctx, struct pipe_resource *dst, unsigned dst_level,
                            unsigned dstx, unsigned dsty, unsigned dstz, struct pipe_resource *src,
                            unsigned src_level, const struct pipe_box *src_box)
{
   util_resource_copy_region(pctx, dst, dst_level, dstx, dsty, dstz, src, src_level, src_box);
}

/* A colour blit on the CPU, through the resources' transfers (which wait for
 * the GPU and re-lay textures out): nearest or bilinear, any scale, any pair
 * of formats util_format can unpack and pack. What st/mesa blits are mostly
 * mipmap levels (generated a level from the one above) and glCopyTexImage;
 * the driver has no GPU blit yet. */
static bool
sgx545_blit_cpu(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   const struct pipe_box *sb = &info->src.box, *db = &info->dst.box;
   struct pipe_transfer *st, *dt;
   uint8_t *src, *dst;
   int sw = abs(sb->width), sh = abs(sb->height), dw = abs(db->width), dh = abs(db->height);

   if (info->mask != PIPE_MASK_RGBA && (info->mask & ~PIPE_MASK_RGBA))
      return false;
   if (info->scissor_enable || info->render_condition_enable || info->alpha_blend ||
       info->num_window_rectangles || sb->depth != 1 || db->depth != 1 || !sw || !sh || !dw || !dh ||
       util_format_is_depth_or_stencil(info->src.format) || util_format_is_pure_integer(info->src.format) ||
       !util_format_unpack_description(info->src.format)->unpack_rgba ||
       !util_format_pack_description(info->dst.format)->pack_rgba_float)
      return false;
   src = pipe_texture_map(pctx, info->src.resource, info->src.level, sb->z, PIPE_MAP_READ,
                          MIN2(sb->x, sb->x + sb->width), MIN2(sb->y, sb->y + sb->height), sw, sh, &st);
   if (!src)
      return false;
   dst = pipe_texture_map(pctx, info->dst.resource, info->dst.level, db->z, PIPE_MAP_WRITE,
                          MIN2(db->x, db->x + db->width), MIN2(db->y, db->y + db->height), dw, dh, &dt);
   if (!dst) {
      pipe_texture_unmap(pctx, st);
      return false;
   }

   unsigned sbpp = util_format_get_blocksize(info->src.format), dbpp = util_format_get_blocksize(info->dst.format);
   bool linear = info->filter == PIPE_TEX_FILTER_LINEAR && (sw != dw || sh != dh);
   float *row = malloc(sizeof(float) * 4 * dw);

   for (int y = 0; y < dh && row; y++) {
      /* the destination row's centre in source pixels; a negative size flips */
      float fy = (y + 0.5f) * sh / dh - 0.5f;
      int yy = db->height < 0 ? dh - 1 - y : y;

      for (int x = 0; x < dw; x++) {
         float fx = (x + 0.5f) * sw / dw - 0.5f, c[4] = { 0 };
         int xx = db->width < 0 ? dw - 1 - x : x;

         if (linear) {
            int x0 = MAX2((int)floorf(fx), 0), y0 = MAX2((int)floorf(fy), 0);
            int x1 = MIN2(x0 + 1, sw - 1), y1 = MIN2(y0 + 1, sh - 1);
            float ax = CLAMP(fx - x0, 0.0f, 1.0f), ay = CLAMP(fy - y0, 0.0f, 1.0f), t[4][4];
            const int px[4] = { x0, x1, x0, x1 }, py[4] = { y0, y0, y1, y1 };
            const float wt[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };

            for (int k = 0; k < 4; k++) {
               int sx = sb->width < 0 ? sw - 1 - px[k] : px[k], sy = sb->height < 0 ? sh - 1 - py[k] : py[k];

               util_format_unpack_rgba(info->src.format, t[k], src + sy * st->stride + sx * sbpp, 1);
               for (int i = 0; i < 4; i++)
                  c[i] += wt[k] * t[k][i];
            }
         } else {
            int sx = CLAMP((int)floorf(fx + 0.5f), 0, sw - 1), sy = CLAMP((int)floorf(fy + 0.5f), 0, sh - 1);

            if (sb->width < 0)
               sx = sw - 1 - sx;
            if (sb->height < 0)
               sy = sh - 1 - sy;
            util_format_unpack_rgba(info->src.format, c, src + sy * st->stride + sx * sbpp, 1);
         }
         memcpy(&row[4 * xx], c, sizeof(c));
      }
      util_format_pack_rgba(info->dst.format, dst + yy * dt->stride, row, dw);
   }
   (void)dbpp;
   free(row);
   pipe_texture_unmap(pctx, st);
   pipe_texture_unmap(pctx, dt);
   return true;
}

/* A colour blit into a render target from anything the TPU samples, drawn
 * by u_blitter through our own shaders: the swap's copy of the back buffer
 * to fb0 (a CPU copy reads the back buffer out of uncached memory). */
static bool
sgx545_blit_gpu(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_resource *src = sgx545_resource(info->src.resource);
   struct sgx545_resource *dst = sgx545_resource(info->dst.resource);
   struct pipe_scissor_state scissor = { 0 };
   struct pipe_stencil_ref ref = { 0 };
   unsigned n_samplers = 0, n_views = 0;

   if (!ctx->blitter || info->mask != PIPE_MASK_RGBA || info->scissor_enable ||
       info->render_condition_enable || info->alpha_blend || info->num_window_rectangles ||
       info->src.box.depth != 1 || info->dst.box.depth != 1 || info->dst.level || info->src.level ||
       !sgx545_resource_sampleable(src) || dst->texture || !dst->bo.va || !dst->pitch ||
       util_format_get_blocksize(info->dst.format) != 4 || util_format_is_pure_integer(info->src.format) ||
       util_format_is_depth_or_stencil(info->src.format))
      return false;
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->samplers[i])
         n_samplers = i + 1;
   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++)
      if (ctx->views[i])
         n_views = i + 1;
   util_blitter_save_vertex_buffers(ctx->blitter, ctx->vertex_buffers, ctx->num_vertex_buffers);
   util_blitter_save_vertex_elements(ctx->blitter, ctx->velems);
   util_blitter_save_vertex_shader(ctx->blitter, ctx->vs);
   util_blitter_save_so_targets(ctx->blitter, 0, NULL, MESA_PRIM_UNKNOWN);
   util_blitter_save_rasterizer(ctx->blitter, ctx->rasterizer);
   util_blitter_save_viewport(ctx->blitter, &ctx->viewport);
   util_blitter_save_scissor(ctx->blitter, &scissor);
   util_blitter_save_fragment_shader(ctx->blitter, ctx->fs);
   util_blitter_save_blend(ctx->blitter, ctx->blend);
   util_blitter_save_depth_stencil_alpha(ctx->blitter, ctx->dsa);
   util_blitter_save_stencil_ref(ctx->blitter, &ref);
   util_blitter_save_sample_mask(ctx->blitter, ~0u, 0);
   util_blitter_save_framebuffer(ctx->blitter, &ctx->framebuffer);
   util_blitter_save_fragment_sampler_states(ctx->blitter, n_samplers, (void **)ctx->samplers);
   util_blitter_save_fragment_sampler_views(ctx->blitter, n_views, ctx->views);
   util_blitter_save_fragment_constant_buffer_slot(ctx->blitter, &ctx->constants[MESA_SHADER_FRAGMENT]);
   util_blitter_save_render_condition(ctx->blitter, NULL, false, 0);
   util_blitter_blit(ctx->blitter, info, NULL);
   return true;
}

static void
sgx545_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (sgx545_blit_gpu(pctx, info))
      return;
   if (util_try_blit_via_copy_region(pctx, info, false))
      return;
   if (sgx545_blit_cpu(pctx, info))
      return;
   fprintf(stderr, "sgx545: blit %s -> %s not supported yet\n",
           util_format_short_name(info->src.format), util_format_short_name(info->dst.format));
}

void
sgx545_state_init(struct sgx545_context *ctx)
{
   struct pipe_context *p = &ctx->base;

   p->create_blend_state = sgx545_create_blend_state;
   p->bind_blend_state = sgx545_bind_blend_state;
   p->delete_blend_state = sgx545_delete_state;
   p->create_rasterizer_state = sgx545_create_rasterizer_state;
   p->bind_rasterizer_state = sgx545_bind_rasterizer_state;
   p->delete_rasterizer_state = sgx545_delete_state;
   p->create_depth_stencil_alpha_state = sgx545_create_dsa_state;
   p->bind_depth_stencil_alpha_state = sgx545_bind_dsa_state;
   p->delete_depth_stencil_alpha_state = sgx545_delete_state;
   p->create_vertex_elements_state = sgx545_create_vertex_elements;
   p->bind_vertex_elements_state = sgx545_bind_vertex_elements;
   p->delete_vertex_elements_state = sgx545_delete_state;
   p->set_vertex_buffers = sgx545_set_vertex_buffers;
   p->set_constant_buffer = sgx545_set_constant_buffer;
   p->set_framebuffer_state = sgx545_set_framebuffer_state;
   p->set_viewport_states = sgx545_set_viewport_states;
   p->set_scissor_states = sgx545_set_scissor_states;
   p->set_blend_color = sgx545_set_blend_color;
   p->set_stencil_ref = sgx545_set_stencil_ref;
   p->set_sample_mask = sgx545_set_sample_mask;
   p->set_polygon_stipple = sgx545_set_polygon_stipple;
   p->set_clip_state = sgx545_set_clip_state;
   p->create_sampler_state = sgx545_create_sampler_state;
   p->bind_sampler_states = sgx545_bind_sampler_states;
   p->delete_sampler_state = sgx545_delete_state;
   p->create_sampler_view = sgx545_create_sampler_view;
   p->sampler_view_destroy = sgx545_sampler_view_destroy;
   p->sampler_view_release = u_default_sampler_view_release;
   p->set_sampler_views = sgx545_set_sampler_views;
   p->texture_barrier = sgx545_texture_barrier;
   p->resource_release = u_default_resource_release;
   p->create_query = sgx545_create_query;
   p->destroy_query = sgx545_destroy_query;
   p->begin_query = sgx545_begin_end_query;
   p->end_query = sgx545_begin_end_query;
   p->get_query_result = sgx545_get_query_result;
   p->set_active_query_state = sgx545_set_active_query_state;
   p->resource_copy_region = sgx545_resource_copy_region;
   p->blit = sgx545_blit;
}
