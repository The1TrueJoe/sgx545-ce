/*
 * Copyright © 2026 Joseph Telaak
 * SPDX-License-Identifier: MIT
 */

/*
 * Draws and clears: gallium state in, sgxws objects out.  The vertices a
 * draw uses are gathered on the CPU (indices resolved, attributes converted
 * to the dwords the vertex program fetches) and copied into the frame's
 * ring; the hardware then draws them through its identity index list.
 */

#include "compiler/nir/nir.h"
#include "util/format/u_format.h"
#include "util/half_float.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/os_time.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_prim.h"
#include "util/u_prim_restart.h"

#include "sgx_compiler.h"
#include "sgx_context.h"
#include "sgx_csb.h"
#include "sgx_program.h"
#include "sgx_resource.h"
#include "sgx_screen.h"
#include "sgx_texture.h"

/* A target's TA/3D structures, one set per target address while there are
 * few: a target has one render in flight, so the back buffer and fb0 (the
 * swap's copy) need sets of their own to render one after the other without
 * waiting.  Past SGX_MAX_RTS targets share by size, and a size none has
 * takes the place of the set used longest ago (never the frame being
 * recorded, which was taken last). */
static struct sgxws_rt *
get_rt(struct sgx_context *ctx, uint32_t w, uint32_t h, uint32_t va,
       unsigned samples)
{
   unsigned i, n = ctx->num_rts;

   for (i = 0; i < n; i++)
      if (ctx->rt_w[i] == w && ctx->rt_h[i] == h && ctx->rt_va[i] == va &&
          ctx->rt_samples[i] == samples)
         goto found;
   if (n == SGX_MAX_RTS) {
      for (i = 0; i < n; i++)
         if (ctx->rt_w[i] == w && ctx->rt_h[i] == h &&
             ctx->rt_samples[i] == samples)
            goto found;
      i = 0;
      for (unsigned j = 1; j < n; j++)
         if (ctx->rt_used[j] < ctx->rt_used[i])
            i = j;
      sgxws_rt_destroy(ctx->hw, ctx->rts[i]); /* waits for its renders */
   } else {
      i = ctx->num_rts++;
   }
   ctx->rts[i] = sgxws_rt_create_samples(ctx->hw, w, h, samples);
   ctx->rt_va[i] = va;
   ctx->rt_w[i] = w;
   ctx->rt_h[i] = h;
   ctx->rt_samples[i] = samples;
   if (!ctx->rts[i]) {
      /* none made: the place goes (the last set moves into it) */
      ctx->rts[i] = ctx->rts[--ctx->num_rts];
      ctx->rt_va[i] = ctx->rt_va[ctx->num_rts];
      ctx->rt_w[i] = ctx->rt_w[ctx->num_rts];
      ctx->rt_h[i] = ctx->rt_h[ctx->num_rts];
      ctx->rt_samples[i] = ctx->rt_samples[ctx->num_rts];
      ctx->rt_used[i] = ctx->rt_used[ctx->num_rts];
      return NULL;
   }
found:
   ctx->rt_used[i] = ++ctx->rt_clock;
   return ctx->rts[i];
}

/* The depth/stencil buffer of a render that starts now, kept across renders
 * (ZLS): its buffer, which the render stores at its end, loaded unless the
 * render starts with a clear of all of it (zs_cleared) or nothing was stored
 * at this size.  A 4x render keeps a depth and a stencil per sample, 4x the
 * bytes, and needs the buffer even with no depth buffer bound: then it's
 * the context's own, never loaded (STATE.md "Multisampling (4x)": the DDK
 * allocates one too). */
static bool
zls_target(struct sgx_context *ctx, struct sgx_resource *zs, uint32_t w,
           uint32_t h, bool zs_cleared, unsigned samples, struct sgxws_target *t)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   uint32_t bytes = align(sgxws_zls_pixels(w, h) * 5 * samples, 4096);

   if (!zs && samples > 1) {
      if (sgxws_bo_valid(&ctx->ms_zls) && ctx->ms_zls_bytes < bytes) {
         sgxws_flush(ctx->hw, 0);
         ctx->recording = NULL;
         sgxws_wait_idle(ctx->hw, 2000);
         sgxws_bo_free(screen->dev, &ctx->ms_zls);
      }
      if (!sgxws_bo_valid(&ctx->ms_zls)) {
         if (sgxws_bo_alloc(screen->dev, &ctx->ms_zls, SGXWS_MEM_ZLS, bytes,
                            0x1000))
            return false;
         ctx->ms_zls_bytes = bytes;
      }
      t->zls_va = ctx->ms_zls.va;
      t->zls_load = false;
      return true;
   }
   if (!zs || (sgx_drv_debug & SGX_DEBUG_NO_ZLS))
      return true;
   if (sgxws_bo_valid(&zs->zls) && zs->zls_bytes < bytes) {
      /* renders that store into it: the frame being recorded (another
       * target's, which begin_frame hasn't flushed yet) too */
      sgxws_flush(ctx->hw, 0);
      ctx->recording = NULL;
      sgxws_wait_idle(ctx->hw, 2000);
      sgxws_bo_free(screen->dev, &zs->zls);
      zs->zls_w = zs->zls_h = 0;
   }
   if (!sgxws_bo_valid(&zs->zls)) {
      if (sgxws_bo_alloc(screen->dev, &zs->zls, SGXWS_MEM_ZLS, bytes, 0x1000))
         return false;
      zs->zls_bytes = bytes;
   }
   t->zls_va = zs->zls.va;
   t->zls_load = !zs_cleared && zs->zls_w == w && zs->zls_h == h;
   SGX_DBG(SYNC, "depth/stencil %s %ux%u: ZLS buffer at %#x, %s\n",
           util_format_short_name(zs->base.format), w, h, t->zls_va,
           t->zls_load ? "load and store" : "store");
   zs->zls_w = w;
   zs->zls_h = h;
   return true;
}

/* start (or continue) the frame into colour buffer 0; called with the lock
 * held.  zs_cleared: the caller clears all of the depth/stencil buffer, so a
 * render that starts here needn't load it. */
static bool
begin_frame(struct sgx_context *ctx, bool zs_cleared)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   const struct pipe_surface *cbuf = sgx_cbuf(ctx);
   struct sgx_resource *res, *zs;
   struct sgxws_target t;
   struct sgxws_rt *rt;
   uint32_t rw, rh;
   unsigned level = 0, layer = 0;

   if (cbuf->texture) {
      res = sgx_resource(cbuf->texture);
      level = cbuf->level;
      layer = cbuf->first_layer;
   } else if (ctx->framebuffer.zsbuf.texture && ctx->dummy_cb) {
      /* depth only: the context's own colour buffer (set_framebuffer_state) */
      res = sgx_resource(ctx->dummy_cb);
   } else {
      SGX_DBG(DRAW, "no colour buffer bound\n");
      return false;
   }
   if (!sgx_resource_target(screen, res, level, layer, &t)) {
      SGX_DBG(DRAW, "colour buffer %s %ux%u is not a render target\n",
              util_format_short_name(res->base.format), res->base.width0,
              res->base.height0);
      return false;
   }
   t.scanout = res->wrapped;
   /* a planar colour buffer: this pass's plane */
   t.va += ctx->rt_plane * sgx_tex_plane_stride(&res->base);
   /* the render is the framebuffer's size: with attachments of different
    * sizes that is the smallest (st takes their intersection), which may be
    * less than the colour buffer's */
   rw = u_minify(res->base.width0, level);
   rh = u_minify(res->base.height0, level);
   if (ctx->framebuffer.width && ctx->framebuffer.height) {
      rw = MIN2(rw, ctx->framebuffer.width);
      rh = MIN2(rh, ctx->framebuffer.height);
   }
   /* 4x: rendered at 2x2 samples a pixel and resolved as the tiles are
    * stored, so the colour buffer holds the resolved pixels */
   t.samples = MAX2(res->base.nr_samples, 1);
   rt = get_rt(ctx, rw, rh, t.va, t.samples);
   if (!rt)
      return false;
   zs = ctx->framebuffer.zsbuf.texture
           ? sgx_resource(ctx->framebuffer.zsbuf.texture)
           : NULL;
   bool clears_only = sgx_recording(ctx) && ctx->frame_clears &&
                      sgxws_frame_objects(ctx->hw) == ctx->frame_clears;

   if (sgx_recording(ctx) &&
       (ctx->recording != res || ctx->recording_plane != ctx->rt_plane)) {
      SGX_DBG(SYNC, "target switch: %s %ux%u -> %s %ux%u\n",
              util_format_short_name(ctx->recording->base.format),
              ctx->recording->base.width0, ctx->recording->base.height0,
              util_format_short_name(res->base.format), res->base.width0,
              res->base.height0);
      ctx->recording = NULL; /* sgxws_begin flushes the other frame */
      ctx->stats.fl_switch++;
      ctx->stats.fl_switch_clear += clears_only;
   } else if (sgx_recording(ctx) && ctx->recording_zs != zs) {
      SGX_DBG(SYNC, "depth/stencil buffer switch under %s %ux%u\n",
              util_format_short_name(res->base.format), res->base.width0,
              res->base.height0);
      sgxws_flush(ctx->hw, 0);
      ctx->recording = NULL;
      ctx->stats.fl_switch++;
      ctx->stats.fl_switch_clear += clears_only;
   }
   if (!sgx_recording(ctx)) {
      if (!zls_target(ctx, zs, rw, rh, zs_cleared, t.samples, &t))
         return false;
      ctx->frame_clears = 0;
   }
   if (sgxws_begin(ctx->hw, rt, &t))
      return false;
   ctx->recording = res;
   ctx->recording_zs = zs;
   ctx->recording_plane = ctx->rt_plane;
   /* its ZLS buffer is stored into by this render: freeing it flushes and
    * waits (sgx_resource_destroy).  After sgxws_begin, which may have
    * flushed another target's frame: the next kick is this render's. */
   if (zs && sgxws_bo_valid(&zs->zls) && t.zls_va == zs->zls.va) {
      zs->use_seq = sgxws_next_seq(ctx->hw);
      zs->zls_stored = true;
   }
   /* a write to the texture waits for this render, as for one that samples it */
   if (res->texture)
      res->use_seq = sgxws_next_seq(ctx->hw);
   return true;
}

static unsigned target_mask(const struct sgx_context *ctx);

/* GL's colour mask as bytes of the word a raw 32-bit colour buffer gets:
 * an R32 word, or the plane of channel rt_plane of an RG32 / RGBA32 one, is
 * that one channel, all four bytes or none */
static unsigned
word_mask(const struct sgx_context *ctx, unsigned mask)
{
   if (!sgx_cbuf(ctx)->texture ||
       !sgx_tex_raw32(sgx_cbuf(ctx)->format))
      return mask;
   return mask >> ctx->rt_plane & 1 ? PIPE_MASK_RGBA : 0;
}

/* the planes of a planar colour buffer (RG32 2, RGBA32 4), 0 for others */
static unsigned
planar_planes(const struct sgx_context *ctx)
{
   if (!sgx_cbuf(ctx)->texture)
      return 0;
   unsigned n = sgx_tex_raw32(sgx_cbuf(ctx)->format);
   return n > 1 ? n : 0;
}

/* does the bound fragment shader write colour buffer t (gl_FragColor writes
 * them all) */
static bool
fs_writes_target(const struct sgx_context *ctx, unsigned t)
{
   const nir_shader *nir = ctx->fs ? ctx->fs->nir : NULL;

   if (!nir)
      return t == 0;
   return nir->info.outputs_written &
          (BITFIELD64_BIT(FRAG_RESULT_COLOR) |
           BITFIELD64_BIT(FRAG_RESULT_DATA0 + t));
}

/* the bound colour mask of render target 0 */
static unsigned
colour_mask(const struct sgx_context *ctx)
{
   return ctx->blend ? ctx->blend->base.rt[0].colormask : PIPE_MASK_RGBA;
}

static void clear_one(struct pipe_context *pctx, unsigned buffers,
                      uint32_t color_clear_mask, uint8_t stencil_clear_mask,
                      const struct pipe_scissor_state *scissor_state,
                      const union pipe_color_union *color, double depth,
                      unsigned stencil);

/* A planar colour buffer is cleared a plane at a time (channel c's value
 * into plane c), depth and stencil with the first */
static void clear_target(struct pipe_context *pctx, unsigned buffers,
                         uint32_t color_clear_mask, uint8_t stencil_clear_mask,
                         const struct pipe_scissor_state *scissor_state,
                         const union pipe_color_union *color, double depth,
                         unsigned stencil);

