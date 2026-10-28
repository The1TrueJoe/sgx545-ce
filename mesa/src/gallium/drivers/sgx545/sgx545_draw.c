/*
 * Copyright © 2026 The sgx545-ce authors
 * SPDX-License-Identifier: MIT
 */

/*
 * Draws and clears: gallium state in, sgxws objects out.  The vertices a
 * draw uses are gathered on the CPU (indices resolved, attributes converted
 * to the dwords the vertex program fetches) and copied into the frame's
 * ring; the hardware then draws them through its identity index list.
 */

#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/u_inlines.h"
#include "util/u_prim.h"

#include "sgx545_context.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"

static struct sgxws_rt *
get_rt(struct sgx545_context *ctx, uint32_t w, uint32_t h)
{
   for (unsigned i = 0; i < ctx->num_rts; i++)
      if (ctx->rts[i]->w == w && ctx->rts[i]->h == h)
         return ctx->rts[i];
   if (ctx->num_rts == SGX545_MAX_RTS)
      return NULL;
   ctx->rts[ctx->num_rts] = sgxws_rt_create(ctx->hw, w, h);
   return ctx->rts[ctx->num_rts] ? ctx->rts[ctx->num_rts++] : NULL;
}

/* start (or continue) the frame into colour buffer 0; called with the lock held */
static bool
begin_frame(struct sgx545_context *ctx)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   struct pipe_surface *cbuf = &ctx->framebuffer.cbufs[0];
   struct sgx545_resource *res;
   struct sgxws_target t;
   struct sgxws_rt *rt;

   if (!ctx->framebuffer.nr_cbufs || !cbuf->texture) {
      SGX545_DBG(DRAW, "no colour buffer bound\n");
      return false;
   }
   res = sgx545_resource(cbuf->texture);
   if (cbuf->level || !sgx545_resource_target(screen, res, &t)) {
      SGX545_DBG(DRAW, "colour buffer %s %ux%u is not a render target\n",
                 util_format_short_name(cbuf->texture->format), cbuf->texture->width0,
                 cbuf->texture->height0);
      return false;
   }
   rt = get_rt(ctx, res->base.width0, res->base.height0);
   if (!rt)
      return false;
   if (ctx->recording && ctx->recording != res)
      ctx->recording = NULL;    /* sgxws_begin flushes the other frame */
   if (sgxws_begin(ctx->hw, rt, &t))
      return false;
   ctx->recording = res;
   return true;
}

static void
sgx545_clear(struct pipe_context *pctx, unsigned buffers, uint32_t color_clear_mask,
             uint8_t stencil_clear_mask, const struct pipe_scissor_state *scissor_state,
             const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_screen *screen = sgx545_screen(pctx->screen);
   bool have_depth = ctx->framebuffer.zsbuf.texture != NULL;

   SGX545_DBG(DRAW, "clear %#x (%.3f %.3f %.3f %.3f) depth %.3f\n", buffers, color->f[0], color->f[1],
              color->f[2], color->f[3], depth);
   if (!(buffers & PIPE_CLEAR_COLOR0) && !(have_depth && (buffers & PIPE_CLEAR_DEPTH)))
      return;
   simple_mtx_lock(&screen->lock);
   if (begin_frame(ctx)) {
      static const float black[4];

      /* The ISP starts every render at depth 1.0 (render details +0x40), so
       * a depth clear only needs drawing when it is not 1.0. */
      sgxws_clear(ctx->hw, (buffers & PIPE_CLEAR_COLOR0) ? color->f : black, (float)depth,
                  have_depth && (buffers & PIPE_CLEAR_DEPTH));
   }
   simple_mtx_unlock(&screen->lock);
}

/* the ISPA depth compare is the GL enum's low bits (STATE.md section 3) */
static uint32_t
depth_func(const struct sgx545_context *ctx)
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa ? &ctx->dsa->base : NULL;

   if (!dsa || !dsa->depth_enabled || !ctx->framebuffer.zsbuf.texture)
      return 0;
   return SGXWS_NEVER + dsa->depth_func;   /* PIPE_FUNC_* is GL's order */
}

