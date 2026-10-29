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
#include "util/os_time.h"
#include "util/half_float.h"
#include "util/hash_table.h"
#include "util/list.h"
#include "util/u_math.h"
#include "util/u_inlines.h"
#include "util/u_prim.h"

#include "sgx545_context.h"
#include "sgx545_program.h"
#include "sgx545_resource.h"
#include "sgx545_screen.h"
#include "sgx545_texture.h"

/* A target's TA/3D structures, one set per target address while there are
 * few: a target has one render in flight, so the back buffer and fb0 (the
 * swap's copy) need sets of their own to render one after the other without
 * waiting.  Past SGX545_MAX_RTS targets share by size. */
static struct sgxws_rt *
get_rt(struct sgx545_context *ctx, uint32_t w, uint32_t h, uint32_t va)
{
   for (unsigned i = 0; i < ctx->num_rts; i++)
      if (ctx->rts[i]->w == w && ctx->rts[i]->h == h && ctx->rt_va[i] == va)
         return ctx->rts[i];
   if (ctx->num_rts == SGX545_MAX_RTS) {
      for (unsigned i = 0; i < ctx->num_rts; i++)
         if (ctx->rts[i]->w == w && ctx->rts[i]->h == h)
            return ctx->rts[i];
      return NULL;
   }
   ctx->rts[ctx->num_rts] = sgxws_rt_create(ctx->hw, w, h);
   ctx->rt_va[ctx->num_rts] = va;
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
   rt = get_rt(ctx, res->base.width0, res->base.height0, t.va);
   if (!rt)
      return false;
   if (ctx->recording && ctx->recording != res) {
      ctx->recording = NULL;    /* sgxws_begin flushes the other frame */
      ctx->stats.fl_switch++;
   }
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
   if (debug_get_num_option("SGX545_DEBUG_ATTRS", 0))
      fprintf(stderr, "sgx545 clear %#x mask %#x depth %.3f, %u draws in the frame\n", buffers, color_clear_mask,
              depth, ctx->hw ? ctx->hw->draws : 0);
   /* the colour bytes written, as the draws' colour mask (B b0 G b1 R b2 A
    * b3); a buffer without alpha keeps its X byte written. A colour clear
    * with a mask comes here, not as a quad: glColorMask(0, 0, 0, 1) and a
    * clear (the openhc-minecraft overlay, every frame) must not touch RGB. */
   unsigned cm = buffers & PIPE_CLEAR_COLOR0 ? color_clear_mask & 0xf : 0, wmask;

   if (ctx->framebuffer.nr_cbufs && ctx->framebuffer.cbufs[0].texture &&
       !util_format_has_alpha(ctx->framebuffer.cbufs[0].format) && cm)
      cm |= PIPE_MASK_A;
   wmask = (cm & PIPE_MASK_B ? 1 : 0) | (cm & PIPE_MASK_G ? 2 : 0) | (cm & PIPE_MASK_R ? 4 : 0) |
           (cm & PIPE_MASK_A ? 8 : 0);
   /* a depth-only clear writes no colour (mask 0) */
   if (!(buffers & PIPE_CLEAR_COLOR0) && !(have_depth && (buffers & PIPE_CLEAR_DEPTH)))
      return;
   if ((buffers & PIPE_CLEAR_COLOR0) && !wmask && !(have_depth && (buffers & PIPE_CLEAR_DEPTH)))
      return;        /* every colour channel masked, no depth */
   simple_mtx_lock(&screen->lock);
   if (begin_frame(ctx)) {
      static const float black[4];

      /* The ISP starts every render at depth 1.0 (render details +0x40), so
       * a depth clear only needs drawing when it is not 1.0. */
      sgxws_clear(ctx->hw, (buffers & PIPE_CLEAR_COLOR0) ? color->f : black, (float)depth,
                  have_depth && (buffers & PIPE_CLEAR_DEPTH), wmask);
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

static const float *
constant_data(struct sgx545_context *ctx, mesa_shader_stage stage)
{
   struct pipe_constant_buffer *cb = &ctx->constants[stage];

   if (cb->user_buffer)
      return (const float *)((const uint8_t *)cb->user_buffer + cb->buffer_offset);
   if (cb->buffer)
      return (const float *)((const uint8_t *)sgx545_resource(cb->buffer)->data + cb->buffer_offset);
   return NULL;
}

/* where one vertex element comes from */
struct fetch {
   const uint8_t *base;
   uint32_t stride;
   enum pipe_format format;
   unsigned dwords;             /* dwords it fills in the vertex */
   bool defaults;               /* missing components 0, 0, 0, 1 (compiled shaders) */
   const struct sgx545_resource *res;   /* the buffer, NULL for client memory */
   uint32_t offset;             /* where in it */
};

static bool
setup_fetch(struct sgx545_context *ctx, unsigned e, struct fetch *f)
{
   const struct pipe_vertex_element *ve;
   const struct pipe_vertex_buffer *vb;

   if (!ctx->velems || e >= ctx->velems->num_elements)
      return false;
   ve = &ctx->velems->elements[e];
   if (ve->vertex_buffer_index >= ctx->num_vertex_buffers)
      return false;
   vb = &ctx->vertex_buffers[ve->vertex_buffer_index];
   f->res = NULL;
   if (vb->is_user_buffer) {
      f->base = vb->buffer.user;
   } else if (vb->buffer.resource) {
      f->res = sgx545_resource(vb->buffer.resource);
      f->base = f->res->data;
   } else {
      return false;
   }
   f->offset = vb->buffer_offset + ve->src_offset;
   f->base += f->offset;
   /* an instanced element is one value for the whole draw (one instance) */
   f->stride = ve->instance_divisor ? 0 : ve->src_stride;
   f->format = ve->src_format;
   return true;
}

static void
fetch_vertex(const struct fetch *f, unsigned index, uint32_t *out)
{
   const uint8_t *src = f->base + (size_t)index * f->stride;
   float v[4] = { 0, 0, 0, 1 };

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

      util_format_unpack_rgba(f->format, t, src, 1);
      memcpy(v, t, n * sizeof(float));
   }
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

/* element f of vertices first .. first + n - 1 into dst, vertex_dwords
 * apart: the formats Minecraft's terrain and gl4es use go without a call
 * per vertex (they took most of a frame's CPU time) */
static void
fetch_range(const struct fetch *f, unsigned first, unsigned n, uint32_t *dst, unsigned vertex_dwords)
{
   static float unorm8[256];
   const uint8_t *src = f->base + (size_t)first * f->stride;
   float *o = (float *)dst;

   if (!f->defaults) {
      for (unsigned i = 0; i < n; i++)
         fetch_vertex(f, first + i, dst + i * vertex_dwords);
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
fetch_vertices(const struct fetch *f, unsigned n_fetch, unsigned first, unsigned n, uint32_t *v,
               unsigned vertex_dwords)
{
   unsigned off = 0;

   for (unsigned e = 0; e < n_fetch; e++) {
      fetch_range(&f[e], first, n, v + off, vertex_dwords);
      off += f[e].dwords;
   }
}

static uint16_t *
index_staging(struct sgx545_context *ctx, unsigned n)
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

/* a compiled shader's constants: sa[base + slot] = constant buffer 0 float k */
static void
fill_consts(struct sgx545_context *ctx, mesa_shader_stage stage, const struct sgx545_shader *so,
            uint32_t *block, unsigned base)
{
   const float *cb = constant_data(ctx, stage);
   unsigned avail = ctx->constants[stage].buffer_size / 4;

   /* F16 halves and C10 channels share a slot */
   for (unsigned i = 0; i < so->n_consts; i++)
      if (so->consts[i].fmt != 1)
         block[base + so->consts[i].slot] = 0;
   for (unsigned i = 0; i < so->n_consts; i++) {
      const struct sgx545_const *k = &so->consts[i];
      float v = cb && k->k < avail ? cb[k->k] : 0.0f;

      if (k->fmt == 2) {
         block[base + k->slot] |= (uint32_t)sgxws_f16(v) << k->shift;
      } else if (k->fmt == 3) {
         /* C10: round(v * 255), signed 10 bits */
         int c = (int)lrintf(CLAMP(v, -2.0f, 2.0f) * 255.0f);

         block[base + k->slot] |= ((uint32_t)CLAMP(c, -512, 511) & 0x3ff) << k->shift;
      } else {
         memcpy(&block[base + k->slot], &v, 4);
      }
      SGX545_DBG(DRAW, "  %s constant %u -> sa%u (fmt %u at %u) = %f\n", stage == MESA_SHADER_VERTEX ? "vs" : "fs",
                 k->k, base + k->slot, k->fmt, k->shift, v);
   }
}

/* Every set the fragment program iterates or samples with must be one the
 * vertex program outputs, with as many components: iterating a set the vertex
 * program doesn't write locks the GPU up (EA3; first seen with the colour
 * placeholder, then with gl4es' neverball shaders). */
static bool
varyings_match(const struct sgxws_prog *vs, const struct sgxws_prog *fs)
{
   for (unsigned i = 0; i < fs->n_douti; i++) {
      uint32_t w = fs->douti[i];
      unsigned need[2][2] = { { 0, 0 } }, n = 0;

      /* plain iteration: set + 1 at b11-16, components - 1 at b22-23; sources
       * from 0x20 on are not varyings (0x22: the depth plane of a discard) */
      if ((w & (1u << 24)) && ((w >> 11) & 0x3f) < 0x20)
         need[n][0] = ((w >> 11) & 0x3f) - 1, need[n++][1] = ((w >> 22) & 3) + 1;
      if (w & 0x600)               /* sample half: coordinate set + 1 in the low bits, 2-D or projected */
         need[n][0] = (w & 0x3f) - 1, need[n++][1] = w & 0x400 ? 3 : 2;
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
 * gl4es draws its display lists (Minecraft's chunks) from buffers that don't
 * change between frames, and every frame the draw gathered, converted and
 * copied the same vertices again: a third of the render thread's time.  A
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
      uint32_t konst[4];        /* a constant element's value (stride 0) */
   } el[PIPE_MAX_ATTRIBS];
};

struct vcache_entry {
   struct vcache_key key;       /* first: the hash table's key */
   struct list_head link;
   struct sgxws_bo bo;
   uint32_t verts_va, idx_va, nverts, nidx, last_seq, bytes;
};

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
vcache_free(struct sgx545_context *ctx, struct vcache_entry *v)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);

   _mesa_hash_table_remove_key(ctx->vcache, &v->key);
   list_del(&v->link);
   ctx->vcache_bytes -= v->bytes;
   sgxws_bo_free(&screen->dev, &v->bo);
   free(v);
}

/* room for bytes more: the least recently used entries the GPU is done with go */
static bool
vcache_room(struct sgx545_context *ctx, uint32_t bytes)
{
   uint32_t done = sgxws_done_seq(ctx->hw);

   while (ctx->vcache_bytes + bytes > VCACHE_BUDGET && !list_is_empty(&ctx->vcache_lru)) {
      struct vcache_entry *v = list_last_entry(&ctx->vcache_lru, struct vcache_entry, link);

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
vcache_draw(struct sgx545_context *ctx, const struct pipe_draw_info *info,
            const struct pipe_draw_start_count_bias *draw, const struct fetch *f, unsigned n_fetch,
            unsigned vertex_dwords, struct sgxws_draw *d)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);
   const struct sgx545_resource *ib = NULL;
   struct vcache_key key;
   struct hash_entry *he;
   struct vcache_entry *v;
   bool fan = info->mode == MESA_PRIM_TRIANGLE_FAN;
   unsigned count = draw->count, out_count = fan ? (count >= 3 ? (count - 2) * 3 : 0) : count;
   uint64_t now = os_time_get_nano();

   if (!ctx->vcache || out_count < 3 || n_fetch > PIPE_MAX_ATTRIBS)
      return false;
   if (info->index_size) {
      if (info->has_user_indices)
         return false;
      ib = sgx545_resource(info->index.resource);
   }
   if (out_count > SGXWS_MAX_INDICES || (ib && now - ib->written_ns < 1000000000ull))
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
      key.el[e].format = f[e].format;
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
         unsigned k = fan ? ((i % 3 == 0) ? 0 : i / 3 + i % 3) : i;
         unsigned index = info->index_size ? get_index(info, indices, draw->start + k) + draw->index_bias
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
      if (sgxws_bo_alloc(&screen->dev, &v->bo, SGX545_HEAP_GENERAL, 0x1, align(vbytes + ibytes, 4096), 0x1000)) {
         free(v);
         ctx->stats.vc_uncacheable++;
         return false;
      }
      /* gathered in cached memory, then copied whole (the bo is write-combined) */
      verts = staging(ctx, vbytes / 4 + ibytes / 4);
      if (!verts) {
         sgxws_bo_free(&screen->dev, &v->bo);
         free(v);
         return false;
      }
      fetch_vertices(f, n_fetch, lo, hi - lo + 1, verts, vertex_dwords);
      ix = (uint16_t *)((uint8_t *)verts + vbytes);
      for (unsigned i = 0; i < out_count; i++) {
         unsigned k = fan ? ((i % 3 == 0) ? 0 : i / 3 + i % 3) : i;
         unsigned index = info->index_size ? get_index(info, indices, draw->start + k) + draw->index_bias
                                           : draw->start + k;

         ix[i] = (uint16_t)(index - lo);
      }
      memcpy(v->bo.map, verts, vbytes + out_count * 2);
      v->key = key;
      v->verts_va = v->bo.va;
      v->idx_va = v->bo.va + vbytes;
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
      fprintf(stderr, "sgx545: draw of %u indices dropped\n", v->nidx);
   d->verts_va = d->idx_va = 0;
   return true;
}

void
sgx545_draw_fini(struct sgx545_context *ctx)
{
   struct sgx545_screen *screen = sgx545_screen(ctx->base.screen);

   if (!ctx->vcache)
      return;
   simple_mtx_lock(&screen->lock);
   list_for_each_entry_safe(struct vcache_entry, v, &ctx->vcache_lru, link)
      vcache_free(ctx, v);
   simple_mtx_unlock(&screen->lock);
   _mesa_hash_table_destroy(ctx->vcache, NULL);
   ctx->vcache = NULL;
}

static void
sgx545_draw_vbo_impl(struct pipe_context *pctx, const struct pipe_draw_info *info,
                unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
                const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   struct sgx545_screen *screen = sgx545_screen(pctx->screen);
   struct sgxws_state st;
   struct sgxws_draw d = { 0 };
   struct sgxws_prog fsp;
   struct sgxws_blend epi = { 0 };
   const struct sgxws_blend *blend = NULL;
   uint32_t fsc[SGXWS_FS_UNIFORM_DWORDS] = { 0 }, vsc[SGX545_MAX_CONSTS] = { 0 };
   uint32_t fsk[9 + SGX545_MAX_CONSTS] = { 0 }, tex[PIPE_MAX_SAMPLERS][4];
   const void *indices = NULL;
   struct fetch f[PIPE_MAX_ATTRIBS];
   unsigned n_fetch = 1, vertex_dwords = 0;
   float color[4];
   uint32_t prim;
   bool compiled;

   if (indirect || !ctx->vs || !ctx->fs)
      return;
   build_state(ctx, &st);
   if (ctx->framebuffer.nr_cbufs && ctx->framebuffer.cbufs[0].texture) {
      /* the colour epilogue: blend, then the colour mask (sgxws_blend) */
      bool has_alpha = util_format_has_alpha(ctx->framebuffer.cbufs[0].format);
      unsigned mask = ctx->blend ? ctx->blend->base.rt[0].colormask : PIPE_MASK_RGBA;

      if (ctx->blend && ctx->blend->enable)
         epi = ctx->blend->hw[has_alpha];
      if (!has_alpha)
         mask |= PIPE_MASK_A;
      /* bytes of the packed BGRA8 colour */
      epi.wmask = (mask & PIPE_MASK_B ? 1 : 0) | (mask & PIPE_MASK_G ? 2 : 0) |
                  (mask & PIPE_MASK_R ? 4 : 0) | (mask & PIPE_MASK_A ? 8 : 0);
      if (epi.blend || epi.wmask != 0xf)
         blend = &epi;
   }
   /* each stage compiled or one of the fixed programs */
   compiled = ctx->vs->kind == SGX545_SHADER_COMPILED;
   if (compiled) {
      d.vs = &ctx->vs->prog;
      fill_consts(ctx, MESA_SHADER_VERTEX, ctx->vs, vsc, 0);
      d.vs_consts = vsc;
      n_fetch = ctx->vs->n_attribs;
   } else if (ctx->vs->kind != SGX545_SHADER_VS_POSITION) {
      return;
   }
   /* SGX545_EXP_FS_CONST=1 (measuring): every fragment shader a constant white */
   static int exp_fs_const = -1;

   if (exp_fs_const < 0)
      exp_fs_const = debug_get_bool_option("SGX545_EXP_FS_CONST", false);
   if (ctx->fs->kind == SGX545_SHADER_COMPILED && !exp_fs_const) {
      fsp = ctx->fs->prog;
      if (blend) {
         fsp.code = sgx545_fs_program(ctx, ctx->fs, blend);
         if (!fsp.code)
            return;
      }
      /* pass type: a blend 1; discard 4, or 2 with a blend (ISA.md) */
      st.pass_type = ctx->fs->discard_pa >= 0 ? (blend && blend->blend ? 2 : 4) : blend && blend->blend ? 1 : 0;
      d.fs = &fsp;
      fill_consts(ctx, MESA_SHADER_FRAGMENT, ctx->fs, fsk, 9);
      /* a discard's visibility test (atst8 and pcoeff read sa3, sa4), as the
       * DDK writes them (EA3): sa3 = 0xe000 | ISPA's depth compare (7 with the
       * test off) | 8 without depth writes (always 0xe00f, LESS 0xe001,
       * LEQUAL no write 0xe00b); sa4 = 0xee00 [G] */
      if (ctx->fs->discard_pa >= 0) {
         fsk[3] = 0xe000 | (st.depth_func ? (st.depth_func - 0x200) & 7 : 7) | (st.depth_write ? 0 : 8);
         fsk[4] = 0xee00;
      }
      /* a dependent sample's texture state: the DOUTT words, in SA */
      for (unsigned j = 0; j < ctx->fs->n_deps; j++) {
         unsigned unit = ctx->fs->dep_sampler[j];
         struct pipe_sampler_view *view = unit < PIPE_MAX_SAMPLERS ? ctx->views[unit] : NULL;

         if (!view || !sgx545_resource_sampleable(sgx545_resource(view->texture))) {
            SGX545_DBG(DRAW, "sampler %u: no texture the GPU can sample, draw skipped\n", unit);
            return;
         }
         sgx545_tex_words(view, ctx->samplers[unit], &fsk[ctx->fs->dep_sa[j]]);
         sgx545_resource(view->texture)->use_seq = sgxws_next_seq(ctx->hw);
         SGX545_DBG(DRAW, "  dependent sample %u: unit %u %s %ux%u target %u levels %u, state sa%u %08x %08x %08x %08x\n",
                    j, unit, util_format_short_name(view->format), view->texture->width0,
                    view->texture->height0, view->texture->target, view->texture->last_level + 1,
                    ctx->fs->dep_sa[j], fsk[ctx->fs->dep_sa[j]], fsk[ctx->fs->dep_sa[j] + 1],
                    fsk[ctx->fs->dep_sa[j] + 2], fsk[ctx->fs->dep_sa[j] + 3]);
      }
      d.fs_consts = fsk;
      for (unsigned i = 0; i < d.fs->n_douti; i++) {
         unsigned unit = d.fs->douti_tex[i];
         struct pipe_sampler_view *view = unit ? ctx->views[unit - 1] : NULL;

         if (!unit)
            continue;
         if (!view || !sgx545_resource_sampleable(sgx545_resource(view->texture))) {
            SGX545_DBG(DRAW, "sampler %u: no texture the GPU can sample, draw skipped\n", unit - 1);
            return;
         }
         sgx545_tex_words(view, ctx->samplers[unit - 1], tex[unit - 1]);
         sgx545_resource(view->texture)->use_seq = sgxws_next_seq(ctx->hw);
      }
      d.tex = (const uint32_t (*)[4])tex;
   } else {
      if (blend && !ctx->warned_blend) {
         fprintf(stderr, "sgx545: blending and colour masks need a compiled fragment shader\n");
         ctx->warned_blend = true;
      }
      switch (exp_fs_const ? SGX545_SHADER_FS_CONSTANT : ctx->fs->kind) {
      case SGX545_SHADER_FS_CONSTANT:
         if (exp_fs_const) {
            /* a colour per fragment program, so each shader's objects show
             * as flat silhouettes */
            uint32_t h = (uint32_t)(uintptr_t)ctx->fs * 2654435761u;

            color[0] = ((h >> 8) & 0xff) / 255.0f;
            color[1] = ((h >> 16) & 0xff) / 255.0f;
            color[2] = ((h >> 24) & 0xff) / 255.0f;
            color[3] = 1.0f;
            break;
         }
         memcpy(color, ctx->fs->color, sizeof(color));
         break;
      case SGX545_SHADER_FS_UNIFORM: {
         const uint8_t *cb = (const uint8_t *)constant_data(ctx, MESA_SHADER_FRAGMENT);

         if (!cb)
            return;
         memcpy(color, cb + ctx->fs->ubo_offset, sizeof(color));
         break;
      }
      default:
         return;
      }
      d.fs = &ctx->fs_uniform;
      fsc[SGXWS_FS_UNIFORM_OFF] = sgxws_f16(color[0]) | sgxws_f16(color[1]) << 16;
      fsc[SGXWS_FS_UNIFORM_OFF + 1] = sgxws_f16(color[2]) | sgxws_f16(color[3]) << 16;
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
      prim = SGXWS_TRIANGLES;     /* drawn as a list */
      break;
   default:
      if (!ctx->warned_prim)
         fprintf(stderr, "sgx545: %s not drawn yet\n", u_prim_name(info->mode));
      ctx->warned_prim = true;
      return;
   }
   for (unsigned e = 0; e < n_fetch; e++) {
      if (!setup_fetch(ctx, e, &f[e]))
         return;
      if (compiled) {
         f[e].dwords = 4;
         f[e].defaults = true;
      } else {
         /* gl_Position = attribute 0: two components fetched -> (x, y, 0, 1); more -> (x, y, z, 1) */
         f[e].dwords = util_format_get_nr_components(f[e].format) == 2 ? 2 : 3;
         f[e].defaults = false;
         d.vs = f[e].dwords == 2 ? &ctx->vs_xy : &ctx->vs_xyz;
      }
      vertex_dwords += f[e].dwords;
   }
   /* SGX545_DEBUG_ATTRS=n: the vertex elements of the first n draws with 5 or more inputs */
   {
      static int left = -1;

      if (left < 0)
         left = debug_get_num_option("SGX545_DEBUG_ATTRS", 0);
      if (left > 0 && compiled && n_fetch >= 5) {
         left--;
         fprintf(stderr, "sgx545 attrs: %s x%u start %u index_size %u; depth func %#x write %d, cull %#x front_ccw %d, "
                 "pass %u, blend %d wmask %#x, fs douti %u\n", u_prim_name(info->mode), draws[0].count, draws[0].start,
                 info->index_size, st.depth_func, st.depth_write, st.cull, st.front_ccw, st.pass_type,
                 blend ? blend->blend : 0, epi.wmask, d.fs ? d.fs->n_douti : 0);
         for (unsigned u = 0; u < PIPE_MAX_SAMPLERS; u++) {
            struct pipe_sampler_view *view = ctx->views[u];

            if (view)
               fprintf(stderr, "  sampler %u: %s %ux%u levels %u sampleable %d texture %d\n", u,
                       util_format_short_name(view->format), view->texture->width0, view->texture->height0,
                       view->texture->last_level + 1, sgx545_resource_sampleable(sgx545_resource(view->texture)),
                       sgx545_resource(view->texture)->texture);
         }
         for (unsigned e = 0; e < n_fetch; e++) {
            const struct pipe_vertex_element *ve = &ctx->velems->elements[e];
            uint32_t val[4];

            fetch_vertex(&f[e], info->index_size ? 0 : draws[0].start, val);
            fprintf(stderr, "  el %u: %s stride %u offset %u divisor %u vb %u %s -> %f %f %f %f\n", e,
                    util_format_short_name(f[e].format), f[e].stride, f[e].offset, ve->instance_divisor,
                    ve->vertex_buffer_index, f[e].res ? "buffer" : "client", uif(val[0]), uif(val[1]),
                    uif(val[2]), uif(val[3]));
         }
         {
            const float *cb = constant_data(ctx, MESA_SHADER_VERTEX);

            fprintf(stderr, "  vs constants (buffer %u floats):", ctx->constants[MESA_SHADER_VERTEX].buffer_size / 4);
            for (unsigned i = 0; i < ctx->vs->n_consts; i++)
               fprintf(stderr, " k%u>sa%u=%g", ctx->vs->consts[i].k, ctx->vs->consts[i].slot,
                       cb && ctx->vs->consts[i].k < ctx->constants[MESA_SHADER_VERTEX].buffer_size / 4
                          ? cb[ctx->vs->consts[i].k] : -999.0);
            fprintf(stderr, "\n");
         }
      }
   }
   if (compiled && ctx->fs->kind == SGX545_SHADER_COMPILED && !varyings_match(d.vs, d.fs)) {
      if (!ctx->warned_varyings)
         fprintf(stderr, "sgx545: the fragment shader iterates a varying the vertex shader doesn't "
                 "write (VS sets %#x, FS DOUTI %#x...): draw skipped\n", d.vs->tsp_sizes, d.fs->douti[0]);
      ctx->warned_varyings = true;
      return;
   }
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
      unsigned count = draws[n].count, start = draws[n].start, out_count;
      bool fan = info->mode == MESA_PRIM_TRIANGLE_FAN;
      uint32_t *v;

      out_count = fan ? (count >= 3 ? (count - 2) * 3 : 0) : count;
      if (!out_count)
         continue;
      if (compiled && prim != SGXWS_TRIANGLE_STRIP && vcache_draw(ctx, info, &draws[n], f, n_fetch, vertex_dwords, &d))
         continue;
      /* glDrawArrays of a list or strip: the vertices in order */
      if (!info->index_size && !fan && count <= SGXWS_MAX_INDICES) {
         if (!(v = staging(ctx, (size_t)count * vertex_dwords)))
            continue;
         fetch_vertices(f, n_fetch, start, count, v, vertex_dwords);
         d.verts = v;
         d.count = count;
         d.indices = NULL;
         if (sgxws_draw(ctx->hw, &d))
            fprintf(stderr, "sgx545: draw of %u vertices dropped\n", count);
         continue;
      }
      /* indices (and fans): each vertex the draw uses fetched once, the GPU
       * given 16-bit indices into them, as the DDK does.  A list longer than
       * the index limit goes in pieces of whole triangles. */
      if (fan || prim == SGXWS_TRIANGLES || out_count <= SGXWS_MAX_INDICES) {
         unsigned piece = prim == SGXWS_TRIANGLES ? SGXWS_MAX_INDICES / 3 * 3 : SGXWS_MAX_INDICES;

         for (unsigned p0 = 0; p0 < out_count; p0 += piece) {
            unsigned np = MIN2(piece, out_count - p0), lo = ~0u, hi = 0;
            uint32_t *sparse = NULL;
            uint32_t *ix32 = staging(ctx, np);    /* the piece's indices, before rebasing */
            uint16_t *ix = index_staging(ctx, np);

            if (!ix || !ix32)
               break;
            for (unsigned i = 0; i < np; i++) {
               unsigned k = p0 + i;

               if (fan)
                  k = (k % 3 == 0) ? 0 : k / 3 + k % 3;
               ix32[i] = info->index_size ? get_index(info, indices, start + k) + draws[n].index_bias
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
            SGX545_DBG(DRAW, "draw %s x%u (%u vertices)%s\n", u_prim_name(info->mode), np, d.count,
                       compiled ? " (compiled)" : "");
            if (sgxws_draw(ctx->hw, &d))
               fprintf(stderr, "sgx545: draw of %u indices dropped\n", np);
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
         unsigned k = i;
         uint32_t index, *dst = v + i * vertex_dwords;

         if (fan)
            k = (i % 3 == 0) ? 0 : i / 3 + i % 3;
         index = info->index_size ? get_index(info, indices, start + k) + draws[n].index_bias
                                  : start + k;
         for (unsigned e = 0; e < n_fetch; e++) {
            fetch_vertex(&f[e], index, dst);
            dst += f[e].dwords;
         }
      }
      d.verts = v;
      d.count = out_count;
      SGX545_DBG(DRAW, "draw %s x%u%s\n", u_prim_name(info->mode), out_count, compiled ? " (compiled)" : "");
      if (sgxws_draw(ctx->hw, &d))
         fprintf(stderr, "sgx545: draw of %u vertices dropped\n", out_count);
   }
out:
   simple_mtx_unlock(&screen->lock);
}

static void
sgx545_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info, unsigned drawid_offset,
                const struct pipe_draw_indirect_info *indirect, const struct pipe_draw_start_count_bias *draws,
                unsigned num_draws)
{
   struct sgx545_context *ctx = sgx545_context(pctx);
   uint64_t t;

   if (!ctx->stats.on) {
      sgx545_draw_vbo_impl(pctx, info, drawid_offset, indirect, draws, num_draws);
      return;
   }
   t = os_time_get_nano();
   sgx545_draw_vbo_impl(pctx, info, drawid_offset, indirect, draws, num_draws);
   ctx->stats.draw_ns += os_time_get_nano() - t;
   ctx->stats.draws += num_draws;
   for (unsigned n = 0; n < num_draws; n++)
      ctx->stats.verts += draws[n].count;
}

void
sgx545_draw_init(struct sgx545_context *ctx)
{
   /* SGX545_VCACHE=0: no vertex cache */
   if (debug_get_bool_option("SGX545_VCACHE", true)) {
      ctx->vcache = _mesa_hash_table_create(NULL, vcache_hash, vcache_equal);
      list_inithead(&ctx->vcache_lru);
   }
   ctx->base.draw_vbo = sgx545_draw_vbo;
   ctx->base.clear = sgx545_clear;
}