/* MRT: a clear a colour buffer (PIPE_CLEAR_COLOR0 << t), depth and stencil
 * with the first */
static void
sgx_clear(struct pipe_context *pctx, unsigned buffers,
          uint32_t color_clear_mask, uint8_t stencil_clear_mask,
          const struct pipe_scissor_state *scissor_state,
          const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct sgx_context *ctx = sgx_context(pctx);
   unsigned targets = MAX2(ctx->framebuffer.nr_cbufs, 1);
   unsigned zs = buffers & PIPE_CLEAR_DEPTHSTENCIL;
   bool done = false;

   for (unsigned t = 0; t < targets; t++) {
      bool colour = buffers & (PIPE_CLEAR_COLOR0 << t);

      ctx->rt_index = t;
      if (!colour && (done || !zs))
         continue;
      if (targets > 1 && !sgx_cbuf(ctx)->texture && done)
         continue;
      clear_target(pctx, (colour ? PIPE_CLEAR_COLOR0 : 0) | (done ? 0 : zs),
                   color_clear_mask, stencil_clear_mask, scissor_state, color,
                   depth, stencil);
      done = true;
   }
   ctx->rt_index = 0;
}

/* a clear of the current colour buffer (rt_index), a plane at a time for
 * a planar one */
static void
clear_target(struct pipe_context *pctx, unsigned buffers,
             uint32_t color_clear_mask, uint8_t stencil_clear_mask,
             const struct pipe_scissor_state *scissor_state,
             const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct sgx_context *ctx = sgx_context(pctx);
   unsigned planes = planar_planes(ctx);

   if (!planes) {
      clear_one(pctx, buffers, color_clear_mask, stencil_clear_mask,
                scissor_state, color, depth, stencil);
      return;
   }
   for (unsigned c = 0; c < planes; c++) {
      union pipe_color_union v = {.ui = {color->ui[c]}};
      unsigned b = c ? buffers & PIPE_CLEAR_COLOR0 : buffers;

      if (!(buffers & PIPE_CLEAR_COLOR0) && c)
         break;
      ctx->rt_plane = c;
      clear_one(pctx, b, color_clear_mask >> c & 1 ? PIPE_MASK_R : 0,
                stencil_clear_mask, scissor_state, &v, depth, stencil);
   }
   ctx->rt_plane = 0;
}

static void
clear_one(struct pipe_context *pctx, unsigned buffers,
          uint32_t color_clear_mask, uint8_t stencil_clear_mask,
          const struct pipe_scissor_state *scissor_state,
          const union pipe_color_union *color, double depth, unsigned stencil)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_screen *screen = sgx_screen(pctx->screen);
   bool have_depth = ctx->framebuffer.zsbuf.texture != NULL;

   SGX_DBG(DRAW,
           "clear %#x mask %#x (%.3f %.3f %.3f %.3f) depth %.3f, %u draws in "
           "the frame\n",
           buffers, color_clear_mask, color->f[0], color->f[1], color->f[2],
           color->f[3], depth, ctx->hw ? sgxws_frame_objects(ctx->hw) : 0);
   /* the colour bytes written, as the draws' colour mask (B b0 G b1 R b2 A
    * b3); a buffer without alpha keeps its X byte written. A colour clear
    * with a mask comes here, not as a quad: glColorMask(0, 0, 0, 1) and a
    * clear must not touch RGB. */
   unsigned cm = buffers & PIPE_CLEAR_COLOR0 ? color_clear_mask & 0xf : 0,
            wmask;
   bool clear_stencil = have_depth && (buffers & PIPE_CLEAR_STENCIL) &&
                        util_format_has_stencil(util_format_description(
                           ctx->framebuffer.zsbuf.format));

   if (sgx_cbuf(ctx)->texture &&
       !util_format_has_alpha(sgx_cbuf(ctx)->format) && cm)
      cm |= PIPE_MASK_A;
   /* R8 and RG8 (stored as RGBA8): a clear of all their channels writes
    * every byte with the colour they read as, (r, g or 0, 0, 1) */
   unsigned tm = target_mask(ctx);
   bool rg_whole = tm != PIPE_MASK_RGBA && (cm & tm) == tm;

   cm = rg_whole ? PIPE_MASK_RGBA : cm & tm;
   /* an R32 word, or a plane, is one channel: its four bytes or none */
   if (sgx_cbuf(ctx)->texture &&
       sgx_tex_raw32(sgx_cbuf(ctx)->format))
      cm = cm & PIPE_MASK_R ? PIPE_MASK_RGBA : 0;
   wmask = (cm & PIPE_MASK_B ? 1 : 0) | (cm & PIPE_MASK_G ? 2 : 0) |
           (cm & PIPE_MASK_R ? 4 : 0) | (cm & PIPE_MASK_A ? 8 : 0);
   /* a depth-only clear writes no colour (mask 0) */
   if (!wmask && !(have_depth && (buffers & PIPE_CLEAR_DEPTH)) &&
       !clear_stencil)
      return; /* nothing to clear: every colour channel masked, no depth or
                 stencil */
   /* all of depth and stencil cleared: a render starting here loads neither */
   bool zs_cleared =
      have_depth && (buffers & PIPE_CLEAR_DEPTH) && !scissor_state &&
      (!util_format_has_stencil(
          util_format_description(ctx->framebuffer.zsbuf.format)) ||
       (clear_stencil && stencil_clear_mask == 0xff));

   simple_mtx_lock(&screen->lock);
   if (begin_frame(ctx, zs_cleared)) {
      static const float black[4];

      /* A render that loads no depth starts at 1.0 (render details +0x40):
       * sgxws_clear skips a 1.0 depth clear there.  A depth-only clear of a
       * stencil format loads the stencil, and the depth with it, so it's
       * drawn (ioquake3 starts every frame so). */
      /* a buffer without alpha reads it as 1 wherever it's sampled: clear the X
       * byte to that */
      float rgba[4] = {color->f[0], color->f[1], color->f[2], color->f[3]};

      if (sgx_cbuf(ctx)->texture &&
          !util_format_has_alpha(sgx_cbuf(ctx)->format))
         rgba[3] = 1.0f;
      if (rg_whole) {
         if (!(tm & PIPE_MASK_G))
            rgba[1] = 0.0f;
         rgba[2] = 0.0f;
      }
      /* an integer target: the word's bytes as k / 255, which the clear
       * converts back to exactly k (the colour word is B, G, R, A bytes):
       * each channel's low byte for an 8-bit format (1 for a missing
       * alpha), an R32 value's four bytes */
      if (sgx_cbuf(ctx)->texture &&
          util_format_is_pure_integer(sgx_cbuf(ctx)->format)) {
         enum pipe_format f = sgx_cbuf(ctx)->format;
         uint32_t v = color->ui[0];

         if (sgx_tex_raw32(f)) {
            rgba[0] = (v >> 16 & 0xff) / 255.0f;
            rgba[1] = (v >> 8 & 0xff) / 255.0f;
            rgba[2] = (v & 0xff) / 255.0f;
            rgba[3] = (v >> 24) / 255.0f;
         } else {
            for (unsigned c = 0; c < 4; c++)
               rgba[c] = (color->ui[c] & 0xff) / 255.0f;
            if (!util_format_has_alpha(f))
               rgba[3] = 1.0f / 255.0f;
            if (rg_whole) {
               if (!(tm & PIPE_MASK_G))
                  rgba[1] = 0.0f;
               rgba[2] = 0.0f;
            }
         }
      }
      unsigned n0 = sgxws_frame_objects(ctx->hw);

      sgxws_clear(ctx->hw, (buffers & PIPE_CLEAR_COLOR0) ? rgba : black,
                  (float)depth, have_depth && (buffers & PIPE_CLEAR_DEPTH),
                  wmask, clear_stencil ? (int)stencil : -1);
      ctx->frame_clears += sgxws_frame_objects(ctx->hw) - n0;
   }
   simple_mtx_unlock(&screen->lock);
}

/* The channels a target takes: R8 and RG8 are stored as RGBA8 with B = 0
 * and A = 255 (sgx_tex_bpp), which draws and clears leave as they are. */
static unsigned
target_mask(const struct sgx_context *ctx)
{
   if (!sgx_cbuf(ctx)->texture)
      return PIPE_MASK_RGBA;
   switch (sgx_cbuf(ctx)->format) {
   case PIPE_FORMAT_R8_UNORM:
   case PIPE_FORMAT_R8_UINT:
   case PIPE_FORMAT_R8_SINT:
      return PIPE_MASK_R;
   case PIPE_FORMAT_R8G8_UNORM:
   case PIPE_FORMAT_R8G8_UINT:
   case PIPE_FORMAT_R8G8_SINT:
      return PIPE_MASK_R | PIPE_MASK_G;
   default:
      return PIPE_MASK_RGBA;
   }
}

/* the ISPA depth compare is the GL enum's low bits (STATE.md section 3) */
static uint32_t
depth_func(const struct sgx_context *ctx)
{
   const struct pipe_depth_stencil_alpha_state *dsa =
      ctx->dsa ? &ctx->dsa->base : NULL;

   if (!dsa || !dsa->depth_enabled || !ctx->framebuffer.zsbuf.texture)
      return 0;
   return SGXWS_NEVER + dsa->depth_func; /* PIPE_FUNC_* is GL's order */
}

/* the ISPC word of one face (STATE.md "ISPC"; the ops' codes differ from
 * gallium's) */
static uint32_t
stencil_word(const struct pipe_stencil_state *s)
{
   static const uint8_t op[] = {
      [PIPE_STENCIL_OP_KEEP] = SGX_TA_STENCILOP_KEEP,
      [PIPE_STENCIL_OP_ZERO] = SGX_TA_STENCILOP_ZERO,
      [PIPE_STENCIL_OP_REPLACE] = SGX_TA_STENCILOP_REPLACE,
      [PIPE_STENCIL_OP_INCR] = SGX_TA_STENCILOP_INCR,
      [PIPE_STENCIL_OP_DECR] = SGX_TA_STENCILOP_DECR,
      [PIPE_STENCIL_OP_INVERT] = SGX_TA_STENCILOP_INVERT,
      [PIPE_STENCIL_OP_INCR_WRAP] = SGX_TA_STENCILOP_INCR_WRAP,
      [PIPE_STENCIL_OP_DECR_WRAP] = SGX_TA_STENCILOP_DECR_WRAP,
   };
   uint32_t w;

   sgx_pack(&w, TA_STATE_ISPC, ispc)
   {
      ispc.write_mask = s->writemask;
      ispc.compare_mask = s->valuemask;
      ispc.zpass = op[s->zpass_op];
      ispc.zfail = op[s->zfail_op];
      ispc.sfail = op[s->fail_op];
      ispc.func = s->func; /* PIPE_FUNC_* is GL's order */
   }
   return w;
}

static void
build_stencil(const struct sgx_context *ctx, struct sgxws_state *st)
{
   const struct pipe_depth_stencil_alpha_state *dsa =
      ctx->dsa ? &ctx->dsa->base : NULL;

   if (!dsa || !dsa->stencil[0].enabled || !ctx->framebuffer.zsbuf.texture ||
       !util_format_has_stencil(
          util_format_description(ctx->framebuffer.zsbuf.format)))
      return;
   st->stencil = true;
   /* the hardware's first group is the back of gallium's front_ccw sense: with
    * it set, gallium's face 1 (A/B against the DDK, windows and pbuffers) */
   unsigned first = dsa->stencil[1].enabled && ctx->rasterizer &&
                          ctx->rasterizer->base.front_ccw
                       ? 1
                       : 0;

   st->ispc[0] = stencil_word(&dsa->stencil[first]);
   st->stencil_ref[0] = ctx->stencil_ref.ref_value[first];
   if (dsa->stencil[1].enabled) {
      st->two_sided = true;
      st->ispc[1] = stencil_word(&dsa->stencil[!first]);
      st->stencil_ref[1] = ctx->stencil_ref.ref_value[!first];
   }
}