static void
build_state(struct sgx545_context *ctx, struct sgxws_state *st)
{
   const struct pipe_rasterizer_state *rs = ctx->rasterizer ? &ctx->rasterizer->base : NULL;
   unsigned mask = ctx->blend ? ctx->blend->base.rt[0].colormask : PIPE_MASK_RGBA;

   memset(st, 0, sizeof(*st));
   st->depth_func = depth_func(ctx);
   st->depth_write = st->depth_func && ctx->dsa->base.depth_writemask;
   if (rs && rs->cull_face == PIPE_FACE_BACK)
      st->cull = SGXWS_BACK;
   else if (rs && rs->cull_face == PIPE_FACE_FRONT)
      st->cull = SGXWS_FRONT;
   st->front_ccw = rs ? rs->front_ccw : true;
   /* ISPB colour mask: R8 G4 B2 A1 */
   st->color_mask = (mask & PIPE_MASK_R ? 8 : 0) | (mask & PIPE_MASK_G ? 4 : 0) |
                    (mask & PIPE_MASK_B ? 2 : 0) | (mask & PIPE_MASK_A ? 1 : 0);
   memcpy(st->vp_scale, ctx->viewport.scale, sizeof(st->vp_scale));
   memcpy(st->vp_translate, ctx->viewport.translate, sizeof(st->vp_translate));
}

static const void *
constant_data(struct sgx545_context *ctx, mesa_shader_stage stage)
{
   struct pipe_constant_buffer *cb = &ctx->constants[stage];

   if (cb->user_buffer)
      return (const uint8_t *)cb->user_buffer + cb->buffer_offset;
   if (cb->buffer)
      return (const uint8_t *)sgx545_resource(cb->buffer)->data + cb->buffer_offset;
   return NULL;
}

/* where the vertex program's attribute 0 comes from */
struct fetch {
   const uint8_t *base;
   uint32_t stride;
   enum pipe_format format;
   unsigned dwords;
};

static bool
setup_fetch(struct sgx545_context *ctx, struct fetch *f)
{
   const struct pipe_vertex_element *ve;
   const struct pipe_vertex_buffer *vb;

   if (!ctx->velems || !ctx->velems->num_elements)
      return false;
   ve = &ctx->velems->elements[0];
   if (ve->vertex_buffer_index >= ctx->num_vertex_buffers || ve->instance_divisor)
      return false;
   vb = &ctx->vertex_buffers[ve->vertex_buffer_index];
   if (vb->is_user_buffer)
      f->base = vb->buffer.user;
   else if (vb->buffer.resource)
      f->base = sgx545_resource(vb->buffer.resource)->data;
   else
      return false;
   f->base += vb->buffer_offset + ve->src_offset;
   f->stride = ve->src_stride;
   f->format = ve->src_format;
   return true;
}

static void
fetch_vertex(const struct fetch *f, unsigned index, uint32_t *out)
{
   const uint8_t *src = f->base + (size_t)index * f->stride;
   float v[4];

   if (f->format == PIPE_FORMAT_R32G32_FLOAT || f->format == PIPE_FORMAT_R32G32B32_FLOAT ||
       f->format == PIPE_FORMAT_R32G32B32A32_FLOAT) {
      memcpy(out, src, f->dwords * 4);
      return;
   }
   util_format_unpack_rgba(f->format, v, src, 1);
   memcpy(out, v, f->dwords * 4);
}

static uint32_t
get_index(const struct pipe_draw_info *info, const void *indices, unsigned i)
{
   switch (info->index_size) {
   case 1: return ((const uint8_t *)indices)[i];
   case 2: return ((const uint16_t *)indices)[i];
   default: return ((const uint32_t *)indices)[i];
   }
}

static uint32_t *
staging(struct sgx545_context *ctx, size_t dwords)
{
   if (dwords * 4 > ctx->staging_size) {
      free(ctx->staging);
      ctx->staging_size = MAX2(dwords * 4, 64 * 1024);
      ctx->staging = malloc(ctx->staging_size);
      if (!ctx->staging)
         ctx->staging_size = 0;
   }
   return ctx->staging;
}