static void
build_state(struct sgx_context *ctx, struct sgxws_state *st)
{
   const struct pipe_rasterizer_state *rs =
      ctx->rasterizer ? &ctx->rasterizer->base : NULL;
   unsigned mask = word_mask(
      ctx, (ctx->blend ? ctx->blend->base.rt[0].colormask : PIPE_MASK_RGBA) &
              target_mask(ctx));

   memset(st, 0, sizeof(*st));
   st->depth_func = depth_func(ctx);
   st->depth_write = st->depth_func && ctx->dsa->base.depth_writemask;
   if (rs && rs->cull_face == PIPE_FACE_BACK)
      st->cull = SGXWS_BACK;
   else if (rs && rs->cull_face == PIPE_FACE_FRONT)
      st->cull = SGXWS_FRONT;
   st->front_ccw = rs ? rs->front_ccw : true;
   /* ISPB colour mask: R8 G4 B2 A1 */
   st->color_mask = (mask & PIPE_MASK_R ? 8 : 0) |
                    (mask & PIPE_MASK_G ? 4 : 0) |
                    (mask & PIPE_MASK_B ? 2 : 0) | (mask & PIPE_MASK_A ? 1 : 0);
   memcpy(st->vp_scale, ctx->viewport.scale, sizeof(st->vp_scale));
   memcpy(st->vp_translate, ctx->viewport.translate, sizeof(st->vp_translate));
   st->line_width = rs ? rs->line_width : 1.0f;
   build_stencil(ctx, st);
   /* a later pass of a planar colour buffer: the fragments the first pass
    * kept, near enough (depth EQUAL; stencil off, a fragment that failed it
    * there passes here when its depth is equal: docs/GL3.md), writing
    * neither */
   if (ctx->rt_later_pass) {
      if (st->depth_func)
         st->depth_func = SGXWS_NEVER + PIPE_FUNC_EQUAL;
      st->depth_write = false;
      st->stencil = false;
   }
}

/* The pixels a draw reaches, in the target's memory order: the viewport's
 * rectangle (the hardware clips to the guard band, not to the viewport) and
 * the scissor.  False when that is nothing. */
static bool
build_clip(const struct sgx_context *ctx, struct sgxws_rect *clip)
{
   const struct pipe_rasterizer_state *rs =
      ctx->rasterizer ? &ctx->rasterizer->base : NULL;
   const struct pipe_viewport_state *vp = &ctx->viewport;
   unsigned w = ctx->framebuffer.width, h = ctx->framebuffer.height;
   float x0 = vp->translate[0] - fabsf(vp->scale[0]),
         x1 = vp->translate[0] + fabsf(vp->scale[0]);
   float y0 = vp->translate[1] - fabsf(vp->scale[1]),
         y1 = vp->translate[1] + fabsf(vp->scale[1]);

   clip->x0 = x0 > 0 ? MIN2((unsigned)lrintf(x0), w) : 0;
   clip->y0 = y0 > 0 ? MIN2((unsigned)lrintf(y0), h) : 0;
   clip->x1 = x1 > 0 ? MIN2((unsigned)lrintf(x1), w) : 0;
   clip->y1 = y1 > 0 ? MIN2((unsigned)lrintf(y1), h) : 0;
   if (rs && rs->scissor) {
      clip->x0 = MAX2(clip->x0, ctx->scissor.minx);
      clip->y0 = MAX2(clip->y0, ctx->scissor.miny);
      clip->x1 = MIN2(clip->x1, ctx->scissor.maxx);
      clip->y1 = MIN2(clip->y1, ctx->scissor.maxy);
   }
   return clip->x0 < clip->x1 && clip->y0 < clip->y1;
}

static const float *
constant_data(struct sgx_context *ctx, mesa_shader_stage stage)
{
   struct pipe_constant_buffer *cb = &ctx->constants[stage];

   if (cb->user_buffer)
      return (const float *)((const uint8_t *)cb->user_buffer +
                             cb->buffer_offset);
   if (cb->buffer)
      return (const float *)((const uint8_t *)sgx_resource(cb->buffer)->data +
                             cb->buffer_offset);
   return NULL;
}

/* primitives the hardware doesn't have, drawn as lists of triangles or lines
 * (desktop GL's quads, quad strips and polygons too) */
static bool
expanded_prim(enum mesa_prim mode)
{
   return mode == MESA_PRIM_TRIANGLE_FAN || mode == MESA_PRIM_LINE_STRIP ||
          mode == MESA_PRIM_LINE_LOOP || mode == MESA_PRIM_QUADS ||
          mode == MESA_PRIM_QUAD_STRIP || mode == MESA_PRIM_POLYGON;
}

/* how many vertices the list has (the count already trimmed to whole
 * primitives) */
static unsigned
expanded_count(enum mesa_prim mode, unsigned count)
{
   switch (mode) {
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      return (count - 2) * 3;
   case MESA_PRIM_TRIANGLE_STRIP:
      return (count - 2) * 3;
   case MESA_PRIM_LINE_STRIP:
      return 2 * (count - 1);
   case MESA_PRIM_LINE_LOOP:
      return 2 * count;
   case MESA_PRIM_QUADS:
      return count / 4 * 6;
   case MESA_PRIM_QUAD_STRIP:
      return (count / 2 - 1) * 6;
   default:
      return count;
   }
}

/* the list's i-th vertex, as an offset into the draw */
static unsigned
expanded_vertex(enum mesa_prim mode, unsigned i, unsigned out_count)
{
   switch (mode) {
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_POLYGON:
      return i % 3 == 0 ? 0 : i / 3 + i % 3;
   /* a quad's two triangles keep its winding and both end on its last
    * vertex, GL's provoking vertex for quads: 0 1 3, 1 2 3 */
   case MESA_PRIM_QUADS: {
      static const uint8_t quad[6] = {0, 1, 3, 1, 2, 3};
      return i / 6 * 4 + quad[i % 6];
   }
   /* a strip's quad is 2q, 2q+1, 2q+3, 2q+2 around, its last vertex 2q+3:
    * 0 1 3, 2 0 3 */
   case MESA_PRIM_QUAD_STRIP: {
      static const uint8_t quad[6] = {0, 1, 3, 2, 0, 3};
      return i / 6 * 2 + quad[i % 6];
   }
   /* a strip's triangle t is t, t+1, t+2, its odd ones t+1, t, t+2 (GL's
    * order, so every triangle keeps the strip's winding) */
   case MESA_PRIM_TRIANGLE_STRIP: {
      static const uint8_t odd[3] = {1, 0, 2};
      return i / 3 + (i / 3 & 1 ? odd[i % 3] : i % 3);
   }
   case MESA_PRIM_LINE_STRIP:
      return (i + 1) / 2;
   case MESA_PRIM_LINE_LOOP:
      return i == out_count - 1 ? 0 : (i + 1) / 2;
   default:
      return i;
   }
}

/* where one vertex element comes from */
struct fetch {
   const uint8_t *base;
   uint32_t stride;
   enum pipe_format format;
   unsigned dwords; /* dwords it fills in the vertex */
   bool defaults;   /* missing components 0, 0, 0, 1 (compiled shaders) */
   /* the bytes as they are, for a vertex shader that unpacks them
    * (zero-copy: sgx_screen.zero_copy, sgx_pipe_shader.vtx_fmt): raw_bytes of
    * them */
   bool raw;
   unsigned raw_bytes;
   struct sgx_resource *res; /* the buffer, NULL for client memory */
   uint32_t offset;          /* where in it */
   unsigned vb;              /* its vertex buffer binding */
};

static bool
setup_fetch(struct sgx_context *ctx, unsigned e, struct fetch *f)
{
   const struct pipe_vertex_element *ve;
   const struct pipe_vertex_buffer *vb;

   if (!ctx->velems || e >= ctx->velems->num_elements)
      return false;
   ve = &ctx->velems->elements[e];
   if (ve->vertex_buffer_index >= ctx->num_vertex_buffers)
      return false;
   vb = &ctx->vertex_buffers[ve->vertex_buffer_index];
   f->vb = ve->vertex_buffer_index;
   f->res = NULL;
   f->raw = false;
   if (vb->is_user_buffer) {
      f->base = vb->buffer.user;
   } else if (vb->buffer.resource) {
      f->res = sgx_resource(vb->buffer.resource);
      f->base = f->res->data;
   } else {
      return false;
   }
   f->offset = vb->buffer_offset + ve->src_offset;
   /* an instanced element is one value for the whole draw, as each instance
    * is a draw of its own (sgx_draw_vbo): GL's element
    * base instance + instance / divisor */
   if (ve->instance_divisor)
      f->offset += (ctx->base_instance + ctx->instance / ve->instance_divisor) *
                   ve->src_stride;
   f->stride = ve->instance_divisor ? 0 : ve->src_stride;
   f->base += f->offset;
   f->format = ve->src_format;
   return true;
}

static void
fetch_vertex(const struct fetch *f, unsigned index, uint32_t *out)
{
   const uint8_t *src = f->base + (size_t)index * f->stride;
   float v[4] = {0, 0, 0, 1};

   if (f->raw) {
      memcpy(out, src, f->raw_bytes);
      return;
   }
   if (!f->defaults && (f->format == PIPE_FORMAT_R32G32_FLOAT ||
                        f->format == PIPE_FORMAT_R32G32B32_FLOAT ||
                        f->format == PIPE_FORMAT_R32G32B32A32_FLOAT)) {
      memcpy(out, src, f->dwords * 4);
      return;
   }
   if (f->format == PIPE_FORMAT_R32G32B32A32_FLOAT)
      memcpy(v, src, 16);
   else if (f->format == PIPE_FORMAT_R32G32B32_FLOAT)
      memcpy(v, src, 12);
   else if (f->format == PIPE_FORMAT_R32G32_FLOAT)
      memcpy(v, src, 8);
   else if (f->format == PIPE_FORMAT_R32_FLOAT)
      memcpy(v, src, 4);
   else {
      float t[4];
      unsigned n = util_format_get_nr_components(f->format);

      /* a pure integer format unpacks to integers (glVertexAttribIPointer),
       * and its missing w is the integer 1 */
      if (util_format_is_pure_integer(f->format))
         v[3] = uif(1);
      util_format_unpack_rgba(f->format, t, src, 1);
      memcpy(v, t, n * sizeof(float));
   }
   memcpy(out, v, f->dwords * 4);
}

static uint32_t
get_index(const struct pipe_draw_info *info, const void *indices, unsigned i)
{
   switch (info->index_size) {
   case 1:
      return ((const uint8_t *)indices)[i];
   case 2:
      return ((const uint16_t *)indices)[i];
   default:
      return ((const uint32_t *)indices)[i];
   }
}

/* element f of vertices first .. first + n - 1 into dst, vertex_dwords
 * apart: the common formats (f32 x 1-4, u8 x 4 unorm, s16 x 2 scaled) go
 * without a call per vertex */
static void
fetch_range(const struct fetch *f, unsigned first, unsigned n, uint32_t *dst,
            unsigned vertex_dwords)
{
   static float unorm8[256];
   const uint8_t *src = f->base + (size_t)first * f->stride;
   float *o = (float *)dst;

   if (!f->defaults) {
      for (unsigned i = 0; i < n; i++)
         fetch_vertex(f, first + i, dst + i * vertex_dwords);
      return;
   }
   if (f->raw) {
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords)
         memcpy(o, src, f->raw_bytes);
      return;
   }
   switch (f->format) {
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords)
         memcpy(o, src, 16);
      return;
   case PIPE_FORMAT_R32G32B32_FLOAT:
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords) {
         memcpy(o, src, 12);
         o[3] = 1.0f;
      }
      return;
   case PIPE_FORMAT_R32G32_FLOAT:
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords) {
         memcpy(o, src, 8);
         o[2] = 0.0f;
         o[3] = 1.0f;
      }
      return;
   case PIPE_FORMAT_R32_FLOAT:
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords) {
         memcpy(o, src, 4);
         o[1] = o[2] = 0.0f;
         o[3] = 1.0f;
      }
      return;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      if (unorm8[255] != 1.0f)
         for (unsigned i = 0; i < 256; i++)
            unorm8[i] = i / 255.0f;
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords) {
         o[0] = unorm8[src[0]];
         o[1] = unorm8[src[1]];
         o[2] = unorm8[src[2]];
         o[3] = unorm8[src[3]];
      }
      return;
   case PIPE_FORMAT_R16G16_SSCALED:
      for (unsigned i = 0; i < n; i++, src += f->stride, o += vertex_dwords) {
         int16_t v[2];

         memcpy(v, src, 4);
         o[0] = v[0];
         o[1] = v[1];
         o[2] = 0.0f;
         o[3] = 1.0f;
      }
      return;
   default:
      for (unsigned i = 0; i < n; i++)
         fetch_vertex(f, first + i, dst + i * vertex_dwords);
      return;
   }
}

/* every element of vertices first .. first + n - 1, packed */
static void
fetch_vertices(const struct fetch *f, unsigned n_fetch, unsigned first,
               unsigned n, uint32_t *v, unsigned vertex_dwords)
{
   unsigned off = 0;

   for (unsigned e = 0; e < n_fetch; e++) {
      fetch_range(&f[e], first, n, v + off, vertex_dwords);
      off += f[e].dwords;
   }
}

static uint16_t *
index_staging(struct sgx_context *ctx, unsigned n)
{
   if (n > ctx->idx_staging_size) {
      free(ctx->idx_staging);
      ctx->idx_staging_size = MAX2(n, 16384);
      ctx->idx_staging = malloc(ctx->idx_staging_size * 2);
      if (!ctx->idx_staging)
         ctx->idx_staging_size = 0;
   }
   return ctx->idx_staging;
}

static uint32_t *
staging(struct sgx_context *ctx, size_t dwords)
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

/* uniform block n of a stage for this draw (load_ubo's block n, which st
 * binds as constant buffer 1 + n): where its copy's address goes (sa: an
 * index into the program's constants, -1 none; mem: a word of its memory
 * block, 0 none). An unbound block reads as zeros */
static void
record_ubo(struct sgx_context *ctx, mesa_shader_stage stage, unsigned n,
           int sa, unsigned mem)
{
   static const uint32_t zeros[4];
   const struct pipe_constant_buffer *cb = &ctx->ubos[stage][n];
   struct sgxws_ubo *u;

   if (ctx->n_draw_ubos == SGXWS_MAX_UBOS)
      return;
   u = &ctx->draw_ubos[ctx->n_draw_ubos++];
   memset(u, 0, sizeof(*u));
   u->fs = stage == MESA_SHADER_FRAGMENT;
   u->sa = sa;
   u->mem = mem;
   u->data = zeros;
   u->bytes = sizeof(zeros);
   if (cb->user_buffer) {
      u->data = (const uint8_t *)cb->user_buffer + cb->buffer_offset;
      u->bytes = cb->buffer_size;
   } else if (cb->buffer) {
      struct sgx_resource *res = sgx_resource(cb->buffer);
      unsigned size = cb->buffer->width0;

      if (!res->data || cb->buffer_offset >= size)
         return;
      u->data = (const uint8_t *)res->data + cb->buffer_offset;
      u->bytes = MIN2(cb->buffer_size, size - cb->buffer_offset);
      /* copied once a frame while nothing writes it (gen counts the write
       * maps; one still open may write more) */
      if (!res->write_maps)
         u->key = (uint64_t)res->uid << 32 | res->gen;
   }
   u->bytes = MIN2(u->bytes, (unsigned)SGX_UBO_BYTES);
}

/* a compiled shader's constants: sa[base + slot] = constant buffer 0 float k */
static void
fill_consts(struct sgx_context *ctx, mesa_shader_stage stage,
            const struct sgx_pipe_shader *so, uint32_t *block, unsigned base,
            uint32_t *mem)
{
   const float *cb = constant_data(ctx, stage);
   unsigned avail = ctx->constants[stage].buffer_size / 4;

   /* F16 halves and C10 channels share a slot (or a word of the memory
    * block, filled the same way; mem is mem_dwords long) */
   for (unsigned i = 0; i < so->n_consts; i++) {
      const struct sgx_const *k = &so->consts[i];

      if (k->fmt == 1)
         continue;
      if (k->slot != 0xffff)
         block[base + k->slot] = 0;
      if (k->mem && mem)
         mem[k->mem] = 0;
   }
   for (unsigned i = 0; i < so->n_consts; i++) {
      const struct sgx_const *k = &so->consts[i];
      float v = cb && k->k < avail ? cb[k->k] : 0.0f;

      /* a texture's size (sgx_program.c lower_txs) */
      if (stage == MESA_SHADER_FRAGMENT && k->k >= SGX_K_TEXSIZE &&
          k->k < SGX_K_TEXSIZE + 2 * SGX_K_TEXSIZE_UNITS) {
         const struct pipe_sampler_view *view =
            ctx->views[(k->k - SGX_K_TEXSIZE) / 2];

         v = !view ? 1.0f
             : (k->k - SGX_K_TEXSIZE) & 1
                ? (float)u_minify(view->texture->height0,
                                  view->u.tex.first_level)
                : (float)u_minify(view->texture->width0,
                                  view->u.tex.first_level);
      }
      /* gl_InstanceID (sgx_program.c lower_instance_id) */
      if (stage == MESA_SHADER_VERTEX && k->k == SGX_K_INSTANCE)
         v = (float)ctx->instance;
      /* a 32-bit word as it is: a uniform's bits (an integer's too), or one
       * of the native-integer block's integers; the driver's other
       * constants are the float v */
      bool raw = cb && k->k < avail;
      uint32_t word = raw ? ((const uint32_t *)cb)[k->k] : 0;

      /* uniform block n's address: the winsys's copy of it, written in
       * where the constant lives (sgxws_ubo); 0 until then */
      if (k->k >= SGX_K_UBO && k->k < SGX_K_UBO + SGX_MAX_UBOS) {
         record_ubo(ctx, stage, k->k - SGX_K_UBO,
                    k->slot != 0xffff ? (int)(base + k->slot) : -1,
                    mem ? k->mem : 0);
         word = 0;
         raw = true;
      }

      if (k->k >= SGX_K_LEVELS && k->k < SGX_K_LEVELS + SGX_K_SIZE_UNITS) {
         /* a view's level count (sgx_program.c lower_txs): a texture of F32
          * words is sampled at its base level alone (sgx_tex_words) */
         const struct pipe_sampler_view *view =
            stage == MESA_SHADER_FRAGMENT ? ctx->views[k->k - SGX_K_LEVELS]
                                          : NULL;

         word = !view || sgx_tex_word32(view->format)
                   ? 1
                   : view->u.tex.last_level - view->u.tex.first_level + 1;
         raw = true;
      }
      if (k->k >= SGX_K_SIZE && k->k < SGX_K_SIZE + 3 * SGX_K_SIZE_UNITS) {
         /* the base level's width, height, and depth or layers
          * (sgx_program.c lower_txs_int) */
         unsigned u = (k->k - SGX_K_SIZE) / 3, c = (k->k - SGX_K_SIZE) % 3;
         const struct pipe_sampler_view *view =
            stage == MESA_SHADER_FRAGMENT ? ctx->views[u] : NULL;
         const struct pipe_resource *t = view ? view->texture : NULL;

         word = !t ? 1
                : c == 0 ? u_minify(t->width0, view->u.tex.first_level)
                : c == 1 ? (t->target == PIPE_TEXTURE_1D_ARRAY
                               ? view->u.tex.last_layer -
                                    view->u.tex.first_layer + 1
                               : u_minify(t->height0, view->u.tex.first_level))
                : t->target == PIPE_TEXTURE_3D
                   ? u_minify(t->depth0, view->u.tex.first_level)
                   : view->u.tex.last_layer - view->u.tex.first_layer + 1;
         raw = true;
      } else if (stage == MESA_SHADER_VERTEX && k->k == SGX_K_INSTANCE_INT) {
         word = ctx->instance;
         raw = true;
      }
      uint32_t bits,
         *at[2] = {k->slot != 0xffff ? &block[base + k->slot] : NULL,
                   k->mem && mem ? &mem[k->mem] : NULL};

      if (k->fmt == 2) {
         bits = (uint32_t)sgxws_f16(v) << k->shift;
      } else if (k->fmt == 3 || k->fmt == 0x10) {
         /* C10: round(v * 255), signed 10 bits, the product in double as the
          * DDK has it (0.3f * 255 is 76.500003: 77, where a float product is
          * 76.5 and rounds to even, 76); 0x10 is that value as F16, c / 255
          * rounded to nearest as pck.f16.c10 gives it (c / 255 isn't dyadic,
          * so the float in between can't round the F16 differently) */
         int c =
            CLAMP((int)lrint(CLAMP((double)v, -2.0, 2.0) * 255.0), -512, 511);

         if (k->fmt == 3)
            bits = ((uint32_t)c & 0x3ff) << k->shift;
         else
            bits = (uint32_t)sgxws_f16((float)(c / 255.0)) << k->shift;
      } else if (k->fmt == 0x11) {
         bits = k->value << k->shift;
      } else if (k->fmt == 0x12) {
         /* the vec4 at k as the BGRA8 colour output, packed once a draw
          * instead of every pixel (the blend constant's packing, sa2) */
         float c[4];

         for (unsigned j = 0; j < 4; j++)
            c[j] = cb && k->k + j < avail ? cb[k->k + j] : 0.0f;
         bits = (uint32_t)float_to_ubyte(c[3]) << 24 |
                (uint32_t)float_to_ubyte(c[0]) << 16 |
                (uint32_t)float_to_ubyte(c[1]) << 8 | float_to_ubyte(c[2]);
         for (unsigned j = 0; j < 2; j++)
            if (at[j])
               *at[j] = bits;
         continue;
      } else {
         /* F32: the driver's float constants, or the word as it is (no
          * float in between, which could quiet a NaN's payload) */
         if (raw)
            bits = word;
         else
            memcpy(&bits, &v, 4);
         for (unsigned j = 0; j < 2; j++)
            if (at[j])
               *at[j] = bits;
         continue;
      }
      for (unsigned j = 0; j < 2; j++)
         if (at[j])
            *at[j] |= bits;
      SGX_DBG(DRAW, "  %s constant %u -> sa%d mem%u (fmt %u at %u) = %f\n",
              stage == MESA_SHADER_VERTEX ? "vs" : "fs", k->k,
              k->slot != 0xffff ? (int)(base + k->slot) : -1, k->mem, k->fmt,
              k->shift, v);
   }
}

/* a program's constants in memory for this draw (sgxws_const_mem), filled into
 * a buffer of the context's; NULL when it has none */
static void
mem_consts(struct sgx_context *ctx, mesa_shader_stage stage,
           const struct sgx_pipe_shader *so, uint32_t *block, unsigned base,
           struct sgxws_const_mem *m)
{
   memset(m, 0, sizeof(*m));
   if (!so->mem_dwords)
      return;
   uint32_t **buf = &ctx->mem_consts[stage == MESA_SHADER_VERTEX ? 0 : 1];

   if (!*buf && !(*buf = malloc(SGX_MAX_CONST_MAP * 4 + 4)))
      return;
   memset(*buf, 0, so->mem_dwords * 4);
   fill_consts(ctx, stage, so, block, base, *buf);
   m->words = *buf;
   m->dwords = so->mem_dwords;
   m->n_bases = so->n_mem_bases;
   for (unsigned j = 0; j < so->n_mem_bases; j++)
      m->base[j] = base + so->mem_base_slot[j];
}

/* Every set the fragment program iterates or samples with must be one the
 * vertex program outputs, with as many components: iterating a set the vertex
 * program doesn't write locks the GPU up. */
static bool
varyings_match(const struct sgxws_prog *vs, const struct sgxws_prog *fs)
{
   for (unsigned i = 0; i < fs->n_douti; i++) {
      const struct sgx_pds_douti *d = &fs->douti[i];
      unsigned need[2][2] = {{0, 0}}, n = 0;
      /* (with the vertex colour in set 0 a set's source is the set itself) */
      unsigned base = vs->colour ? 0 : 1;

      /* plain iteration: source set + 1; source 0 (gl_FragCoord) and sources
       * from 0x20 on are not varyings (0x22: the depth plane of a discard) */
      if (d->iterate && d->src > 0 && d->src < 0x20)
         need[n][0] = d->src - base, need[n++][1] = d->comps;
      /* the vertex colour (set 0, output select b11) */
      if (d->iterate && d->src == 0x20) {
         if (!vs->colour)
            return false;
         need[n][0] = 0, need[n++][1] = 4;
      }
      if (d->dim2 ||
          d->proj) /* sample half: coordinate set + 1, 2-D or projected */
         need[n][0] = d->sample_set - base, need[n++][1] = d->proj ? 3 : 2;
      for (unsigned k = 0; k < n; k++) {
         unsigned size = (vs->tsp_sizes >> (3 * need[k][0])) & 7;

         if (need[k][0] >= 10 || !size || (size == 5 ? 4 : size) < need[k][1])
            return false;
      }
   }
   return true;
}

/* ------------------------------------------------------------- vertex cache */

/*
 * Display lists and static vertex buffers are drawn from buffers that don't
 * change between frames, and every frame the draw gathered, converted and
 * copied the same vertices again.  A
 * draw whose vertices all come from buffers (or are constant) keeps its
 * gathered vertices and rebased 16-bit indices in device memory, found again
 * by what they were made from: the buffers (by id and write count), where in
 * them, the formats, the index range.  An entry the GPU may still read is
 * not freed.
 */