static void
sgx545_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
                unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
                const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_screen *screen = sgx545_screen(pctx->screen);
   struct sgxws_state st;
   struct sgxws_draw d = { 0 };
   uint32_t fsc[SGXWS_FS_UNIFORM_DWORDS] = { 0 };
   const void *indices = NULL;
   struct fetch f;
   float color[4];
   uint32_t prim;

   if (indirect || !ctx->vs || !ctx->fs || ctx->vs->kind != SGX545_SHADER_VS_POSITION)
      return;
   switch (ctx->fs->kind) {
   case SGX545_SHADER_FS_CONSTANT:
      memcpy(color, ctx->fs->color, sizeof(color));
      break;
   case SGX545_SHADER_FS_UNIFORM: {
      const uint8_t *cb = constant_data(ctx, MESA_SHADER_FRAGMENT);

      if (!cb)
         return;
      memcpy(color, cb + ctx->fs->ubo_offset, sizeof(color));
      break;
   }
   default:
      return;
   }
   switch (info->mode) {
   case MESA_PRIM_TRIANGLES:
      prim = SGXWS_TRIANGLES;
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      prim = SGXWS_TRIANGLE_STRIP;
      break;
   case MESA_PRIM_TRIANGLE_FAN:
      prim = SGXWS_TRIANGLES;     /* drawn as a list */
      break;
   default:
      if (!ctx->warned_prim)
         fprintf(stderr, "sgx545: %s not drawn yet\n", u_prim_name(info->mode));
      ctx->warned_prim = true;
      return;
   }
   if (!setup_fetch(ctx, &f))
      return;
   /* gl_Position = attribute 0: two components fetched -> (x, y, 0, 1); more -> (x, y, z, 1) */
   f.dwords = util_format_get_nr_components(f.format) == 2 ? 2 : 3;
   d.vs = f.dwords == 2 ? &ctx->vs_xy : &ctx->vs_xyz;
   d.fs = &ctx->fs_uniform;
   fsc[SGXWS_FS_UNIFORM_OFF] = sgxws_f16(color[0]) | sgxws_f16(color[1]) << 16;
   fsc[SGXWS_FS_UNIFORM_OFF + 1] = sgxws_f16(color[2]) | sgxws_f16(color[3]) << 16;
   d.fs_consts = fsc;
   build_state(ctx, &st);
   d.state = &st;
   d.prim = prim;

   if (info->index_size) {
      if (info->has_user_indices)
         indices = info->index.user;
      else
         indices = sgx545_resource(info->index.resource)->data;
   }

   simple_mtx_lock(&screen->lock);
   if (!begin_frame(ctx))
      goto out;
   for (unsigned n = 0; n < num_draws; n++) {
      unsigned count = draws[n].count, out_count;
      uint32_t *v;

      if (info->mode == MESA_PRIM_TRIANGLE_FAN)
         out_count = count >= 3 ? (count - 2) * 3 : 0;
      else
         out_count = count;
      if (!out_count || !(v = staging(ctx, (size_t)out_count * f.dwords)))
         continue;
      for (unsigned i = 0; i < out_count; i++) {
         unsigned k = i;
         uint32_t index;

         if (info->mode == MESA_PRIM_TRIANGLE_FAN)
            k = (i % 3 == 0) ? 0 : i / 3 + i % 3;
         index = info->index_size ? get_index(info, indices, draws[n].start + k) + draws[n].index_bias
                                  : draws[n].start + k;
         fetch_vertex(&f, index, v + i * f.dwords);
      }
      d.verts = v;
      d.count = out_count;
      SGX545_DBG(DRAW, "draw %s x%u\n", u_prim_name(info->mode), out_count);
      if (sgxws_draw(ctx->hw, &d))
         fprintf(stderr, "sgx545: draw of %u vertices dropped\n", out_count);
   }
out:
   simple_mtx_unlock(&screen->lock);
}

void
sgx545_draw_init(struct sgx545_context *ctx)
{
   ctx->base.draw_vbo = sgx545_draw_vbo;
   ctx->base.clear = sgx545_clear;
}