#define VCACHE_BUDGET (40u << 20)

struct vcache_key {
   uint32_t n_fetch, mode, index_size, ib_uid, ib_gen, start, count;
   int32_t index_bias;
   struct {
      uint32_t uid, gen, offset, stride, format;
      uint32_t konst[4]; /* a constant element's value (stride 0) */
   } el[PIPE_MAX_ATTRIBS];
};

/* Entries are cut from 2 MB chunks of device memory, not allocated one by
 * one: with hundreds of entries alive, every allocation crawled through the
 * services' segment list (over a third of the render thread in
 * _SegmentListInsert).  A chunk whose entries are all gone is used again. */
#define VCACHE_CHUNK (2u << 20)

struct vcache_chunk {
   struct list_head link;
   struct sgxws_bo bo;
   uint32_t used, live;
};

struct vcache_entry {
   struct vcache_key key; /* first: the hash table's key */
   struct list_head link;
   struct vcache_chunk *chunk;
   uint32_t offset;
   uint32_t verts_va, idx_va, nverts, nidx, last_seq, bytes;
};

static struct vcache_chunk *
vcache_alloc(struct sgx_context *ctx, uint32_t bytes, uint32_t *offset)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct vcache_chunk *k = ctx->vcache_cur;

   if (bytes > VCACHE_CHUNK)
      return NULL;
   if (!k || k->used + bytes > VCACHE_CHUNK) {
      /* an emptied chunk, else a new one */
      k = NULL;
      list_for_each_entry (struct vcache_chunk, c, &ctx->vcache_chunks, link) {
         if (!c->live) {
            k = c;
            break;
         }
      }
      if (!k) {
         if (list_length(&ctx->vcache_chunks) >= 48 ||
             !(k = calloc(1, sizeof(*k))))
            return NULL;
         if (sgxws_bo_alloc(screen->dev, &k->bo, SGXWS_MEM_VERTEX, VCACHE_CHUNK,
                            0x1000)) {
            free(k);
            return NULL;
         }
         list_add(&k->link, &ctx->vcache_chunks);
      }
      k->used = 0;
      ctx->vcache_cur = k;
   }
   *offset = k->used;
   k->used += bytes;
   k->live++;
   return k;
}

static uint32_t
vcache_hash(const void *k)
{
   return _mesa_hash_data(k, sizeof(struct vcache_key));
}

static bool
vcache_equal(const void *a, const void *b)
{
   return !memcmp(a, b, sizeof(struct vcache_key));
}

static void
vcache_free(struct sgx_context *ctx, struct vcache_entry *v)
{
   _mesa_hash_table_remove_key(ctx->vcache, &v->key);
   list_del(&v->link);
   ctx->vcache_bytes -= v->bytes;
   v->chunk->live--;
   free(v);
}

/* room for bytes more: the least recently used entries the GPU is done with go */
static bool
vcache_room(struct sgx_context *ctx, uint32_t bytes)
{
   uint32_t done = sgxws_done_seq(ctx->hw);

   while (ctx->vcache_bytes + bytes > VCACHE_BUDGET &&
          !list_is_empty(&ctx->vcache_lru)) {
      struct vcache_entry *v =
         list_last_entry(&ctx->vcache_lru, struct vcache_entry, link);

      if (sgxws_seq_after(v->last_seq, done))
         return false;
      vcache_free(ctx, v);
   }
   return ctx->vcache_bytes + bytes <= VCACHE_BUDGET;
}

/* Draw draws[n] from the cache, filling it on a miss; false: not cacheable
 * (the caller draws it the usual way).  Called with the lock held, the frame
 * begun and d set up but for its vertices. */
static bool
vcache_draw(struct sgx_context *ctx, const struct pipe_draw_info *info,
            const struct pipe_draw_start_count_bias *draw,
            const struct fetch *f, unsigned n_fetch, unsigned vertex_dwords,
            struct sgxws_draw *d)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   const struct sgx_resource *ib = NULL;
   struct vcache_key key;
   struct hash_entry *he;
   struct vcache_entry *v;
   /* triangle lists, and what is drawn as one: fans, and desktop GL's quads,
    * quad strips and polygons (display lists of quads, drawn every frame) */
   bool expand = expanded_prim(info->mode) &&
                 u_reduced_prim(info->mode) == MESA_PRIM_TRIANGLES;
   unsigned count = draw->count, out_count;
   uint64_t now = os_time_get_nano();

   if (!u_trim_pipe_prim(info->mode, &count))
      return false;
   out_count = expand ? expanded_count(info->mode, count) : count;
   if (!ctx->vcache || out_count < 3 || n_fetch > PIPE_MAX_ATTRIBS ||
       (info->mode != MESA_PRIM_TRIANGLES && !expand))
      return false;
   if (info->index_size) {
      if (info->has_user_indices)
         return false;
      ib = sgx_resource(info->index.resource);
   }
   if (out_count > SGXWS_MAX_INDICES ||
       (ib && now - ib->written_ns < 1000000000ull))
      return false;
   memset(&key, 0, sizeof(key));
   key.n_fetch = n_fetch;
   key.mode = info->mode;
   key.index_size = info->index_size;
   key.ib_uid = ib ? ib->uid : 0;
   key.ib_gen = ib ? ib->gen : 0;
   key.start = draw->start;
   key.count = count;
   key.index_bias = info->index_size ? draw->index_bias : 0;
   for (unsigned e = 0; e < n_fetch; e++) {
      if (!f[e].defaults)
         return false;
      key.el[e].format = f[e].format | (f[e].raw ? 0x80000000u : 0);
      key.el[e].stride = f[e].stride;
      if (!f[e].stride) {
         fetch_vertex(&f[e], 0, key.el[e].konst);
      } else {
         /* a buffer rewritten in the last second (the GUI's, the hand's
          * streams) would only churn the cache */
         if (!f[e].res || now - f[e].res->written_ns < 1000000000ull)
            return false;
         key.el[e].uid = f[e].res->uid;
         key.el[e].gen = f[e].res->gen;
         key.el[e].offset = f[e].offset;
      }
   }

   he = _mesa_hash_table_search(ctx->vcache, &key);
   if (he) {
      v = he->data;
      ctx->stats.vc_hits++;
   } else {
      /* gather once: the vertices the draw uses, the indices rebased */
      const void *indices = ib ? ib->data : NULL;
      unsigned lo = ~0u, hi = 0, vbytes, ibytes;
      uint32_t *verts;
      uint16_t *ix;

      for (unsigned i = 0; i < out_count; i++) {
         unsigned k = expand ? expanded_vertex(info->mode, i, out_count) : i;
         unsigned index =
            info->index_size
               ? get_index(info, indices, draw->start + k) + draw->index_bias
               : draw->start + k;

         lo = MIN2(lo, index);
         hi = MAX2(hi, index);
      }
      if (hi - lo >= 65536 || hi - lo + 1 > 4 * out_count + 64) {
         ctx->stats.vc_uncacheable++;
         return false;
      }
      vbytes = align((hi - lo + 1) * vertex_dwords * 4, 64);
      ibytes = align(out_count * 2, 16);
      if (!vcache_room(ctx, vbytes + ibytes) || !(v = calloc(1, sizeof(*v)))) {
         ctx->stats.vc_uncacheable++;
         return false;
      }
      verts = staging(ctx, vbytes / 4 + ibytes / 4);
      if (!verts ||
          !(v->chunk = vcache_alloc(ctx, vbytes + ibytes, &v->offset))) {
         free(v);
         ctx->stats.vc_uncacheable++;
         return false;
      }
      /* gathered in cached memory, then copied whole (the chunk is
       * write-combined) */
      fetch_vertices(f, n_fetch, lo, hi - lo + 1, verts, vertex_dwords);
      ix = (uint16_t *)((uint8_t *)verts + vbytes);
      for (unsigned i = 0; i < out_count; i++) {
         unsigned k = expand ? expanded_vertex(info->mode, i, out_count) : i;
         unsigned index =
            info->index_size
               ? get_index(info, indices, draw->start + k) + draw->index_bias
               : draw->start + k;

         ix[i] = (uint16_t)(index - lo);
      }
      memcpy((uint8_t *)v->chunk->bo.map + v->offset, verts,
             vbytes + out_count * 2);
      v->key = key;
      v->verts_va = v->chunk->bo.va + v->offset;
      v->idx_va = v->verts_va + vbytes;
      v->nverts = hi - lo + 1;
      v->nidx = out_count;
      v->bytes = vbytes + ibytes;
      ctx->vcache_bytes += v->bytes;
      list_add(&v->link, &ctx->vcache_lru);
      _mesa_hash_table_insert(ctx->vcache, &v->key, v);
      ctx->stats.vc_misses++;
   }
   /* most recently used first; the frame being recorded reads it */
   list_del(&v->link);
   list_add(&v->link, &ctx->vcache_lru);
   v->last_seq = sgxws_next_seq(ctx->hw);
   d->verts = NULL;
   d->indices = NULL;
   d->verts_va = v->verts_va;
   d->idx_va = v->idx_va;
   d->count = v->nverts;
   d->n_indices = v->nidx;
   if (sgxws_draw(ctx->hw, d))
      fprintf(stderr, "sgx: draw of %u indices dropped\n", v->nidx);
   d->verts_va = d->idx_va = 0;
   return true;
}

/* an input format the compiler unpacks (sgx_compile_options.vtx_fmt), and
 * the bytes a vertex has of it */
static bool
vtx_format(enum pipe_format format, uint8_t *fmt, unsigned *bytes)
{
   switch (format) {
   case PIPE_FORMAT_R32_FLOAT:
      *fmt = SGX_VF_F32_1;
      *bytes = 4;
      return true;
   case PIPE_FORMAT_R32G32_FLOAT:
      *fmt = SGX_VF_F32_2;
      *bytes = 8;
      return true;
   case PIPE_FORMAT_R32G32B32_FLOAT:
      *fmt = SGX_VF_F32_3;
      *bytes = 12;
      return true;
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      *fmt = SGX_VF_F32_4;
      *bytes = 16;
      return true;
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      *fmt = SGX_VF_UNORM8_4;
      *bytes = 4;
      return true;
   case PIPE_FORMAT_R16G16_SSCALED:
      *fmt = SGX_VF_SSCALED16_2;
      *bytes = 4;
      return true;
   default:
      return false;
   }
}

/* raw inputs DMAed where they are (zero-copy, sgx_screen.zero_copy): a buffer's
 * device copy in place, a client array's bytes copied as they are, one block
 * per binding; the vertex shader unpacks them.  Draws up to the index limit;
 * the rest take the gather, raw too. */
static bool
resident_indexed(const struct pipe_draw_info *info,
                 const struct pipe_draw_start_count_bias *draw,
                 const struct fetch *f, unsigned n_fetch)
{
   if (info->index_size != 2 || info->has_user_indices ||
       info->mode != MESA_PRIM_TRIANGLES || draw->index_bias ||
       info->primitive_restart)
      return false;
   for (unsigned e = 0; e < n_fetch; e++)
      if (!f[e].res)
         return false;
   return true;
}

/* (zc_draw's: every fetch raw, from a buffer) */
static bool
zc_draw_resident(struct sgx_context *ctx, const struct pipe_draw_info *info,
                 const struct pipe_draw_start_count_bias *draw,
                 const struct fetch *f, unsigned n_fetch, struct sgxws_draw *d,
                 struct sgxws_fetch *zf)
{
   struct sgx_resource *ib = sgx_resource(info->index.resource);
   /* whole triangles in a piece */
   const unsigned piece = SGXWS_MAX_INDICES / 3 * 3;
   unsigned count = draw->count;

   if (!u_trim_pipe_prim(info->mode, &count) || !count ||
       !sgx_buffer_vbo(ctx, ib, draw->start * 2, count * 2))
      return false;
   for (unsigned e = 0; e < n_fetch; e++) {
      if (!sgx_buffer_vbo(ctx, f[e].res, f[e].offset, ~0u))
         return false;
      zf[e] = (struct sgxws_fetch){.stride = f[e].stride,
                                   .dwords = f[e].raw_bytes / 4,
                                   .reg = 4 * e,
                                   .va = f[e].res->vbo.va + f[e].offset};
      if (zf[e].va & 3)
         return false;
   }
   for (unsigned e = 0; e < n_fetch; e++)
      f[e].res->vbo_seq = sgxws_next_seq(ctx->hw);
   ib->vbo_seq = sgxws_next_seq(ctx->hw);
   d->fetch = zf;
   d->n_fetch = n_fetch;
   d->blocks = NULL;
   d->n_blocks = 0;
   d->indices = NULL;
   d->verts = NULL;
   d->verts_va = 0;
   d->count = 0x10000; /* (16-bit indices) */
   for (unsigned p0 = 0; p0 < count; p0 += piece) {
      d->idx_va = ib->vbo.va + (draw->start + p0) * 2;
      d->n_indices = MIN2(piece, count - p0);
      if (sgxws_draw(ctx->hw, d))
         fprintf(stderr, "sgx: draw of %u indices dropped\n", d->n_indices);
   }
   SGX_DBG(DRAW, "resident indexed draw x%u\n", count);
   ctx->stats.zc_draws++;
   d->fetch = NULL;
   d->n_fetch = 0;
   d->idx_va = 0;
   d->n_indices = 0;
   return true;
}

static bool
zc_draw(struct sgx_context *ctx, const struct pipe_draw_info *info,
        const struct pipe_draw_start_count_bias *draw, const void *indices,
        const struct fetch *f, unsigned n_fetch, struct sgxws_draw *d)
{
   struct sgxws_fetch zf[PIPE_MAX_ATTRIBS];
   struct sgxws_block blocks[8];
   const uint8_t *block_lo[8];
   unsigned block_vb[8], n_blocks = 0;
   bool list = expanded_prim(info->mode);
   unsigned count = draw->count, out_count, lo = ~0u, hi = 0;
   uint16_t *ix = NULL;

   if (!u_trim_pipe_prim(info->mode, &count))
      return false;
   if (info->mode == MESA_PRIM_TRIANGLE_STRIP && count > SGXWS_MAX_INDICES)
      return false;
   out_count = list ? expanded_count(info->mode, count) : count;
   /* a vertex program that reads no attribute takes the gather (sgxws_draw
    * tells zero-copy from it by its fetches) */
   if (!out_count || !n_fetch || n_fetch > PIPE_MAX_ATTRIBS)
      return false;
   for (unsigned e = 0; e < n_fetch; e++) {
      if (!f[e].raw || f[e].dwords != 4 || (f[e].stride & 3) ||
          f[e].stride > 0xffff)
         return false;
   }
   /* a list of 16-bit indices in a buffer, every vertex buffer in device
    * memory: the indices as they are, each fetch from vertex 0 (the DDK's
    * draw from a VBO and an IBO: nothing read or copied a draw), in pieces
    * of the index limit */
   if (resident_indexed(info, draw, f, n_fetch))
      return zc_draw_resident(ctx, info, draw, f, n_fetch, d, zf);
   if (out_count > SGXWS_MAX_INDICES)
      return false;
   /* the vertices the draw uses: in order, or through 16-bit indices from lo */
   if (info->index_size || list) {
      uint32_t *ix32 = staging(ctx, out_count);

      if (!ix32 || !(ix = index_staging(ctx, out_count)))
         return false;
      for (unsigned i = 0; i < out_count; i++) {
         unsigned k = list ? expanded_vertex(info->mode, i, out_count) : i;

         ix32[i] =
            info->index_size
               ? get_index(info, indices, draw->start + k) + draw->index_bias
               : draw->start + k;
         lo = MIN2(lo, ix32[i]);
         hi = MAX2(hi, ix32[i]);
      }
      if (hi - lo >= 65536)
         return false;
      for (unsigned i = 0; i < out_count; i++)
         ix[i] = (uint16_t)(ix32[i] - lo);
   } else {
      lo = draw->start;
      hi = draw->start + count - 1;
   }
   for (unsigned e = 0; e < n_fetch; e++) {
      unsigned n = f[e].raw_bytes / 4;

      zf[e] =
         (struct sgxws_fetch){.stride = f[e].stride, .dwords = n, .reg = 4 * e};
      if (f[e].res) {
         if (!sgx_buffer_vbo(ctx, f[e].res, f[e].offset + lo * f[e].stride,
                             (hi - lo) * f[e].stride + n * 4))
            return false;
         zf[e].va = f[e].res->vbo.va + f[e].offset + lo * f[e].stride;
         if (zf[e].va & 3)
            return false;
         continue;
      }
      /* a client array: its binding's bytes from vertex lo to the end of vertex
       * hi */
      unsigned b;
      const uint8_t *first = f[e].base + (size_t)lo * f[e].stride;
      const uint8_t *end = f[e].base + (size_t)hi * f[e].stride + n * 4;

      for (b = 0; b < n_blocks && block_vb[b] != f[e].vb; b++)
         ;
      if (b == n_blocks) {
         if (n_blocks == ARRAY_SIZE(blocks))
            return false;
         block_vb[n_blocks] = f[e].vb;
         block_lo[n_blocks] = first;
         blocks[n_blocks].cpu = first;
         blocks[n_blocks++].bytes = end - first;
      } else {
         const uint8_t *bend = (const uint8_t *)blocks[b].cpu + blocks[b].bytes;

         if (first < block_lo[b]) {
            blocks[b].cpu = block_lo[b] = first;
         }
         blocks[b].bytes = MAX2(end, bend) - block_lo[b];
      }
      zf[e].block = b + 1;
   }
   /* offsets into the blocks, now that each block's start is known (a
    * frame's ring part holds 16 MB: blocks past 4 MB take the gather) */
   uint32_t total = 0;

   for (unsigned b = 0; b < n_blocks; b++)
      total += blocks[b].bytes;
   if (total > 4u << 20)
      return false;
   for (unsigned e = 0; e < n_fetch; e++) {
      if (!zf[e].block)
         continue;
      zf[e].va =
         (f[e].base + (size_t)lo * f[e].stride) - block_lo[zf[e].block - 1];
      if (zf[e].va & 3)
         return false;
   }
   for (unsigned e = 0; e < n_fetch; e++)
      if (f[e].res)
         f[e].res->vbo_seq = sgxws_next_seq(ctx->hw);
   d->fetch = zf;
   d->n_fetch = n_fetch;
   d->blocks = blocks;
   d->n_blocks = n_blocks;
   d->indices = ix;
   d->n_indices = ix ? out_count : 0;
   d->count = ix ? hi - lo + 1 : count;
   d->verts = NULL;
   d->verts_va = 0;
   SGX_DBG(DRAW, "zero-copy draw %s x%u (%u vertices, %u blocks)\n",
           u_prim_name(info->mode), out_count, d->count, n_blocks);
   if (sgxws_draw(ctx->hw, d))
      fprintf(stderr, "sgx: zero-copy draw of %u vertices dropped\n",
              out_count);
   ctx->stats.zc_draws++;
   d->fetch = NULL;
   d->n_fetch = 0;
   d->blocks = NULL;
   d->n_blocks = 0;
   d->indices = NULL;
   return true;
}

void
sgx_draw_fini(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);

   if (!ctx->vcache)
      return;
   simple_mtx_lock(&screen->lock);
   list_for_each_entry_safe (struct vcache_entry, v, &ctx->vcache_lru, link)
      vcache_free(ctx, v);
   list_for_each_entry_safe (struct vcache_chunk, k, &ctx->vcache_chunks,
                             link) {
      sgxws_bo_free(screen->dev, &k->bo);
      free(k);
   }
   simple_mtx_unlock(&screen->lock);
   _mesa_hash_table_destroy(ctx->vcache, NULL);
   ctx->vcache = NULL;
}

static void
sgx_draw_vbo_impl(struct pipe_context *pctx, const struct pipe_draw_info *info,
                  unsigned drawid_offset,
                  const struct pipe_draw_indirect_info *indirect,
                  const struct pipe_draw_start_count_bias *draws,
                  unsigned num_draws)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct sgx_screen *screen = sgx_screen(pctx->screen);
   struct sgxws_state st;
   struct sgxws_draw d = {0};
   struct sgxws_prog fsp;
   struct sgxws_blend epi = {0};
   const struct sgxws_blend *blend = NULL;
   uint32_t fsc[SGXWS_FS_UNIFORM_DWORDS] = {0}, vsc[SGX_MAX_CONSTS] = {0};
   uint32_t fsk[9 + SGX_MAX_CONSTS] = {0}, tex[PIPE_MAX_SAMPLERS][4];
   const void *indices = NULL;
   struct fetch f[PIPE_MAX_ATTRIBS];
   unsigned n_fetch = 1, vertex_dwords = 0;
   float color[4];
   uint32_t prim;
   uint32_t sampled = 0; /* the sampler units this draw reads */
   bool compiled, raw = false;
   bool points = info->mode == MESA_PRIM_POINTS;
   uint8_t vfmt[16] = {0};
   unsigned raw_bytes[16];

   if (indirect || !ctx->vs || !ctx->fs)
      return;
   /* the fragment shader as these textures need it (sRGB decoded) */
   struct sgx_pipe_shader *fs = sgx_fs_for(ctx, ctx->fs);
   if (!fs)
      return;
   /* cube maps sampled without mipmapping, from their level-0 copy: brought
    * up to date before anything of this draw is recorded (it may wait).
    * Only the units the shader samples: a view left bound to another unit
    * is not this draw's */
   uint32_t used = sgx_fs_textures_used(ctx->fs);

   for (unsigned unit = 0; unit < PIPE_MAX_SAMPLERS; unit++) {
      struct pipe_sampler_view *view =
         used >> unit & 1 ? ctx->views[unit] : NULL;

      /* stopgap: no layout is known for NPOT cube faces, and sampling one of
       * 16 texels or more, or its mipmaps, locks the 3D (piglit cubemap
       * npot: a recovery a draw) */
      if (view && view->texture->target == PIPE_TEXTURE_CUBE &&
          !util_is_power_of_two_or_zero(view->texture->width0) &&
          (view->texture->width0 >= 16 ||
           (ctx->samplers[unit] &&
            ctx->samplers[unit]->min_mip_filter != PIPE_TEX_MIPFILTER_NONE &&
            view->u.tex.last_level > view->u.tex.first_level))) {
         if (!ctx->warned_cube)
            fprintf(
               stderr,
               "sgx: NPOT cube maps of 16 texels or more, or mipmapped, not drawn yet\n");
         ctx->warned_cube = true;
         return;
      }
      /* sampled past its last level: the device tail from that level */
      if (view && ctx->samplers[unit] &&
          ctx->samplers[unit]->min_mip_filter != PIPE_TEX_MIPFILTER_NONE)
         sgx_resource_tail(ctx, sgx_resource(view->texture),
                           view->u.tex.last_level);
      if (view && sgx_tex_uses_cube0(view, ctx->samplers[unit]) &&
          !sgx_resource_cube0(ctx, sgx_resource(view->texture)))
         return;
   }
   build_state(ctx, &st);
   if (ctx->rasterizer) {
      const struct pipe_rasterizer_state *rs = &ctx->rasterizer->base;
      enum mesa_prim r = u_reduced_prim(info->mode);

      st.depth_bias = (r == MESA_PRIM_TRIANGLES ? rs->offset_tri
                       : r == MESA_PRIM_LINES   ? rs->offset_line
                                                : rs->offset_point) &&
                      (rs->offset_units != 0.0f || rs->offset_scale != 0.0f);
      st.bias_units = st.depth_bias ? lrintf(rs->offset_units) : 0;
      st.bias_factor = st.depth_bias ? rs->offset_scale : 0.0f;
   }
   if (!build_clip(ctx, &st.clip))
      return; /* nothing visible: a zero-area scissor or viewport */
   /* both faces culled: no triangle survives (lines and points aren't culled) */
   if (ctx->rasterizer &&
       ctx->rasterizer->base.cull_face == PIPE_FACE_FRONT_AND_BACK &&
       u_reduced_prim(info->mode) == MESA_PRIM_TRIANGLES)
      return;
   if (sgx_cbuf(ctx)->texture) {
      /* the colour epilogue: blend, then the colour mask (sgxws_blend) */
      bool has_alpha = util_format_has_alpha(sgx_cbuf(ctx)->format);
      unsigned mask =
         (ctx->blend ? ctx->blend->base.rt[0].colormask : PIPE_MASK_RGBA) &
         target_mask(ctx);

      enum pipe_format cf = sgx_cbuf(ctx)->format;
      bool int_rt = util_format_is_pure_integer(cf);

      /* an integer target isn't blended (GL); its packed word is masked by
       * byte, an R32 word being all of R */
      if (ctx->blend && ctx->blend->enable && !int_rt)
         epi = ctx->blend->hw[has_alpha];
      if (int_rt && sgx_tex_raw32(cf))
         mask = word_mask(ctx, mask);
      else if (int_rt && !has_alpha)
         mask &= ~PIPE_MASK_A; /* the 1 it was created and cleared with */
      if (!has_alpha && !int_rt)
         mask |= PIPE_MASK_A;
      /* A window's X byte takes whatever alpha came out.  A texture's is read
       * as alpha where it's sampled, so it keeps the 1 it was created and
       * cleared with: the blend's alpha is the tile's (tile * 1 + colour * 0) */
      if (!has_alpha && !int_rt &&
          sgx_resource(sgx_cbuf(ctx)->texture)->texture) {
         if (!epi.blend) {
            epi.blend = 1;
            epi.c1 = SGXWS_BS_ZERO | SGXWS_BS_INV;
            epi.c2 = SGXWS_BS_ZERO;
            epi.cop = SGXWS_BOP_ADD;
         }
         epi.aop = SGXWS_BOP_ADD;
         epi.atrade = 0;
         if (epi.constant) {
            epi.fsa = SGXWS_BF_ZERO;
            epi.fda = SGXWS_BF_ONE;
         } else {
            /* with swap the tile is the first source */
            epi.a1 = epi.swap ? SGXWS_BS_ZERO | SGXWS_BS_INV : SGXWS_BS_ZERO;
            epi.a2 = epi.swap ? SGXWS_BS_ZERO : SGXWS_BS_ZERO | SGXWS_BS_INV;
         }
      }
      /* bytes of the packed BGRA8 colour */
      epi.wmask = (mask & PIPE_MASK_B ? 1 : 0) | (mask & PIPE_MASK_G ? 2 : 0) |
                  (mask & PIPE_MASK_R ? 4 : 0) | (mask & PIPE_MASK_A ? 8 : 0);
      if (epi.blend || epi.wmask != 0xf)
         blend = &epi;
   }
   /* each stage compiled or one of the fixed programs */
   compiled = ctx->vs->kind == SGX_SHADER_COMPILED;
   if (compiled) {
      struct sgx_pipe_shader *vs = ctx->vs;
      const struct sgx_pipe_shader *vfs =
         fs->kind == SGX_SHADER_COMPILED ? fs : NULL;

      /* zero-copy (sgx_screen.zero_copy): inputs the vertex shader unpacks
       * itself, when the compiler knows every format */
      raw = sgx_screen(ctx->base.screen)->zero_copy && ctx->velems &&
            ctx->vs->n_attribs <= 16 &&
            ctx->vs->n_attribs <= ctx->velems->num_elements;
      for (unsigned e = 0; raw && e < ctx->vs->n_attribs; e++)
         raw = vtx_format(ctx->velems->elements[e].src_format, &vfmt[e],
                          &raw_bytes[e]);
      /* the fragment shader's sets (gl_PointCoord: the vertex shader's
       * varyings move up to leave set 0 free) */
      if (vfs && fs->reads_pointcoord && !ctx->vs->writes_psize)
         return;
      if (vfs || raw || points) {
         if (raw && !(vs = sgx_vs_for(ctx, ctx->vs, vfs, vfmt, points)))
            raw = false;
         if (!raw && !(vs = sgx_vs_for(ctx, ctx->vs, vfs, NULL, points)))
            return;
      }
      if (vfs)
         st.sprite = fs->reads_pointcoord && info->mode == MESA_PRIM_POINTS;
      d.vs = &vs->prog;
      ctx->n_draw_ubos = 0;
      d.ubos = ctx->draw_ubos;
      if (vs->mem_dwords)
         mem_consts(ctx, MESA_SHADER_VERTEX, vs, vsc, 0, &d.vs_mem);
      else
         fill_consts(ctx, MESA_SHADER_VERTEX, vs, vsc, 0, NULL);
      d.n_ubos = ctx->n_draw_ubos;
      d.vs_consts = vsc;
      n_fetch = ctx->vs->n_attribs;
   } else if (ctx->vs->kind != SGX_SHADER_VS_POSITION) {
      return;
   }
   if (fs->kind == SGX_SHADER_COMPILED) {
      fsp = fs->prog;
      if (blend) {
         fsp.code = sgx_fs_program(ctx, fs, blend);
         if (!fsp.code)
            return;
      }
      /* pass type: a blend 1; discard 4, or 2 with a blend; a depthf
       * (gl_FragDepth) 5 (ISA.md, H10) */
      st.pass_type = fs->depth_pa >= 0       ? 5
                     : fs->discard_pa >= 0   ? (blend && blend->blend ? 2 : 4)
                     : blend && blend->blend ? 1
                                             : 0;
      d.fs = &fsp;
      if (!d.ubos) {
         ctx->n_draw_ubos = 0;
         d.ubos = ctx->draw_ubos;
      }
      if (fs->mem_dwords)
         mem_consts(ctx, MESA_SHADER_FRAGMENT, fs, fsk, 9, &d.fs_mem);
      else
         fill_consts(ctx, MESA_SHADER_FRAGMENT, fs, fsk, 9, NULL);
      d.n_ubos = ctx->n_draw_ubos;
      /* a constant-colour blend reads the blend colour, packed BGRA8, in sa2 */
      if (blend && blend->blend && blend->constant) {
         const float *c = ctx->blend_color.color;

         fsk[SGXWS_BLEND_CONST_SA] =
            float_to_ubyte(c[3]) << 24 | float_to_ubyte(c[0]) << 16 |
            float_to_ubyte(c[1]) << 8 | float_to_ubyte(c[2]);
         fsp.sec_dwords = MAX2(fsp.sec_dwords, 9);
         fsp.temps = MAX2(fsp.temps, 2);
      }
      if (fs->uses_front_face) {
         fsk[SGX_SA_FRONT] =
            !(ctx->rasterizer && ctx->rasterizer->base.front_ccw);
         fsp.sec_dwords = MAX2(fsp.sec_dwords, 9);
      }
      /* a discard's visibility test (atst8 and pcoeff read sa3, sa4; ISA.md
       * "Phases, visibility feedback and discard"): sa3 = 0xe000 | ISPA's depth
       * compare (7 with the test off) | 8 without depth writes (always 0xe00f,
       * LESS 0xe001, LEQUAL no write 0xe00b); sa4 = 0xee00 [G] */
      if (fs->discard_pa >= 0 || fs->depth_pa >= 0) {
         fsk[3] = 0xe000 | (st.depth_func ? (st.depth_func - 0x200) & 7 : 7) |
                  (st.depth_write ? 0 : 8);
         fsk[4] = 0xee00;
      }
      /* a dependent sample's texture state: the DOUTT words, in SA */
      for (unsigned j = 0; j < fs->n_deps; j++) {
         unsigned unit = fs->dep_sampler[j];
         struct pipe_sampler_view *view =
            unit < PIPE_MAX_SAMPLERS ? ctx->views[unit] : NULL;

         if (!view || !sgx_resource_sampleable(sgx_resource(view->texture))) {
            SGX_DBG(DRAW,
                    "sampler %u: no texture the GPU can sample, draw skipped\n",
                    unit);
            return;
         }
         /* a texelFetch reads the texel at an explicit level, whatever the
          * bound sampler filters or wraps (the compiler's sgx_lower_tex
          * puts the coordinate at the texel's centre) */
         const struct pipe_sampler_state fetch = {
            .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
            .min_img_filter = PIPE_TEX_FILTER_NEAREST,
            .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
            .min_mip_filter = PIPE_TEX_MIPFILTER_NEAREST,
            .max_lod = 1000.0f,
         };
         sgx_tex_words(view, fs->dep_fetch[j] ? &fetch : ctx->samplers[unit],
                       &fsk[fs->dep_sa[j]]);
         /* a read of plane c of an RG32 / RGBA32 texture: c planes on
          * (T2 is the address, unshifted) */
         if (fs->dep_plane[j])
            fsk[fs->dep_sa[j] + 2] +=
               fs->dep_plane[j] *
               (uint32_t)sgx_tex_plane_stride(view->texture);
         /* an F16 texel: the whole T3, as for a PDS sample's */
         if (fs->dep_f16[j])
            fsk[fs->dep_sa[j] + 3] = 0x01028800u;
         sgx_resource(view->texture)->use_seq = sgxws_next_seq(ctx->hw);
         sampled |= 1u << unit;
         SGX_DBG(
            DRAW,
            "  dependent sample %u: unit %u %s %ux%u target %u levels %u, state sa%u %08x %08x %08x %08x\n",
            j, unit, util_format_short_name(view->format),
            view->texture->width0, view->texture->height0,
            view->texture->target, view->texture->last_level + 1, fs->dep_sa[j],
            fsk[fs->dep_sa[j]], fsk[fs->dep_sa[j] + 1], fsk[fs->dep_sa[j] + 2],
            fsk[fs->dep_sa[j] + 3]);
      }
      d.fs_consts = fsk;
      if (sgx_drv_debug & SGX_DEBUG_DRAW) {
         fprintf(stderr, "  fs SA block: %u dwords, %u temps:", fsp.sec_dwords,
                 fsp.temps);
         for (unsigned i = 0; i < fsp.sec_dwords; i++)
            fprintf(stderr, " %08x", fsk[i]);
         fprintf(stderr, "\n");
      }
      for (unsigned i = 0; i < d.fs->n_douti; i++) {
         unsigned unit = d.fs->douti_tex[i];
         struct pipe_sampler_view *view = unit ? ctx->views[unit - 1] : NULL;

         if (!unit)
            continue;
         if (!view || !sgx_resource_sampleable(sgx_resource(view->texture))) {
            SGX_DBG(DRAW,
                    "sampler %u: no texture the GPU can sample, draw skipped\n",
                    unit - 1);
            return;
         }
         sgx_tex_words(view, ctx->samplers[unit - 1], tex[unit - 1]);
         sgx_resource(view->texture)->use_seq = sgxws_next_seq(ctx->hw);
         sampled |= 1u << (unit - 1);
      }
      d.tex = (const uint32_t (*)[4])tex;
   } else {
      if (blend && !ctx->warned_blend) {
         fprintf(
            stderr,
            "sgx: blending and colour masks need a compiled fragment shader\n");
         ctx->warned_blend = true;
      }
      switch (fs->kind) {
      case SGX_SHADER_FS_CONSTANT:
         memcpy(color, fs->color, sizeof(color));
         break;
      case SGX_SHADER_FS_UNIFORM: {
         const uint8_t *cb =
            (const uint8_t *)constant_data(ctx, MESA_SHADER_FRAGMENT);

         if (!cb)
            return;
         memcpy(color, cb + fs->ubo_offset, sizeof(color));
         break;
      }
      default:
         return;
      }
      d.fs = &ctx->fs_uniform;
      fsc[SGXWS_FS_UNIFORM_OFF] = sgxws_f16(color[0]) | sgxws_f16(color[1])
                                                           << 16;
      fsc[SGXWS_FS_UNIFORM_OFF + 1] = sgxws_f16(color[2]) | sgxws_f16(color[3])
                                                               << 16;
      d.fs_consts = fsc;
   }
   switch (info->mode) {
   case MESA_PRIM_TRIANGLES:
      prim = SGXWS_TRIANGLES;
      break;
   case MESA_PRIM_TRIANGLE_STRIP:
      prim = SGXWS_TRIANGLE_STRIP;
      break;
   case MESA_PRIM_TRIANGLE_FAN:
   case MESA_PRIM_QUADS:
   case MESA_PRIM_QUAD_STRIP:
   case MESA_PRIM_POLYGON:
      prim = SGXWS_TRIANGLES; /* drawn as a list */
      break;
   case MESA_PRIM_LINES:
   case MESA_PRIM_LINE_STRIP: /* drawn as a list: the hardware has no line strip
                               */
   case MESA_PRIM_LINE_LOOP:
      prim = SGXWS_LINES;
      break;
   case MESA_PRIM_POINTS:
      /* the size comes from the vertex program; without one the DDK draws
       * nothing too */
      if (!compiled || !ctx->vs->writes_psize)
         return;
      prim = SGXWS_POINTS;
      break;
   default:
      if (!ctx->warned_prim)
         fprintf(stderr, "sgx: %s not drawn yet\n", u_prim_name(info->mode));
      ctx->warned_prim = true;
      return;
   }
   for (unsigned e = 0; e < n_fetch; e++) {
      if (!setup_fetch(ctx, e, &f[e]))
         return;
      if (compiled) {
         f[e].dwords = 4;
         f[e].defaults = true;
         f[e].raw = raw;
         f[e].raw_bytes = raw ? raw_bytes[e] : 0;
      } else {
         /* gl_Position = attribute 0: two components fetched -> (x, y, 0, 1);
          * more -> (x, y, z, 1) */
         f[e].dwords = util_format_get_nr_components(f[e].format) == 2 ? 2 : 3;
         f[e].defaults = false;
         d.vs = f[e].dwords == 2 ? &ctx->vs_xy : &ctx->vs_xyz;
      }
      vertex_dwords += f[e].dwords;
   }
   if (compiled && fs->kind == SGX_SHADER_COMPILED &&
       !varyings_match(d.vs, d.fs)) {
      if (!ctx->warned_varyings)
         fprintf(
            stderr,
            "sgx: the fragment shader iterates a varying the vertex shader doesn't "
            "write (VS sets %#x, FS DOUTI %#x...): draw skipped\n",
            d.vs->tsp_sizes, sgx_pds_douti_w0(&d.fs->douti[0]));
      ctx->warned_varyings = true;
      return;
   }
   d.state = &st;
   d.prim = prim;

   if (info->index_size) {
      if (info->has_user_indices)
         indices = info->index.user;
      else
         indices = sgx_resource(info->index.resource)->data;
   }

   simple_mtx_lock(&screen->lock);
   if (!begin_frame(ctx, false))
      goto out;
   /* every shared texture it samples goes in the render's buffer list: a
    * frame whose list is full is submitted first */
   {
      const struct sgxws_bo *shared[PIPE_MAX_SAMPLERS];
      unsigned n_shared = 0;

      u_foreach_bit (unit, sampled) {
         struct sgx_resource *tex = sgx_resource(ctx->views[unit]->texture);

         if (tex->shared)
            shared[n_shared++] = &tex->bo;
      }
      if (!sgxws_use_bo_room(ctx->hw, shared, n_shared)) {
         sgxws_flush(ctx->hw, 0);
         ctx->recording = NULL;
         ctx->stats.fl_other++;
         if (!begin_frame(ctx, false))
            goto out;
      }
   }
   /* The textures it samples were marked used by the next kick before
    * begin_frame, which flushes the previous target's frame when this draw
    * switches targets: mark them again with the frame they're now in (a
    * resource freed after this draw flushes that frame first: glCopyPixels'
    * temporary, sampled into the window and freed at once).  Only those: a
    * texture merely bound would look in flight, and a later upload into it
    * would go to its second copy (dEQP fbo.render.stencil*) */
   u_foreach_bit (unit, sampled) {
      struct sgx_resource *tex = sgx_resource(ctx->views[unit]->texture);

      tex->use_seq = sgxws_next_seq(ctx->hw);
      if (tex->shared)
         sgxws_use_bo(ctx->hw, &tex->bo, false);
   }
   for (unsigned n = 0; n < num_draws; n++) {
      unsigned count = draws[n].count, start = draws[n].start, out_count;
      bool list = expanded_prim(info->mode);
      uint32_t *v;

      /* only whole primitives: the VDM takes the count as it is */
      if (!u_trim_pipe_prim(info->mode, &count))
         continue;
      /* a strip longer than the index limit is drawn as a list of its
       * triangles, which goes in pieces like any list */
      d.prim = prim;
      if (info->mode == MESA_PRIM_TRIANGLE_STRIP && count > SGXWS_MAX_INDICES) {
         list = true;
         d.prim = SGXWS_TRIANGLES;
      }
      out_count = list ? expanded_count(info->mode, count) : count;
      if (!out_count)
         continue;
      /* the vertex cache first: a display list drawn every frame is one DMA
       * a vertex from its cached gather and its quads' indices expanded
       * once, where zero-copy DMAs each input and expands them every draw
       * (docs/PERF.md) */
      bool zc = compiled && sgx_screen(ctx->base.screen)->zero_copy;

      /* (zero-copy's draw from an index buffer as it is goes first: nothing
       * of it is read on the CPU) */
      if (zc && resident_indexed(info, &draws[n], f, n_fetch) &&
          zc_draw(ctx, info, &draws[n], indices, f, n_fetch, &d))
         continue;
      if (compiled && (prim == SGXWS_TRIANGLES) &&
          vcache_draw(ctx, info, &draws[n], f, n_fetch, vertex_dwords, &d))
         continue;
      if (zc && zc_draw(ctx, info, &draws[n], indices, f, n_fetch, &d))
         continue;
      /* glDrawArrays of a list or strip: the vertices in order */
      if (!info->index_size && !list && count <= SGXWS_MAX_INDICES) {
         if (!(v = staging(ctx, (size_t)count * vertex_dwords)))
            continue;
         fetch_vertices(f, n_fetch, start, count, v, vertex_dwords);
         d.verts = v;
         d.count = count;
         d.indices = NULL;
         if (sgxws_draw(ctx->hw, &d))
            fprintf(stderr, "sgx: draw of %u vertices dropped\n", count);
         continue;
      }
      /* indices (and what is drawn as a list): each vertex the draw uses
       * fetched once, the GPU given 16-bit indices into them, as the DDK does.
       * A list longer than the index limit goes in pieces of whole primitives. */
      if (list || prim != SGXWS_TRIANGLE_STRIP ||
          out_count <= SGXWS_MAX_INDICES) {
         unsigned piece = d.prim == SGXWS_TRIANGLES ? SGXWS_MAX_INDICES / 3 * 3
                          : d.prim == SGXWS_LINES   ? SGXWS_MAX_INDICES / 2 * 2
                                                    : SGXWS_MAX_INDICES;

         for (unsigned p0 = 0; p0 < out_count; p0 += piece) {
            unsigned np = MIN2(piece, out_count - p0), lo = ~0u, hi = 0;
            uint32_t *sparse = NULL;
            uint32_t *ix32 =
               staging(ctx, np); /* the piece's indices, before rebasing */
            uint16_t *ix = index_staging(ctx, np);

            if (!ix || !ix32)
               break;
            for (unsigned i = 0; i < np; i++) {
               unsigned k = list
                               ? expanded_vertex(info->mode, p0 + i, out_count)
                               : p0 + i;

               ix32[i] =
                  info->index_size
                     ? get_index(info, indices, start + k) + draws[n].index_bias
                     : start + k;
               lo = MIN2(lo, ix32[i]);
               hi = MAX2(hi, ix32[i]);
            }
            if (hi - lo < 65536 && hi - lo + 1 <= 4 * np + 64) {
               for (unsigned i = 0; i < np; i++)
                  ix[i] = (uint16_t)(ix32[i] - lo);
               /* (the indices are copied out of the staging buffer first) */
               if (!(v = staging(ctx, (size_t)(hi - lo + 1) * vertex_dwords)))
                  break;
               fetch_vertices(f, n_fetch, lo, hi - lo + 1, v, vertex_dwords);
               d.count = hi - lo + 1;
               d.indices = ix;
               d.n_indices = np;
            } else {
               /* a sparse range would fetch vertices nobody draws: this piece
                * expanded, a vertex per index (ix32 is in the staging buffer) */
               if (!(sparse = malloc((size_t)np * vertex_dwords * 4)))
                  break;
               for (unsigned i = 0; i < np; i++) {
                  uint32_t *dst = sparse + i * vertex_dwords;

                  for (unsigned e = 0; e < n_fetch; e++) {
                     fetch_vertex(&f[e], ix32[i], dst);
                     dst += f[e].dwords;
                  }
               }
               v = sparse;
               d.count = np;
               d.indices = NULL;
            }
            d.verts = v;
            SGX_DBG(DRAW, "draw %s x%u (%u vertices)%s\n",
                    u_prim_name(info->mode), np, d.count,
                    compiled ? " (compiled)" : "");
            if (sgxws_draw(ctx->hw, &d))
               fprintf(stderr, "sgx: draw of %u indices dropped\n", np);
            free(sparse);
            sparse = NULL;
         }
         d.indices = NULL;
         continue;
      }
      /* the rest: every index expanded to its own vertex */
      if (!(v = staging(ctx, (size_t)out_count * vertex_dwords)))
         continue;
      for (unsigned i = 0; i < out_count; i++) {
         unsigned k = list ? expanded_vertex(info->mode, i, out_count) : i;
         uint32_t index, *dst = v + i * vertex_dwords;

         index = info->index_size
                    ? get_index(info, indices, start + k) + draws[n].index_bias
                    : start + k;
         for (unsigned e = 0; e < n_fetch; e++) {
            fetch_vertex(&f[e], index, dst);
            dst += f[e].dwords;
         }
      }
      d.verts = v;
      d.count = out_count;
      SGX_DBG(DRAW, "draw %s x%u%s\n", u_prim_name(info->mode), out_count,
              compiled ? " (compiled)" : "");
      if (sgxws_draw(ctx->hw, &d))
         fprintf(stderr, "sgx: draw of %u vertices dropped\n", out_count);
   }
out:
   simple_mtx_unlock(&screen->lock);
}

static void
sgx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
             unsigned drawid_offset,
             const struct pipe_draw_indirect_info *indirect,
             const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct sgx_context *ctx = sgx_context(pctx);
   uint64_t t = 0;

   if (!info->instance_count)
      return;
   /* Primitive restart, split on the CPU: no restart index is known in the
    * VDM draw words (docs/GL3.md), and the indices are read here anyway.
    * The split comes back here as several draws without restart. */
   if (info->primitive_restart && info->index_size && !indirect) {
      for (unsigned n = 0; n < num_draws; n++)
         util_draw_vbo_without_prim_restart(pctx, info, drawid_offset + n,
                                            NULL, &draws[n]);
      return;
   }
   /* Instancing: one draw per instance.  No instance count is known in the
    * VDM draw words (docs/GL3.md), so each instance gets its own draw, with
    * gl_InstanceID in a vertex constant and its instanced elements' values
    * fetched for it (setup_fetch). */
   /* depth textures renders stored into, before the frame records a draw
    * that samples them (sgx_resource_zls_for_sampling) */
   for (unsigned i = 0; i < ARRAY_SIZE(ctx->views); i++)
      if (ctx->views[i] && ctx->views[i]->texture)
         sgx_resource_zls_for_sampling(ctx,
                                       sgx_resource(ctx->views[i]->texture));
   if (ctx->stats.on)
      t = os_time_get_nano();
   ctx->base_instance = info->start_instance;
   /* a planar colour buffer (RG32, RGBA32): the draw once a channel, into
    * its plane; the first pass the colour mask lets through tests and writes
    * depth and stencil, the later ones only test depth (build_state) */
   /* MRT: a pass a colour buffer the shader writes (H1: a
    * second PBE emit hangs the GPU), the first pass of all testing and
    * writing depth and stencil */
   unsigned targets = MAX2(ctx->framebuffer.nr_cbufs, 1);
   bool drawn = false;

   for (unsigned t = 0; t < targets; t++) {
      ctx->rt_index = t;
      if (targets > 1 && (!sgx_cbuf(ctx)->texture || !fs_writes_target(ctx, t)) &&
          (drawn || t + 1 < targets))
         continue;
      unsigned planes = planar_planes(ctx), cm = planes ? colour_mask(ctx) : 1;

      for (unsigned c = 0; c < MAX2(planes, 1); c++) {
         if (planes && !(cm >> c & 1) && (drawn || c + 1 < planes))
            continue;
         ctx->rt_plane = c;
         ctx->rt_later_pass = drawn;
         for (unsigned i = 0; i < info->instance_count; i++) {
            ctx->instance = i;
            sgx_draw_vbo_impl(pctx, info, drawid_offset, indirect, draws,
                              num_draws);
         }
         drawn = true;
      }
   }
   ctx->rt_index = 0;
   ctx->rt_plane = 0;
   ctx->rt_later_pass = false;
   ctx->instance = ctx->base_instance = 0;
   if (!ctx->stats.on)
      return;
   ctx->stats.draw_ns += os_time_get_nano() - t;
   ctx->stats.draws += num_draws;
   for (unsigned n = 0; n < num_draws; n++)
      ctx->stats.verts += draws[n].count;
}

void
sgx_draw_init(struct sgx_context *ctx)
{
   /* SGX_DEBUG=novcache: no vertex cache */
   if (!(sgx_drv_debug & SGX_DEBUG_NO_VCACHE)) {
      ctx->vcache = _mesa_hash_table_create(NULL, vcache_hash, vcache_equal);
      list_inithead(&ctx->vcache_lru);
      list_inithead(&ctx->vcache_chunks);
   }
   ctx->base.draw_vbo = sgx_draw_vbo;
   ctx->base.clear = sgx_clear;
}
